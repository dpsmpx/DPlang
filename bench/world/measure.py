#!/usr/bin/env python3
"""Замер «большие данные»: генератор мира, кодирование в байты, чтение
обратно и запись файла (заметка docs/research/notes/11_big_data_io.md;
docs/policy/BENCHMARKS.md, раздел 3).

    python3 bench/world/measure.py [--runs 5] [--side 128] [--big 300] [--threads 1,4] [--save NAME]

Программа (bench/world/world.dpm, эталон bench/world/world.cpp) строит мир
из side × side чанков по 64 × 64 клетки. Режимы накопительные — разность
соседних даёт цену шага: g — генерация, ge — ещё кодирование в List[u8],
encode — ещё хэш байтов, roundtrip — ещё чтение байтов обратно и сверка.
У C++ ещё запись файла: write — по чанку, write1 — склейка в один буфер
и одна запись (как у API «файл целиком»), readback — генерация, чтение
файла, сверка и хэш. Вывод dpl и C++ должен совпасть.

Размер side — все режимы, runs кругов. Размер big (около гигабайта
данных) — один прогон, только при числе потоков из --threads побольше.
"""
import argparse
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import native  # noqa: E402  bench/native.py

HERE = os.path.join(native.ROOT, 'bench', 'world')
MODES = ('g', 'ge', 'encode', 'roundtrip')
FILE_MODES = ('write', 'write1', 'readback')
ENV = shutil.which('env') or '/usr/bin/env'


