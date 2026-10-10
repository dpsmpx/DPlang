"""Запуск шлюза онлайн-компилятора.

    python3 service/gateway.py --config service/config/online-v1.json \\
        --worker build/release/dpl --listen 127.0.0.1:8080 \\
        [--metrics-listen 127.0.0.1:9100]

При запуске шлюз спрашивает у worker-а версии (`dpl worker
--describe`) и проверяет профиль; ошибка — код 2 и сообщение. Журнал —
строки JSON в stderr; первая — адреса, на которых шлюз слушает (порт 0 —
любой свободный, его назовёт журнал). SIGTERM и SIGINT — остановка.
"""
import argparse
import asyncio
import json
import os
import signal
import subprocess
import sys

from . import config
from .app import Gateway

DESCRIBE_TIMEOUT_S = 10


def parse_address(text):
    host, sep, port = text.rpartition(':')
    if not sep or not port.isdigit():
        raise argparse.ArgumentTypeError(f'expected HOST:PORT, got {text!r}')
    return host.strip('[]'), int(port)


def describe(worker):
    """Версии worker-а; SystemExit с сообщением, если он не отвечает как надо."""
    try:
        r = subprocess.run([worker, 'worker', '--describe'], capture_output=True, timeout=DESCRIBE_TIMEOUT_S,
                           env={})
        info = json.loads(r.stdout.decode('utf-8'))
    except (OSError, subprocess.TimeoutExpired, UnicodeDecodeError, ValueError) as e:
        raise SystemExit(f'error: worker {worker!r} does not describe itself: {e}')
    if r.returncode != 0 or info.get('api') != 'v1' or not isinstance(info.get('compiler'), dict):
        raise SystemExit(f'error: worker {worker!r} does not speak API v1')
    return info


async def serve(args, cfg, info):
    gw = Gateway(cfg, args.worker, info)
    limit = cfg['http']['header_bytes']
    server = await asyncio.start_server(gw.handle, args.listen[0], args.listen[1], limit=limit)
    servers = [server]
    entry = {'event': 'listening', 'api': [list(s.getsockname()[:2]) for s in server.sockets],
             'available': cfg['available'], 'profile': cfg['profile'], 'compiler': info['compiler']}
    if args.metrics_listen:
        metrics = await asyncio.start_server(gw.handle_metrics, args.metrics_listen[0], args.metrics_listen[1],
                                             limit=limit)
        servers.append(metrics)
        entry['metrics'] = [list(s.getsockname()[:2]) for s in metrics.sockets]
    gw.log(entry)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, stop.set)
    await stop.wait()
    for s in servers:
        s.close()
    for s in servers:
        await s.wait_closed()
    gw.log({'event': 'stopped'})


def main(argv=None):
    ap = argparse.ArgumentParser(prog='gateway', description='dpl online compiler gateway (API v1)')
    ap.add_argument('--config', required=True, help='service profile, e.g. service/config/online-v1.json')
    ap.add_argument('--worker', required=True, help='dpl binary of the release build')
    ap.add_argument('--listen', required=True, type=parse_address, help='HOST:PORT of the API')
    ap.add_argument('--metrics-listen', type=parse_address, help='HOST:PORT of /metrics (keep it private)')
    args = ap.parse_args(argv)
    try:
        cfg = config.load(args.config)
    except config.ConfigError as e:
        print(f'error: {e}', file=sys.stderr)
        return 2
    # Worker запускается из временного каталога запроса: путь к нему —
    # абсолютный, иначе относительный путь из командной строки не найдётся.
    args.worker = os.path.abspath(args.worker)
    info = describe(args.worker)
    asyncio.run(serve(args, cfg, info))
    return 0
