"""Проход bindings исполнителя тестов (tests/run.py): привязки bindings/
(Р58, Р62) против настоящих библиотек хоста.

- vulkan/layout — раскладка каждой структуры привязки Vulkan (размер,
  выравнивание, смещение и размер поля) сверяется с vulkan.h: программа
  с параметром на каждую структуру переводится в C (`dpl debug c`), к
  нему — _Static_assert на каждое поле; компилятор C проверяет с
  заголовком. Нужен vulkan.h (пакет libvulkan-dev).
- sdl3/layout — то же для SDL3 против закреплённых заголовков
  bindings/sdl3/include: пакеты хоста не нужны.
- vulkan/run, sdl3/run — tests/bindings/X/main.dpm собирается `dpl
  build` и идёт на программном драйвере lavapipe (пакет
  mesa-vulkan-drivers); SDL3 — с видео offscreen, без экрана: вывод — с
  tests/bindings/X.out (--bless пишет его). Библиотеки ставит
  tools/setup/host_libs.sh.

Без заголовка, библиотеки или драйвера случай пропускается (missing), и
итог прогона это называет.
"""
import difflib
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, 'tests')
GEN = os.path.join(ROOT, 'bindings', 'gen')
CC = os.environ.get('CC', 'cc')
TIMEOUT = 120
VULKAN_H = '/usr/include/vulkan/vulkan.h'
LAVAPIPE = '/usr/share/vulkan/icd.d/lvp_icd.json'

CASES = ['vulkan/layout', 'sdl3/layout', 'vulkan/run', 'sdl3/run']


def _links(lib):
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 'probe.c')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('int main(void) { return 0; }\n')
        r = subprocess.run([CC, src, '-l' + lib, '-o', os.path.join(d, 'probe')], capture_output=True)
        return r.returncode == 0


_missing = {}


def missing(case):
    """Чего нет хосту для случая; None — всё есть."""
    if case not in _missing:
        need = None
        if case == 'vulkan/layout' and not os.path.exists(VULKAN_H):
            need = 'vulkan.h (libvulkan-dev)'
        elif case.endswith('/run'):
            if not _links('vulkan'):
                need = 'libvulkan для компоновки (libvulkan-dev)'
            elif not os.path.exists(LAVAPIPE):
                need = 'lavapipe (mesa-vulkan-drivers)'
            elif case == 'sdl3/run' and not _links('SDL3'):
                need = 'libSDL3 (tools/setup/host_libs.sh)'
        _missing[case] = need
    return _missing[case]


def _generator(name):
    """Модуль генератора bindings/gen/<name>.py."""
    sys.path.insert(0, GEN)
    try:
        return __import__(name)
    finally:
        sys.path.pop(0)


def _vulkan_structs():
    """[(имя C, [(поле dpl, поле C)])] — структуры привязки по генератору."""
    vkgen = _generator('vulkan')
    import xml.etree.ElementTree as ET
    reg = vkgen.Registry(ET.parse(vkgen.XML).getroot(), vkgen.read_extensions())
    gen = vkgen.Gen(reg)
    gen.run()
    out = []
    for name, fields in gen.structs.items():
        members = [m for m in reg.types[name].findall('member') if vkgen.api_ok(m)]
        out.append((name, [(f[0], m.find('name').text) for f, m in zip(fields, members)]))
    return out


def _sdl3_structs():
    gen = _generator('sdl3').Gen()
    gen.run()
    return [(n, [(f[0], f[3]) for f in fields]) for n, fields in gen.structs.items()]


def _lp(s):
    return f'{len(s)}{s}'


