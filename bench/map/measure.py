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
import platform
import shlex
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, 'bench', 'map')
DPL = os.path.join(ROOT, 'build', 'release', 'dpl')
PHASES = ['build', 'lookup', 'iter', 'churn']
KEYS = ['int', 'str']
VARIANTS = {'dpl': 'Map (dpl)', 'map': 'std::map', 'umap': 'std::unordered_map'}


def cc_wrapper(tmp, align):
    path = os.path.join(tmp, f'cc-align{align}')
    if not os.path.exists(path):
        with open(path, 'w', encoding='utf-8') as f:
            f.write(f'#!/bin/sh\nexec {shlex.quote(os.environ.get("CC") or "cc")} -falign-loops={align} "$@"\n')
        os.chmod(path, 0o755)
    return path


def build(variant, align, tmp):
    exe = os.path.join(tmp, f'{variant}-{align}')
    if variant == 'dpl':
        env = dict(os.environ, CC=cc_wrapper(tmp, align))
        cmd = [DPL, 'build', '--release', '-o', exe, os.path.join(HERE, 'map.dpm')]
    else:
        env = None
        cxx = os.environ.get('CXX') or 'c++'
        cmd = [cxx, '-std=c++17', '-O2', f'-falign-loops={align}', '-o', exe, os.path.join(HERE, 'map.cpp')]
        if variant == 'umap':
            cmd.insert(3, '-DUNORDERED')
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        sys.exit(f'{variant}: сборка: {r.stderr.strip()}')
    return exe


# Запускатель: пиковый RSS из wait4 включает память процесса до exec, и у
# потомка Python это память самого Python. Маленький процесс на C её не
# добавляет.
LAUNCHER = r"""
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char** argv) {
    pid_t p = fork();
    if (p == 0) { execv(argv[1], argv + 1); _exit(127); }
    int st; struct rusage u;
    if (wait4(p, &st, 0, &u) < 0) return 126;
    fprintf(stderr, "%ld\n", u.ru_maxrss);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 125;
}
"""


def launcher(tmp):
    src, exe = os.path.join(tmp, 'launch.c'), os.path.join(tmp, 'launch')
    with open(src, 'w', encoding='utf-8') as f:
        f.write(LAUNCHER)
    subprocess.run([os.environ.get('CC') or 'cc', '-O2', '-o', exe, src], check=True)
    return exe


def once(launch, argv):
    """Время (мс), пиковый RSS (КБ) и вывод одного прогона."""
    a = time.perf_counter()
    r = subprocess.run([launch] + argv, capture_output=True, text=True)
    ms = (time.perf_counter() - a) * 1000
    if r.returncode != 0:
        sys.exit(f'{" ".join(argv)}: код {r.returncode}')
    return ms, int(r.stderr.split()[-1]), r.stdout


def environment():
    cpu = '?'
    try:
        with open('/proc/cpuinfo', encoding='utf-8') as f:
            cpu = next((ln.split(':', 1)[1].strip() for ln in f if ln.startswith('model name')), '?')
    except OSError:
        pass
    cc = subprocess.run(['cc', '--version'], capture_output=True, text=True).stdout.splitlines()
    commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True, text=True)
    return {'cpu': f'{os.cpu_count()} CPU, {cpu}', 'os': platform.platform(), 'cc': cc[0] if cc else '?',
            'commit': commit.stdout.strip() or '?'}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--runs', type=int, default=11)
    ap.add_argument('--n', type=int, default=200000)
    ap.add_argument('--layouts', default='1,16,32,64', help='значения -falign-loops через запятую')
    ap.add_argument('--save', default='')
    args = ap.parse_args()
    if not os.path.exists(DPL):
        sys.exit(f'нет {DPL}: соберите компилятор (python3 tests/run.py)')
    layouts = [int(a) for a in args.layouts.split(',')]
    data = {'env': environment(), 'runs': args.runs, 'n': args.n, 'layouts': layouts, 'keys': {}}
    same = True
    with tempfile.TemporaryDirectory() as tmp:
        launch = launcher(tmp)
        exes = {(v, a): build(v, a, tmp) for v in VARIANTS for a in layouts}
        for key in KEYS:
            res = {v: {} for v in VARIANTS}
            for phase in PHASES:
                cells = list(exes)
                times = {c: [] for c in cells}
                rss = {}
                outs = {c: once(launch, [exes[c], key, phase, str(args.n)])[2] for c in cells}  # прогрев
                same = same and len(set(outs.values())) == 1
                for _ in range(args.runs):
                    for c in cells:  # в круге — все сборки по очереди: дрейф ложится на все
                        ms, kb, _ = once(launch, [exes[c], key, phase, str(args.n)])
                        times[c].append(ms)
                        rss[c] = max(rss.get(c, 0), kb)
                for v in VARIANTS:
                    medians = {a: statistics.median(times[(v, a)]) for a in layouts}
                    res[v][phase] = {'best': min(medians.values()), 'medians': medians,
                                     'rss_kb': max(rss[(v, a)] for a in layouts)}
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
