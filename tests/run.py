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
же ожидаемым, что у эталонного интерпретатора; проход build — то же
через `dplang build` в профилях debug и release. Устройство —
docs/compiler/README.md, docs/compiler/backend_c.md, docs/compiler/cli.md.
Проход runtime — тесты runtime по функциям и запись плавающих против
образца (tests/runtime.py). Проходы limits, worker, sandbox и service —
tests/limits.py, tests/worker.py и tests/service.py; worker, sandbox и
service идут на второй сборке, Release без санитайзеров (build/release/).

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
import shutil
import subprocess
import sys
import tempfile

import limits
import runtime
import schema
import service
import worker

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, 'tests')
SRC = os.path.join(ROOT, 'compiler')
BUILD = os.path.join(ROOT, 'build', 'compiler')
DPLANG = os.path.join(BUILD, 'dplang')
DPFUZZ = os.path.join(BUILD, 'dpfuzz')
# Схема машиночитаемой диагностики: ей подчиняется каждый снимок набора diag.
DIAG_SCHEMA = os.path.join(ROOT, 'docs', 'compiler', 'diagnostics.schema.json')
# Сборка Release без санитайзеров — для worker-а онлайн-сервиса и пробника
# песочницы (tests/worker.py).
BUILD_RELEASE = os.path.join(ROOT, 'build', 'release')
DPLANG_RELEASE = os.path.join(BUILD_RELEASE, 'dplang')
PROBE = os.path.join(BUILD_RELEASE, 'dpsandbox_probe')
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
    'fmt': ('.dpm', DPLANG, lambda name, path: ['fmt', name]),
    'test': ('.dpm', DPLANG, lambda name, path: ['test', name]),
    'fuzz': ('.args', DPFUZZ, lambda name, path: args_file(path)),
}
# Наборы, где тест может быть каталогом из нескольких модулей: вход —
# X/main.dpm, ожидаемое — X.out рядом с каталогом.
DIR_SUITES = {'sema', 'summary', 'types', 'fail', 'ir', 'run', 'test'}
# Проход verify: каждая программа наборов понижается в Core IR, и
# верификатор проверяет её (docs/ir/README.md). Ожидаемого нет: программа
# с ошибками даёт код 1, без них — 0; внутренняя ошибка и падение — сбой.
VERIFY_FROM = ('.dpm',)


def build_one(build_dir, build_type, sanitize):
    """Собирает компилятор в build_dir; None — успех, иначе текст ошибки."""
    if not os.path.exists(os.path.join(build_dir, 'build.ninja')):
        cxx = os.environ.get('CXX', 'g++')
        r = subprocess.run(['cmake', '-S', SRC, '-B', build_dir, '-G', 'Ninja', f'-DCMAKE_BUILD_TYPE={build_type}',
                            f'-DDPLANG_SANITIZE={"ON" if sanitize else "OFF"}', f'-DCMAKE_CXX_COMPILER={cxx}'],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return 'cmake: ' + (r.stderr or r.stdout).strip()[-2000:]
    r = subprocess.run(['cmake', '--build', build_dir], capture_output=True, text=True)
    if r.returncode != 0:
        lines = (r.stdout + r.stderr).splitlines()
        errs = [ln for ln in lines if 'error' in ln][:10] or lines[-20:]
        return 'сборка:\n' + '\n'.join(errs)
    return None


def build():
    """Отладочная сборка с ASan и UBSan (наборы) и Release (worker)."""
    err = build_one(BUILD, 'Debug', True)
    if err:
        return err
    err = build_one(BUILD_RELEASE, 'Release', False)
    return 'Release: ' + err if err else None


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
    native += [('build', path + profile) for suite, path in tests if suite == 'run' for profile in ('', ' --release')]
    deep = [('limits', name) for name in limits.CASES if pattern in 'limits/' + name]
    deep += [('runtime', name) for name in runtime.CASES if pattern in 'runtime/' + name]
    wdir = os.path.join(TESTS, 'worker')
    online = [('worker', os.path.join(wdir, name)) for name in sorted(os.listdir(wdir))
              if name.endswith('.json') and pattern in 'worker/' + name]
    online += [('sandbox', a) for a in ['ok'] + worker.PROBE_FORBIDDEN if pattern in 'sandbox/' + a]
    online += [('service', name) for name in service.CASES if pattern in 'service/' + name]
    return tests + verify + native + deep + online


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
    return runtime.prepare(CC, native_flags, NATIVE, runtime_objects(), os.path.join(RUNTIME, 'core'))


def runtime_objects():
    return [os.path.join(NATIVE, os.path.basename(s) + '.o') for s in RUNTIME_SOURCES]


def build_native(path, exe, cwd):
    """Программа path — в нативную exe (debug c из каталога cwd: места в
    ловушках — пути от него; runtime). None — собрана; 'check' — не проходит
    проверку; иначе текст ошибки."""
    name = os.path.relpath(path, cwd)
    try:
        r = subprocess.run([DPLANG, 'debug', 'c', name], capture_output=True, text=True, timeout=TIMEOUT, cwd=cwd)
    except subprocess.TimeoutExpired:
        return f'debug c: [timeout {TIMEOUT} s]'
    if r.returncode == 1:
        return 'check'
    if r.returncode != 0:
        return f'debug c: код {r.returncode}: ' + '\n'.join(r.stderr.splitlines()[:5])
    c_path = exe[:-len('.bin')] + '.c'
    with open(c_path, 'w', encoding='utf-8') as f:
        f.write(r.stdout)
    r = subprocess.run([CC] + native_flags + ['-I', os.path.join(RUNTIME, 'core'), c_path] + runtime_objects() +
                       ['-lm', '-pthread', '-o', exe], capture_output=True, text=True)
    if r.returncode != 0:
        errs = [ln for ln in r.stderr.splitlines() if 'error' in ln][:5] or r.stderr.splitlines()[:5]
        return 'сборка C:\n' + '\n'.join(errs)
    return None


def build_pass_one(arg):
    """Проход build: `dplang build` (профиль debug или --release), запуск,
    вывод — как у интерпретатора (docs/compiler/cli.md, «build»)."""
    path, _, profile = arg.partition(' ')
    cwd = os.path.join(TESTS, 'run')
    name = os.path.relpath(path, cwd)
    exe = os.path.join(NATIVE, 'build_' + name.replace(os.sep, '_')[:-len('.dpm')] + ('_release' if profile else ''))
    argv = [DPLANG, 'build'] + ([profile] if profile else []) + ['-o', exe, name]
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=TIMEOUT, cwd=cwd)
    except subprocess.TimeoutExpired:
        return f'build: [timeout {TIMEOUT} s]'
    if r.returncode == 1:
        return None  # программа не проходит проверку: её диагностику сверяет набор run
    if r.returncode != 0:
        return f'build: код {r.returncode}: ' + '\n'.join(r.stderr.splitlines()[:5])
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
    diff = difflib.unified_diff(want.splitlines(), got.splitlines(), 'интерпретатор', 'build', lineterm='', n=1)
    return 'вывод расходится с интерпретатором:\n' + '\n'.join(list(diff)[:20])


