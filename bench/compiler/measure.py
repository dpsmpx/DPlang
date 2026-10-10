#!/usr/bin/env python3
"""Замеры компилятора v0.1 (ROADMAP/IMPLEMENTATION.md, шаг 13;
docs/policy/BENCHMARKS.md): время и память `check`, время сборки,
первая нагрузка, объём компилятора (бюджет ядра — Р-4).

    python3 bench/compiler/measure.py [--runs 10] [--save NAME]

Компилятор — сборка Release (build/release/dpl; её собирает
tests/run.py). --save — данные и отчёт в bench/results/NAME/
(measure.json, summary.md). Без --save — отчёт в stdout.
"""
import argparse
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DPL = os.path.join(ROOT, 'build', 'release', 'dpl')
EXAMPLE = os.path.join(ROOT, 'examples', 'tomlcheck')


# Помощник на C: запускает команду и пишет её rusage. Linux сохраняет
# maxrss через exec, поэтому у потомка самого Python в пик попал бы и
# размер Python; у маленького помощника — меньше мегабайта.
HELPER_C = r"""
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    pid_t p = fork();
    if (p == 0) {
        execvp(argv[2], argv + 2);
        _exit(127);
    }
    int st = 0;
    struct rusage ru;
    wait4(p, &st, 0, &ru);
    clock_gettime(CLOCK_MONOTONIC, &b);
    FILE* f = fopen(argv[1], "w");
    if (!f) return 2;
    double wall = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
    double cpu = (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    fprintf(f, "%d %.6f %.6f %ld\n", WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st), wall, cpu, ru.ru_maxrss);
    fclose(f);
    return 0;
}
"""
HELPER = []


def run(argv, cwd=None):
    """(код, стена с, CPU с, maxrss МБ) одного запуска. CPU и пик памяти —
    процесса и его потомков, которых он дождался (у build — и компилятора C)."""
    out = HELPER[0] + '.txt'
    subprocess.run([HELPER[0], out] + argv, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   stdin=subprocess.DEVNULL)
    with open(out, encoding='utf-8') as f:
        code, wall, cpu, rss = f.read().split()
    return int(code), float(wall), float(cpu), int(rss) / 1024


def measure(argv, runs, cwd=None):
    """Прогрев и runs прогонов: медиана, наименьшее и наибольшее (мс), память."""
    run(argv, cwd)
    walls, cpus, rss, codes = [], [], [], set()
    for _ in range(runs):
        code, w, c, m = run(argv, cwd)
        codes.add(code)
        walls.append(w * 1000)
        cpus.append(c * 1000)
        rss.append(m)
    return {'code': sorted(codes), 'wall_ms': statistics.median(walls), 'wall_min_ms': min(walls),
            'wall_max_ms': max(walls), 'cpu_ms': statistics.median(cpus), 'maxrss_mb': max(rss)}


def lines_of(path):
    with open(path, encoding='utf-8') as f:
        return sum(1 for _ in f)


# --- входы

def scaled_module(copies):
    """Модуль из copies копий toml.dpm (без тестов): у каждой копии имена
    верхнего уровня — со своим суффиксом. Живой код, а не синтетика."""
    with open(os.path.join(EXAMPLE, 'toml.dpm'), encoding='utf-8') as f:
        text = f.read().split('@test')[0]
    names = re.findall(r'^(?:pub )?(?:fn|enum|struct) (\w+)', text, re.M)
    out = []
    for k in range(copies):
        part = text
        for n in names:
            part = re.sub(r'\b' + n + r'\b', f'{n}_{k}', part)
        out.append(part)
    return '\n'.join(out) + '\nfn main() {\n    println("ok")\n}\n'


def big_toml(tables, keys):
    """Верный файл подмножества TOML: tables таблиц по keys пар."""
    lines = ['# сгенерировано bench/compiler/measure.py']
    for t in range(tables):
        lines.append(f'[table_{t}]')
        for k in range(keys):
            v = [f'{k * 7 - 3}', 'true', f'"value {t} {k}"'][k % 3]
            lines.append(f'key_{k} = {v}   # comment')
    return '\n'.join(lines) + '\n'


