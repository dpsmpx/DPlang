"""Шлюз онлайн-компилятора: маршруты /api/v1 и ответы контракта (Р49).

    POST    /api/v1/compile — тело запроса уходит worker-у как есть;
                              ответ worker-а + meta.duration_ms
    OPTIONS /api/v1/compile — preflight CORS
    GET     /api/v1/info    — available, версии, пределы профиля
    GET     /metrics        — только на отдельном адресе метрик

Шлюз не разбирает JSON запроса и не компилирует: это делает worker в
отдельном процессе. Порядок проверок запроса: маршрут, available, rate
limit, Content-Type, длина тела, очередь. Журнал — строка JSON на
запрос, без исходника и тела; адрес клиента — хэш с солью процесса.
Контракт — docs/service/integration.md.
"""
import asyncio
import hashlib
import json
import math
import os
import signal
import sys
import time

from . import config
from . import runner
from .http import HttpError, content_length, read_body, read_head, response
from .limits import Admission, QueueFull, QueueTimeout, RateLimiter, client_address, client_key

# Код ошибки контракта → статус HTTP.
ERROR_STATUS = {
    'invalid_request': 400,
    'unsupported_language': 400,
    'unsupported_media_type': 415,
    'payload_too_large': 413,
    'limit_exceeded': 422,
    'compile_timeout': 422,
    'rate_limited': 429,
    'queue_full': 503,
    'queue_timeout': 503,
    'service_unavailable': 503,
    'compiler_internal_error': 500,
    'not_found': 404,
    'method_not_allowed': 405,
}
# Коды, которые может вернуть сам worker.
WORKER_CODES = {'invalid_request', 'unsupported_language', 'limit_exceeded'}
SECURITY_HEADERS = [
    ('Content-Type', 'application/json; charset=utf-8'),
    ('Cache-Control', 'no-store'),
    ('X-Content-Type-Options', 'nosniff'),
    ('Content-Security-Policy', "default-src 'none'; frame-ancestors 'none'"),
    ('Referrer-Policy', 'no-referrer'),
    ('Vary', 'Origin'),
]


def dumps(obj):
    return (json.dumps(obj, ensure_ascii=False, separators=(',', ':')) + '\n').encode('utf-8')


class Metrics:
    def __init__(self):
        self.counters = {}

    def inc(self, name, **labels):
        key = (name, tuple(sorted(labels.items())))
        self.counters[key] = self.counters.get(key, 0) + 1

    def text(self, gauges):
        lines = []
        for (name, labels), v in sorted(self.counters.items()):
            lab = ','.join(f'{k}="{val}"' for k, val in labels)
            lines.append(f'{name}{{{lab}}} {v}' if lab else f'{name} {v}')
        for name, v in sorted(gauges.items()):
            lines.append(f'{name} {v}')
        return ('\n'.join(lines) + '\n').encode('ascii')


class Reply:
    def __init__(self, status, body, headers=(), code=None, worker=None):
        self.status = status
        self.body = body
        self.headers = list(headers)
        self.code = code
        self.worker = worker  # runner.Outcome или None


