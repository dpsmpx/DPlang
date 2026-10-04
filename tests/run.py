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
Наборы и их команды — SUITES ниже; устройство — docs/compiler/README.md.

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
    'fuzz': ('.args', DPFUZZ, lambda name, path: args_file(path)),
}
# Наборы, где тест может быть каталогом из нескольких модулей: вход —
# X/main.dpm, ожидаемое — X.out рядом с каталогом.
DIR_SUITES = {'sema', 'summary'}


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
    return tests


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


def run_one(suite, path, bless):
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
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        results = list(pool.map(lambda t: run_one(t[0], t[1], a.bless), tests))
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
