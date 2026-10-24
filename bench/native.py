"""Общее для замеров нативных программ против C++ (bench/map, bench/ecs,
bench/frame; docs/policy/BENCHMARKS.md, раздел 3).

- Раскладка кода: каждый вариант собирается с несколькими -falign-loops;
  берётся лучшая медиана (как bench/det/measure.py).
- Прогоны чередуются: в круге — все сборки по очереди, дрейф машины
  ложится на все.
- Пиковый RSS — через запускатель на C: у потомка Python wait4 считает и
  память самого Python до exec.
"""
import os
import platform
import shlex
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DPL = os.path.join(ROOT, 'build', 'release', 'dpl')

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


def need_dpl():
    if not os.path.exists(DPL):
        sys.exit(f'нет {DPL}: соберите компилятор (python3 tests/run.py)')


def cc_wrapper(tmp, align):
    """Обёртка компилятора C: тот же CC с -falign-loops=align."""
    path = os.path.join(tmp, f'cc-align{align}')
    if not os.path.exists(path):
        with open(path, 'w', encoding='utf-8') as f:
            f.write(f'#!/bin/sh\nexec {shlex.quote(os.environ.get("CC") or "cc")} -falign-loops={align} "$@"\n')
        os.chmod(path, 0o755)
    return path


def build_dpl(src, exe, tmp, align):
    r = subprocess.run([DPL, 'build', '--release', '-o', exe, src], capture_output=True, text=True,
                       env=dict(os.environ, CC=cc_wrapper(tmp, align)))
    if r.returncode != 0:
        sys.exit(f'{src}: сборка: {r.stderr.strip()}')
    return exe


def build_cpp(src, exe, align, defines=()):
    cxx = os.environ.get('CXX') or 'c++'
    r = subprocess.run([cxx, '-std=c++17', '-O2', f'-falign-loops={align}', *[f'-D{d}' for d in defines], '-o', exe,
                        src], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f'{src}: сборка: {r.stderr.strip()}')
    return exe


def launcher(tmp):
    src, exe = os.path.join(tmp, 'launch.c'), os.path.join(tmp, 'launch')
    with open(src, 'w', encoding='utf-8') as f:
        f.write(LAUNCHER)
    subprocess.run([os.environ.get('CC') or 'cc', '-O2', '-o', exe, src], check=True)
    return exe


def once(launch, argv, env=None):
    """Время (мс), пиковый RSS (КБ), stdout и stderr программы одного прогона."""
    a = time.perf_counter()
    r = subprocess.run([launch] + argv, capture_output=True, text=True,
                       env=dict(os.environ, **env) if env else None)
    ms = (time.perf_counter() - a) * 1000
    if r.returncode != 0:
        sys.exit(f'{" ".join(argv)}: код {r.returncode}: {r.stderr.strip()}')
    lines = r.stderr.splitlines()
    return ms, int(lines[-1]), r.stdout, '\n'.join(lines[:-1])


def sweep(launch, cells, runs):
    """cells: {ключ: argv}. Прогрев, затем runs кругов по очереди. Медианы
    (мс), пиковый RSS (КБ) и вывод каждого ключа."""
    outs = {k: once(launch, argv)[2] for k, argv in cells.items()}
    times = {k: [] for k in cells}
    rss = {k: 0 for k in cells}
    for _ in range(runs):
        for k, argv in cells.items():
            ms, kb, _, _ = once(launch, argv)
            times[k].append(ms)
            rss[k] = max(rss[k], kb)
    return {k: statistics.median(v) for k, v in times.items()}, rss, outs


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