class Gateway:
    def __init__(self, cfg, worker_path, describe, clock=time.monotonic, log=None):
        self.cfg = cfg
        self.argv = [worker_path, 'worker'] + config.worker_flags(cfg)
        self.describe = describe  # {"api": "v1", "compiler": {...}} от `dpl worker --describe`
        self.limiter = RateLimiter(cfg['rate_limit'], clock)
        self.admission = Admission(cfg['queue'])
        self.connections = 0
        self.metrics = Metrics()
        self.salt = os.urandom(16)
        self.log_stream = log or sys.stderr

    # --- ответы -----------------------------------------------------------

    def error(self, code, message, limit=None, extra_headers=(), worker=None):
        body = {'api': self.describe['api'], 'compiler': self.describe['compiler'],
                'error': {'code': code, 'message': message}}
        if limit:
            body['error']['limit'] = {'name': limit[0], 'max': limit[1]}
        if worker is not None:
            body['meta'] = {'duration_ms': worker.duration_ms}
        return Reply(ERROR_STATUS[code], dumps(body), extra_headers, code, worker)

    def cors(self, req):
        origin = req.header('origin') if req else None
        if origin and origin in self.cfg['cors']['origins']:
            return [('Access-Control-Allow-Origin', origin)]
        return []

    # --- соединение -------------------------------------------------------

    async def handle(self, reader, writer):
        peer = (writer.get_extra_info('peername') or ('unknown',))[0]
        start = time.monotonic()
        req = None
        if self.connections >= self.cfg['http']['connections']:
            reply = self.error('service_unavailable', 'too many connections; retry later',
                               extra_headers=[('Retry-After', '1')])
        else:
            self.connections += 1
            try:
                req = await read_head(reader, self.cfg['http'])
                reply = None if req is None else await self.route(reader, req, peer)
            except HttpError as e:
                reply = self.error(e.code, e.message)
                reply.status = e.status
            except Exception as e:  # ошибка шлюза — не падение процесса
                self.log({'event': 'gateway_error', 'error': type(e).__name__})
                reply = self.error('compiler_internal_error', 'internal error of the online compiler')
            finally:
                self.connections -= 1
        try:
            if reply is not None:
                headers = SECURITY_HEADERS + self.cors(req) + reply.headers
                if reply.status == 204:
                    headers = [h for h in headers if h[0] != 'Content-Type']
                writer.write(response(reply.status, reply.body, headers))
                await asyncio.wait_for(writer.drain(), self.cfg['http']['read_timeout_ms'] / 1000)
                await self.linger(reader, writer)
        except (ConnectionError, asyncio.TimeoutError):
            pass
        finally:
            writer.close()
        if reply is not None:
            self.account(req, peer, reply, start)

    async def linger(self, reader, writer):
        """Мягкое закрытие: конец записи, затем недочитанный вход (тело
        при 413, лишние байты) читается и отбрасывается — не дольше секунды
        и не больше предела запроса. Иначе ядро ответило бы на непрочитанные
        данные сбросом (RST), и клиент мог бы не получить ответ."""
        if not writer.can_write_eof():
            return
        writer.write_eof()
        cap = self.cfg['limits']['request_bytes'] + self.cfg['http']['header_bytes']

        async def discard():
            n = 0
            while n <= cap:
                chunk = await reader.read(65536)
                if not chunk:
                    return
                n += len(chunk)

        try:
            await asyncio.wait_for(discard(), 1.0)
        except (asyncio.TimeoutError, ConnectionError, asyncio.LimitOverrunError):
            pass

    def account(self, req, peer, reply, start):
        code = reply.code or ('ok' if reply.status < 300 else 'error')
        self.metrics.inc('dpl_requests_total', code=code)
        entry = {
            'event': 'request',
            'client': hashlib.sha256(self.salt + peer.encode()).hexdigest()[:16],
            'route': self.route_name(req),
            'status': reply.status,
            'code': code,
            'in_bytes': len(req.body) if req else 0,
            'out_bytes': len(reply.body),
            'ms': int((time.monotonic() - start) * 1000),
        }
        if reply.worker is not None:
            entry['worker'] = {'kind': reply.worker.kind, 'exit': reply.worker.returncode,
                               'ms': reply.worker.duration_ms}
        self.log(entry)

    def log(self, entry):
        entry = {'ts': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), **entry}
        self.log_stream.write(json.dumps(entry, ensure_ascii=True, separators=(',', ':')) + '\n')
        self.log_stream.flush()

    @staticmethod
    def route_name(req):
        if req is None:
            return '-'
        return {'/api/v1/compile': 'compile', '/api/v1/info': 'info'}.get(req.path, 'other')

    # --- маршруты ---------------------------------------------------------

    async def route(self, reader, req, peer):
        if req.path == '/api/v1/compile':
            if req.method == 'OPTIONS':
                return self.preflight(req)
            if req.method != 'POST':
                return self.error('method_not_allowed', 'use POST', extra_headers=[('Allow', 'POST, OPTIONS')])
            return await self.compile(reader, req, peer)
        if req.path == '/api/v1/info':
            if req.method != 'GET':
                return self.error('method_not_allowed', 'use GET', extra_headers=[('Allow', 'GET')])
            return self.info()
        return self.error('not_found', 'unknown path; the API is under /api/v1/')

    def preflight(self, req):
        cors = self.cors(req)
        if not cors or req.header('access-control-request-method') != 'POST':
            return Reply(204, b'', code='preflight_denied')
        return Reply(204, b'', [('Access-Control-Allow-Methods', 'POST'),
                                ('Access-Control-Allow-Headers', 'Content-Type'),
                                ('Access-Control-Max-Age', str(self.cfg['cors']['max_age_s']))], code='preflight')

    def info(self):
        body = {'api': self.describe['api'], 'available': self.cfg['available'],
                'compiler': self.describe['compiler'], 'limits': config.public_limits(self.cfg)}
        return Reply(200, dumps(body), code='info')

    async def compile(self, reader, req, peer):
        if not self.cfg['available']:
            return self.error('service_unavailable', 'the online compiler is not available')
        rl = self.cfg['rate_limit']
        key = client_key(client_address(peer, req.header('x-forwarded-for'), self.cfg['trusted_proxies']),
                         rl['ipv6_prefix'])
        wait = self.limiter.check(key)
        if wait is not None:
            return self.error('rate_limited', 'too many requests; retry later',
                              extra_headers=[('Retry-After', str(max(1, math.ceil(wait))))])
        media, _, params = req.header('content-type', '').partition(';')
        charset = params.strip().lower()
        if media.strip().lower() != 'application/json' or charset not in ('', 'charset=utf-8'):
            return self.error('unsupported_media_type', 'send the request as application/json in UTF-8')
        n = content_length(req)
        limit = self.cfg['limits']['request_bytes']
        if n > limit:
            return self.error('payload_too_large', f'request body exceeds the limit of {limit} bytes',
                              limit=('request_bytes', limit))
        await read_body(reader, req, n, self.cfg['http'])
        try:
            async with self.admission:
                outcome = await runner.run(self.argv, req.body, self.cfg['run'])
        except QueueFull:
            return self.error('queue_full', 'the compiler is busy; retry later', extra_headers=[('Retry-After', '1')])
        except QueueTimeout:
            return self.error('queue_timeout', 'the compiler is busy; retry later',
                              extra_headers=[('Retry-After', '1')])
        self.metrics.inc('dpl_worker_runs_total', kind=outcome.kind)
        return self.from_worker(outcome)

    def from_worker(self, o):
        if o.kind == 'timeout':
            ms = self.cfg['run']['timeout_ms']
            return self.error('compile_timeout', f'compilation took longer than {ms} ms',
                              limit=('timeout_ms', ms), worker=o)
        if o.kind == 'crash' and o.returncode in (-signal.SIGXCPU, -signal.SIGKILL):
            cpu = self.cfg['worker']['cpu_seconds']
            return self.error('compile_timeout', f'compilation needs more than {cpu} s of CPU time',
                              limit=('cpu_seconds', cpu), worker=o)
        if o.kind == 'crash' and o.returncode == -signal.SIGSYS:
            self.log({'event': 'sandbox_violation'})
        if o.kind != 'ok':
            return self.error('compiler_internal_error', 'internal error of the online compiler', worker=o)
        try:
            body = json.loads(o.stdout.decode('utf-8'))
        except (UnicodeDecodeError, ValueError):
            body = None
        if not self.valid_reply(body):
            return self.error('compiler_internal_error', 'internal error of the online compiler', worker=o)
        body['meta'] = {'duration_ms': o.duration_ms}
        status = 200 if 'result' in body else ERROR_STATUS[body['error']['code']]
        return Reply(status, dumps(body), code='result' if status == 200 else body['error']['code'], worker=o)

    def valid_reply(self, body):
        if not isinstance(body, dict) or body.get('api') != 'v1' or not isinstance(body.get('compiler'), dict):
            return False
        if set(body) == {'api', 'compiler', 'result'}:
            return isinstance(body['result'], dict)
        if set(body) == {'api', 'compiler', 'error'}:
            err = body['error']
            return isinstance(err, dict) and err.get('code') in WORKER_CODES and isinstance(err.get('message'), str)
        return False

    # --- метрики ----------------------------------------------------------

    async def handle_metrics(self, reader, writer):
        try:
            req = await read_head(reader, self.cfg['http'])
            if req is not None and req.method == 'GET' and req.path == '/metrics':
                gauges = {'dpl_queue_active': self.admission.active,
                          'dpl_queue_waiting': self.admission.waiting,
                          'dpl_connections': self.connections}
                writer.write(response(200, self.metrics.text(gauges),
                                      [('Content-Type', 'text/plain; version=0.0.4')]))
            elif req is not None:
                writer.write(response(404, b'', []))
            await writer.drain()
        except (HttpError, ConnectionError):
            pass
        finally:
            writer.close()
