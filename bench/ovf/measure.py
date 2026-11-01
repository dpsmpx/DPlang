#!/usr/bin/env python3
"""Цена проверки переполнения целых (trap.int): прежний runtime против
нынешнего (TODO: «Проверка переполнения целых дорога в горячем цикле»;
docs/policy/BENCHMARKS.md, «цена гарантий»).

    python3 bench/ovf/measure.py [--before origin/main] [--runs 9] [--layouts 1,16,32,64] [--save NAME]

Варианты одной программы — тем же компилятором (build/release/dpl):
- before — runtime из коммита --before (git archive): проверки
  сравнениями до операции, у умножения — с делением;
- after — runtime этого дерева: __builtin_*_overflow gcc и clang;
- wrap — только у М2: цикл движения на `+%` и `*%` (без проверки) и
  runtime этого дерева — нижняя граница.

Нагрузки:
- М2 (bench/ecs/pool.dpm): цена кадра — (время с кадрами − время без
  кадров) / кадров;
- рельеф по шуму и симуляция тел (как в bench/det/measure.py);
- генерация мира (bench/world/world.dpm, режим g, DPL_THREADS=1).

Сборка `--release` хоста в нескольких раскладках кода (--layouts:
-falign-loops), у варианта — лучшая; прогоны чередуются. Вывод вариантов
должен совпасть. --save — данные и отчёт в bench/results/NAME/.
"""
import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import native  # noqa: E402  bench/native.py

ROOT = native.ROOT
ENV = shutil.which('env') or '/usr/bin/env'
N, FRAMES = 100000, 100
WORLD_SIDE = 24

# Нагрузка: файлы (модуль → путь), тело main (None — main есть в main.dpm).
LOADS = {
    'ecs': ({'main': os.path.join(ROOT, 'bench', 'ecs', 'pool.dpm')}, None),
    'terrain': ({'terrain': os.path.join(ROOT, 'bench', 'det', 'terrain.dpm')},
                '    println(str(terrain.total(20261018, 1024)))\n'),
    'bodies': ({'physics': os.path.join(ROOT, 'tests', 'run', 'det_sim', 'physics.dpm')},
               '    var w = physics.new_world(20261010, 4096)\n'
               '    for i in 0..400 {\n'
               '        physics.step(&mut w, 0.01)\n'
               '    }\n'
               '    println(physics.report(&w))\n'),
    'world': ({'main': os.path.join(ROOT, 'bench', 'world', 'world.dpm')}, None),
}
LOAD_NAMES = {'ecs': f'М2: кадр, {N // 1000} тыс. сущностей', 'terrain': 'рельеф по шуму',
              'bodies': 'симуляция тел', 'world': f'генерация мира, {WORLD_SIDE} × {WORLD_SIDE} чанков'}
# Цикл движения М2 без проверки переполнения: нижняя граница.
WRAP_ECS = [('b.x += b.vx', 'b.x = b.x +% b.vx'), ('b.x += 2 * b.vx', 'b.x = b.x +% 2 *% b.vx'),
            ('b.y += b.vy', 'b.y = b.y +% b.vy'), ('b.y += 2 * b.vy', 'b.y = b.y +% 2 *% b.vy')]


def runtime_at(ref, tmp):
    """Каталог runtime/ коммита ref."""
    data = subprocess.run(['git', 'archive', ref, 'runtime'], cwd=ROOT, capture_output=True, check=True).stdout
    d = os.path.join(tmp, 'before')
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(d)
    return os.path.join(d, 'runtime')


def build(load, variant, align, runtime, tmp):
    files, body = LOADS[load]
    d = os.path.join(tmp, f'{load}-{variant}-{align}')
    os.makedirs(d)
    for mod, src in files.items():
        with open(src, encoding='utf-8') as f:
            text = f.read()
        if variant == 'wrap':
            for old, new in WRAP_ECS:
                if old not in text:
                    sys.exit(f'{src}: нет строки «{old}» для варианта wrap')
                text = text.replace(old, new)
        with open(os.path.join(d, mod + '.dpm'), 'w', encoding='utf-8') as f:
            f.write(text)
    if body is not None:
        mod = next(iter(files))
        with open(os.path.join(d, 'main.dpm'), 'w', encoding='utf-8') as f:
            f.write(f'import {mod}\n\nfn main() {{\n{body}}}\n')
    exe = os.path.join(d, 'main.bin')
    env = dict(os.environ, CC=native.cc_wrapper(tmp, align), DPL_RUNTIME=runtime)
    r = subprocess.run([native.DPL, 'build', '--release', '-o', exe, 'main.dpm'], cwd=d, capture_output=True,
                       text=True, env=env)
    if r.returncode != 0:
        sys.exit(f'{load} {variant}: сборка: {r.stderr.strip()}')
    return exe