def runtime_native(src, exe):
    """Сборка для прохода runtime: программа без ошибок проверки."""
    err = build_native(src, exe, os.path.dirname(src))
    return 'не проходит проверку' if err == 'check' else err


def native_one(path):
    """Различающий тест: нативная программа против ожидаемого вывода интерпретатора."""
    cwd = os.path.join(TESTS, 'run')
    name = os.path.relpath(path, cwd)
    flat = name.replace(os.sep, '_')[:-len('.dpm')]
    exe = os.path.join(NATIVE, flat + '.bin')
    err = build_native(path, exe, cwd)
    if err == 'check':
        return None  # программа не проходит проверку: её диагностику сверяет набор run
    if err:
        return err
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
    if suite == 'fmt':
        return fmt_one(path)
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


def fmt_one(path):
    """Набор fmt: `dplang fmt` правит копию входа во временном каталоге.
    Ожидаемое — текст файла после него, затем вывод и код. Второй прогон
    `fmt --check` уже ничего не находит (запись неподвижна)."""
    name = os.path.basename(path)
    tmp = tempfile.mkdtemp(prefix='dpfmt-')
    try:
        shutil.copy(path, os.path.join(tmp, name))
        try:
            r = subprocess.run([DPLANG, 'fmt', name], capture_output=True, text=True, errors='backslashreplace',
                               timeout=TIMEOUT, cwd=tmp)
            again = subprocess.run([DPLANG, 'fmt', '--check', name], capture_output=True, text=True,
                                   errors='backslashreplace', timeout=TIMEOUT, cwd=tmp)
        except subprocess.TimeoutExpired:
            return f'[timeout {TIMEOUT} s]\n'
        with open(os.path.join(tmp, name), encoding='utf-8', errors='backslashreplace', newline='') as f:
            out = f.read()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if r.stdout:
        out += '--- stdout ---\n' + r.stdout
    if r.stderr:
        out += '--- stderr ---\n' + r.stderr
    if r.returncode != 0:
        out += f'[exit {r.returncode}]\n'
    if r.returncode == 0 and (again.returncode != 0 or again.stdout or again.stderr):
        out += f'[fmt --check after fmt: exit {again.returncode}: {again.stdout + again.stderr}]\n'
    return out


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
    if suite == 'build':
        return build_pass_one(path)
    if suite == 'limits':
        return limits.check_case(path, DPLANG)
    if suite == 'runtime':
        return runtime.check_case(path, DPLANG, runtime_native, NATIVE)
    if suite == 'worker':
        return worker.worker_case(path, DPLANG_RELEASE, bless)
    if suite == 'sandbox':
        return worker.sandbox_case(path, PROBE)
    if suite == 'service':
        return service.check_case(path, DPLANG_RELEASE)
    got = actual_output(suite, path)
    if suite == 'diag':
        doc = got.split('--- stderr ---\n')[0].split('[exit ')[0]
        errs = schema.check_text(doc, DIAG_SCHEMA)
        if errs:
            return 'вывод не подходит к схеме diagnostics.schema.json: ' + '; '.join(errs[:5])
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
    if any(suite in ('native', 'runtime') for suite, _ in tests):
        err = prepare_native()
        if err:
            print('компилятор: runtime не собирается')
            print(err)
            return 1
    # Проходы native и build сверяют с ожидаемым набора run — после того, как --bless его перепишет.
    late = ('native', 'build')
    tests = [t for t in tests if t[0] not in late] + [t for t in tests if t[0] in late]
    split = sum(1 for t in tests if t[0] not in late)
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
            name = suite + '/' + path if suite in ('limits', 'runtime', 'sandbox', 'service') else os.path.relpath(path, TESTS)
            if suite == 'build':
                name = 'build/' + os.path.relpath(path, TESTS)
            print(f'  {name}: {r}')
        return 1
    verb = 'записано ожидаемое' if a.bless else 'прошли'
    print(f'компилятор: собран, тестов {len(tests)} {verb} ({summary})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