def layout(dpl, module):
    """module — vulkan или sdl3: раскладка структур против заголовков C."""
    if module == 'vulkan':
        structs = _vulkan_structs()
        includes = ['#include <vulkan/vulkan.h>']
        flags = []
    else:
        sdl = _generator('sdl3')
        structs = _sdl3_structs()
        includes = [f'#include <SDL3/{h}>' for h in sdl.read_list()]
        flags = ['-I', os.path.join(ROOT, 'bindings', 'sdl3', 'include')]
    with tempfile.TemporaryDirectory() as d:
        prog = [f'import {module}', '']
        for i in range(0, len(structs), 16):  # по 16 параметров в функции
            part = structs[i:i + 16]
            params = ', '.join(f'a{k}: &{module}.{n}' for k, (n, _) in enumerate(part))
            prog += [f'fn touch{i // 16}({params}) {{', '}', '']
        prog += ['fn main() {', '}']
        with open(os.path.join(d, 'main.dpm'), 'w', encoding='utf-8') as f:
            f.write('\n'.join(prog) + '\n')
        r = subprocess.run([dpl, 'debug', 'c', 'main.dpm'], capture_output=True, text=True, cwd=d, timeout=TIMEOUT)
        if r.returncode != 0:
            return 'debug c: ' + '\n'.join((r.stdout + r.stderr).splitlines()[:8])
        checks = ['', '#include <stddef.h>'] + includes
        for n, fields in structs:
            t = f'dp_T_{_lp(module)}{_lp(n)}'
            checks.append(f'_Static_assert(sizeof({t}) == sizeof({n}), "size of {n}");')
            checks.append(f'_Static_assert(_Alignof({t}) == _Alignof({n}), "alignment of {n}");')
            for dn, cn in fields:
                checks.append(f'_Static_assert(offsetof({t}, f_{dn}) == offsetof({n}, {cn}), "offset of {n}.{cn}");')
                checks.append(f'_Static_assert(sizeof((({t}*)0)->f_{dn}) == sizeof((({n}*)0)->{cn}), '
                              f'"size of {n}.{cn}");')
        src = os.path.join(d, 'layout.c')
        with open(src, 'w', encoding='utf-8') as f:
            f.write(r.stdout + '\n'.join(checks) + '\n')
        r = subprocess.run([CC, '-std=c11', '-fsyntax-only', '-w', '-I', os.path.join(ROOT, 'runtime', 'core')] + flags +
                           [src], capture_output=True, text=True, timeout=TIMEOUT)
        if r.returncode != 0:
            errs = [ln for ln in r.stderr.splitlines() if 'error' in ln][:8]
            return f'раскладка расходится с заголовками C ({len(structs)} структур):\n' + '\n'.join(errs)
    return None


def run(dpl, name, bless):
    case = os.path.join(TESTS, 'bindings', name)
    exp = case + '.out'
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, name)
        r = subprocess.run([dpl, 'build', '-o', exe, 'main.dpm'], capture_output=True, text=True, cwd=case,
                           timeout=TIMEOUT)
        if r.returncode != 0:
            return f'build: код {r.returncode}: ' + '\n'.join((r.stdout + r.stderr).splitlines()[:6])
        env = dict(os.environ, VK_ICD_FILENAMES=LAVAPIPE, VK_DRIVER_FILES=LAVAPIPE)
        try:
            p = subprocess.run([exe], capture_output=True, text=True, errors='backslashreplace', timeout=TIMEOUT,
                               cwd=case, env=env)
        except subprocess.TimeoutExpired:
            return f'[timeout {TIMEOUT} s]'
    got = p.stdout + ('--- stderr ---\n' + p.stderr if p.stderr else '') + (f'[exit {p.returncode}]\n' if p.returncode else '')
    if bless:
        with open(exp, 'w', encoding='utf-8') as f:
            f.write(got)
        return None
    if not os.path.exists(exp):
        return f'нет ожидаемого bindings/{name}.out (--bless)'
    with open(exp, encoding='utf-8') as f:
        want = f.read()
    if got == want:
        return None
    diff = difflib.unified_diff(want.splitlines(), got.splitlines(), 'ожидалось', 'получено', lineterm='', n=1)
    return 'вывод расходится:\n' + '\n'.join(list(diff)[:20])


def check_case(case, dpl, bless=False):
    if case == 'vulkan/layout':
        return layout(dpl, 'vulkan')
    if case == 'sdl3/layout':
        return layout(dpl, 'sdl3')
    if case.endswith('/run'):
        return run(dpl, case.split('/')[0], bless)
    return f'нет случая {case}'
