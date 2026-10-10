#!/usr/bin/env python3
"""Замеры онлайн-компилятора: по ним выбраны числа профиля (Р49, Р24).

    python3 bench/service/measure.py [--save DIR] [--runs N]

Нужна сборка Release: build/release/dpl (её делает tests/run.py).
Что меряется (docs/service/README.md, «Замеры и числа профиля»):

1. worker на одном запросе — время (стена, user, sys) и пиковая память
   (maxrss) на входах: пустой запуск (--describe), малая программа,
   программа у пределов профиля (строки и байты), она же с ошибками, худшие
   законные цепочки у предела строк, наибольшая принятая вложенность каждой
   конструкции прохода limits (tests/limits.py);
2. наименьший стек потока компилятора и наименьший RLIMIT_AS, на которых
   проходят все входы, — делением пополам;
3. шлюз целиком по HTTP: запросы в секунду и задержка p50/p95 при
   concurrency 1, 2, 4.

Числа зависят от машины; итог печатается, --save пишет measure.json и
summary.md в каталог.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DPL = os.path.join(ROOT, 'build', 'release', 'dpl')
PROFILE = os.path.join(ROOT, 'service', 'config', 'online-v1.json')
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import limits as limit_cases  # noqa: E402  конструкции прохода limits
import service as service_tests  # noqa: E402  шлюз в тестовом режиме

BLOCK = '''struct Vec{i} {{
    x: i32
    y: i32
}}

enum Shape{i} {{
    Dot
    Circle(i32)
    Rect {{ w: i32, h: i32 }}
}}

fn area{i}(s: Shape{i}) i64 {{
    match s {{
        .Dot => return 0
        .Circle(r) => return 3 * i64(r) * i64(r)
        .Rect {{ w, h }} => return i64(w) * i64(h)
    }}
}}

fn total{i}(xs: List[Shape{i}]) i64 {{
    var t: i64 = 0
    for s in &xs {{
        t += area{i}(s)
    }}
    return t
}}

fn walk{i}(n: i32) i32 {{
    var a: i32 = 0
    var k: i32 = 0
    while k < n {{
        if k % 3 == 0 {{
            a += k
        }} else if k % 3 == 1 {{
            a -= 1
        }} else {{
            a = a * 2 % 1000
        }}
        k += 1
    }}
    return a + Vec{i} {{ x: 1, y: 2 }}.x
}}

'''


def fill(make, lines, size):
    """Повторяет make(i), пока текст укладывается в строки и байты."""
    out, i = '', 0
    while True:
        nxt = out + make(i)
        if nxt.count('\n') + 4 > lines or len(nxt.encode()) + 64 > size:
            return out + 'fn main() {\n    println("ok")\n}\n'
        out, i = nxt, i + 1


def deepest(make, cfg):
    """Наибольшая глубина make(n), которую worker принимает без E0002."""
    argv = [DPL, 'worker'] + flags(cfg, cpu_seconds=30)

    def ok(n):
        r = run_measured(argv, body(make(n)))
        return b'"E0002"' not in r['out'] and outcome(r) != 'limit_exceeded:lines'

    lo, hi = 1, cfg['limits']['nesting'] + 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if ok(mid):
            lo = mid
        else:
            hi = mid
    return make(lo)


def inputs(cfg):
    limits = cfg['limits']
    lines, size = limits['lines'], limits['source_bytes']
    real = fill(lambda i: BLOCK.format(i=i), lines, size)
    broken = real.replace('return t\n', 'return u\n')
    n = lines - 4
    out = {
        'small': 'fn main() {\n    println("hello")\n}\n',
        'realistic_at_limit': real,
        'realistic_with_errors': broken,
        'struct_chain': ''.join(f'struct S{i} {{ x: S{i + 1} }}\n' for i in range(n)) + f'struct S{n} {{ x: i32 }}\n',
        'const_chain': ''.join(f'const C{i}: i64 = C{i + 1} + 1\n' for i in range(n)) + f'const C{n}: i64 = 0\n',
        'try_chain': 'enum Er {\n    A\n}\n' + ''.join(
            f'fn f{i}() !i32 {{\n    return try f{i + 1}()\n}}\n' for i in range(n // 3 - 2)) +
            f'fn f{n // 3 - 2}() !i32 {{\n    fail Er.A\n}}\n',
    }
    # Вложенность — наибольшая принятая для каждой конструкции прохода limits.
    for name, make in limit_cases.CASES.items():
        out['nesting_' + name] = deepest(make, cfg)
    return out


def flags(cfg, **over):
    vals = dict(cfg['limits'])
    vals.update(cfg['worker'])
    vals.update(over)
    return [f'--{k.replace("_", "-")}={v}' for k, v in vals.items()]


def body(src):
    return json.dumps({'language': '0.1', 'mode': 'check', 'file_name': 'main.dpm', 'source': src}).encode()


def run_measured(argv, data):
    """Запуск с ожиданием через wait4: время и память одного процесса."""
    t = time.monotonic()
    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env={})
    p.stdin.write(data)
    p.stdin.close()
    out = p.stdout.read()
    _, status, ru = os.wait4(p.pid, 0)
    p.returncode = os.waitstatus_to_exitcode(status)
    wall = time.monotonic() - t
    return {'wall_ms': wall * 1000, 'user_ms': ru.ru_utime * 1000, 'sys_ms': ru.ru_stime * 1000,
            'maxrss_kb': ru.ru_maxrss, 'exit': p.returncode, 'out': out}


def outcome(r):
    """Кратко: result (ok или с ошибками), error.code или смерть."""
    if r['exit'] not in (0, 4):
        return f'exit {r["exit"]}'
    try:
        j = json.loads(r['out'])
    except ValueError:
        return 'bad json'
    if 'result' in j:
        return 'ok' if j['result']['success'] else f'errors {j["result"]["total"]}'
    return j['error']['code'] + ':' + j['error'].get('limit', {}).get('name', '')


def measure_inputs(cfg, srcs, runs):
    out = {}
    base = [DPL, 'worker']
    rs = [run_measured(base + ['--describe'], b'') for _ in range(runs)]
    out['describe'] = summarize(rs, 'ok', 0, 0)
    for name, src in srcs.items():
        argv = base + flags(cfg, cpu_seconds=30)
        rs = [run_measured(argv, body(src)) for _ in range(runs)]
        out[name] = summarize(rs, outcome(rs[0]), src.count('\n'), len(src.encode()))
    return out


def summarize(rs, what, lines, size):
    return {'outcome': what, 'lines': lines, 'bytes': size,
            'wall_ms_median': round(statistics.median(r['wall_ms'] for r in rs), 1),
            'wall_ms_max': round(max(r['wall_ms'] for r in rs), 1),
            'cpu_ms_median': round(statistics.median(r['user_ms'] + r['sys_ms'] for r in rs), 1),
            'maxrss_mb': round(max(r['maxrss_kb'] for r in rs) / 1024, 1)}


def passes(cfg, srcs, **over):
    argv = [DPL, 'worker'] + flags(cfg, cpu_seconds=30, **over)
    for src in srcs.values():
        r = run_measured(argv, body(src))
        if outcome(r).startswith(('exit', 'bad', 'limit_exceeded:memory')):
            return False
    return True


def bisect(lo, hi, ok):
    """Наименьшее значение в (lo, hi], при котором ok — истина (ok(hi) истинно)."""
    while hi - lo > max(1, hi // 64):
        mid = (lo + hi) // 2
        if ok(mid):
            hi = mid
        else:
            lo = mid
    return hi


def measure_minimums(cfg, srcs):
    big = cfg['worker']['memory_bytes'] * 4
    stack = bisect(1 << 16, cfg['worker']['stack_bytes'], lambda s: passes(cfg, srcs, stack_bytes=s,
                                                                            memory_bytes=big))
    memory = bisect(stack, big, lambda m: passes(cfg, srcs, memory_bytes=m))
    return {'min_stack_bytes': stack, 'min_memory_bytes': memory}


def measure_gateway(cfg, src, conc, total, clients):
    with tempfile.TemporaryDirectory(prefix='dpmeasure-') as tmp:
        gw = service_tests.Gateway(tmp, DPL, 'real', queue__concurrency=conc, queue__length=clients * 2,
                                   queue__wait_ms=60000, rate_limit__per_ip_burst=total * 2,
                                   rate_limit__global_burst=total * 2)
        try:
            lat, errors, lock = [], [], threading.Lock()
            data = body(src).decode()

            def client(k):
                for _ in range(total // clients):
                    t = time.monotonic()
                    st = gw.post(data)[0]
                    with lock:
                        lat.append((time.monotonic() - t) * 1000)
                        if st != 200:
                            errors.append(st)

            t0 = time.monotonic()
            threads = [threading.Thread(target=client, args=(k,)) for k in range(clients)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            dt = time.monotonic() - t0
        finally:
            gw.stop()
    lat.sort()
    return {'concurrency': conc, 'clients': clients, 'requests': len(lat), 'errors': len(errors),
            'rps': round(len(lat) / dt, 1), 'p50_ms': round(lat[len(lat) // 2], 1),
            'p95_ms': round(lat[int(len(lat) * 0.95)], 1)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--save', help='каталог для measure.json и summary.md')
    ap.add_argument('--runs', type=int, default=5)
    a = ap.parse_args()
    if not os.path.exists(DPL):
        print('нет build/release/dpl: сначала python3 tests/run.py', file=sys.stderr)
        return 2
    with open(PROFILE, encoding='utf-8') as f:
        cfg = json.load(f)
    srcs = inputs(cfg)
    res = {'machine': {'cpus': os.cpu_count(), 'uname': ' '.join(os.uname()[2:5])},
           'profile': {'limits': cfg['limits'], 'worker': cfg['worker'], 'run': cfg['run'], 'queue': cfg['queue']}}
    res['inputs'] = measure_inputs(cfg, srcs, a.runs)
    res['minimums'] = measure_minimums(cfg, srcs)
    res['gateway'] = [measure_gateway(cfg, srcs['realistic_at_limit'], c, 48, 8) for c in (1, 2, 4)]
    res['gateway_small'] = [measure_gateway(cfg, srcs['small'], c, 96, 8) for c in (1, 2, 4)]
    text = summary(res)
    print(text)
    if a.save:
        os.makedirs(a.save, exist_ok=True)
        with open(os.path.join(a.save, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(res, f, ensure_ascii=False, indent=1)
            f.write('\n')
        with open(os.path.join(a.save, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(text)
    return 0


def summary(res):
    out = [f'# Замеры онлайн-компилятора\n', f'Машина: {res["machine"]["cpus"]} CPU, {res["machine"]["uname"]}.\n',
           '| вход | строк | байт | итог | стена, мс (медиана / макс.) | CPU, мс | maxrss, МБ |', '|---|---|---|---|---|---|---|']
    for name, r in res['inputs'].items():
        out.append(f'| {name} | {r["lines"]} | {r["bytes"]} | {r["outcome"]} | {r["wall_ms_median"]} / '
                   f'{r["wall_ms_max"]} | {r["cpu_ms_median"]} | {r["maxrss_mb"]} |')
    m = res['minimums']
    out.append(f'\nНаименьший стек: {m["min_stack_bytes"] / 2**20:.2f} МБ; наименьший RLIMIT_AS: '
               f'{m["min_memory_bytes"] / 2**20:.1f} МБ (со стеком профиля).\n')
    for title, key in (('у предела', 'gateway'), ('малая программа', 'gateway_small')):
        out.append(f'Шлюз по HTTP, {title}:\n')
        out.append('| concurrency | клиентов | запросов | ошибок | запросов/с | p50, мс | p95, мс |')
        out.append('|---|---|---|---|---|---|---|')
        for g in res[key]:
            out.append(f'| {g["concurrency"]} | {g["clients"]} | {g["requests"]} | {g["errors"]} | {g["rps"]} | '
                       f'{g["p50_ms"]} | {g["p95_ms"]} |')
        out.append('')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    sys.exit(main())