def environment():
    cpu = 'unknown'
    try:
        with open('/proc/cpuinfo', encoding='utf-8') as f:
            for line in f:
                if line.startswith('model name'):
                    cpu = line.split(':', 1)[1].strip()
                    break
    except OSError:
        pass
    cc = subprocess.run(['cc', '--version'], capture_output=True, text=True).stdout.splitlines()[:1]
    commit = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--short', 'HEAD'], capture_output=True,
                            text=True).stdout.strip()
    return {'cpu': cpu, 'cpus': os.cpu_count(), 'os': platform.platform(), 'cc': cc[0] if cc else '?',
            'commit': commit, 'compiler': 'build/release/dpl (Release)'}


def loc():
    """Строки кода без пустых: компилятор (без фаззера), runtime, библиотека."""
    def count(paths):
        n = 0
        for p in paths:
            with open(p, encoding='utf-8') as f:
                n += sum(1 for line in f if line.strip())
        return n

    def files(d, exts, skip=()):
        out = []
        for base, _, fs in os.walk(os.path.join(ROOT, d)):
            if any(s in base for s in skip):
                continue
            out += [os.path.join(base, f) for f in fs if f.endswith(exts)]
        return sorted(out)

    core = files('compiler', ('.cpp', '.h', '.def'), skip=('/fuzz',))
    service = [p for p in core if '/api/' in p or 'worker' in p or 'sandbox' in p]
    return {'compiler': count(core), 'compiler_service_part': count(service),
            'fuzzer': count(files('compiler/fuzz', ('.cpp',))), 'runtime': count(files('runtime', ('.c', '.h'))),
            'stdlib': count(files('stdlib', ('.dpm',)))}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--runs', type=int, default=10)
    ap.add_argument('--save')
    a = ap.parse_args()
    if not os.path.exists(DPL):
        print('нет build/release/dpl: сначала python3 tests/run.py')
        return 1
    data = {'env': environment(), 'runs': a.runs, 'check': [], 'scaling': [], 'build': [], 'load': [], 'loc': loc()}
    tmp = tempfile.mkdtemp(prefix='dpmeasure-')
    src = os.path.join(tmp, 'rusage.c')
    with open(src, 'w', encoding='utf-8') as f:
        f.write(HELPER_C)
    HELPER.append(os.path.join(tmp, 'rusage'))
    subprocess.run(['cc', '-O2', '-o', HELPER[0], src], check=True)

    # check: реальные программы — набор run и пример
    run_dir = os.path.join(ROOT, 'tests', 'run')
    progs = [('examples/tomlcheck', EXAMPLE, 'main.dpm',
              lines_of(os.path.join(EXAMPLE, 'main.dpm')) + lines_of(os.path.join(EXAMPLE, 'toml.dpm')))]
    for name in sorted(os.listdir(run_dir)):
        if name.endswith('.dpm'):
            progs.append(('tests/run/' + name, run_dir, name, lines_of(os.path.join(run_dir, name))))
    for label, cwd, name, n in progs:
        m = measure([DPL, 'check', name], a.runs, cwd)
        data['check'].append({'input': label, 'lines': n, **m})

    # check: рост размера модуля
    for copies in (1, 4, 16, 64):
        path = os.path.join(tmp, f'scaled{copies}.dpm')
        with open(path, 'w', encoding='utf-8') as f:
            f.write(scaled_module(copies))
        m = measure([DPL, 'check', os.path.basename(path)], a.runs, tmp)
        data['scaling'].append({'copies': copies, 'lines': lines_of(path), **m})

    # сборка примера: debug c (фронтенд и бэкенд) и полная сборка в профилях
    data['build'].append({'what': 'debug c', **measure([DPL, 'debug', 'c', 'main.dpm'], a.runs, EXAMPLE)})
    for prof in ('debug', 'release'):
        exe = os.path.join(tmp, 'tomlcheck_' + prof)
        argv = [DPL, 'build'] + (['--release'] if prof == 'release' else []) + ['-o', exe, 'main.dpm']
        data['build'].append({'what': f'build {prof}', **measure(argv, max(3, a.runs // 2), EXAMPLE)})

    # первая нагрузка: tomlcheck на большом файле — нативно (release) и интерпретатором
    big = os.path.join(tmp, 'big.toml')
    with open(big, 'w', encoding='utf-8') as f:
        f.write(big_toml(2000, 100))
    small = os.path.join(tmp, 'small.toml')
    with open(small, 'w', encoding='utf-8') as f:
        f.write(big_toml(200, 50))
    exe = os.path.join(tmp, 'tomlcheck_release')
    for label, argv, path in [('native release', [exe], big), ('native release', [exe], small),
                              ('interpreter (debug run, Release)', [DPL, 'debug', 'run', 'main.dpm'], small)]:
        m = measure(argv + [path], max(3, a.runs // 2), EXAMPLE)
        data['load'].append({'what': label, 'file_lines': lines_of(path), 'file_mb': os.path.getsize(path) / 2**20, **m})

    report = summary(data)
    if a.save:
        out = os.path.join(ROOT, 'bench', 'results', a.save)
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        with open(os.path.join(out, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(report)
        print(f'замеры: {os.path.relpath(out, ROOT)}/summary.md')
    else:
        print(report)
    return 0


def summary(d):
    e = d['env']
    out = ['# Замеры компилятора v0.1', '',
           f'Машина: {e["cpus"]} CPU, {e["cpu"]}; {e["os"]}. Компилятор C: {e["cc"]}. '
           f'Коммит {e["commit"]}, {e["compiler"]}. Прогонов: {d["runs"]} (медиана; прогрев — один прогон).', '']

    def row(r, first):
        return (f'| {first} | {r["wall_ms"]:.1f} / {r["wall_max_ms"]:.1f} | {r["cpu_ms"]:.1f} | '
                f'{r["maxrss_mb"]:.1f} | {",".join(map(str, r["code"]))} |')

    checks = d['check']
    total_lines = sum(c['lines'] for c in checks)
    total_ms = sum(c['wall_ms'] for c in checks)
    out += ['## `check` на реальных программах', '',
            f'{len(checks)} программ, {total_lines} строк: в сумме {total_ms:.0f} мс '
            f'({total_lines / total_ms * 1000:.0f} строк/с с запуском процесса).', '',
            '| программа | строк | стена, мс (медиана / макс.) | CPU, мс | maxrss, МБ | код |', '|---|---|---|---|---|---|']
    for c in sorted(checks, key=lambda c: -c['wall_ms'])[:8]:
        out.append(row(c, f'{c["input"]} | {c["lines"]}'))
    out += ['', 'Показаны 8 самых долгих; остальные — в `measure.json`.', '',
            '## `check` при росте модуля', '',
            'Модуль — копии `examples/tomlcheck/toml.dpm` без тестов со своими именами.', '',
            '| копий | строк | стена, мс (медиана / макс.) | CPU, мс | maxrss, МБ | код | мкс на строку |',
            '|---|---|---|---|---|---|---|']
    for s in d['scaling']:
        out.append(row(s, f'{s["copies"]} | {s["lines"]}') + f' {s["wall_ms"] * 1000 / s["lines"]:.1f} |')
    out += ['', '## Сборка `examples/tomlcheck`', '',
            '| что | стена, мс (медиана / макс.) | CPU, мс | maxrss, МБ | код |', '|---|---|---|---|---|']
    for b in d['build']:
        out.append(row(b, b['what']))
    out += ['', 'CPU и память `build` — вместе с компилятором C: он дочерний процесс `dpl`.', '',
            '## Первая нагрузка: `tomlcheck` на большом файле', '',
            '| исполнение | строк файла | МБ | стена, мс (медиана / макс.) | CPU, мс | maxrss, МБ | код |',
            '|---|---|---|---|---|---|---|']
    for l in d['load']:
        out.append(row(l, f'{l["what"]} | {l["file_lines"]} | {l["file_mb"]:.1f}'))
    n = d['loc']
    out += ['', '## Объём (Р-4: бюджет ядра — около 25 тыс. строк компилятора без тестов)', '',
            '| часть | непустых строк |', '|---|---|',
            f'| компилятор (`compiler/` без фаззера) | {n["compiler"]} |',
            f'| из него — онлайн-сервис (`api/`, worker, песочница) | {n["compiler_service_part"]} |',
            f'| фаззер | {n["fuzzer"]} |', f'| runtime | {n["runtime"]} |', f'| стандартная библиотека | {n["stdlib"]} |', '']
    return '\n'.join(out)


if __name__ == '__main__':
    sys.exit(main())
