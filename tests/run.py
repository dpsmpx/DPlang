#!/usr/bin/env python3
"""Исполнитель тестов компилятора dpl (docs/policy/TESTING.md).

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
через `dpl build` в профилях debug и release; проходы windows и android —
то же для целей windows-x86_64 (под Wine) и android-arm64 (под
qemu-aarch64; docs/policy/PLATFORMS.md), если их инструменты есть; иначе
проход пропускается, и итог говорит об этом. Набор ffi — программы с
функциями C (Р59): интерпретатор их не исполняет, вывод сборки хоста и
целей сверяется с X.out. Проход lib — tests/lib/X/: библиотека
`dpl build --lib` из X/main.dpm и хост на C X/host.c, собранный с ней
компилятором цели (Р61); вывод хоста на хосте и целях сверяется с X.out. Устройство —
docs/compiler/README.md, docs/compiler/backend_c.md, docs/compiler/cli.md.
Проход runtime — тесты runtime по функциям и запись плавающих против
образца (tests/runtime.py). Проход example — программы examples/X/: их
тесты, запуск интерпретатором и сборкой, канонический вид. Проход bench —
эталонный набор задач на dpl (bench/tasks/X/dp/, bench/dp_check.py). Проход
bindings — привязки bindings/ против библиотек хоста (tests/bindings.py):
раскладка структур Vulkan по vulkan.h и программа на lavapipe; без них
случай пропускается, и итог говорит об этом. Проходы limits, worker, sandbox и service —
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

import bindings
import limits
import runtime
import schema
import service
import worker

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'bench'))
import dp_check  # noqa: E402 — проверка решений набора задач (bench/dp_check.py)

TESTS = os.path.join(ROOT, 'tests')
SRC = os.path.join(ROOT, 'compiler')
BUILD = os.path.join(ROOT, 'build', 'compiler')
EXAMPLES = os.path.join(ROOT, 'examples')
# Проход example: у программы examples/X/ с каталогом expected/ — четыре случая.
EXAMPLE_CASES = ('test', 'run', 'build', 'fmt')
# Проход bench: задачи bench/tasks/X/ с каталогом dp/ (эталон набора на dpl).
BENCH_TASKS = os.path.join(ROOT, 'bench', 'tasks')
BENCH_CASES = ('ref', 'start', 'fmt')
# Начальный код задачи не принимается, и отказ — ожидаемого рода.
BENCH_START = {'write': {'сборка'}, 'fix': {'вывод', 'ловушка'}, 'refactor': {'интерфейс'}}
DPL = os.path.join(BUILD, 'dpl')
DPFUZZ = os.path.join(BUILD, 'dpfuzz')
# Схема машиночитаемой диагностики: ей подчиняется каждый снимок набора diag.
DIAG_SCHEMA = os.path.join(ROOT, 'docs', 'compiler', 'diagnostics.schema.json')
# Сборка Release без санитайзеров — для worker-а онлайн-сервиса и пробника
# песочницы (tests/worker.py).
BUILD_RELEASE = os.path.join(ROOT, 'build', 'release')
DPL_RELEASE = os.path.join(BUILD_RELEASE, 'dpl')
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


def program_args(path):
    """run: аргументы программы X.dpm — из X.args рядом, если он есть (os.args)."""
    a = path[:-len('.dpm')] + '.args'
    return args_file(a) if os.path.exists(a) else []


# Набор → (расширение входа, программа, построитель аргументов по имени входа).
SUITES = {
    'cli': ('.args', DPL, lambda name, path: args_file(path)),
    'lex': ('.dpm', DPL, lambda name, path: ['debug', 'lex', name]),
    'parse': ('.dpm', DPL, lambda name, path: ['debug', 'parse', name]),
    'print': ('.dpm', DPL, lambda name, path: ['debug', 'print', name]),
    'sema': ('.dpm', DPL, lambda name, path: ['check', name]),
    'summary': ('.dpm', DPL, lambda name, path: ['debug', 'summary', name]),
    'types': ('.dpm', DPL, lambda name, path: ['debug', 'types', name]),
    'fail': ('.dpm', DPL, lambda name, path: ['check', name]),
    'diag': ('.dpm', DPL, lambda name, path: ['check', '--format=json', name]),
    'ir': ('.dpm', DPL, lambda name, path: ['debug', 'ir', name]),
    'run': ('.dpm', DPL, lambda name, path: ['debug', 'run', name] + program_args(path)),
    'fmt': ('.dpm', DPL, lambda name, path: ['fmt', name]),
    # X.args рядом — флаги команды (например, --timeout).
    'test': ('.dpm', DPL, lambda name, path: ['test'] + program_args(path) + [name]),
    # Функции C (Р59): программа собирается `dpl build` и запускается —
    # интерпретатор C не вызывает (build_pass_one).
    'ffi': ('.dpm', DPL, lambda name, path: ['check', name]),
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
    want = f'DPL_SANITIZE:BOOL={"ON" if sanitize else "OFF"}'
    cache = os.path.join(build_dir, 'CMakeCache.txt')
    stale = False  # сборка настроена с другими санитайзерами — настроить заново
    if os.path.exists(cache):
        with open(cache, encoding='utf-8', errors='replace') as f:
            stale = want not in f.read().splitlines()
    if stale or not os.path.exists(os.path.join(build_dir, 'build.ninja')):
        cxx = os.environ.get('CXX', 'g++')
        r = subprocess.run(['cmake', '-S', SRC, '-B', build_dir, '-G', 'Ninja', f'-DCMAKE_BUILD_TYPE={build_type}',
                            f'-DDPL_SANITIZE={"ON" if sanitize else "OFF"}', f'-DCMAKE_CXX_COMPILER={cxx}'],
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
    if tsan_cc():
        native += [('tsan', path) for suite, path in tests if suite == 'run' and uses_parallel(path)]
    native += [('build', path + profile) for suite, path in tests if suite == 'run' for profile in ('', ' --release')]
    ffi = [path for suite, path in tests if suite == 'ffi']
    tests = [t for t in tests if t[0] != 'ffi']
    native += [('ffi', path + profile) for path in ffi for profile in ('', ' --release')]
    for cross in CROSS:
        if cross_tools(cross) is None:
            native += [(cross, path + profile) for suite, path in tests if suite == 'run' for profile in ('', ' --release')]
            native += [(cross, path + profile) for path in ffi for profile in ('', ' --release')]
    ldir = os.path.join(TESTS, 'lib')
    if os.path.isdir(ldir):
        for name in sorted(os.listdir(ldir)):
            main_dpm = os.path.join(ldir, name, 'main.dpm')
            if not os.path.exists(main_dpm) or pattern not in os.path.relpath(main_dpm, TESTS):
                continue
            verify.append(('verify', main_dpm))
            native.append(('lib', main_dpm + ' host'))
            native += [('lib', main_dpm + ' ' + c) for c in CROSS if cross_tools(c) is None]
    deep = [('limits', name) for name in limits.CASES if pattern in 'limits/' + name]
    deep += [('limits', 'scale/' + name) for name in limits.SCALE if pattern in 'limits/scale/' + name]
    deep += [('runtime', name) for name in runtime.CASES if pattern in 'runtime/' + name]
    for case in bindings.CASES:
        if pattern in 'bindings/' + case and not bindings.missing(case):
            deep.append(('bindings', case))
    bdir = os.path.join(TESTS, 'bindings')
    for name in sorted(os.listdir(bdir)) if os.path.isdir(bdir) else []:
        main_dpm = os.path.join(bdir, name, 'main.dpm')
        if os.path.exists(main_dpm) and pattern in os.path.relpath(main_dpm, TESTS):
            verify.append(('verify', main_dpm))
    if pattern in 'examples/triangle/main.dpm':  # графическая программа (Р64): IR без библиотек хоста
        verify.append(('verify', os.path.join(bindings.TRIANGLE, 'main.dpm')))
    if os.path.isdir(EXAMPLES):
        for ex in sorted(os.listdir(EXAMPLES)):
            if os.path.isdir(os.path.join(EXAMPLES, ex, 'expected')):
                deep += [('example', f'{ex}/{c}') for c in EXAMPLE_CASES if pattern in f'example/{ex}/{c}']
    if os.path.isdir(BENCH_TASKS):
        for t in sorted(os.listdir(BENCH_TASKS)):
            if os.path.isdir(os.path.join(BENCH_TASKS, t, 'dp')):
                cases = BENCH_CASES + (('before',) if bench_kind(t) == 'refactor' else ())
                deep += [('bench', f'{t}/{c}') for c in cases if pattern in f'bench/{t}/{c}']
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


# Проход tsan (Р69; docs/policy/TESTING.md): программы набора run с
# `parallel for` — сгенерированный C и runtime под ThreadSanitizer, четыре
# потока. Сообщение TSan или расхождение с интерпретатором — сбой.
TSAN_FLAGS = ['-std=c11', '-O1', '-g', '-ffp-contract=off', '-fno-strict-aliasing', '-w', '-fsanitize=thread']
_tsan_cc = None


def tsan_cc():
    """Компилятор C, у которого ThreadSanitizer собирает и запускает; '' — нет."""
    global _tsan_cc
    if _tsan_cc is None:
        _tsan_cc = ''
        os.makedirs(NATIVE, exist_ok=True)
        probe = os.path.join(NATIVE, 'tsan_probe.c')
        with open(probe, 'w', encoding='utf-8') as f:
            f.write('int main(void) { return 0; }\n')
        for cc in dict.fromkeys([CC, 'gcc', 'clang']):
            try:
                r = subprocess.run([cc] + TSAN_FLAGS + [probe, '-o', probe + '.bin'], capture_output=True)
                if r.returncode == 0 and subprocess.run([probe + '.bin'], capture_output=True).returncode == 0:
                    _tsan_cc = cc
                    break
            except OSError:
                continue
    return _tsan_cc


def uses_parallel(path):
    """В программе (или в модулях её каталога) есть `parallel for` или задачи (О13)."""
    d = os.path.dirname(path)
    files = [os.path.join(d, n) for n in os.listdir(d) if n.endswith('.dpm')] if os.path.basename(path) == 'main.dpm' else [path]
    for f in files:
        with open(f, encoding='utf-8') as h:
            text = h.read()
            if 'parallel for' in text or 'tasks {' in text:
                return True
    return False


def tsan_one(path):
    cwd = os.path.join(TESTS, 'run')
    name = os.path.relpath(path, cwd)
    exe = os.path.join(NATIVE, 'tsan_' + name.replace(os.sep, '_')[:-len('.dpm')] + '.bin')
    try:
        r = subprocess.run([DPL, 'debug', 'c', name], capture_output=True, text=True, timeout=TIMEOUT, cwd=cwd)
    except subprocess.TimeoutExpired:
        return f'debug c: [timeout {TIMEOUT} s]'
    if r.returncode == 1:
        return None  # не проходит проверку: диагностику сверяет набор run
    if r.returncode != 0:
        return f'debug c: код {r.returncode}: ' + '\n'.join(r.stderr.splitlines()[:5])
    c_path = exe[:-len('.bin')] + '.c'
    with open(c_path, 'w', encoding='utf-8') as f:
        f.write(r.stdout)
    r = subprocess.run([tsan_cc()] + TSAN_FLAGS + ['-I', os.path.join(RUNTIME, 'core'), c_path] + RUNTIME_SOURCES +
                       ['-lm', '-pthread', '-o', exe], capture_output=True, text=True)
    if r.returncode != 0:
        return 'сборка C с TSan:\n' + '\n'.join(r.stderr.splitlines()[:5])
    env = dict(os.environ, DPL_THREADS='4', TSAN_OPTIONS='halt_on_error=1')
    try:
        p = subprocess.run([exe] + program_args(path), capture_output=True, text=True, errors='backslashreplace',
                           timeout=TIMEOUT * 4, cwd=cwd, env=env)
    except subprocess.TimeoutExpired:
        return f'[timeout {TIMEOUT * 4} s]'
    if 'ThreadSanitizer' in p.stderr:
        return 'гонка (TSan):\n' + '\n'.join(p.stderr.splitlines()[:20])
    return compare_native(p, path)


def build_native(path, exe, cwd):
    """Программа path — в нативную exe (debug c из каталога cwd: места в
    ловушках — пути от него; runtime). None — собрана; 'check' — не проходит
    проверку; иначе текст ошибки."""
    name = os.path.relpath(path, cwd)
    try:
        r = subprocess.run([DPL, 'debug', 'c', name], capture_output=True, text=True, timeout=TIMEOUT, cwd=cwd)
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


# Проходы другой цели: цель, чем запускать, расширение программы.
CROSS = {
    'windows': ('windows-x86_64', ['wine'], '.exe'),
    'android': ('android-arm64', ['qemu-aarch64'], ''),
}
NDK = os.environ.get('ANDROID_NDK_HOME') or '/opt/android-ndk'
# Компилятор C хоста библиотеки (проход lib): команда и флаги цели, как у
# драйвера (compiler/driver/build.cpp); библиотеки — из строки заголовка.
LIB_CC = {
    'host': lambda: [os.environ.get('CC') or 'cc'],
    'windows': lambda: [os.environ.get('DPL_CROSS_CC') or 'clang', '--target=x86_64-w64-mingw32', '-fuse-ld=lld'],
    'android': lambda: [os.environ.get('DPL_CROSS_CC') or
                        os.path.join(NDK, 'toolchains', 'llvm', 'prebuilt', 'linux-x86_64', 'bin', 'clang'),
                        '--target=aarch64-linux-android24', '-static'],
}


def cross_tools(cross):
    """Инструменты прохода другой цели: None — есть; иначе — чего нет."""
    if cross == 'windows':
        missing = [t for t in ('clang', 'ld.lld', 'wine') if not shutil.which(t)]
        if not os.path.isdir('/usr/x86_64-w64-mingw32'):
            missing.append('MinGW-w64')
    else:
        missing = [t for t in ('qemu-aarch64',) if not shutil.which(t)]
        if not os.path.exists(os.path.join(NDK, 'toolchains', 'llvm', 'prebuilt', 'linux-x86_64', 'bin', 'clang')):
            missing.append('Android NDK')
    return ', '.join(missing) or None


def prepare_cross():
    """Каталог Wine создаётся при первом запуске (секунды): заранее, чтобы
    параллельные тесты не создавали его наперегонки."""
    env = dict(os.environ, WINEDEBUG='-all')
    try:
        subprocess.run(['wineboot', '--init'], capture_output=True, timeout=300, env=env)
    except (OSError, subprocess.TimeoutExpired) as e:
        return f'wineboot: {e}'
    return None


def build_pass_one(arg, cross=None, bless=False):
    """Проход build: `dpl build` (профиль debug или --release), запуск,
    вывод — как у интерпретатора (docs/compiler/cli.md, «build»). С
    cross — проход другой цели: сборка для неё и запуск её runner-ом.
    Программы набора ffi сверяются со своим ожидаемым; --bless пишет его
    по сборке хоста в профиле debug."""
    path, _, profile = arg.partition(' ')
    suite = 'ffi' if os.path.relpath(path, TESTS).split(os.sep)[0] == 'ffi' else 'run'
    cwd = os.path.join(TESTS, suite)
    name = os.path.relpath(path, cwd)
    target, runner, suffix = CROSS[cross] if cross else (None, [], '')
    exe = os.path.join(NATIVE, (cross or 'build') + '_' + suite + '_' + name.replace(os.sep, '_')[:-len('.dpm')] +
                       ('_release' if profile else '') + suffix)
    argv = [DPL, 'build'] + ([profile] if profile else []) + (['--target', target] if target else []) + ['-o', exe, name]
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=TIMEOUT, cwd=cwd)
    except subprocess.TimeoutExpired:
        return f'build: [timeout {TIMEOUT} s]'
    if r.returncode == 1 and suite == 'run':
        return None  # программа не проходит проверку: её диагностику сверяет набор run
    if r.returncode != 0:  # у набора ffi диагностику никто не сверяет: отказ — сбой
        return f'build: код {r.returncode}: ' + '\n'.join(r.stderr.splitlines()[:5])
    try:
        p = subprocess.run(runner + [exe] + program_args(path), capture_output=True, text=True,
                           errors='backslashreplace', timeout=TIMEOUT, cwd=cwd, env=dict(os.environ, WINEDEBUG='-all'))
    except subprocess.TimeoutExpired:
        return f'[timeout {TIMEOUT} s]'
    got = p.stdout
    if p.stderr:
        got += '--- stderr ---\n' + p.stderr
    if p.returncode != 0:
        got += f'[exit {p.returncode}]\n'
    exp = expected_path(suite, path)
    if bless and suite == 'ffi' and not cross and not profile:
        with open(exp, 'w', encoding='utf-8') as f:
            f.write(got)
        return None
    if not os.path.exists(exp):
        return f'нет ожидаемого {os.path.relpath(exp, TESTS)} (--bless)'
    with open(exp, encoding='utf-8') as f:
        want = f.read()
    if got == want:
        return None
    diff = difflib.unified_diff(want.splitlines(), got.splitlines(), 'интерпретатор' if suite == 'run' else 'ожидалось',
                                target or 'build', lineterm='', n=1)
    return 'вывод расходится с интерпретатором:\n' + '\n'.join(list(diff)[:20])


def lib_one(arg, bless=False):
    """Проход lib (Р61): `dpl build --lib` для цели, хост host.c — её
    компилятором C с библиотекой и строкой компоновки из заголовка, запуск
    runner-ом цели; вывод — с X.out (--bless пишет его по хосту)."""
    path, _, where = arg.partition(' ')
    case = os.path.dirname(path)
    name = os.path.basename(case)
    target, runner, suffix = CROSS[where] if where in CROSS else (None, [], '')
    out = os.path.join(NATIVE, f'lib_{name}_{where}')
    os.makedirs(out, exist_ok=True)
    lib = os.path.join(out, f'lib{name}.a')
    argv = [DPL, 'build', '--lib'] + (['--target', target] if target else []) + ['-o', lib, 'main.dpm']
    r = subprocess.run(argv, capture_output=True, text=True, timeout=TIMEOUT, cwd=case)
    if r.returncode != 0:
        return f'build --lib: код {r.returncode}: ' + '\n'.join((r.stdout + r.stderr).splitlines()[:5])
    header = os.path.join(out, f'{name}.h')
    with open(header, encoding='utf-8') as f:
        link = next((ln for ln in f if ln.startswith('// Компоновка хоста')), None)
    if not link:
        return 'в заголовке нет строки компоновки'
    libs = link.split(':', 1)[1].split()[1:]  # первое — имя библиотеки
    exe = os.path.join(out, 'host' + suffix)
    cc = LIB_CC[where]() + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I', out, os.path.join(case, 'host.c'), lib] + libs
    r = subprocess.run(cc + ['-o', exe], capture_output=True, text=True, timeout=TIMEOUT)
    if r.returncode != 0:
        return 'хост не собирается: ' + '\n'.join(r.stderr.splitlines()[:8])
    try:
        p = subprocess.run(runner + [exe], capture_output=True, text=True, errors='backslashreplace', timeout=TIMEOUT,
                           cwd=case, env=dict(os.environ, WINEDEBUG='-all'))
    except subprocess.TimeoutExpired:
        return f'[timeout {TIMEOUT} s]'
    got = p.stdout + ('--- stderr ---\n' + p.stderr if p.stderr else '') + (f'[exit {p.returncode}]\n' if p.returncode else '')
    exp = case + '.out'
    if bless and where == 'host':
        with open(exp, 'w', encoding='utf-8') as f:
            f.write(got)
        return None
    if not os.path.exists(exp):
        return f'нет ожидаемого {os.path.relpath(exp, TESTS)} (--bless)'
    with open(exp, encoding='utf-8') as f:
        want = f.read()
    if got == want:
        return None
    diff = difflib.unified_diff(want.splitlines(), got.splitlines(), 'ожидалось', where, lineterm='', n=1)
    return 'вывод хоста расходится:\n' + '\n'.join(list(diff)[:20])


def capture(argv, cwd):
    """Вывод команды в формате ожидаемого: stdout, stderr, код."""
    try:
        r = subprocess.run(argv, capture_output=True, text=True, errors='backslashreplace', timeout=TIMEOUT, cwd=cwd)
    except subprocess.TimeoutExpired:
        return f'[timeout {TIMEOUT} s]\n'
    out = r.stdout
    if r.stderr:
        out += '--- stderr ---\n' + r.stderr
    if r.returncode != 0:
        out += f'[exit {r.returncode}]\n'
    return out


def example_one(case, bless):
    """Проход example (examples/X/, шаг 12 v0.1): test — отчёт `dpl test`
    (expected/test.out); run — интерпретатор с аргументами expected/run.args
    (expected/run.out); build — `dpl build --release` и запуск, вывод тот
    же; fmt — исходники в каноническом виде."""
    ex, kind = case.split('/')
    cwd = os.path.join(EXAMPLES, ex)
    exp = os.path.join(cwd, 'expected')
    args = args_file(os.path.join(exp, 'run.args')) if os.path.exists(os.path.join(exp, 'run.args')) else []
    if kind == 'fmt':
        srcs = sorted(f for f in os.listdir(cwd) if f.endswith('.dpm'))
        got = capture([DPL, 'fmt', '--check'] + srcs, cwd)
        return None if got == '' else 'не в каноническом виде (dpl fmt):\n' + got
    if kind == 'test':
        got, want_path = capture([DPL, 'test', 'main.dpm'], cwd), os.path.join(exp, 'test.out')
    elif kind == 'run':
        got, want_path = capture([DPL, 'debug', 'run', 'main.dpm'] + args, cwd), os.path.join(exp, 'run.out')
    else:
        exe = os.path.join(NATIVE, 'example_' + ex)
        r = subprocess.run([DPL, 'build', '--release', '-o', exe, 'main.dpm'], capture_output=True, text=True,
                           timeout=TIMEOUT, cwd=cwd)
        if r.returncode != 0:
            return f'build: код {r.returncode}: ' + '\n'.join(r.stderr.splitlines()[:5])
        got, want_path = capture([exe] + args, cwd), os.path.join(exp, 'run.out')
        bless = False  # ожидаемое пишет случай run
    if bless:
        with open(want_path, 'w', encoding='utf-8') as f:
            f.write(got)
        return None
    if not os.path.exists(want_path):
        return f'нет ожидаемого {os.path.relpath(want_path, ROOT)} (--bless)'
    with open(want_path, encoding='utf-8') as f:
        want = f.read()
    if got == want:
        return None
    diff = difflib.unified_diff(want.splitlines(), got.splitlines(), 'ожидалось', 'получено', lineterm='', n=1)
    return 'вывод расходится:\n' + '\n'.join(list(diff)[:20])


def bench_kind(task):
    path = os.path.join(BENCH_TASKS, task)
    if os.path.isdir(os.path.join(path, 'before')):
        return 'refactor'
    return 'fix' if os.path.exists(os.path.join(path, 'buggy.cpp')) else 'write'


def bench_files(task, which):
    """Модули решения задачи (имя → текст): эталон ref, начальный код start, before."""
    dp = os.path.join(BENCH_TASKS, task, 'dp')
    if bench_kind(task) == 'refactor':
        d = os.path.join(dp, 'before' if which in ('start', 'before') else 'ref')
        return {n: dp_check.read(os.path.join(d, n)) for n in sorted(os.listdir(d)) if n.endswith('.dpm')}
    name = 'sol.dpm' if which == 'ref' else ('buggy.dpm' if bench_kind(task) == 'fix' else 'stub.dpm')
    return {'sol.dpm': dp_check.read(os.path.join(dp, name))}


def same_numbers(a, b):
    """Выводы равны с точностью до записи чисел: слова те же, числа равны как f64."""
    la, lb = a.splitlines(), b.splitlines()
    if len(la) != len(lb):
        return False
    for x, y in zip(la, lb):
        wx, wy = x.split(), y.split()
        if len(wx) != len(wy):
            return False
        for u, v in zip(wx, wy):
            if u == v:
                continue
            try:
                if float(u) != float(v):
                    return False
            except ValueError:
                return False
    return True


def bench_one(case):
    """Проход bench (bench/tasks/X/dp/, шаг 13 v0.1): ref — эталон с драйвером
    принимается (bench/dp_check.py), свой dp/expected.txt численно равен
    expected.txt задачи; start — начальный код для модели не принимается, и
    отказ ожидаемого рода; before — модули до рефакторинга со старым
    драйвером дают тот же вывод; fmt — исходники в каноническом виде."""
    task, kind = case.split('/')
    path = os.path.join(BENCH_TASKS, task)
    dp = os.path.join(path, 'dp')
    if kind == 'fmt':
        srcs = sorted(os.path.relpath(os.path.join(d, f), dp) for d, _, fs in os.walk(dp) for f in fs if f.endswith('.dpm'))
        got = capture([DPL, 'fmt', '--check'] + srcs, dp)
        return None if got == '' else 'не в каноническом виде (dpl fmt):\n' + got
    want = dp_check.expected(path)
    if kind == 'ref' and os.path.exists(os.path.join(dp, 'expected.txt')):
        if not same_numbers(want, dp_check.read(os.path.join(path, 'expected.txt'))):
            return 'dp/expected.txt не равен expected.txt задачи даже с точностью до записи чисел'
    driver = dp_check.read(os.path.join(dp, 'before_main.dpm' if kind == 'before' else dp_check.DRIVER))
    build_dir = os.path.join(NATIVE, 'bench', task, kind)
    if os.path.isdir(build_dir):
        shutil.rmtree(build_dir)
    cls, msg = dp_check.evaluate(bench_files(task, kind), driver, want, build_dir, DPL)
    if kind == 'start':
        if cls in BENCH_START[bench_kind(task)]:
            return None
        return f'начальный код: «{cls}», ожидалось {"/".join(sorted(BENCH_START[bench_kind(task)]))}: {msg}'
    return None if cls == dp_check.ACCEPTED else f'«{cls}»: {msg}'


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
        p = subprocess.run([exe] + program_args(path), capture_output=True, text=True, errors='backslashreplace',
                           timeout=TIMEOUT, cwd=cwd)
    except subprocess.TimeoutExpired:
        return f'[timeout {TIMEOUT} s]'
    return compare_native(p, path)


def compare_native(p, path):
    """Вывод и код выхода нативной программы против ожидаемого набора run."""
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
    """Набор fmt: `dpl fmt` правит копию входа во временном каталоге.
    Ожидаемое — текст файла после него, затем вывод и код. Второй прогон
    `fmt --check` уже ничего не находит (запись неподвижна)."""
    name = os.path.basename(path)
    tmp = tempfile.mkdtemp(prefix='dpfmt-')
    try:
        shutil.copy(path, os.path.join(tmp, name))
        try:
            r = subprocess.run([DPL, 'fmt', name], capture_output=True, text=True, errors='backslashreplace',
                               timeout=TIMEOUT, cwd=tmp)
            again = subprocess.run([DPL, 'fmt', '--check', name], capture_output=True, text=True,
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
        r = subprocess.run([DPL, 'debug', 'ir', os.path.basename(path)], capture_output=True, text=True,
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
    if suite == 'tsan':
        return tsan_one(path)
    if suite == 'build':
        return build_pass_one(path)
    if suite in CROSS:
        return build_pass_one(path, suite)
    if suite == 'ffi':
        return build_pass_one(path, bless=bless)
    if suite == 'lib':
        return lib_one(path, bless)
    if suite == 'limits':
        return limits.check_case(path, DPL)
    if suite == 'example':
        return example_one(path, bless)
    if suite == 'bench':
        return bench_one(path)
    if suite == 'runtime':
        return runtime.check_case(path, DPL, runtime_native, NATIVE)
    if suite == 'bindings':
        return bindings.check_case(path, DPL, bless)
    if suite == 'worker':
        return worker.worker_case(path, DPL_RELEASE, bless)
    if suite == 'sandbox':
        return worker.sandbox_case(path, PROBE)
    if suite == 'service':
        return service.check_case(path, DPL_RELEASE)
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
    if any(suite == 'windows' or (suite == 'lib' and path.endswith(' windows')) for suite, path in tests):
        err = prepare_cross()
        if err:
            print('компилятор: Wine не готовится')
            print(err)
            return 1
    # Проходы native, build, windows и android сверяют с ожидаемым набора run — после того, как --bless его перепишет.
    late = ('native', 'tsan', 'build', 'example', 'bench', 'lib') + tuple(CROSS)
    if a.bless:  # ожидаемое ffi и lib пишет сборка хоста — до сверки других целей с ним
        first = [t for t in tests if (t[0] == 'ffi' and ' ' not in t[1]) or (t[0] == 'lib' and t[1].endswith(' host'))]
        for t in first:
            run_one(t[0], t[1], True)
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
            name = suite + '/' + path if suite in ('bench', 'bindings', 'example', 'limits', 'runtime', 'sandbox', 'service') else os.path.relpath(path, TESTS)
            if suite in ('build', 'ffi', 'lib') + tuple(CROSS):
                name = suite + '/' + os.path.relpath(path, TESTS)
            print(f'  {name}: {r}')
        return 1
    verb = 'записано ожидаемое' if a.bless else 'прошли'
    note = ''.join(f'; {c} пропущен — нет {cross_tools(c)}' for c in CROSS if cross_tools(c) and not a.k)
    note += ''.join(f'; bindings/{c} пропущен — нет {bindings.missing(c)}' for c in bindings.CASES
                    if bindings.missing(c) and not a.k)
    if not tsan_cc() and not a.k:
        note += '; tsan пропущен — нет компилятора C с ThreadSanitizer'
    print(f'компилятор: собран, тестов {len(tests)} {verb} ({summary}){note}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