def cells_for(exes, side, threads, tmp, file_modes):
    cells = {}
    for th in threads:
        for lang, exe in exes.items():
            for m in MODES:
                cells[(lang, m, th, side)] = [ENV, f'DPL_THREADS={th}', exe, m, str(side), '20261031']
        if file_modes:
            path = os.path.join(tmp, f'world-{side}-{th}.bin')
            for m in FILE_MODES:
                cells[('cpp', m, th, side)] = [ENV, f'DPL_THREADS={th}', exes['cpp'], m, str(side), '20261031', path]
    return cells


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--side', type=int, default=128)
    ap.add_argument('--big', type=int, default=300)
    ap.add_argument('--threads', default='1,4')
    ap.add_argument('--align', type=int, default=32, help='-falign-loops у обеих сборок')
    ap.add_argument('--save', default='')
    args = ap.parse_args()
    native.need_dpl()
    threads = [int(t) for t in args.threads.split(',')]
    data = {'env': native.environment(), 'runs': args.runs, 'side': args.side, 'big': args.big, 'threads': threads,
            'cells': {}}
    with tempfile.TemporaryDirectory() as tmp:
        launch = native.launcher(tmp)
        exes = {'dpl': native.build_dpl(os.path.join(HERE, 'world.dpm'), os.path.join(tmp, 'world-dpl'), tmp,
                                        args.align),
                'cpp': native.build_cpp(os.path.join(HERE, 'world.cpp'), os.path.join(tmp, 'world-cpp'), args.align)}
        # write идёт до readback: порядок ключей в круге — порядок вставки.
        cells = cells_for(exes, args.side, threads, tmp, True)
        medians, rss, outs = native.sweep(launch, cells, args.runs)
        big = cells_for(exes, args.big, [max(threads)], tmp, True) if args.big else {}
        bmed, brss, bouts = native.sweep(launch, big, 1) if big else ({}, {}, {})
    medians.update(bmed)
    rss.update(brss)
    outs.update(bouts)
    # Без имени режима: g и ge сверяются с C++ того же режима, остальные —
    # с encode у C++ (те же байты и хэш).
    same = True
    for (lang, m, th, side), out in outs.items():
        ref = outs[('cpp', m if m in ('g', 'ge') else 'encode', th, side)]
        same = same and out.split(' ', 1)[-1] == ref.split(' ', 1)[-1]
        data['cells']['/'.join(map(str, (lang, m, th, side)))] = {'ms': medians[(lang, m, th, side)],
                                                                  'rss_kb': rss[(lang, m, th, side)],
                                                                  'out': out.strip()}
    data['same_output'] = same
    report = summary(data)
    if args.save:
        d = os.path.join(native.ROOT, 'bench', 'results', args.save)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        with open(os.path.join(d, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(report)
    print(report, end='')
    if not same:
        sys.exit(1)


def field(out, name):
    for part in out.split():
        if part.startswith(name + '='):
            return part.split('=', 1)[1]
    return '?'


def summary(data):
    env, cells = data['env'], data['cells']

    def get(lang, m, th, side):
        return cells.get(f'{lang}/{m}/{th}/{side}')

    lines = ['# Большие данные: генератор мира, кодирование, чтение, запись', '',
             f'Машина: {env["cpu"]}; {env["os"]}. Компилятор C и C++: {env["cc"]}. Коммит {env["commit"]}; '
             'dpl — `dpl build --release` (C с -O2), C++ — -O2. Чанк — 64 × 64 клетки. '
             f'Кругов: {data["runs"]} (большой размер — один прогон).', '']
    sides = [data['side']] + ([data['big']] if data['big'] else [])
    for side in sides:
        ths = data['threads'] if side == data['side'] else [max(data['threads'])]
        any_cell = get('cpp', 'encode', ths[0], side)
        if not any_cell:
            continue
        mb = int(field(any_cell['out'], 'bytes')) / 2**20
        lines += [f'## {side} × {side} чанков: {side * side} чанков, {mb:.0f} МБ данных', '',
                  '| шаг | потоков | dpl, мс | C++, мс | dpl / C++ |', '|---|---|---|---|---|']
        for th in ths:
            prev = {'dpl': 0.0, 'cpp': 0.0}
            for m, name in (('g', 'генерация'), ('ge', 'кодирование'), ('encode', 'хэш байтов'),
                            ('roundtrip', 'чтение и сверка')):
                d, c = get('dpl', m, th, side)['ms'], get('cpp', m, th, side)['ms']
                sd, sc = d - prev['dpl'], c - prev['cpp']
                prev = {'dpl': d, 'cpp': c}
                ratio = f'{sd / sc:.2f}' if sc > 1 else '—'
                lines.append(f'| {name} | {th} | {sd:.0f} | {sc:.0f} | {ratio} |')
            lines.append(f'| всего (roundtrip) | {th} | {prev["dpl"]:.0f} | {prev["cpp"]:.0f} | '
                         f'{prev["dpl"] / prev["cpp"]:.2f} |')
        lines += ['', '| программа | потоков | мс | память, МБ |', '|---|---|---|---|']
        for th in ths:
            for lang, m in (('dpl', 'encode'), ('cpp', 'encode'), ('cpp', 'write'), ('cpp', 'write1'),
                            ('cpp', 'readback')):
                c = get(lang, m, th, side)
                if c:
                    lines.append(f'| {lang} {m} | {th} | {c["ms"]:.0f} | {c["rss_kb"] / 1024:.0f} |')
        enc, wr, w1 = get('cpp', 'encode', ths[-1], side), get('cpp', 'write', ths[-1], side), get(
            'cpp', 'write1', ths[-1], side)
        if enc and wr and w1:
            lines += ['', f'Запись файла (C++, {ths[-1]} потока): по чанку — {wr["ms"] - enc["ms"]:.0f} мс '
                          f'({mb / max(wr["ms"] - enc["ms"], 1) * 1000:.0f} МБ/с), одним буфером — '
                          f'{w1["ms"] - enc["ms"]:.0f} мс; пиковая память одним буфером — '
                          f'{w1["rss_kb"] / 1024:.0f} МБ против {wr["rss_kb"] / 1024:.0f} МБ.', '']
    lines += [f'Вывод dpl и C++ совпал: {"да" if data["same_output"] else "нет"}.']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    main()
