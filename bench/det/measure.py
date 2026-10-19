#!/usr/bin/env python3
"""Цена класса D (Р56: канонический NaN после каждой операции с
плавающими; docs/policy/BENCHMARKS.md, «цена гарантий»): те же нагрузки
с `@det import` и с обычным `import`, сборка release.

    python3 bench/det/measure.py [--runs 21] [--save NAME]

Нагрузки — рельеф по шуму (bench/det/terrain.dpm) и симуляция тел
(tests/run/det_sim/physics.dpm) в большем масштабе. Компилятор — сборка
Release (build/release/dpl; её собирает tests/run.py). Вывод двух
вариантов должен совпасть: канон меняет только биты NaN. --save — данные
и отчёт в bench/results/NAME/ (measure.json, summary.md).
"""
import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DPL = os.path.join(ROOT, 'build', 'release', 'dpl')

# Нагрузка: модуль, его имя и тело main.
LOADS = {
    'terrain': (os.path.join(ROOT, 'bench', 'det', 'terrain.dpm'), 'terrain',
                '    println(str(terrain.total(20261018, 1024)))\n'),
    'bodies': (os.path.join(ROOT, 'tests', 'run', 'det_sim', 'physics.dpm'), 'physics',
               '    var w = physics.new_world(20261010, 4096)\n'
               '    for i in 0..400 {\n'
               '        physics.step(&mut w, 0.01)\n'
               '    }\n'
               '    println(physics.report(&w))\n'),
}


def build(load, det, tmp):
    """Программа нагрузки в варианте det; путь к исполняемому и число canon в IR."""
    src, mod, body = LOADS[load]
    d = os.path.join(tmp, f'{load}-{"det" if det else "plain"}')
    os.makedirs(d)
    shutil.copy(src, os.path.join(d, mod + '.dpm'))
    with open(os.path.join(d, 'main.dpm'), 'w', encoding='utf-8') as f:
        f.write(('@det ' if det else '') + f'import {mod}\n\nfn main() {{\n{body}}}\n')
    exe = os.path.join(d, 'main.bin')
    r = subprocess.run([DPL, 'build', '--release', '-o', exe, 'main.dpm'], cwd=d, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f'{load}: сборка: {r.stderr.strip()}')
    ir = subprocess.run([DPL, 'debug', 'ir', 'main.dpm'], cwd=d, capture_output=True, text=True).stdout
    return exe, sum(1 for line in ir.splitlines() if ' = canon ' in line)


def run(exes, runs):
    """Прогрев, затем runs кругов: в каждом — по прогону каждого варианта
    по очереди (дрейф машины ложится на оба). Времена (мс) и выводы."""
    outs = [subprocess.run([e], capture_output=True, text=True, check=True).stdout for e in exes]
    times = [[] for _ in exes]
    for _ in range(runs):
        for i, e in enumerate(exes):
            a = time.perf_counter()
            r = subprocess.run([e], capture_output=True, text=True, check=True)
            times[i].append((time.perf_counter() - a) * 1000)
            if r.stdout != outs[i]:
                sys.exit(f'{e}: вывод меняется от прогона к прогону')
    return times, outs


def environment():
    cpu = '?'
    try:
        with open('/proc/cpuinfo', encoding='utf-8') as f:
            cpu = next((ln.split(':', 1)[1].strip() for ln in f if ln.startswith('model name')), '?')
    except OSError:
        pass
    cc = subprocess.run(['cc', '--version'], capture_output=True, text=True).stdout.splitlines()
    commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout
    return {'cpu': f'{os.cpu_count()} CPU, {cpu}', 'os': platform.platform(), 'cc': cc[0] if cc else '?',
            'commit': commit.strip()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--runs', type=int, default=21)
    ap.add_argument('--save', default='')
    args = ap.parse_args()
    if not os.path.exists(DPL):
        sys.exit(f'нет {DPL}: соберите компилятор (python3 tests/run.py)')
    data = {'env': environment(), 'runs': args.runs, 'loads': {}}
    with tempfile.TemporaryDirectory() as tmp:
        for load in LOADS:
            built = [build(load, det, tmp) for det in (False, True)]
            times, outs = run([b[0] for b in built], args.runs)
            res = {}
            for i, v in enumerate(('plain', 'det')):
                res[v] = {'ms': times[i], 'median': statistics.median(times[i]), 'canon': built[i][1], 'out': outs[i]}
            res['same_output'] = outs[0] == outs[1]
            res['ratio'] = res['det']['median'] / res['plain']['median']
            res['ratio_min'] = min(res['det']['ms']) / min(res['plain']['ms'])
            data['loads'][load] = res
    env = data['env']
    lines = ['# Цена класса D: канонический NaN (Р56)', '',
             f'Машина: {env["cpu"]}; {env["os"]}. Компилятор C: {env["cc"]}. Коммит {env["commit"]}, '
             f'build/release/dpl, `dpl build --release`. Кругов: {args.runs}; в круге варианты идут по очереди, '
             'чтобы дрейф машины ложился на оба; прогрев — один прогон.', '',
             'Те же нагрузки с `@det import` (канон после каждой операции с плавающими) и с обычным `import`.', '',
             '| нагрузка | canon в IR | без класса D, мс (медиана / мин. / макс.) | класс D, мс | разница по медиане | '
             'по минимуму | вывод совпал |',
             '|---|---|---|---|---|---|---|']
    for load, r in data['loads'].items():
        p, d = r['plain'], r['det']
        lines.append(f'| {load} | {d["canon"]} | {p["median"]:.1f} / {min(p["ms"]):.1f} / {max(p["ms"]):.1f} | '
                     f'{d["median"]:.1f} / {min(d["ms"]):.1f} / {max(d["ms"]):.1f} | {(r["ratio"] - 1) * 100:+.1f}% | '
                     f'{(r["ratio_min"] - 1) * 100:+.1f}% | {"да" if r["same_output"] else "нет"} |')
    lines += ['', 'Нагрузки: `terrain` — рельеф 1024 × 1024 по шуму значений, шесть октав (`bench/det/terrain.dpm`); '
              '`bodies` — 4096 тел, 400 шагов (`tests/run/det_sim/physics.dpm`).']
    report = '\n'.join(lines) + '\n'
    if args.save:
        d = os.path.join(ROOT, 'bench', 'results', args.save)
        os.makedirs(d, exist_ok=True)
        for r in data['loads'].values():
            for v in ('det', 'plain'):
                r[v].pop('out')
        with open(os.path.join(d, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        with open(os.path.join(d, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(report)
    print(report, end='')
    if not all(r['same_output'] for r in data['loads'].values()):
        sys.exit(1)


if __name__ == '__main__':
    main()
