#!/usr/bin/env python3
"""Исполнитель тестов компилятора DPlang (docs/policy/TESTING.md).

Собирает компилятор (CMake и Ninja; отладочная сборка с ASan и UBSan) в
build/compiler/ и прогоняет золотые тесты наборов tests/<набор>/.
Ожидаемое — файл X.out рядом с тестом X:

    вывод stdout
    --- stderr ---        (если stderr не пуст)
    вывод stderr
    [exit N]              (если код выхода не 0)

Команда запускается из каталога набора, пути в выводе — относительные.
Наборы и их команды — SUITES ниже; проход verify — верификатор Core IR на
каждой программе наборов; проход native — различающие тесты C-бэкенда:
программа набора run собирается в нативную (debug c, runtime, компилятор
C — $CC или cc, с ASan и UBSan, если они есть), и её вывод сверяется с тем
же ожидаемым, что у эталонного интерпретатора. Устройство —
docs/compiler/README.md, docs/compiler/backend_c.md.

    python3 tests/run.py              — собрать и прогнать
    python3 tests/run.py --bless      — переписать ожидаемое (дифф — на ревью)
    python3 tests/run.py -k lex       — только тесты, в пути которых есть подстрока
    python3 tests/run.py --no-build   — без пересборки

Вывод краткий: итог; на красном — сбои и начало диффа.
"""
import argparse
import concurrent.futures
import difflib
import os
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, 'tests')
SRC = os.path.join(ROOT, 'compiler')
BUILD = os.path.join(ROOT, 'build', 'compiler')
DPLANG = os.path.join(BUILD, 'dplang')
DPFUZZ = os.path.join(BUILD, 'dpfuzz')
TIMEOUT = 60
RUNTIME = os.path.join(ROOT, 'runtime')
NATIVE = os.path.join(BUILD, 'native')
CC = os.environ.get('CC', 'cc')
# Флаги сгенерированного C (docs/compiler/backend_c.md).
C_FLAGS = ['-std=c11', '-O1', '-ffp-contract=off', '-fno-strict-aliasing', '-Wall', '-Wextra', '-Wpedantic',
           '-Werror', '-Wno-infinite-recursion']
C_SAN = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
RUNTIME_SOURCES = [os.path.join(RUNTIME, 'core', 'dp_rt.c'), os.path.join(RUNTIME, 'linux', 'dp_entry.c')]
native_flags = []


def args_file(path):
    """cli: аргументы — содержимое X.args (разбор как в shell)."""
    with open(path, encoding='utf-8') as f:
        return shlex.split(f.read())


# Набор → (расширение входа, программа, построитель аргументов по имени входа).
SUITES = {
    'cli': ('.args', DPLANG, lambda name, path: args_file(path)),
    'lex': ('.dpm', DPLANG, lambda name, path: ['debug', 'lex', name]),
    'parse': ('.dpm', DPLANG, lambda name, path: ['debug', 'parse', name]),
    'print': ('.dpm', DPLANG, lambda name, path: ['debug', 'print', name]),
    'sema': ('.dpm', DPLANG, lambda name, path: ['check', name]),
    'summary': ('.dpm', DPLANG, lambda name, path: ['debug', 'summary', name]),
    'types': ('.dpm', DPLANG, lambda name, path: ['debug', 'types', name]),
    'fail': ('.dpm', DPLANG, lambda name, path: ['check', name]),
    'diag': ('.dpm', DPLANG, lambda name, path: ['check', '--format=json', name]),
    'ir': ('.dpm', DPLANG, lambda name, path: ['debug', 'ir', name]),
    'run': ('.dpm', DPLANG, lambda name, path: ['debug', 'run', name]),
    'fuzz': ('.args', DPFUZZ, lambda name, path: args_file(path)),
}
# Наборы, где тест может быть каталогом из нескольких модулей: вход —
# X/main.dpm, ожидаемое — X.out рядом с каталогом.
DIR_SUITES = {'sema', 'summary', 'types', 'fail', 'ir', 'run'}
# Проход verify: каждая программа наборов понижается в Core IR, и
# верификатор проверяет её (docs/ir/README.md). Ожидаемого нет: программа
# с ошибками даёт код 1, без них — 0; внутренняя ошибка и падение — сбой.
VERIFY_FROM = ('.dpm',)


