#!/usr/bin/env python3
"""Замер М2: обход 100 тыс. сущностей по компонентам (Р67; заметка 9,
раздел 5; docs/policy/BENCHMARKS.md, раздел 2).

    python3 bench/ecs/measure.py [--runs 11] [--n 100000] [--frames 100] [--layouts 1,16,32,64] [--save NAME]

Варианты: pool — встроенный Pool[T] (bench/ecs/pool.dpm); manual — пул
вручную на List со списком свободных номеров (bench/ecs/manual.dpm);
cpp — slot map на std::vector (bench/ecs/ecs.cpp, -O2). Кадр — движение
живых, удар по цели через хэндл, смерть и рождение. Цена кадра —
(время с кадрами − время без кадров) / кадров. Вывод вариантов должен
совпасть. Раскладки кода, чередование прогонов и пиковый RSS — как у
bench/native.py.
"""
import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import native  # noqa: E402  bench/native.py

HERE = os.path.join(native.ROOT, 'bench', 'ecs')
VARIANTS = {'pool': 'Pool (dpl)', 'manual': 'пул вручную на List (dpl)', 'cpp': 'slot map (C++)'}


def build(v, align, tmp):
    exe = os.path.join(tmp, f'{v}-{align}')
    if v == 'cpp':
        return native.build_cpp(os.path.join(HERE, 'ecs.cpp'), exe, align)
    return native.build_dpl(os.path.join(HERE, v + '.dpm'), exe, tmp, align)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--runs', type=int, default=11)
    ap.add_argument('--n', type=int, default=100000)
    ap.add_argument('--frames', type=int, default=100)
    ap.add_argument('--layouts', default='1,16,32,64', help='значения -falign-loops через запятую')
    ap.add_argument('--save', default='')
    args = ap.parse_args()
    native.need_dpl()
    layouts = [int(a) for a in args.layouts.split(',')]
    data = {'env': native.environment(), 'runs': args.runs, 'n': args.n, 'frames': args.frames, 'layouts': layouts,
            'variants': {}}
    with tempfile.TemporaryDirectory() as tmp:
        launch = native.launcher(tmp)
        exes = {(v, a): build(v, a, tmp) for v in VARIANTS for a in layouts}
        cells = {(v, a, f): [exes[(v, a)], str(args.n), str(f)] for (v, a) in exes for f in (0, args.frames)}
        medians, rss, outs = native.sweep(launch, cells, args.runs)
    data['same_output'] = len({outs[c] for c in cells if c[2] == args.frames}) == 1
    for v in VARIANTS:
        frame = {a: (medians[(v, a, args.frames)] - medians[(v, a, 0)]) / args.frames for a in layouts}
        setup = {a: medians[(v, a, 0)] for a in layouts}
        data['variants'][v] = {'frame_ms': min(frame.values()), 'frame_by_layout': frame,
                               'setup_ms': min(setup.values()), 'rss_kb': max(rss[(v, a, args.frames)] for a in layouts)}
    report = summary(data)
    if args.save:
        d = os.path.join(native.ROOT, 'bench', 'results', args.save)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        with open(os.path.join(d, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(report)
    print(report, end='')
    if not data['same_output']:
        sys.exit(1)


def summary(data):
    env, res = data['env'], data['variants']
    cpp = res['cpp']
    lines = ['# М2: обход сущностей по компонентам — Pool против ручного пула и C++', '',
             f'Машина: {env["cpu"]}; {env["os"]}. Компилятор C и C++: {env["cc"]}. Коммит {env["commit"]}; '
             f'dpl — `dpl build --release` (C с -O2), C++ — -O2. Сущностей: {data["n"]}; кадров: {data["frames"]}; '
             f'кругов: {data["runs"]}; раскладки кода: `-falign-loops` = {", ".join(map(str, data["layouts"]))}, '
             'у каждого варианта — лучшая.', '',
             'Кадр: движение живых, удар по цели через хэндл (проверка поколения), смерть при hp ≤ 0 и рождение '
             'новой сущности с целью из списка живых. Цена кадра — (время с кадрами − время без кадров) / кадров; '
             'заполнение — время без кадров. Память — пиковый RSS прогона с кадрами.', '',
             '| вариант | кадр, мс | к C++ | заполнение, мс | память, МБ |', '|---|---|---|---|---|']
    for v, name in VARIANTS.items():
        r = res[v]
        lines.append(f'| {name} | {r["frame_ms"]:.2f} | {r["frame_ms"] / cpp["frame_ms"]:.2f} | {r["setup_ms"]:.1f} | '
                     f'{r["rss_kb"] / 1024:.1f} |')
    lines += ['', f'Вывод вариантов совпал: {"да" if data["same_output"] else "нет"}.']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    main()
