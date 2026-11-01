#!/usr/bin/env python3
"""Цена счётчика глубины вызовов dp_depth в эмулированном TLS (TODO:
«dp_depth на Android»; docs/policy/BENCHMARKS.md, «цена гарантий»).

    python3 bench/tls/measure.py [--runs 7] [--layouts 1,16,32,64] [--qemu-cpu cortex-a72] [--targets android,host] [--save NAME]

dp_depth — `_Thread_local` (Р69): функция меняет его на входе и выходе.
Цель android-arm64 (`aarch64-linux-android24`) не знает ELF TLS (он есть
с API 29), clang ставит эмулированный: обращение — вызов
`__emutls_get_address`. Варианты сборки одной программы:
- emutls — эмулированный TLS (у android — как сейчас);
- elftls — ELF TLS (у android — `--target=aarch64-linux-android29
  -fno-emulated-tls`, то есть Android 10 и новее);
- notls — без TLS (`-D_Thread_local=`): нижняя граница, только для
  замера — с потоками так нельзя, нагрузки однопоточные.

Цели:
- android — сборка `--target android-arm64` (компилятор C — обёртка над
  clang NDK через DPL_CROSS_CC), запуск под qemu-aarch64. Процессор qemu —
  --qemu-cpu: по умолчанию cortex-a72 (ARMv8.0, без аутентификации
  указателей). У `-cpu max` qemu считает PAuth программно, а
  `__emutls_get_address` из NDK начинается с `paciasp` — эмуляция TLS
  выходит дороже в десятки раз, чем на железе.
- host — сборка для x86-64 clang-ом хоста (`-femulated-tls` — у emutls),
  настоящее время на железе, в нескольких раскладках кода (--layouts:
  -falign-loops), у варианта — лучшая.

Нагрузки — вызовы (bench/tls/fib.dpm), рельеф по шуму и симуляция тел
(как в bench/det/measure.py), М2 (bench/ecs/pool.dpm). Вывод вариантов
должен совпасть. --save — данные и отчёт в bench/results/NAME/.
"""
import argparse
import json
import os
import platform
import shlex
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DPL = os.path.join(ROOT, 'build', 'release', 'dpl')
NDK = os.environ.get('ANDROID_NDK_HOME') or '/opt/android-ndk'
NDK_CC = os.path.join(NDK, 'toolchains', 'llvm', 'prebuilt', 'linux-x86_64', 'bin', 'clang')

# Нагрузка: файлы (модуль → путь), тело main (None — main есть в модуле main.dpm), аргументы.
LOADS = {
    'fib': ({'main': os.path.join(ROOT, 'bench', 'tls', 'fib.dpm')}, None, []),
    'terrain': ({'terrain': os.path.join(ROOT, 'bench', 'det', 'terrain.dpm')},
                '    println(str(terrain.total(20261018, 1024)))\n', []),
    'bodies': ({'physics': os.path.join(ROOT, 'tests', 'run', 'det_sim', 'physics.dpm')},
               '    var w = physics.new_world(20261010, 4096)\n'
               '    for i in 0..400 {\n'
               '        physics.step(&mut w, 0.01)\n'
               '    }\n'
               '    println(physics.report(&w))\n', []),
    'ecs': ({'main': os.path.join(ROOT, 'bench', 'ecs', 'pool.dpm')}, None, ['100000', '100']),
}
LOAD_NAMES = {'fib': 'вызовы: fib(32)', 'terrain': 'рельеф по шуму', 'bodies': 'симуляция тел',
              'ecs': 'М2: Pool, 100 тыс. × 100 кадров'}
VARIANTS = ('emutls', 'elftls', 'notls')
FLAGS = {
    'android': {'emutls': [], 'elftls': ['--target=aarch64-linux-android29', '-fno-emulated-tls'],
                'notls': ['-D_Thread_local=']},
    'host': {'emutls': ['-femulated-tls'], 'elftls': [], 'notls': ['-D_Thread_local=']},
}


def wrapper(tmp, target, variant, align):
    """Компилятор C варианта: clang цели с флагами варианта (после флагов драйвера)."""
    path = os.path.join(tmp, f'cc-{target}-{variant}-{align}')
    if not os.path.exists(path):
        cc = NDK_CC if target == 'android' else 'clang'
        extra = FLAGS[target][variant] + [f'-falign-loops={align}']
        with open(path, 'w', encoding='utf-8') as f:
            f.write(f'#!/bin/sh\nexec {shlex.quote(cc)} "$@" {" ".join(map(shlex.quote, extra))}\n')
        os.chmod(path, 0o755)
    return path