def build():
    """Собирает компилятор; None — успех, иначе текст ошибки."""
    if not os.path.exists(os.path.join(BUILD, 'build.ninja')):
        cxx = os.environ.get('CXX', 'g++')
        r = subprocess.run(['cmake', '-S', SRC, '-B', BUILD, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug',
                            '-DDPLANG_SANITIZE=ON', f'-DCMAKE_CXX_COMPILER={cxx}'],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return 'cmake: ' + (r.stderr or r.stdout).strip()[-2000:]
    r = subprocess.run(['cmake', '--build', BUILD], capture_output=True, text=True)
    if r.returncode != 0:
        lines = (r.stdout + r.stderr).splitlines()
        errs = [ln for ln in lines if 'error' in ln][:10] or lines[-20:]
        return 'сборка:\n' + '\n'.join(errs)
    return None


def discover(pattern):
    tests = []
    for suite, (ext, _, _) in SUITES.items():
        d = os.path.join(TESTS, suite)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            path = os.path.join(d, name)
            if suite in DIR_SUITES and os.path.isdir(path) and os.path.exists(os.path.join(path, 'main.dpm')):
                path = os.path.join(path, 'main.dpm')
            elif not (os.path.isfile(path) and name.endswith(ext)):
                continue
            if pattern in os.path.relpath(path, TESTS):
                tests.append((suite, path))
    verify = [('verify', path) for suite, path in tests if SUITES[suite][0] in VERIFY_FROM]
    native = [('native', path) for suite, path in tests if suite == 'run']
    return tests + verify + native


def prepare_native():
    """Флаги и объекты runtime для прохода native; None — успех, иначе текст ошибки."""
    os.makedirs(NATIVE, exist_ok=True)
    probe = os.path.join(NATIVE, 'probe.c')
    with open(probe, 'w', encoding='utf-8') as f:
        f.write('int main(void) { return 0; }\n')
    r = subprocess.run([CC] + C_FLAGS + C_SAN + [probe, '-o', probe + '.bin'], capture_output=True, text=True)
    native_flags[:] = C_FLAGS + (C_SAN if r.returncode == 0 else [])
    for src in RUNTIME_SOURCES:
        obj = os.path.join(NATIVE, os.path.basename(src) + '.o')
        r = subprocess.run([CC] + native_flags + ['-I', os.path.join(RUNTIME, 'core'), '-c', src, '-o', obj],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return f'runtime {os.path.relpath(src, ROOT)}: ' + (r.stderr.strip().splitlines() or ['?'])[0]
    return None


def native_one(path):
    """Различающий тест: нативная программа против ожидаемого вывода интерпретатора."""
    cwd = os.path.join(TESTS, 'run')
    name = os.path.relpath(path, cwd)
    flat = name.replace(os.sep, '_')[:-len('.dpm')]
    try:
        r = subprocess.run([DPLANG, 'debug', 'c', name], capture_output=True, text=True, timeout=TIMEOUT, cwd=cwd)
    except subprocess.TimeoutExpired:
        return f'debug c: [timeout {TIMEOUT} s]'
    if r.returncode == 1:
        return None  # программа не проходит проверку: её диагностику сверяет набор run
    if r.returncode != 0:
        return f'debug c: код {r.returncode}: ' + '\n'.join(r.stderr.splitlines()[:5])
    c_path = os.path.join(NATIVE, flat + '.c')
    exe = os.path.join(NATIVE, flat + '.bin')
    with open(c_path, 'w', encoding='utf-8') as f:
        f.write(r.stdout)
    objs = [os.path.join(NATIVE, os.path.basename(s) + '.o') for s in RUNTIME_SOURCES]
    r = subprocess.run([CC] + native_flags + ['-I', os.path.join(RUNTIME, 'core'), c_path] + objs +
                       ['-lm', '-pthread', '-o', exe], capture_output=True, text=True)
    if r.returncode != 0:
        errs = [ln for ln in r.stderr.splitlines() if 'error' in ln][:5] or r.stderr.splitlines()[:5]
        return 'сборка C:\n' + '\n'.join(errs)
    try:
        p = subprocess.run([exe], capture_output=True, text=True, errors='backslashreplace', timeout=TIMEOUT, cwd=cwd)
    except subprocess.TimeoutExpired:
        return f'[timeout {TIMEOUT} s]'
    got = p.stdout
    if p.stderr:
        got += '--- stderr ---\n' + p.stderr
    if p.returncode != 0:
        got += f'[exit {p.returncode}]\n'
    with open(expected_path('run', path), encoding='utf-8') as f:
        want = f.read()
    if got == want:
        return None
    diff = difflib.unified_diff(want.splitlines(), got.splitlines(), 'интерпретатор', 'нативная', lineterm='', n=1)
    return 'вывод расходится с интерпретатором:\n' + '\n'.join(list(diff)[:20])


def suite_dir_of(suite, path):
    return os.path.join(TESTS, suite)


def actual_output(suite, path):
    ext, program, make_args = SUITES[suite]
    cwd = suite_dir_of(suite, path)
    name = os.path.relpath(path, cwd)
    argv = [program] + make_args(name, path)
    try:
        r = subprocess.run(argv, capture_output=True, text=True, errors='backslashreplace', timeout=TIMEOUT,
                           cwd=cwd)
    except subprocess.TimeoutExpired:
        return f'[timeout {TIMEOUT} s]\n'
    out = r.stdout
    if r.stderr:
        out += '--- stderr ---\n' + r.stderr
    if r.returncode != 0:
        out += f'[exit {r.returncode}]\n'
    return out


def expected_path(suite, path):
    ext = SUITES[suite][0]
    if os.path.basename(path) == 'main.dpm' and os.path.dirname(path) != suite_dir_of(suite, path):
        return os.path.dirname(path) + '.out'
    return path[:-len(ext)] + '.out'


def verify_one(path):
    cwd = os.path.dirname(path)
    try:
        r = subprocess.run([DPLANG, 'debug', 'ir', os.path.basename(path)], capture_output=True, text=True,
                           errors='backslashreplace', timeout=TIMEOUT, cwd=cwd)
    except subprocess.TimeoutExpired:
        return f'debug ir: [timeout {TIMEOUT} s]'
    if r.returncode in (0, 1):
        return None
    return f'debug ir: код {r.returncode}:\n' + '\n'.join(r.stderr.splitlines()[:20])


def run_one(suite, path, bless):
    if suite == 'verify':
        return verify_one(path)
    if suite == 'native':
        return native_one(path)
    got = actual_output(suite, path)
    exp_path = expected_path(suite, path)
    if bless:
        with open(exp_path, 'w', encoding='utf-8') as f:
            f.write(got)
        return None
    if not os.path.exists(exp_path):
        return f'нет ожидаемого {os.path.relpath(exp_path, TESTS)} (--bless)'
    with open(exp_path, encoding='utf-8') as f:
        want = f.read()
    if got == want:
        return None
    diff = difflib.unified_diff(want.splitlines(), got.splitlines(), 'ожидалось', 'получено', lineterm='', n=1)
    return 'вывод расходится:\n' + '\n'.join(list(diff)[:20])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--bless', action='store_true')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('-k', default='')
    a = ap.parse_args()
    if not a.no_build:
        err = build()
        if err:
            print('компилятор: не собирается')
            print(err)
            return 1
    tests = discover(a.k)
    if any(suite == 'native' for suite, _ in tests):
        err = prepare_native()
        if err:
            print('компилятор: runtime не собирается')
            print(err)
            return 1
    # Проход native сверяет с ожидаемым набора run — после того, как --bless его перепишет.
    tests = [t for t in tests if t[0] != 'native'] + [t for t in tests if t[0] == 'native']
    split = sum(1 for t in tests if t[0] != 'native')
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        results = list(pool.map(lambda t: run_one(t[0], t[1], a.bless), tests[:split]))
        results += list(pool.map(lambda t: run_one(t[0], t[1], a.bless), tests[split:]))
    fails = [(t, r) for t, r in zip(tests, results) if r]
    counts = {}
    for suite, _ in tests:
        counts[suite] = counts.get(suite, 0) + 1
    summary = ', '.join(f'{s} {n}' for s, n in sorted(counts.items())) or 'тестов нет'
    if fails:
        print(f'компилятор: сбоев {len(fails)} из {len(tests)}')
        for (suite, path), r in fails:
            print(f'  {os.path.relpath(path, TESTS)}: {r}')
        return 1
    verb = 'записано ожидаемое' if a.bless else 'прошли'
    print(f'компилятор: собран, тестов {len(tests)} {verb} ({summary})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
