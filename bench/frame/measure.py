#!/usr/bin/env python3
"""Замер М3: сценарий кадровых аллокаций (Р68; заметка 9, раздел 5;
docs/policy/BENCHMARKS.md, раздел 2).

    python3 bench/frame/measure.py [--runs 11] [--n 10000] [--frames 1000] [--layouts 1,16,32,64] [--save NAME]

Кадр (bench/frame/frame.dpm, эталон bench/frame/frame.cpp): видимые
объекты, сетка клеток (Map и std::unordered_map) и пары «клетка,
объект». Варианты:
- dpl fresh — временные данные заново в каждом кадре;
- dpl reuse — живут между кадрами, clear();
- C++ fresh и reuse — то же на std::vector и std::unordered_map;
- C++ arena — std::pmr::monotonic_buffer_resource со сбросом в кадре.

Выделения — счётчик runtime dpl и замена operator new в C++, переменная
DPL_ALLOC_STATS; число выделений от раскладки кода не зависит. Вывод
вариантов должен совпасть.
"""
import argparse
import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import native  # noqa: E402  bench/native.py

HERE = os.path.join(native.ROOT, 'bench', 'frame')
VARIANTS = {('dpl', 'fresh'): 'dpl: заново в кадре', ('dpl', 'reuse'): 'dpl: clear() (Р68)',
            ('cpp', 'fresh'): 'C++: заново в кадре', ('cpp', 'reuse'): 'C++: clear()',
            ('cpp', 'arena'): 'C++: арена std::pmr'}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--runs', type=int, default=11)
    ap.add_argument('--n', type=int, default=10000)
    ap.add_argument('--frames', type=int, default=1000)
    ap.add_argument('--layouts', default='1,16,32,64', help='значения -falign-loops через запятую')
    ap.add_argument('--save', default='')
    args = ap.parse_args()
    native.need_dpl()
    layouts = [int(a) for a in args.layouts.split(',')]
    data = {'env': native.environment(), 'runs': args.runs, 'n': args.n, 'frames': args.frames, 'layouts': layouts,
            'variants': {}}
    with tempfile.TemporaryDirectory() as tmp:
        launch = native.launcher(tmp)
        exes = {}
        for lang in ('dpl', 'cpp'):
            for a in layouts:
                exe = os.path.join(tmp, f'{lang}-{a}')
                exes[(lang, a)] = (native.build_dpl(os.path.join(HERE, 'frame.dpm'), exe, tmp, a) if lang == 'dpl'
                                   else native.build_cpp(os.path.join(HERE, 'frame.cpp'), exe, a))
        cells = {(lang, mode, a): [exes[(lang, a)], mode, str(args.n), str(args.frames)]
                 for (lang, mode) in VARIANTS for a in layouts}
        medians, rss, outs = native.sweep(launch, cells, args.runs)
        allocs = {}
        for (lang, mode) in VARIANTS:
            _, _, _, err = native.once(launch, cells[(lang, mode, layouts[0])], env={'DPL_ALLOC_STATS': '1'})
            m = re.search(r'(\d+) allocations, (\d+) bytes', err)
            allocs[(lang, mode)] = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
    data['same_output'] = len(set(outs.values())) == 1
    for (lang, mode) in VARIANTS:
        by = {a: medians[(lang, mode, a)] for a in layouts}
        n, b = allocs[(lang, mode)]
        data['variants'][f'{lang}-{mode}'] = {'ms': min(by.values()), 'by_layout': by, 'allocations': n, 'bytes': b,
                                             'rss_kb': max(rss[(lang, mode, a)] for a in layouts)}
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
    env, res, frames = data['env'], data['variants'], data['frames']
    arena = res['cpp-arena']['ms']
    lines = ['# М3: кадровые выделения — clear() против заново и арены C++', '',
             f'Машина: {env["cpu"]}; {env["os"]}. Компилятор C и C++: {env["cc"]}. Коммит {env["commit"]}; '
             f'dpl — `dpl build --release` (C с -O2), C++ — -O2. Объектов: {data["n"]}; кадров: {frames}; '
             f'кругов: {data["runs"]}; раскладки кода: `-falign-loops` = {", ".join(map(str, data["layouts"]))}, '
             'у каждого варианта — лучшая медиана.', '',
             'Кадр: видимые объекты (около 20%), сетка клеток с числом объектов и пары «клетка, объект». Выделения — '
             'за весь прогон, включая заполнение входа; на кадр — их доля.', '',
             '| вариант | время, мс | мс на кадр | к арене C++ | выделений | выделений на кадр | МБ выделено | '
             'память, МБ |', '|---|---|---|---|---|---|---|---|']
    for (lang, mode), name in VARIANTS.items():
        r = res[f'{lang}-{mode}']
        lines.append(f'| {name} | {r["ms"]:.1f} | {r["ms"] / frames:.3f} | {r["ms"] / arena:.2f} | {r["allocations"]} | '
                     f'{r["allocations"] / frames:.1f} | {r["bytes"] / 2**20:.1f} | {r["rss_kb"] / 1024:.1f} |')
    lines += ['', f'Вывод вариантов совпал: {"да" if data["same_output"] else "нет"}.']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    main()