def build(load, target, variant, align, tmp):
    files, body, _ = LOADS[load]
    d = os.path.join(tmp, f'{load}-{target}-{variant}-{align}')
    os.makedirs(d)
    for mod, src in files.items():
        shutil.copy(src, os.path.join(d, mod + '.dpm'))
    if body is not None:
        mod = next(iter(files))
        with open(os.path.join(d, 'main.dpm'), 'w', encoding='utf-8') as f:
            f.write(f'import {mod}\n\nfn main() {{\n{body}}}\n')
    exe = os.path.join(d, 'main.bin')
    cc = wrapper(tmp, target, variant, align)
    if target == 'android':
        argv, env = [DPL, 'build', '--release', '--target', 'android-arm64'], dict(os.environ, DPL_CROSS_CC=cc)
    else:
        argv, env = [DPL, 'build', '--release'], dict(os.environ, CC=cc)
    r = subprocess.run(argv + ['-o', exe, 'main.dpm'], cwd=d, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        sys.exit(f'{load} {target} {variant}: сборка: {r.stderr.strip()}')
    return exe


def run(cells, runs):
    """cells: {ключ: argv}. Прогрев, затем runs кругов по очереди (дрейф
    машины ложится на все). Медианы (мс) и выводы."""
    env = dict(os.environ, DPL_THREADS='1')
    outs = {k: subprocess.run(a, capture_output=True, text=True, check=True, env=env).stdout for k, a in cells.items()}
    times = {k: [] for k in cells}
    for _ in range(runs):
        for k, a in cells.items():
            t = time.perf_counter()
            r = subprocess.run(a, capture_output=True, text=True, check=True, env=env)
            times[k].append((time.perf_counter() - t) * 1000)
            if r.stdout != outs[k]:
                sys.exit(f'{k}: вывод меняется от прогона к прогону')
    return {k: statistics.median(v) for k, v in times.items()}, outs


def tool_version(argv):
    try:
        r = subprocess.run(argv, capture_output=True, text=True)
    except OSError:
        return '?'
    lines = (r.stdout or r.stderr).splitlines()
    return lines[0].strip() if lines else '?'


def environment(qemu_cpu):
    cpu = '?'
    try:
        with open('/proc/cpuinfo', encoding='utf-8') as f:
            cpu = next((ln.split(':', 1)[1].strip() for ln in f if ln.startswith('model name')), '?')
    except OSError:
        pass
    commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True, text=True)
    return {'cpu': f'{os.cpu_count()} CPU, {cpu}', 'os': platform.platform(), 'clang': tool_version(['clang', '--version']),
            'ndk_clang': tool_version([NDK_CC, '--version']), 'qemu': tool_version(['qemu-aarch64', '--version']),
            'qemu_cpu': qemu_cpu, 'commit': commit.stdout.strip() or '?'}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--runs', type=int, default=7)
    ap.add_argument('--layouts', default='1,16,32,64', help='значения -falign-loops у цели host через запятую')
    ap.add_argument('--qemu-cpu', default='cortex-a72')
    ap.add_argument('--targets', default='android,host')
    ap.add_argument('--loads', default=','.join(LOADS))
    ap.add_argument('--save', default='')
    args = ap.parse_args()
    if not os.path.exists(DPL):
        sys.exit(f'нет {DPL}: соберите компилятор (python3 tests/run.py)')
    targets = args.targets.split(',')
    if 'android' in targets and not (os.path.exists(NDK_CC) and shutil.which('qemu-aarch64')):
        sys.exit('нет NDK или qemu-aarch64: bash tools/setup/cross_android.sh')
    layouts = {'android': [1], 'host': [int(a) for a in args.layouts.split(',')]}
    runner = {'android': ['qemu-aarch64', '-cpu', args.qemu_cpu], 'host': []}
    data = {'env': environment(args.qemu_cpu), 'runs': args.runs, 'layouts': layouts['host'], 'results': {},
            'same_output': True}
    with tempfile.TemporaryDirectory() as tmp:
        for load in args.loads.split(','):
            extra = LOADS[load][2]
            cells = {(t, v, a): runner[t] + [build(load, t, v, a, tmp)] + extra
                     for t in targets for v in VARIANTS for a in layouts[t]}
            medians, outs = run(cells, args.runs)
            if len(set(outs.values())) != 1:
                data['same_output'] = False
            for t in targets:
                best = {v: min(medians[(t, v, a)] for a in layouts[t]) for v in VARIANTS}
                data['results'].setdefault(t, {})[load] = {'ms': best, 'by_layout': {
                    v: {str(a): medians[(t, v, a)] for a in layouts[t]} for v in VARIANTS}}
            print(f'{load}: ' + '; '.join(f'{t} ' + ', '.join(f'{v} {data["results"][t][load]["ms"][v]:.0f}'
                                                             for v in VARIANTS) for t in targets), file=sys.stderr)
    report = summary(data)
    if args.save:
        d = os.path.join(ROOT, 'bench', 'results', args.save)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        with open(os.path.join(d, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(report)
    print(report, end='')
    if not data['same_output']:
        sys.exit(1)


def pct(ratio):
    return f'{(ratio - 1) * 100:+.0f}%'


def summary(data):
    env = data['env']
    lines = ['# Цена dp_depth в эмулированном TLS', '',
             f'Машина: {env["cpu"]}; {env["os"]}. Коммит {env["commit"]}; `dpl build --release` (C с -O2), '
             f'`DPL_THREADS=1`; медиана {data["runs"]} кругов, прогоны чередуются.', '',
             f'- android — {env["ndk_clang"]}, запуск под {env["qemu"]}, `-cpu {env["qemu_cpu"]}`; время эмуляции, '
             'не устройства: годится для отношений вариантов, не для абсолютных чисел.',
             f'- host — {env["clang"]}, x86-64; раскладки кода `-falign-loops` = '
             f'{", ".join(map(str, data["layouts"]))}, у варианта — лучшая.', '',
             'Варианты: emutls — эмулированный TLS (у android сейчас), elftls — ELF TLS (у android — API 29), '
             'notls — без TLS (нижняя граница, с потоками нельзя). Цена — к elftls и к notls.', '']
    for t, res in data['results'].items():
        lines += [f'## {t}', '', '| нагрузка | emutls, мс | elftls, мс | notls, мс | emutls к elftls | elftls к notls |',
                  '|---|---|---|---|---|---|']
        for load, r in res.items():
            ms = r['ms']
            lines.append(f'| {LOAD_NAMES[load]} | {ms["emutls"]:.0f} | {ms["elftls"]:.0f} | {ms["notls"]:.0f} | '
                         f'{pct(ms["emutls"] / ms["elftls"])} | {pct(ms["elftls"] / ms["notls"])} |')
        lines.append('')
    lines.append(f'Вывод вариантов совпал: {"да" if data["same_output"] else "нет"}.')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    main()
