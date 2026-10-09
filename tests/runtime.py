# spec: trap.def trap.int trap.float trap.convert trap.index trap.builtin trap.memory trap.exit expr.convert
"""Проход runtime исполнителя tests/run.py: тесты runtime по функциям.

- runtime/unit — граничные значения функций runtime без ловушек
  (tests/runtime/rt_test.c, `rt_test unit`).
- runtime/trap/ИМЯ — одна ловушка на запуск: точный текст stderr, вывод до
  неё и код выхода (trap.def).
- runtime/run/ИМЯ — вход dp_run: код выхода, ошибка main, предел глубины
  (trap.exit, trap.memory).
- runtime/floats — запись плавающих `str(x)` (Р48) у эталонного
  интерпретатора и у нативной программы против образца, вычисленного
  здесь по определению: кратчайшая десятичная запись, которая
  округляется обратно в то же число, — точной арифметикой Fraction, без
  printf и strtod. Значения: все степени двойки f64 и f32 (там промежуток
  округления несимметричен), особые и случайные.

Бинарник rt_test собирается с runtime теми же флагами, что программы
прохода native (C11, -Werror, ASan и UBSan, если они есть). Устройство —
docs/runtime/README.md, раздел 9.
"""
import math
import os
import struct
import subprocess
from fractions import Fraction

TIMEOUT = 120
LOC = 't.dpm:3:7'


def _trap(msg):
    return ('', f'{LOC}: trap: {msg}\n', 101)


# Имя → (stdout, stderr, код выхода).
TRAPS = {
    'sadd': _trap("integer overflow in '+' on 'i64'"),
    'sadd8': _trap("integer overflow in '+' on 'i8'"),
    'ssub': _trap("integer overflow in '-' on 'int'"),
    'smul': _trap("integer overflow in '*' on 'i64'"),
    'smul32': _trap("integer overflow in '*' on 'i32'"),
    'sdiv0': _trap('division by zero'),
    'sdivmin': _trap("integer overflow in '/' on 'i8'"),
    'srem0': _trap('division by zero'),
    'sneg': _trap("integer overflow in '-' on 'i8'"),
    'sabs': _trap("integer overflow in 'abs' on 'i64'"),
    'uadd': _trap("integer overflow in '+' on 'u8'"),
    'usub': _trap("integer overflow in '-' on 'u8'"),
    'umul': _trap("integer overflow in '*' on 'u64'"),
    'udiv0': _trap('division by zero'),
    'urem0': _trap('division by zero'),
    'shift_neg': _trap("shift count -1 is out of range for 'i32'"),
    'shift_wide': _trap("shift count 64 is out of range for 'u64'"),
    'cast_ss': _trap("value 128 does not fit 'i8'"),
    'cast_su': _trap("value -1 does not fit 'u32'"),
    'cast_us': _trap("value 18446744073709551615 does not fit 'i64'"),
    'cast_uu': _trap("value 256 does not fit 'u8'"),
    'cast_nan': _trap("NaN does not fit 'i32'"),
    'cast_inf': _trap("value inf does not fit 'u64'"),
    'cast_big': _trap("value 2147483648 does not fit 'i32'"),
    'cast_neg': _trap("value -1 does not fit 'u8'"),
    'cast_i64_top': _trap("value 9.223372036854776e+18 does not fit 'i64'"),
    'cast_f32_text': _trap("value 3.4e+38 does not fit 'i64'"),
    'f64_to_f32': _trap("value 1e+39 does not fit 'f32'"),
    'index': _trap('index 5 is out of bounds for length 5'),
    'index_neg': _trap('index -1 is out of bounds for length 3'),
    'slice': _trap('slice 2..1 is out of bounds for length 5'),
    'slice_end': _trap('slice 0..6 is out of bounds for length 5'),
    'insert': _trap('insert position 4 is out of bounds for length 3'),
    'repeat': _trap('repeat count -2 is negative'),
    'str_at': _trap('index 3 is out of bounds for length 3'),
    'str_utf8': _trap('slice 0..2 of a str does not fall on character boundaries'),
    'str_bounds': _trap('slice 1..4 is out of bounds for length 3'),
    'alloc': _trap('out of memory'),
    'alloc_elem': _trap('out of memory'),
    'realloc': _trap('out of memory'),
    'concat': _trap('out of memory'),
    'depth': _trap('stack overflow: calls are nested too deeply'),
    'long': _trap('x' * 511),  # буфер сообщения — 512 байт с нулём: длинное обрезается
    'noloc': ('', 'trap: boom\n', 101),
    'emptyloc': ('', 'trap: boom\n', 101),
    'unreachable': ('', 'internal error: reached code marked unreachable in m.f\n', 3),
    'flush': ('before', f'{LOC}: trap: assertion failed\n', 101),
    'order': ('1\n2\n3\n', None, 0),  # None: stderr слит со stdout
}
RUNS = {
    'ok': ('hi\n', '', 0),
    'error': ('before\n', 'error: main failed with ParseError.BadKey\n', 1),
    'trap': ('partial', f'{LOC}: trap: division by zero\n', 101),
    'deep': ('deep ok\n', f'{LOC}: trap: stack overflow: calls are nested too deeply\n', 101),
}

