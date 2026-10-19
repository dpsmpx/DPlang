#!/usr/bin/env python3
"""Цена класса D (Р56: канонический NaN там, где его видно;
docs/policy/BENCHMARKS.md, «цена гарантий»): те же нагрузки с
`@det import` и с обычным `import`, сборка release.

    python3 bench/det/measure.py [--runs 21] [--save NAME] [--before DPL]

Нагрузки — рельеф по шуму (bench/det/terrain.dpm) и симуляция тел
(tests/run/det_sim/physics.dpm) в большем масштабе. Компилятор — сборка
Release (build/release/dpl; её собирает tests/run.py). Вывод двух
вариантов должен совпасть: канон меняет только биты NaN. --save — данные
и отчёт в bench/results/NAME/ (measure.json, summary.md). --before —
третий вариант: `@det import` прежним компилятором DPL (его stdlib и
runtime — из этого репозитория); прогоны чередуются со всеми.
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


def build(load, variant, tmp, dpl=DPL):
    """Программа нагрузки в варианте variant (plain, det, before) компилятором
    dpl; путь к исполняемому и число canon в IR."""
    src, mod, body = LOADS[load]
    d = os.path.join(tmp, f'{load}-{variant}')
    os.makedirs(d)
    shutil.copy(src, os.path.join(d, mod + '.dpm'))
    with open(os.path.join(d, 'main.dpm'), 'w', encoding='utf-8') as f:
        f.write(('' if variant == 'plain' else '@det ') + f'import {mod}\n\nfn main() {{\n{body}}}\n')
    exe = os.path.join(d, 'main.bin')
    env = dict(os.environ)
    if dpl != DPL:  # прежний компилятор вне репозитория: библиотеки — отсюда
        env.update(DPL_STDLIB=os.path.join(ROOT, 'stdlib'), DPL_RUNTIME=os.path.join(ROOT, 'runtime'),
                   DPL_BINDINGS=os.path.join(ROOT, 'bindings'))
    r = subprocess.run([dpl, 'build', '--release', '-o', exe, 'main.dpm'], cwd=d, capture_output=True, text=True,
                       env=env)
    if r.returncode != 0:
        sys.exit(f'{load}: сборка: {r.stderr.strip()}')
    ir = subprocess.run([dpl, 'debug', 'ir', 'main.dpm'], cwd=d, capture_output=True, text=True, env=env).stdout
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
    ap.add_argument('--before', default='', help='прежний компилятор: третий вариант')
    args = ap.parse_args()
    if not os.path.exists(DPL):
        sys.exit(f'нет {DPL}: соберите компилятор (python3 tests/run.py)')
    variants = ['plain', 'det'] + (['before'] if args.before else [])
    data = {'env': environment(), 'runs': args.runs, 'before': args.before, 'loads': {}}
    with tempfile.TemporaryDirectory() as tmp:
        for load in LOADS:
            built = [build(load, v, tmp, args.before if v == 'before' else DPL) for v in variants]
            times, outs = run([b[0] for b in built], args.runs)
            res = {}
            for i, v in enumerate(variants):
                res[v] = {'ms': times[i], 'median': statistics.median(times[i]), 'canon': built[i][1], 'out': outs[i]}
            res['same_output'] = all(o == outs[0] for o in outs)
            for v in variants[1:]:
                res['ratio' if v == 'det' else 'ratio_before'] = res[v]['median'] / res['plain']['median']
                res['ratio_min' if v == 'det' else 'ratio_min_before'] = min(res[v]['ms']) / min(res['plain']['ms'])
            data['loads'][load] = res
    env = data['env']
    lines = ['# Цена класса D: канонический NaN (Р56)', '',
             f'Машина: {env["cpu"]}; {env["os"]}. Компилятор C: {env["cc"]}. Коммит {env["commit"]}, '
             f'build/release/dpl, `dpl build --release`. Кругов: {args.runs}; в круге варианты идут по очереди, '
             'чтобы дрейф машины ложился на все; прогрев — один прогон.', '',
             'Те же нагрузки с `@det import` (канон NaN по Р56) и с обычным `import`.' +
             (f' Вариант «до правки» — `@det import` прежним компилятором `{args.before}`.' if args.before else ''), '',
             '| нагрузка | вариант | canon в IR | мс (медиана / мин. / макс.) | к варианту без класса D: по медиане | '
             'по минимуму | вывод совпал |',
             '|---|---|---|---|---|---|---|']
    names = {'plain': 'без класса D', 'det': 'класс D', 'before': 'класс D до правки'}
    for load, r in data['loads'].items():
        for v in variants:
            x = r[v]
            if v == 'plain':
                rel = rel_min = '—'
            else:
                k = '' if v == 'det' else '_before'
                rel, rel_min = f'{(r["ratio" + k] - 1) * 100:+.1f}%', f'{(r["ratio_min" + k] - 1) * 100:+.1f}%'
            lines.append(f'| {load} | {names[v]} | {x["canon"]} | {x["median"]:.1f} / {min(x["ms"]):.1f} / '
                         f'{max(x["ms"]):.1f} | {rel} | {rel_min} | {"да" if r["same_output"] else "нет"} |')
    lines += ['', 'Нагрузки: `terrain` — рельеф 1024 × 1024 по шуму значений, шесть октав (`bench/det/terrain.dpm`); '
              '`bodies` — 4096 тел, 400 шагов (`tests/run/det_sim/physics.dpm`).']
    report = '\n'.join(lines) + '\n'
    if args.save:
        d = os.path.join(ROOT, 'bench', 'results', args.save)
        os.makedirs(d, exist_ok=True)
        for r in data['loads'].values():
            for v in variants:
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
