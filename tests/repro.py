"""Проход repro (docs/policy/DETERMINISM.md, раздел 6): две сборки одной
программы дают те же байты.

Сборки идут в разных каталогах (разной длины пути), dpl вызывается по
разным путям (абсолютному и относительному), временный каталог у каждой
свой. Случаи:
- host, host-release — программа examples/swarm (модули программы и
  стандартной библиотеки os) в профилях debug и release;
- lib — библиотека tests/lib/chain (`dpl build --lib`): архив и заголовок;
- android, windows — swarm для другой цели, если её инструменты есть;
- apk — пакет examples/triangle (examples/triangle/android/build.sh),
  если есть Android SDK и SDL3 для Android: время записей —
  SOURCE_DATE_EPOCH, ключ отладки — один на обе сборки.
"""
import os
import shutil
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SWARM = os.path.join(ROOT, 'examples', 'swarm')
CHAIN = os.path.join(ROOT, 'tests', 'lib', 'chain')
TRIANGLE = os.path.join(ROOT, 'examples', 'triangle')
NDK = os.environ.get('ANDROID_NDK_HOME') or '/opt/android-ndk'
TIMEOUT = 600

CASES = ['host', 'host-release', 'lib', 'android', 'windows', 'apk']
TARGET = {'android': 'android-arm64', 'windows': 'windows-x86_64'}


def missing(case):
    """Чего нет для случая; None — всё есть."""
    if case == 'android' and not os.path.exists(os.path.join(NDK, 'toolchains', 'llvm', 'prebuilt', 'linux-x86_64',
                                                             'bin', 'clang')):
        return 'Android NDK'
    if case == 'windows' and not (shutil.which('clang') and shutil.which('ld.lld') and
                                  os.path.isdir('/usr/x86_64-w64-mingw32')):
        return 'clang, lld и MinGW-w64'
    if case == 'apk':
        import bindings  # tests/bindings.py: те же инструменты, что у triangle/android
        return bindings.missing('triangle/android')
    return None


def copy_sources(src, dst):
    os.makedirs(dst)
    for name in sorted(os.listdir(src)):
        if name.endswith(('.dpm', '.c', '.h')):
            shutil.copy(os.path.join(src, name), dst)


def differ(a, b):
    """Первое расхождение двух файлов: None — одинаковы."""
    with open(a, 'rb') as f:
        x = f.read()
    with open(b, 'rb') as f:
        y = f.read()
    if x == y:
        return None
    at = next((i for i in range(min(len(x), len(y))) if x[i] != y[i]), min(len(x), len(y)))
    return f'{os.path.basename(a)}: размеры {len(x)} и {len(y)}, первое расхождение — байт {at}'


def build_twice(dpl, src, argv, outs, env=None):
    """Две сборки src: argv — аргументы dpl после имени (без -o), outs —
    файлы результата относительно каталога сборки (первый — в -o)."""
    with tempfile.TemporaryDirectory() as tmp:
        dirs = [os.path.join(tmp, 'a', 'p'), os.path.join(tmp, 'second-place', 'deeper', 'p')]
        for i, d in enumerate(dirs):
            copy_sources(src, d)
            exe = dpl if i == 0 else os.path.relpath(dpl, d)  # абсолютный и относительный путь dpl
            r = subprocess.run([exe] + argv + ['-o', outs[0], 'main.dpm'], cwd=d, capture_output=True, text=True,
                               timeout=TIMEOUT, env=env)
            if r.returncode != 0:
                return f'сборка {i + 1}: код {r.returncode}: ' + '\n'.join((r.stdout + r.stderr).splitlines()[:5])
        for out in outs:
            why = differ(os.path.join(dirs[0], out), os.path.join(dirs[1], out))
            if why:
                return why
    return None


def apk_twice():
    """Две сборки пакета triangle из копий примера в разных каталогах."""
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, DPL=os.path.join(ROOT, 'build', 'compiler', 'dpl'),
                   DPL_APK_KEYSTORE=os.path.join(tmp, 'debug.keystore'), SOURCE_DATE_EPOCH='1793491200')
        apks = []
        for i, base in enumerate(('a', 'second-place/deeper')):
            # Копия примера в своём «корне»: build.sh берёт корень по своему месту.
            root = os.path.join(tmp, base)
            ex = os.path.join(root, 'examples', 'triangle')
            shutil.copytree(TRIANGLE, ex)
            out = os.path.join(tmp, f'out{i}')
            r = subprocess.run(['bash', os.path.join(ex, 'android', 'build.sh'), out], capture_output=True, text=True,
                               timeout=TIMEOUT, env=env)
            if r.returncode != 0:
                return f'пакет {i + 1}: код {r.returncode}: ' + '\n'.join((r.stdout + r.stderr).splitlines()[-5:])
            apks.append(os.path.join(out, 'triangle.apk'))
        return differ(*apks)


def check_case(case, dpl):
    if case == 'host':
        return build_twice(dpl, SWARM, ['build'], ['swarm'])
    if case == 'host-release':
        return build_twice(dpl, SWARM, ['build', '--release'], ['swarm'])
    if case == 'lib':
        return build_twice(dpl, CHAIN, ['build', '--lib'], ['libchain.a', 'chain.h'])
    if case in TARGET:
        return build_twice(dpl, SWARM, ['build', '--target', TARGET[case]], ['swarm'])
    if case == 'apk':
        return apk_twice()
    return f'нет случая {case}'