CASES = ['unit', 'floats'] + ['trap/' + n for n in sorted(TRAPS)] + ['run/' + n for n in sorted(RUNS)]


# --- образец записи плавающих (Р48)

def _neighbours(v, f32):
    """Соседние представимые числа вокруг v > 0 точно: (меньшее, большее,
    мантисса v чётная). За наибольшим конечным — тот же шаг."""
    if f32:
        bits = struct.unpack('<I', struct.pack('<f', v))[0]
        down = struct.unpack('<f', struct.pack('<I', bits - 1))[0]
        up = struct.unpack('<f', struct.pack('<I', bits + 1))[0]
    else:
        bits = struct.unpack('<Q', struct.pack('<d', v))[0]
        down = math.nextafter(v, 0.0)
        up = math.nextafter(v, math.inf)
    x, lo = Fraction(v), Fraction(down)
    hi = 2 * x - lo if math.isinf(up) else Fraction(up)
    return lo, hi, bits & 1 == 0


def _shortest(v, f32):
    """Цифры и порядок кратчайшей записи v > 0 по определению: десятичное
    число с наименьшим числом значащих цифр внутри промежутка округления к
    v (концы — если мантисса v чётная); из таких — ближайшее к v, при
    равенстве — с чётной последней цифрой."""
    down, up, even = _neighbours(v, f32)
    x = Fraction(v)
    lo = (down + x) / 2
    hi = (up + x) / 2
    e10 = math.floor(math.log10(v))
    for p in range(1, 18):
        best = None
        for q in range(e10 - p - 1, e10 - p + 3):
            scale = Fraction(10) ** q
            m_lo = math.ceil(lo / scale)
            m_hi = math.floor(hi / scale)
            for m in range(max(m_lo, 10 ** (p - 1)), min(m_hi, 10 ** p - 1) + 1):
                val = m * scale
                inside = lo < val < hi or (even and (val == lo or val == hi))
                if not inside:
                    continue
                dist = abs(val - x)
                if best is None or dist < best[0] or (dist == best[0] and m % 2 == 0):
                    best = (dist, m, q)
        if best:
            return str(best[1]), best[2]
    raise AssertionError(f'no representation for {v!r}')


def format_float(v, f32):
    """Запись str(x) по Р48 — образец для сверки."""
    if math.isnan(v):
        return 'nan'
    if math.isinf(v):
        return '-inf' if v < 0 else 'inf'
    if v == 0:
        return '-0' if math.copysign(1.0, v) < 0 else '0'
    digits, q = _shortest(abs(v), f32)
    while len(digits) > 1 and digits[-1] == '0':
        digits = digits[:-1]
        q += 1
    exp = q + len(digits) - 1
    sign = '-' if v < 0 else ''
    if exp < -4 or exp >= 16:
        frac = '.' + digits[1:] if len(digits) > 1 else ''
        return f'{sign}{digits[0]}{frac}e{"-" if exp < 0 else "+"}{abs(exp):02d}'
    if exp < 0:
        return sign + '0.' + '0' * (-exp - 1) + digits
    int_len = exp + 1
    if len(digits) <= int_len:
        return sign + digits + '0' * (int_len - len(digits))
    return sign + digits[:int_len] + '.' + digits[int_len:]


def _repr_form(v):
    """repr Python без хвоста .0 — для f64 должен совпасть с образцом."""
    r = repr(v)
    return r[:-2] if r.endswith('.0') else r


def _splitmix(seed):
    while True:
        seed = (seed + 0x9E3779B97F4A7C15) & (2 ** 64 - 1)
        z = seed
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & (2 ** 64 - 1)
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & (2 ** 64 - 1)
        yield z ^ (z >> 31)


def _f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def float_values():
    """(значение, f32): детерминированный набор для сверки."""
    out = []
    f64 = [0.0, -0.0, 5e-324, 2.2250738585072014e-308, 1.7976931348623157e+308, 0.1, 0.2, 0.3, 1 / 3, 2 / 3,
           100.0, 1e15, 1e16, 9999999999999998.0, 0.0001, 0.00001, 123456.789, 1e21, 1e22, 1e23, 2.0 ** 53,
           2.0 ** 63, 2.0 ** 64, -1.5, -0.1, 1.5e-7]
    out += [(v, False) for v in f64]
    out += [(math.ldexp(1.0, k), False) for k in range(-1074, 1024)]
    rng = _splitmix(48)
    for _ in range(600):
        v = struct.unpack('<d', struct.pack('<Q', next(rng)))[0]
        if math.isfinite(v):
            out.append((v, False))
    for _ in range(200):  # числа вида m·10^e у границ обычной и экспоненциальной формы
        m = next(rng) % 10 ** (1 + next(rng) % 7)
        e = next(rng) % 30 - 10
        out.append((float(f'{m}e{e}'), False))
    f32 = [3.4028234663852886e+38, 1.401298464324817e-45, 1.1754943508222875e-38, 16777216.0, 0.1, 1e10, 3.4e38]
    out += [(_f32(v), True) for v in f32]
    out += [(_f32(math.ldexp(1.0, k)), True) for k in range(-149, 128)]
    for _ in range(600):
        v = struct.unpack('<f', struct.pack('<I', next(rng) & 0xFFFFFFFF))[0]
        if math.isfinite(v):
            out.append((v, True))
    return out


