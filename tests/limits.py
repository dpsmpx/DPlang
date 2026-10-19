# spec: syntax.nesting type.recursive expr.compare
"""Проход limits исполнителя tests/run.py: предел вложенности (E0002) и
масштаб графа объявлений.

Для каждой конструкции наибольшее принятое число уровней n ищется
делением пополам по `dpl check`. На n все команды компилятора проходят
без падения (код 0 или 1, без внутренней ошибки и сообщений санитайзеров),
на n + 1 — ровно одно сообщение E0002 и код 1. Устройство —
docs/compiler/README.md, «Предел вложенности».

Масштаб (limits/scale/ИМЯ): команда на большом входе — графе объявлений,
тысячах строк — укладывается в предел времени с ожидаемым кодом:
небольшой вход не должен вешать компилятор (docs/policy/SECURITY.md,
раздел 5).
"""
import time
import os
import subprocess
import tempfile

# Конструкция → исходник с n уровнями вложенности.
CASES = {
    'parens': lambda n: 'fn main() {\n    let x = ' + '(' * n + '1' + ')' * n + '\n    println(str(x))\n}\n',
    'unary': lambda n: 'fn main() {\n    let x = ' + '- ' * n + '1\n    println(str(x))\n}\n',
    'binary': lambda n: 'fn main() {\n    let x = ' + ' + '.join(['1'] * (n + 1)) + '\n    println(str(x))\n}\n',
    'postfix': lambda n: 'fn main() {\n    let s = "a"' + '.trim()' * n + '\n    println(s)\n}\n',
    'calls': lambda n: ('fn f(x: i64) i64 {\n    return x\n}\n\nfn main() {\n    println(str(' + 'f(' * n + '1' +
                        ')' * n + '))\n}\n'),
    'blocks': lambda n: 'fn main() {\n' + '    if true {\n' * n + '    println("x")\n' + '    }\n' * n + '}\n',
    'else_if': lambda n: ('fn main() {\n    let x = 1\n    if x == 0 {\n        println("0")\n' +
                          ''.join(f'    }} else if x == {i} {{\n        println("{i}")\n' for i in range(1, n + 1)) +
                          '    }\n}\n'),
    'lists': lambda n: 'fn main() {\n    let x = ' + '[' * n + '1' + ']' * n + '\n    println(str(len(x)))\n}\n',
    'types': lambda n: 'fn main() {\n    let x: ' + 'List[' * n + 'i32' + ']' * n + ' = []\n}\n',
    'patterns': lambda n: ('fn main() {\n    let x: ?i32 = none\n    match x {\n        ' + 'some(' * n + '_' +
                           ')' * n + ' => println("s")\n        none => println("n")\n    }\n}\n'),
}

def _chain(n):
    """Цепочка struct S0 { x: S1 } … из n структур."""
    return ''.join(f'struct S{i} {{\n    x: S{i + 1}\n}}\n\n' for i in range(n)) + f'struct S{n} {{\n    v: i32\n}}\n\nfn main() {{\n}}\n'


def _diamond(n):
    """«Ромб»: struct Si { a: Si+1, b: Si+1 } — 2^n путей; sort и == обходят все типы."""
    body = ''.join(f'struct S{i} {{\n    a: S{i + 1}\n    b: S{i + 1}\n}}\n\n' for i in range(n))
    return body + (f'struct S{n} {{\n    v: i32\n}}\n\nfn main() {{\n    var xs: List[S0] = []\n    xs.sort()\n'
                   '    println(str(xs == xs))\n}\n')


def _eq_chain(n):
    """Цепочка структур и `==` по первой: проверка обходит её явным стеком, время линейно."""
    return (''.join(f'struct S{i} {{\n    x: S{i + 1}\n}}\n\n' for i in range(n)) +
            f'struct S{n} {{\n    v: i32\n}}\n\nfn same(a: S0, b: S0) bool {{\n    return a == b\n}}\n\n'
            'fn main() {\n}\n')


def _const_chain(n):
    """Обратная цепочка констант C0 = C1 + 1 …: под ASan рекурсия проверки переполняла стек 512 МБ."""
    return (''.join(f'const C{i}: i64 = C{i + 1} + 1\n' for i in range(n)) + f'const C{n}: i64 = 0\n\n'
            'fn main() {\n    println(str(C0))\n}\n')


def _cycle(n):
    """Цикл из n структур: каждая — E0423."""
    return ''.join(f'struct S{i} {{\n    x: S{(i + 1) % n}\n}}\n\n' for i in range(n)) + 'fn main() {\n}\n'


def _strings(n):
    """n разных строковых литералов: lowering ищет их номер (было перебором)."""
    return 'fn main() {\n' + ''.join(f'    println("s{i}")\n' for i in range(n)) + '}\n'


