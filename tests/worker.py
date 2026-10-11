"""Проходы worker и sandbox исполнителя tests/run.py: онлайн-сервис (Р49).

worker — золотые тесты контракта v1: тело запроса tests/worker/X.json
идёт в stdin `dpl worker`, ожидаемое — X.out рядом (stdout, stderr,
код выхода — как у наборов run.py). Пределы — PROFILE ниже, маленькие,
чтобы граница каждого была в тесте; X.args рядом заменяет их по одному
(`--diagnostics=3`); `--test-burn=cpu|memory` в X.args — worker тратит
ресурс до предела процесса. Каждый тест прогоняется дважды: ответы совпадают
байт в байт (детерминизм, docs/service/README.md, «Контракт»).

sandbox — пробник песочницы dpsandbox_probe: разрешённое `ok` работает,
каждое запрещённое действие убивает процесс сигналом SIGSYS.

Оба прохода — на сборке Release без санитайзеров: под ASan worker не
запускается (docs/service/README.md, «Сборка»).
"""
import difflib
import os
import signal
import subprocess

TIMEOUT = 60

# Пределы тестового профиля: имя ключа — как в service/config/online-v1.json.
PROFILE = {
    'request-bytes': 4096,
    'source-bytes': 2048,
    'lines': 60,
    'identifier-bytes': 32,
    'nesting': 64,
    'diagnostics': 5,
    'output-bytes': 8192,
    'memory-bytes': 256 << 20,
    'cpu-seconds': 10,
    'stack-bytes': 64 << 20,
}

# Действия пробника: ok — разрешено, остальные — смерть от SIGSYS.
PROBE_FORBIDDEN = ['open', 'creat', 'socket', 'unix_socket', 'execve', 'fork', 'thread', 'mmap_exec',
                   'mprotect_exec', 'ptrace', 'kill', 'getpid', 'x32']


def worker_argv(dpl, overrides=()):
    limits = dict(PROFILE)
    extra = []
    for o in overrides:
        name, _, value = o.lstrip('-').partition('=')
        if name == 'test-burn':  # не предел: worker тратит CPU или память (worker.cpp)
            extra.append(o)
            continue
        if name not in limits:
            raise ValueError(f'неизвестный предел {o}')
        limits[name] = int(value)
    return [dpl, 'worker'] + [f'--{k}={v}' for k, v in limits.items()] + extra


def run_worker(dpl, path):
    args_path = path[:-len('.json')] + '.args'
    overrides = []
    if os.path.exists(args_path):
        with open(args_path, encoding='utf-8') as f:
            overrides = f.read().split()
    with open(path, 'rb') as f:
        body = f.read()
    try:
        r = subprocess.run(worker_argv(dpl, overrides), input=body, capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return f'[timeout {TIMEOUT} s]\n'
    out = r.stdout.decode('utf-8', 'backslashreplace')
    if r.stderr:
        out += '--- stderr ---\n' + r.stderr.decode('utf-8', 'backslashreplace')
    if r.returncode != 0:
        out += f'[exit {r.returncode}]\n'
    return out


def worker_case(path, dpl, bless):
    """Сбой или None. Ответ сверяется с X.out; два прогона совпадают."""
    got = run_worker(dpl, path)
    again = run_worker(dpl, path)
    if got != again:
        return 'два прогона дали разные ответы'
    exp_path = path[:-len('.json')] + '.out'
    if bless:
        with open(exp_path, 'w', encoding='utf-8') as f:
            f.write(got)
        return None
    if not os.path.exists(exp_path):
        return f'нет ожидаемого {os.path.basename(exp_path)} (--bless)'
    with open(exp_path, encoding='utf-8') as f:
        want = f.read()
    if got == want:
        return None
    diff = difflib.unified_diff(want.splitlines(), got.splitlines(), 'ожидалось', 'получено', lineterm='', n=1)
    return 'вывод расходится:\n' + '\n'.join(list(diff)[:20])


def sandbox_case(action, probe):
    """Сбой или None: ok — код 0 и «ok», запрещённое — SIGSYS."""
    try:
        r = subprocess.run([probe, action], capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return f'[timeout {TIMEOUT} s]'
    if action == 'ok':
        if r.returncode == 0 and r.stdout == 'ok\n':
            return None
        return f'разрешённое не работает: код {r.returncode}: {r.stdout.strip()} {r.stderr.strip()}'
    if r.returncode == -signal.SIGSYS:
        return None
    return f'не убит SIGSYS: код {r.returncode}: {r.stdout.strip()} {r.stderr.strip()}'