def floats_program(values):
    lines = ['// Сгенерирован tests/runtime.py: запись плавающих (Р48).', 'fn main() {']
    for v, f32 in values:
        lit = repr(v)  # repr читается обратно точно; у f32 — значение f32 в записи f64
        lines.append(f'    println(str(f32({lit})))' if f32 else f'    println(str({lit}))')
    lines.append('}')
    return '\n'.join(lines) + '\n'


# --- сборка и запуск

def prepare(cc, flags, native_dir, runtime_objs, runtime_include):
    """Собирает rt_test; None — успех, иначе текст ошибки."""
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'runtime', 'rt_test.c')
    exe = os.path.join(native_dir, 'rt_test.bin')
    r = subprocess.run([cc] + flags + ['-I', runtime_include, src] + runtime_objs + ['-lm', '-pthread', '-o', exe],
                       capture_output=True, text=True)
    if r.returncode != 0:
        errs = [ln for ln in r.stderr.splitlines() if 'error' in ln][:5] or r.stderr.splitlines()[:5]
        return 'rt_test.c: ' + '\n'.join(errs)
    return None


def _run(argv, merge=False, cwd=None):
    try:
        p = subprocess.run(argv, capture_output=not merge, stdout=subprocess.PIPE if merge else None,
                           stderr=subprocess.STDOUT if merge else None, text=True, errors='backslashreplace',
                           timeout=TIMEOUT, cwd=cwd)
    except subprocess.TimeoutExpired:
        return None
    return p


def _compare(got, want, what):
    out, err, code = want
    p = got
    if p is None:
        return f'{what}: [timeout {TIMEOUT} s]'
    problems = []
    if p.stdout != out:
        problems.append(f'stdout {p.stdout!r}, ожидалось {out!r}')
    if err is not None and p.stderr != err:
        problems.append(f'stderr {p.stderr!r}, ожидалось {err!r}')
    if p.returncode != code:
        problems.append(f'код {p.returncode}, ожидался {code}')
    return f'{what}: ' + '; '.join(problems) if problems else None


def _floats(dplang, native, native_dir):
    values = float_values()
    want = [format_float(v, f32) for v, f32 in values]
    for (v, f32), w in zip(values, want):
        if not f32 and w != _repr_form(v):
            return f'образец расходится с repr у {v!r}: {w} и {_repr_form(v)}'
    src = os.path.join(native_dir, 'floats.dpm')
    with open(src, 'w', encoding='utf-8') as f:
        f.write(floats_program(values))
    expected = '\n'.join(want) + '\n'
    p = _run([dplang, 'debug', 'run', 'floats.dpm'], cwd=native_dir)
    if p is None or p.returncode != 0:
        return f'интерпретатор: код {p.returncode if p else "timeout"}: {(p.stderr if p else "")[:300]}'
    if p.stdout != expected:
        return 'интерпретатор: ' + _first_diff(values, want, p.stdout)
    err = native(src, os.path.join(native_dir, 'floats.bin'))
    if err:
        return 'нативная: ' + err
    p = _run([os.path.join(native_dir, 'floats.bin')], cwd=native_dir)
    if p is None or p.returncode != 0:
        return f'нативная: код {p.returncode if p else "timeout"}'
    if p.stdout != expected:
        return 'нативная: ' + _first_diff(values, want, p.stdout)
    return None


def _first_diff(values, want, stdout):
    got = stdout.split('\n')
    bad = [(v, f32, w, g) for (v, f32), w, g in zip(values, want, got) if w != g]
    if not bad and len(got) - 1 != len(want):
        return f'{len(got) - 1} строк, ожидалось {len(want)}'
    v, f32, w, g = bad[0]
    return f'расхождений {len(bad)}; первое — {"f32" if f32 else "f64"} {v!r}: {g!r}, ожидалось {w!r}'


def check_case(name, dplang, native, native_dir):
    """native(src.dpm, exe) — собирает нативную программу; None — успех."""
    exe = os.path.join(native_dir, 'rt_test.bin')
    if name == 'unit':
        p = _run([exe, 'unit'])
        if p is None:
            return f'[timeout {TIMEOUT} s]'
        if p.returncode != 0 or not p.stdout.startswith('ok '):
            return f'код {p.returncode}:\n' + '\n'.join((p.stderr or p.stdout).splitlines()[:20])
        return None
    if name == 'floats':
        return _floats(dplang, native, native_dir)
    kind, case = name.split('/', 1)
    if kind == 'trap':
        want = TRAPS[case]
        return _compare(_run([exe, 'trap', case], merge=want[1] is None), want, case)
    return _compare(_run([exe, 'run', case]), RUNS[case], case)
