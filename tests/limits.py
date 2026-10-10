# spec: syntax.nesting
"""Проход limits исполнителя tests/run.py: предел вложенности (E0002).

Для каждой конструкции наибольшее принятое число уровней n ищется
делением пополам по `dpl check`. На n все команды компилятора проходят
без падения (код 0 или 1, без внутренней ошибки и сообщений санитайзеров),
на n + 1 — ровно одно сообщение E0002 и код 1. Устройство —
docs/compiler/README.md, «Предел вложенности».
"""
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


def check_case(name, dpl):
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
