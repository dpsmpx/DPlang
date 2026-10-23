#!/usr/bin/env python3
"""Цена класса D (Р56: канонический NaN там, где его видно;
docs/policy/BENCHMARKS.md, «цена гарантий»): те же нагрузки с
`@det import` и с обычным `import`, сборка release.

    python3 bench/det/measure.py [--runs 21] [--layouts 1,16,32,64] [--save NAME] [--before DPL]

Нагрузки — рельеф по шуму (bench/det/terrain.dpm) и симуляция тел
(tests/run/det_sim/physics.dpm) в большем масштабе. Компилятор — сборка
Release (build/release/dpl; её собирает tests/run.py). Вывод вариантов
должен совпасть: канон меняет только биты NaN. --save — данные и отчёт в
bench/results/NAME/ (measure.json, summary.md). --before — третий
вариант: `@det import` прежним компилятором DPL (его stdlib и runtime —
из этого репозитория); прогоны чередуются со всеми.

Раскладка кода: время симуляции тел двувершинно (около 38 или 62 мс на
машине замера 23 октября 2026) в зависимости только от выравнивания
циклов — у любого варианта, и с классом D, и без. Поэтому каждый вариант собирается в нескольких
раскладках (--layouts: -falign-loops компилятора C через обёртку CC), и
цена считается по лучшей раскладке каждого варианта.
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


def compiler_c(tmp, align):
    """Обёртка компилятора C: тот же CC с -falign-loops=align."""
    path = os.path.join(tmp, f'cc-align{align}')
    if not os.path.exists(path):
        with open(path, 'w', encoding='utf-8') as f:
            f.write(f'#!/bin/sh\nexec {shlex.quote(os.environ.get("CC") or "cc")} -falign-loops={align} "$@"\n')
        os.chmod(path, 0o755)
    return path


def build(load, variant, align, tmp, dpl=DPL):
    """Программа нагрузки в варианте variant (plain, det, before) компилятором
    dpl, циклы выровнены по align; путь к исполняемому и число canon в IR:
    отдельных (`= canon copy`) и при записи (`= canon(...)`)."""
    src, mod, body = LOADS[load]
    d = os.path.join(tmp, f'{load}-{variant}-{align}')
    os.makedirs(d)
    shutil.copy(src, os.path.join(d, mod + '.dpm'))
    with open(os.path.join(d, 'main.dpm'), 'w', encoding='utf-8') as f:
        f.write(('' if variant == 'plain' else '@det ') + f'import {mod}\n\nfn main() {{\n{body}}}\n')
    exe = os.path.join(d, 'main.bin')
    env = dict(os.environ, CC=compiler_c(tmp, align))
    if dpl != DPL:  # прежний компилятор вне репозитория: библиотеки — отсюда
        env.update(DPL_STDLIB=os.path.join(ROOT, 'stdlib'), DPL_RUNTIME=os.path.join(ROOT, 'runtime'),
                   DPL_BINDINGS=os.path.join(ROOT, 'bindings'))
    r = subprocess.run([dpl, 'build', '--release', '-o', exe, 'main.dpm'], cwd=d, capture_output=True, text=True,
                       env=env)
    if r.returncode != 0:
        sys.exit(f'{load}: сборка: {r.stderr.strip()}')
    ir = subprocess.run([dpl, 'debug', 'ir', 'main.dpm'], cwd=d, capture_output=True, text=True, env=env).stdout
    return exe, sum(1 for line in ir.splitlines() if ' = canon' in line)


def run(exes, runs):
    """Прогрев, затем runs кругов: в каждом — по прогону каждой сборки по
    очереди (дрейф машины ложится на все). Времена (мс) и выводы."""
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
    return {'cpu': f'{os.cpu_count()} CPU, {cpu}', 'os': platform.platform(), 'cc': cc[0] if cc else '?',
            'commit': commit_of(ROOT)}


def commit_of(path):
    """Коммит дерева git, в котором лежит path; '?' — вне git."""
    r = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=path, capture_output=True, text=True)
    return r.stdout.strip() or '?'


def pct(ratio):
    return f'{(ratio - 1) * 100:+.1f}%'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--runs', type=int, default=21)
    ap.add_argument('--layouts', default='1,16,32,64', help='значения -falign-loops через запятую')
    ap.add_argument('--save', default='')
    ap.add_argument('--before', default='', help='прежний компилятор: третий вариант')
    args = ap.parse_args()
    if not os.path.exists(DPL):
        sys.exit(f'нет {DPL}: соберите компилятор (python3 tests/run.py)')
    layouts = [int(a) for a in args.layouts.split(',')]
    variants = ['plain', 'det'] + (['before'] if args.before else [])
    before = commit_of(os.path.dirname(os.path.abspath(args.before))) if args.before else ''
    data = {'env': environment(), 'runs': args.runs, 'layouts': layouts, 'before': before, 'loads': {}}
    with tempfile.TemporaryDirectory() as tmp:
        for load in LOADS:
            keys = [(v, a) for v in variants for a in layouts]
            built = [build(load, v, a, tmp, args.before if v == 'before' else DPL) for v, a in keys]
            times, outs = run([b[0] for b in built], args.runs)
            res = {v: {'canon': built[keys.index((v, layouts[0]))][1], 'layouts': {}} for v in variants}
            for i, (v, a) in enumerate(keys):
                res[v]['layouts'][str(a)] = {'ms': times[i], 'median': statistics.median(times[i])}
            for v in variants:
                medians = [x['median'] for x in res[v]['layouts'].values()]
                res[v]['best'] = min(medians)
                res[v]['worst'] = max(medians)
                res[v]['best_min'] = min(min(x['ms']) for x in res[v]['layouts'].values())
            res['same_output'] = all(o == outs[0] for o in outs)
            for v in variants[1:]:
                k = '' if v == 'det' else '_before'
                res['ratio' + k] = res[v]['best'] / res['plain']['best']
                res['ratio_min' + k] = res[v]['best_min'] / res['plain']['best_min']
            data['loads'][load] = res
    env = data['env']
    lines = ['# Цена класса D: канонический NaN (Р56)', '',
             f'Машина: {env["cpu"]}; {env["os"]}. Компилятор C: {env["cc"]}. Коммит {env["commit"]}, '
             f'build/release/dpl, `dpl build --release`. Раскладки кода: `-falign-loops` = '
             f'{", ".join(map(str, layouts))}; кругов: {args.runs}; в круге все сборки идут по очереди, чтобы '
             'дрейф машины ложился на все; прогрев — один прогон.', '',
             'Те же нагрузки с `@det import` (канон NaN по Р56) и с обычным `import`.' +
             (f' Вариант «до правки» — `@det import` прежним компилятором (коммит {before}).' if args.before else ''),
             '', 'Время нагрузки зависит и от раскладки кода: у одной и той же программы медиана меняется от '
             'выравнивания циклов, у любого варианта. Цена — по лучшей раскладке каждого варианта; столбец '
             '«медианы раскладок» показывает разброс.', '',
             '| нагрузка | вариант | canon в IR | мс: лучшая медиана / минимум | медианы раскладок, мс | '
             'к варианту без класса D: по медиане | по минимуму | вывод совпал |',
             '|---|---|---|---|---|---|---|---|']
    names = {'plain': 'без класса D', 'det': 'класс D', 'before': 'класс D до правки'}
    for load, r in data['loads'].items():
        for v in variants:
            x = r[v]
            if v == 'plain':
                rel = rel_min = '—'
            else:
                k = '' if v == 'det' else '_before'
                rel, rel_min = pct(r['ratio' + k]), pct(r['ratio_min' + k])
            spread = ' / '.join(f'{x["layouts"][str(a)]["median"]:.1f}' for a in layouts)
            lines.append(f'| {load} | {names[v]} | {x["canon"]} | {x["best"]:.1f} / {x["best_min"]:.1f} | {spread} | '
                         f'{rel} | {rel_min} | {"да" if r["same_output"] else "нет"} |')
    lines += ['', 'Нагрузки: `terrain` — рельеф 1024 × 1024 по шуму значений, шесть октав (`bench/det/terrain.dpm`); '
              '`bodies` — 4096 тел, 400 шагов (`tests/run/det_sim/physics.dpm`).']
    report = '\n'.join(lines) + '\n'
    if args.save:
        d = os.path.join(ROOT, 'bench', 'results', args.save)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        with open(os.path.join(d, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(report)
    print(report, end='')
    if not all(r['same_output'] for r in data['loads'].values()):
        sys.exit(1)


if __name__ == '__main__':
    main()