def _try_chain(n):
    """f0 → try f1 → … → fail: вывод типа ошибки (было по проходу на функцию)."""
    fns = ''.join(f'fn f{i}() !i32 {{\n    return try f{i + 1}()\n}}\n\n' for i in range(n - 1))
    return (f'enum E {{\n    X\n}}\n\n{fns}fn f{n - 1}() !i32 {{\n    fail .X\n}}\n\n'
            'fn main() {\n    _ = f0()\n}\n')


# Масштаб: имя → (исходник, команда, ожидаемый код, предел секунд под ASan).
SCALE = {
    'chain': (_chain(20000), ['check'], 0, 30),
    'eq_chain': (_eq_chain(60000), ['check'], 0, 20),
    'const_chain': (_const_chain(200000), ['check'], 0, 60),
    'diamond': (_diamond(64), ['check'], 0, 10),
    'cycle': (_cycle(5000), ['check'], 1, 30),
    'strings': (_strings(40000), ['debug', 'ir'], 0, 5),
    'try_chain': (_try_chain(8000), ['check'], 0, 10),
}

# Команды на наибольшем принятом n: все проходы компилятора.
COMMANDS = [['check'], ['debug', 'parse'], ['debug', 'types'], ['debug', 'ir'], ['debug', 'c']]
# Глубоко вложенные типы: вывод debug types, ir и c растёт квадратично
# (TODO.md) — на пределе минуты под ASan. Здесь —
# только проверка и разбор; полные проходы на пределе прогнаны вручную.
SHORT = [['check'], ['debug', 'parse']]
CASE_COMMANDS = {'lists': SHORT, 'types': SHORT}
SEARCH_MAX = 1 << 12
TIMEOUT = 120


def run(dpl, args, src, tmp):
    path = os.path.join(tmp, 'main.dpm')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    try:
        r = subprocess.run([dpl] + args + ['main.dpm'], capture_output=True, text=True, cwd=tmp,
                           timeout=TIMEOUT, errors='backslashreplace')
    except subprocess.TimeoutExpired:
        return None
    return r


def too_deep(r):
    return r is not None and r.returncode == 1 and 'error[E0002]' in r.stderr


def broken(r):
    """Текст сбоя или None: код 0 или 1 без внутренней ошибки и санитайзеров."""
    if r is None:
        return f'[timeout {TIMEOUT} s]'
    if r.returncode not in (0, 1) or 'internal compiler error' in r.stderr or 'Sanitizer' in r.stderr:
        return f'код {r.returncode}: ' + '\n'.join(r.stderr.splitlines()[:5])
    return None


def check_scale(name, dpl):
    src, cmd, want, limit = SCALE[name]
    with tempfile.TemporaryDirectory(prefix='dplimits-') as tmp:
        start = time.monotonic()
        r = run(dpl, cmd, src, tmp)
        took = time.monotonic() - start
        what = ' '.join(cmd)
        if r is None:
            return f'{what} не уложился в {TIMEOUT} с'
        err = broken(r)
        if err:
            return err
        if r.returncode != want:
            return f'код {r.returncode}, ожидался {want}:\n' + '\n'.join(r.stderr.splitlines()[:5])
        if took > limit:
            return f'{what} — {took:.1f} с, предел {limit} с'
    return None


def check_case(name, dpl):
    if name.startswith('scale/'):
        return check_scale(name[len('scale/'):], dpl)
    gen = CASES[name]
    with tempfile.TemporaryDirectory(prefix='dplimits-') as tmp:
        # Наибольшее n без E0002: n = 1 принимается, SEARCH_MAX — нет.
        lo, hi = 1, SEARCH_MAX
        r = run(dpl, ['check'], gen(lo), tmp)
        if broken(r) or too_deep(r):
            return 'n = 1: ' + (broken(r) or 'E0002')
        r = run(dpl, ['check'], gen(hi), tmp)
        if not too_deep(r):
            return f'n = {hi}: нет E0002: ' + (broken(r) or f'код {r.returncode}')
        while hi - lo > 1:
            mid = (lo + hi) // 2
            r = run(dpl, ['check'], gen(mid), tmp)
            err = broken(r)
            if err:
                return f'n = {mid}: {err}'
            if too_deep(r):
                hi = mid
            else:
                lo = mid
        for cmd in CASE_COMMANDS.get(name, COMMANDS):
            err = broken(run(dpl, cmd, gen(lo), tmp))
            if err:
                return f'n = {lo}, {" ".join(cmd)}: {err}'
        r = run(dpl, ['check'], gen(hi), tmp)
        if not too_deep(r) or r.stderr.count('error[') != 1:
            return f'n = {hi}: ожидалось одно E0002:\n' + '\n'.join(r.stderr.splitlines()[:5])
    return None