def argv_of(load, exe, frames=FRAMES):
    if load == 'ecs':
        return [exe, str(N), str(frames)]
    if load == 'world':
        return [ENV, 'DPL_THREADS=1', exe, 'g', str(WORLD_SIDE), '20261031']
    return [exe]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--before', default='origin/main')
    ap.add_argument('--runs', type=int, default=9)
    ap.add_argument('--layouts', default='1,16,32,64', help='значения -falign-loops через запятую')
    ap.add_argument('--loads', default=','.join(LOADS))
    ap.add_argument('--save', default='')
    args = ap.parse_args()
    native.need_dpl()
    layouts = [int(a) for a in args.layouts.split(',')]
    before = subprocess.run(['git', 'rev-parse', '--short', args.before], cwd=ROOT, capture_output=True,
                            text=True).stdout.strip() or args.before
    env = native.environment()
    cc = subprocess.run([os.environ.get('CC') or 'cc', '--version'], capture_output=True, text=True).stdout.splitlines()
    env['cc'] = cc[0] if cc else '?'  # компилятор C сборок — $CC, а не cc
    data = {'env': env, 'before': before, 'runs': args.runs, 'layouts': layouts, 'loads': {},
            'same_output': True}
    with tempfile.TemporaryDirectory() as tmp:
        runtimes = {'before': runtime_at(args.before, tmp), 'after': os.path.join(ROOT, 'runtime')}
        runtimes['wrap'] = runtimes['after']
        launch = native.launcher(tmp)
        for load in args.loads.split(','):
            variants = ('before', 'after', 'wrap') if load == 'ecs' else ('before', 'after')
            exes = {(v, a): build(load, v, a, runtimes[v], tmp) for v in variants for a in layouts}
            cells = {(v, a, 'run'): argv_of(load, e) for (v, a), e in exes.items()}
            if load == 'ecs':
                cells.update({(v, a, 'setup'): argv_of(load, e, 0) for (v, a), e in exes.items()})
            medians, _, outs = native.sweep(launch, cells, args.runs)
            if len({outs[c] for c in cells if c[2] == 'run'}) != 1:
                data['same_output'] = False
            res = {}
            for v in variants:
                if load == 'ecs':
                    per = {a: (medians[(v, a, 'run')] - medians[(v, a, 'setup')]) / FRAMES for a in layouts}
                else:
                    per = {a: medians[(v, a, 'run')] for a in layouts}
                res[v] = {'ms': min(per.values()), 'by_layout': {str(a): x for a, x in per.items()}}
            data['loads'][load] = res
            print(f'{load}: ' + ', '.join(f'{v} {r["ms"]:.2f}' for v, r in res.items()), file=sys.stderr)
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


def summary(data):
    env = data['env']
    lines = ['# Цена проверки переполнения целых: прежний runtime против __builtin_*_overflow', '',
             f'Машина: {env["cpu"]}; {env["os"]}. Компилятор C: {env["cc"]}. Коммит {env["commit"]}, прежний runtime — '
             f'{data["before"]}; `dpl build --release` (C с -O2); медиана {data["runs"]} кругов, прогоны чередуются; '
             f'раскладки кода `-falign-loops` = {", ".join(map(str, data["layouts"]))}, у варианта — лучшая.', '',
             'before — проверки сравнениями до операции (у умножения — с делением), after — `__builtin_*_overflow`, '
             'wrap — цикл движения М2 на `+%` и `*%`, без проверки.', '',
             '| нагрузка | before, мс | after, мс | after к before | wrap, мс |', '|---|---|---|---|---|']
    for load, r in data['loads'].items():
        b, a = r['before']['ms'], r['after']['ms']
        wrap = f'{r["wrap"]["ms"]:.2f}' if 'wrap' in r else '—'
        lines.append(f'| {LOAD_NAMES[load]} | {b:.2f} | {a:.2f} | {(a / b - 1) * 100:+.0f}% | {wrap} |')
    lines += ['', f'Вывод вариантов совпал: {"да" if data["same_output"] else "нет"}.']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    main()
