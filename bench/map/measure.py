#!/usr/bin/env python3
"""Замер Map против std::map и std::unordered_map (О12: память и скорость;
docs/policy/BENCHMARKS.md).

    python3 bench/map/measure.py [--runs 11] [--n 200000] [--layouts 1,16,32,64] [--save NAME]

Нагрузка — bench/map/map.dpm и эталон bench/map/map.cpp (C++, -O2), ключи
i64 и str. Фазы накопительные: build — заполнение n ключей; lookup —
заполнение и 5 проходов поиска (чётные — ключи таблицы, нечётные —
отсутствующие); iter — заполнение и 20 обходов по возрастанию ключа
(у dpl — `for k in &m` и `m[k]`, у C++ — пары); churn — заполнение,
удаление половины, n/2 новых ключей. Цена фазы — разность медиан с build.
Вывод вариантов должен совпасть.

Каждый вариант собирается в нескольких раскладках кода (-falign-loops,
как bench/det/measure.py); берётся лучшая медиана. Память — пиковый RSS
процесса фазы build.
"""
import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import native  # noqa: E402  bench/native.py

ROOT = native.ROOT
HERE = os.path.join(ROOT, 'bench', 'map')
PHASES = ['build', 'lookup', 'iter', 'churn']
KEYS = ['int', 'str']
VARIANTS = {'dpl': 'Map (dpl)', 'map': 'std::map', 'umap': 'std::unordered_map'}


def build(variant, align, tmp):
    exe = os.path.join(tmp, f'{variant}-{align}')
    if variant == 'dpl':
        return native.build_dpl(os.path.join(HERE, 'map.dpm'), exe, tmp, align)
    return native.build_cpp(os.path.join(HERE, 'map.cpp'), exe, align, ['UNORDERED'] if variant == 'umap' else [])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--runs', type=int, default=11)
    ap.add_argument('--n', type=int, default=200000)
    ap.add_argument('--layouts', default='1,16,32,64', help='значения -falign-loops через запятую')
    ap.add_argument('--save', default='')
    args = ap.parse_args()
    native.need_dpl()
    layouts = [int(a) for a in args.layouts.split(',')]
    data = {'env': native.environment(), 'runs': args.runs, 'n': args.n, 'layouts': layouts, 'keys': {}}
    same = True
    with tempfile.TemporaryDirectory() as tmp:
        launch = native.launcher(tmp)
        exes = {(v, a): build(v, a, tmp) for v in VARIANTS for a in layouts}
        for key in KEYS:
            res = {v: {} for v in VARIANTS}
            for phase in PHASES:
                medians, rss, outs = native.sweep(launch, {c: [exes[c], key, phase, str(args.n)] for c in exes},
                                                  args.runs)
                same = same and len(set(outs.values())) == 1
                for v in VARIANTS:
                    m = {a: medians[(v, a)] for a in layouts}
                    res[v][phase] = {'best': min(m.values()), 'medians': m, 'rss_kb': max(rss[(v, a)] for a in layouts)}
            data['keys'][key] = res
    data['same_output'] = same
    report = summary(data)
    if args.save:
        d = os.path.join(ROOT, 'bench', 'results', args.save)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        with open(os.path.join(d, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(report)
    print(report, end='')
    if not same:
        sys.exit(1)


def summary(data):
    env, n = data['env'], data['n']
    lines = ['# Map против std::map и std::unordered_map (О12)', '',
             f'Машина: {env["cpu"]}; {env["os"]}. Компилятор C и C++: {env["cc"]}. Коммит {env["commit"]}; '
             f'dpl — `dpl build --release` (C с -O2), C++ — -O2. Ключей: {n}; кругов: {data["runs"]}; раскладки кода: '
             f'`-falign-loops` = {", ".join(map(str, data["layouts"]))}, у каждого варианта — лучшая медиана.', '',
             'Цена фазы — разность медиан с фазой build (заполнение). lookup — 5 проходов поиска по n ключей '
             '(половина — отсутствующие); iter — 20 обходов по возрастанию ключа; churn — удаление n/2 и n/2 новых. '
             'Память — пиковый RSS процесса build.', '',
             '| ключ | вариант | build, мс | lookup, мс | iter, мс | churn, мс | память, МБ |',
             '|---|---|---|---|---|---|---|']
    for key, res in data['keys'].items():
        for v, name in VARIANTS.items():
            r = res[v]
            b = r['build']['best']
            cells = [f'{b:.1f}'] + [f'{max(r[p]["best"] - b, 0):.1f}' for p in PHASES[1:]]
            lines.append(f'| {key} | {name} | {" | ".join(cells)} | {r["build"]["rss_kb"] / 1024:.1f} |')
    lines += ['', 'Отношение dpl к эталону (по цене фазы; build — по времени фазы):', '',
              '| ключ | к | build | lookup | iter | churn | память |', '|---|---|---|---|---|---|---|']
    for key, res in data['keys'].items():
        d = res['dpl']
        for v in ('map', 'umap'):
            r = res[v]
            cells = [f'{d["build"]["best"] / r["build"]["best"]:.2f}']
            for p in PHASES[1:]:
                x = max(d[p]['best'] - d['build']['best'], 0.0)
                y = max(r[p]['best'] - r['build']['best'], 0.0)
                cells.append(f'{x / y:.2f}' if y > 0.5 else '—')
            cells.append(f'{d["build"]["rss_kb"] / r["build"]["rss_kb"]:.2f}')
            lines.append(f'| {key} | {VARIANTS[v]} | {" | ".join(cells)} |')
    lines += ['', f'Вывод вариантов совпал: {"да" if data["same_output"] else "нет"}.']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    main()
