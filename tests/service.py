"""Проход service исполнителя tests/run.py: шлюз онлайн-компилятора (Р49).

Чёрный ящик по HTTP: каждый тест запускает свой шлюз (service/gateway.py,
порт 0) с тестовым профилем — копией service/config/online-v1.json с
заменёнными значениями — и шлёт сырые запросы через сокет. Worker —
настоящий `dplang worker` сборки Release или поддельный (FAKES ниже):
падение, зависание с потомком, поток вывода, мусор, медленный — для
очереди и предела конкурентности. Эти тесты — приёмка будущего шлюза на
DPlang (docs/service/README.md, «Шлюз»).

Время в тестах — только с большим запасом: таймаут 300 мс проверяется
ожиданием 1,5 с, очередь — worker-ом, который спит секунду.
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATEWAY = os.path.join(ROOT, 'service', 'gateway.py')
PROFILE = os.path.join(ROOT, 'service', 'config', 'online-v1.json')
START_TIMEOUT = 20

DESCRIBE = '{"api":"v1","compiler":{"version":"0.1.0-dev","language":"0.1"}}'
REPLY_OK = '{"api":"v1","compiler":{"version":"0.1.0-dev","language":"0.1"},' \
           '"result":{"success":true,"diagnostics":[],"truncated":false,"total":0}}'

# Поддельные worker-ы: тело после разбора --describe. {marks} — каталог
# отметок теста, {python} — интерпретатор.
FAKES = {
    'crash': 'os.kill(os.getpid(), signal.SIGSEGV)',
    'hang': ('mark("pid", os.getpid())\n'
             'child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(600)"])\n'
             'mark("child", child.pid)\n'
             'time.sleep(600)'),
    'flood': 'mark("pid", os.getpid())\nwhile True:\n    sys.stdout.write("x" * 65536)\n    sys.stdout.flush()',
    'garbage': 'sys.stdout.write("not json at all")',
    'bad_code': ('sys.stdout.write(\'{"api":"v1","compiler":{"version":"0","language":"0"},'
                 '"error":{"code":"rate_limited","message":"x"}}\')'),
    'stderr_flood': 'sys.stderr.write("e" * (1 << 20))\nsys.stderr.flush()\nsys.stdout.write(REPLY_OK)',
    'slow': ('name = os.path.join(MARKS, "run-%d" % os.getpid())\n'
             'open(name, "w").close()\n'
             'mark("seen", len([n for n in os.listdir(MARKS) if n.startswith("run-")]))\n'
             'time.sleep(float(os.path.basename(sys.argv[0]).split("_")[-1].replace("p", ".")))\n'
             'os.remove(name)\n'
             'sys.stdout.write(REPLY_OK)'),
    # Окружение worker-а: пусто, каталог 0700, открыты только 0, 1, 2.
    # Окружение — исходное, из /proc: Python сам добавляет LC_CTYPE (PEP 538).
    'env': ('fds = sorted(int(f) for f in os.listdir("/proc/self/fd"))\n'
            'mode = os.stat(os.getcwd()).st_mode & 0o777\n'
            'env = [e for e in open("/proc/self/environ", "rb").read().split(b"\\0") if e]\n'
            'mark("env", json.dumps({"env": [e.decode() for e in env], "mode": mode, "fds": fds, '
            '"cwd": os.getcwd()}))\n'
            'sys.stdout.write(REPLY_OK)'),
}

FAKE_HEAD = '''#!{python}
import json, os, signal, subprocess, sys, time
MARKS = {marks!r}
REPLY_OK = {reply!r}
def mark(kind, value):
    with open(os.path.join(MARKS, "%s-%d" % (kind, os.getpid())), "w") as f:
        f.write(str(value))
if sys.argv[2:] == ["--describe"]:
    sys.stdout.write({describe!r})
    sys.exit(0)
sys.stdin.read()
'''


class Gateway:
    """Шлюз с тестовым профилем; worker — путь или имя подделки."""

    def __init__(self, tmp, release_dplang, worker='real', cwd=None, **overrides):
        self.tmp = tmp
        self.marks = os.path.join(tmp, 'marks')
        os.makedirs(self.marks, exist_ok=True)
        with open(PROFILE, encoding='utf-8') as f:
            cfg = json.load(f)
        cfg['available'] = True
        cfg['run']['timeout_ms'] = 10000
        for path, value in overrides.items():
            section, _, key = path.partition('__')
            if key:
                cfg[section][key] = value
            else:
                cfg[section] = value
        self.cfg = cfg
        cfg_path = os.path.join(tmp, 'profile.json')
        with open(cfg_path, 'w', encoding='utf-8') as f:
            json.dump(cfg, f)
        worker_path = release_dplang if worker == 'real' else self.fake(worker)
        if cwd:  # путь к worker-у — относительный, от cwd шлюза
            worker_path = os.path.relpath(worker_path, cwd)
        self.log_path = os.path.join(tmp, 'gateway.log')
        self.log_file = open(self.log_path, 'w+', encoding='utf-8')
        self.proc = subprocess.Popen([sys.executable, '-I', GATEWAY, '--config', cfg_path, '--worker', worker_path,
                                      '--listen', '127.0.0.1:0', '--metrics-listen', '127.0.0.1:0'],
                                     stdout=subprocess.DEVNULL, stderr=self.log_file, cwd=cwd)
        self.port = self.metrics_port = None
        deadline = time.monotonic() + START_TIMEOUT
        while time.monotonic() < deadline and self.port is None:
            if self.proc.poll() is not None:
                break
            for line in self.log_lines():
                if line.get('event') == 'listening':
                    self.port = line['api'][0][1]
                    self.metrics_port = line['metrics'][0][1]
            time.sleep(0.02)
        if self.port is None:
            self.stop()
            raise RuntimeError('шлюз не запустился: ' + self.log_text()[-500:])

    def fake(self, name):
        base, _, arg = name.partition(':')
        path = os.path.join(self.tmp, f'fake_{base}' + (f'_{arg}' if arg else ''))
        with open(path, 'w', encoding='utf-8') as f:
            f.write(FAKE_HEAD.format(python=sys.executable, marks=self.marks, reply=REPLY_OK, describe=DESCRIBE))
            f.write(FAKES[base] + '\n')
        os.chmod(path, 0o700)
        return path

    def log_text(self):
        with open(self.log_path, encoding='utf-8') as f:
            return f.read()

    def log_lines(self):
        out = []
        for line in self.log_text().splitlines():
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
        return out

    def marks_of(self, kind):
        out = []
        for n in sorted(os.listdir(self.marks)):
            if n.startswith(kind + '-'):
                with open(os.path.join(self.marks, n), encoding='utf-8') as f:
                    out.append(f.read())
        return out

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log_file.close()

    # --- запросы ----------------------------------------------------------

    def raw(self, data, port=None, timeout=15, half_close=False):
        """Сырые байты → (статус, заголовки, тело); статус 0 — нет ответа."""
        with socket.create_connection(('127.0.0.1', port or self.port), timeout=timeout) as s:
            if data:
                s.sendall(data)
            if half_close:
                s.shutdown(socket.SHUT_WR)
            chunks = []
            try:
                while True:
                    c = s.recv(65536)
                    if not c:
                        break
                    chunks.append(c)
            except (ConnectionResetError, socket.timeout):
                pass
        return parse_response(b''.join(chunks))

    def post(self, body, content_type='application/json', headers=(), path='/api/v1/compile'):
        if isinstance(body, str):
            body = body.encode('utf-8')
        head = [f'POST {path} HTTP/1.1', 'Host: test', f'Content-Type: {content_type}',
                f'Content-Length: {len(body)}'] + list(headers)
        return self.raw(('\r\n'.join(head) + '\r\n\r\n').encode('ascii') + body)

    def get(self, path, headers=(), method='GET', port=None):
        head = [f'{method} {path} HTTP/1.1', 'Host: test'] + list(headers)
        return self.raw(('\r\n'.join(head) + '\r\n\r\n').encode('ascii'), port=port)


def parse_response(data):
    head, sep, body = data.partition(b'\r\n\r\n')
    if not sep:
        return 0, {}, data
    lines = head.decode('latin-1').split('\r\n')
    status = int(lines[0].split()[1])
    headers = {}
    for ln in lines[1:]:
        k, _, v = ln.partition(':')
        headers[k.strip().lower()] = v.strip()
    return status, headers, body


def request(src, **kw):
    d = {'language': '0.1', 'mode': 'check', 'file_name': 'main.dpm', 'source': src}
    d.update(kw)
    return json.dumps(d)


def body_json(body):
    try:
        return json.loads(body.decode('utf-8'))
    except (UnicodeDecodeError, ValueError):
        return None


def expect(resp, status, code=None):
    """Сбой или None: статус и код ошибки контракта."""
    st, headers, body = resp
    if st != status:
        return f'статус {st}, ожидался {status}: {body[:300]!r}'
    if code is None:
        return None
    j = body_json(body)
    got = j.get('error', {}).get('code') if isinstance(j, dict) else None
    if got != code:
        return f'код {got!r}, ожидался {code!r}: {body[:300]!r}'
    return None


def alive(pid):
    try:
        os.kill(int(pid), 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        with open(f'/proc/{pid}/stat', encoding='ascii') as f:
            return f.read().split(')')[-1].split()[0] != 'Z'
    except OSError:
        return False


def parallel(fn, n):
    """n вызовов fn(i) одновременно; список результатов по порядку."""
    out = [None] * n
    barrier = threading.Barrier(n)

    def one(i):
        barrier.wait()
        out[i] = fn(i)

    threads = [threading.Thread(target=one, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return out


# --- тесты: функция (gw) → сбой или None ------------------------------------

def t_compile_ok(gw):
    r = gw.post(request('fn main() {\n    println("hi")\n}\n'))
    if err := expect(r, 200):
        return err
    j = body_json(r[2])
    if j['result'] != {'success': True, 'diagnostics': [], 'truncated': False, 'total': 0}:
        return f'результат {j["result"]}'
    if not isinstance(j.get('meta', {}).get('duration_ms'), int):
        return f'нет meta.duration_ms: {j}'
    if list(j) != ['api', 'compiler', 'result', 'meta']:
        return f'порядок ключей {list(j)}'
    return None


def t_compile_errors(gw):
    r = gw.post(request('fn main() {\n    let x: i32 = "a"\n}\n'))
    if err := expect(r, 200):
        return err
    res = body_json(r[2])['result']
    if res['success'] or res['diagnostics'][0]['code'] != 'E0401':
        return f'результат {res}'
    return None


def t_deterministic(gw):
    """Детерминированная часть ответа — та же; разное только meta."""
    src = request('fn main() {\n    let y = x\n    let z: bool = 1\n}\n')
    a, b = body_json(gw.post(src)[2]), body_json(gw.post(src)[2])
    a.pop('meta')
    b.pop('meta')
    return None if a == b else f'ответы разные: {a} {b}'


def t_invalid_json(gw):
    return expect(gw.post('{"language": "0.1",'), 400, 'invalid_request')


def t_unsupported_language(gw):
    return expect(gw.post(request('', language='0.9')), 400, 'unsupported_language')


def t_limit_source(gw):
    n = gw.cfg['limits']['source_bytes'] + 1
    r = gw.post(request('/' * n))
    if err := expect(r, 422, 'limit_exceeded'):
        return err
    lim = body_json(r[2])['error'].get('limit')
    return None if lim == {'name': 'source_bytes', 'max': n - 1} else f'limit {lim}'


def t_limit_nesting(gw):
    n = gw.cfg['limits']['nesting'] + 1
    r = gw.post(request('fn main() {\n    let x = ' + '(' * n + '1' + ')' * n + '\n}\n'))
    if err := expect(r, 200):
        return err
    d = body_json(r[2])['result']['diagnostics']
    return None if len(d) == 1 and d[0]['code'] == 'E0002' else f'диагностика {d}'


def t_memory_bomb(gw):
    """Удвоение строки в константах: worker упирается в RLIMIT_AS (TODO.md).
    Время до предела — в основном обнуление страниц ядром, оно растёт с
    пределом памяти: здесь предел 256 МБ и запас CPU."""
    src = 'const C0: str = "ab"\n' + ''.join(f'const C{i}: str = C{i - 1} + C{i - 1}\n' for i in range(1, 48))
    r = gw.post(request(src))
    if err := expect(r, 422, 'limit_exceeded'):
        return err
    lim = body_json(r[2])['error'].get('limit', {})
    return None if lim.get('name') == 'memory_bytes' else f'limit {lim}'


def t_cpu_bomb(gw):
    """«Ромб» из структур — экспоненциальная проверка E0423 (TODO.md):
    worker убивает предел CPU, шлюз отвечает compile_timeout."""
    src = ''.join(f'struct S{i} {{ a: S{i + 1}, b: S{i + 1} }}\n' for i in range(60)) + 'struct S60 { x: i32 }\n'
    r = gw.post(request(src))
    if err := expect(r, 422, 'compile_timeout'):
        return err
    lim = body_json(r[2])['error'].get('limit', {})
    return None if lim.get('name') == 'cpu_seconds' else f'limit {lim}'


def t_info(gw):
    r = gw.get('/api/v1/info')
    if err := expect(r, 200):
        return err
    j = body_json(r[2])
    if j.get('available') is not True or j.get('limits') != gw.cfg['limits'] or j.get('api') != 'v1':
        return f'info {j}'
    return None


def t_unavailable(gw):
    if err := expect(gw.post(request('')), 503, 'service_unavailable'):
        return err
    j = body_json(gw.get('/api/v1/info')[2])
    return None if j.get('available') is False else f'info {j}'


def t_security_headers(gw):
    for r in (gw.get('/api/v1/info'), gw.post(request('')), gw.get('/nope')):
        h = r[1]
        need = {'x-content-type-options': 'nosniff', 'cache-control': 'no-store', 'referrer-policy': 'no-referrer',
                'content-security-policy': "default-src 'none'; frame-ancestors 'none'", 'connection': 'close'}
        for k, v in need.items():
            if h.get(k) != v:
                return f'заголовок {k}: {h.get(k)!r}'
        if r[0] != 204 and h.get('content-type') != 'application/json; charset=utf-8':
            return f'content-type {h.get("content-type")!r}'
    return None


def t_cors(gw):
    ok = gw.post(request(''), headers=['Origin: https://dpsmpx.com'])
    if ok[1].get('access-control-allow-origin') != 'https://dpsmpx.com':
        return f'нет CORS для своего сайта: {ok[1]}'
    evil = gw.post(request(''), headers=['Origin: https://evil.example'])
    if 'access-control-allow-origin' in evil[1]:
        return 'CORS для чужого сайта'
    pre = gw.get('/api/v1/compile', method='OPTIONS',
                 headers=['Origin: https://dpsmpx.com', 'Access-Control-Request-Method: POST'])
    if pre[0] != 204 or pre[1].get('access-control-allow-methods') != 'POST':
        return f'preflight: {pre[0]} {pre[1]}'
    pre = gw.get('/api/v1/compile', method='OPTIONS',
                 headers=['Origin: https://evil.example', 'Access-Control-Request-Method: POST'])
    if pre[0] != 204 or 'access-control-allow-methods' in pre[1] or 'access-control-allow-origin' in pre[1]:
        return f'preflight чужого сайта: {pre[0]} {pre[1]}'
    return None


def t_routes(gw):
    return (expect(gw.get('/api/v1/other'), 404, 'not_found') or
            expect(gw.get('/api/v1/compile'), 405, 'method_not_allowed') or
            expect(gw.post('', path='/api/v1/info'), 405, 'method_not_allowed') or
            expect(gw.get('/api/v1/info?x=1'), 200))


def t_media_type(gw):
    return (expect(gw.post(request(''), content_type='text/plain'), 415, 'unsupported_media_type') or
            expect(gw.post(request(''), content_type='application/json; charset=latin-1'), 415,
                   'unsupported_media_type') or
            expect(gw.post(request(''), content_type='application/json; charset=utf-8'), 200))


def t_payload_too_large(gw):
    """Тело больше предела: 413 до чтения тела; ответ доходит до клиента,
    хотя он успел отправить тело (мягкое закрытие)."""
    body = b'x' * (gw.cfg['limits']['request_bytes'] + 1)
    return expect(gw.post(body), 413, 'payload_too_large')


def t_http_malformed(gw):
    cases = [
        (b'GARBAGE\r\n\r\n', 400),
        (b'GET /api/v1/info HTTP/2.0\r\n\r\n', 400),
        (b'GET api HTTP/1.1\r\n\r\n', 400),
        (b'GET /api/v1/info HTTP/1.1\r\nBad Header: x\r\n\r\n', 400),
        (b'GET /api/v1/info HTTP/1.1\r\nX: \xff\r\n\r\n', 400),
        (b'GET /api/v1/info HTTP/1.1\r\n folded\r\n\r\n', 400),
        (b'POST /api/v1/compile HTTP/1.1\r\nContent-Type: application/json\r\n\r\n', 411),
        (b'POST /api/v1/compile HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 5\r\n'
         b'Content-Length: 5\r\n\r\nhello', 400),
        (b'POST /api/v1/compile HTTP/1.1\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n'
         b'0\r\n\r\n', 400),
        (b'POST /api/v1/compile HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: -1\r\n\r\n', 400),
    ]
    for data, status in cases:
        r = gw.raw(data)
        if r[0] != status:
            return f'{data[:40]!r}: статус {r[0]}, ожидался {status}'
    return None


def t_header_limits(gw):
    big = b'GET /api/v1/info HTTP/1.1\r\nX: ' + b'a' * (gw.cfg['http']['header_bytes'] + 10) + b'\r\n\r\n'
    if err := expect(gw.raw(big), 431):
        return 'большой заголовок: ' + err
    many = b'GET /api/v1/info HTTP/1.1\r\n' + b''.join(b'X%d: y\r\n' % i for i in range(gw.cfg['http']['headers'] + 1))
    if err := expect(gw.raw(many + b'\r\n'), 431):
        return 'много заголовков: ' + err
    return None


def t_slow_headers(gw):
    """Медленный клиент (slowloris): заголовки не дошли за read_timeout — 408."""
    with socket.create_connection(('127.0.0.1', gw.port), timeout=10) as s:
        s.sendall(b'GET /api/v1/info HTTP/1.1\r\nHost: x\r\n')
        time.sleep(1.5)
        data = s.recv(65536)
    return expect(parse_response(data), 408)


def t_slow_body(gw):
    with socket.create_connection(('127.0.0.1', gw.port), timeout=10) as s:
        s.sendall(b'POST /api/v1/compile HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{')
        time.sleep(1.5)
        data = s.recv(65536)
    return expect(parse_response(data), 408)


def t_short_body(gw):
    r = gw.raw(b'POST /api/v1/compile HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{',
               half_close=True)
    return expect(r, 400, 'invalid_request')


def t_connection_limit(gw):
    """Открытые без запроса соединения сверх предела — сразу 503."""
    n = gw.cfg['http']['connections']
    socks = [socket.create_connection(('127.0.0.1', gw.port), timeout=10) for _ in range(n)]
    try:
        time.sleep(0.3)
        r = gw.get('/api/v1/info')
    finally:
        for s in socks:
            s.close()
    if err := expect(r, 503, 'service_unavailable'):
        return err
    time.sleep(0.3)
    return expect(gw.get('/api/v1/info'), 200)


def t_rate_limit_ip(gw):
    burst = gw.cfg['rate_limit']['per_ip_burst']
    for i in range(burst):
        if err := expect(gw.post(request('')), 200):
            return f'запрос {i}: {err}'
    r = gw.post(request(''))
    if err := expect(r, 429, 'rate_limited'):
        return err
    return None if r[1].get('retry-after', '').isdigit() else f'Retry-After {r[1].get("retry-after")!r}'


def t_rate_limit_global(gw):
    """Общий предел: разные клиенты (через доверенный прокси) упираются в него."""
    burst = gw.cfg['rate_limit']['global_burst']
    for i in range(burst):
        if err := expect(gw.post(request(''), headers=[f'X-Forwarded-For: 198.51.100.{i + 1}']), 200):
            return f'клиент {i}: {err}'
    return expect(gw.post(request(''), headers=['X-Forwarded-For: 198.51.100.200']), 429, 'rate_limited')


def t_rate_limit_ipv6(gw):
    """Через доверенный прокси: IPv6 одной сети /64 — один клиент, другой
    сети — другой; X-Forwarded-For от недоверенного прокси не учитывается."""
    if err := expect(gw.post(request(''), headers=['X-Forwarded-For: 2001:db8:1:2::1']), 200):
        return err
    if err := expect(gw.post(request(''), headers=['X-Forwarded-For: 2001:db8:1:2::ffff']), 429, 'rate_limited'):
        return 'та же сеть /64: ' + err
    if err := expect(gw.post(request(''), headers=['X-Forwarded-For: 2001:db8:1:3::1']), 200):
        return 'другая сеть /64: ' + err
    return None


def t_forwarded_untrusted(gw):
    """Без доверенных прокси X-Forwarded-For не меняет клиента."""
    if err := expect(gw.post(request(''), headers=['X-Forwarded-For: 203.0.113.1']), 200):
        return err
    return expect(gw.post(request(''), headers=['X-Forwarded-For: 203.0.113.2']), 429, 'rate_limited')


def t_rate_table_bounded(gw):
    """Таблица клиентов ограничена: при переполнении старый вытесняется."""
    a, b, c = (['X-Forwarded-For: 192.0.2.%d' % i] for i in (1, 2, 3))
    if expect(gw.post(request(''), headers=a), 200) or expect(gw.post(request(''), headers=a), 429):
        return 'клиент a не ограничен'
    gw.post(request(''), headers=b)
    gw.post(request(''), headers=c)  # таблица на 2: a вытеснен
    return expect(gw.post(request(''), headers=a), 200)


def t_queue_full(gw):
    """concurrency 1, length 1: из одновременных запросов лишние — 503 queue_full."""
    rs = parallel(lambda i: gw.post(request('')), 6)
    codes = sorted(r[0] for r in rs)
    full = [r for r in rs if r[0] == 503 and body_json(r[2])['error']['code'] == 'queue_full']
    if codes.count(200) < 1 or not full:
        return f'статусы {codes}'
    return None


def t_queue_timeout(gw):
    """Ожидание в очереди дольше wait_ms — 503 queue_timeout."""
    rs = parallel(lambda i: gw.post(request('')), 2)
    codes = sorted((r[0], (body_json(r[2]) or {}).get('error', {}).get('code')) for r in rs)
    return None if codes == [(200, None), (503, 'queue_timeout')] else f'итоги {codes}'


def t_concurrency(gw):
    """Стресс: 16 одновременных запросов, concurrency 2 — одновременно
    работают ровно 2 worker-а (отметки процессов), все запросы приняты."""
    rs = parallel(lambda i: gw.post(request('')), 16)
    bad = [r[0] for r in rs if r[0] != 200]
    if bad:
        return f'не приняты: {bad}'
    seen = max(int(x) for x in gw.marks_of('seen'))
    return None if seen == gw.cfg['queue']['concurrency'] else f'одновременно работало {seen}'


def t_worker_crash(gw):
    return expect(gw.post(request('')), 500, 'compiler_internal_error')


def t_worker_hang(gw):
    """Зависший worker и его потомок убиты по таймауту (группа процессов)."""
    r = gw.post(request(''))
    if err := expect(r, 422, 'compile_timeout'):
        return err
    lim = body_json(r[2])['error'].get('limit', {})
    if lim.get('name') != 'timeout_ms':
        return f'limit {lim}'
    time.sleep(0.3)
    left = [p for p in gw.marks_of('pid') + gw.marks_of('child') if alive(p)]
    return f'живы процессы {left}' if left else None


def t_worker_flood(gw):
    """Бесконечный вывод: шлюз читает не больше предела и убивает worker."""
    if err := expect(gw.post(request('')), 500, 'compiler_internal_error'):
        return err
    time.sleep(0.3)
    left = [p for p in gw.marks_of('pid') if alive(p)]
    return f'жив worker {left}' if left else None


def t_worker_garbage(gw):
    return expect(gw.post(request('')), 500, 'compiler_internal_error')


def t_worker_bad_code(gw):
    """Код ошибки, которого worker не может вернуть, — внутренняя ошибка."""
    return expect(gw.post(request('')), 500, 'compiler_internal_error')


def t_worker_stderr_flood(gw):
    """Мегабайт в stderr не вешает worker: шлюз дочитывает и отбрасывает."""
    return expect(gw.post(request('')), 200)


def t_worker_env(gw):
    if err := expect(gw.post(request('')), 200):
        return err
    env = json.loads(gw.marks_of('env')[0])
    if env['env']:
        return f'окружение не пусто: {sorted(env["env"])}'
    if env['mode'] != 0o700:
        return f'каталог {oct(env["mode"])}'
    if env['fds'][:3] != [0, 1, 2] or len(env['fds']) > 4:  # 4-й — сам listdir
        return f'дескрипторы {env["fds"]}'
    time.sleep(0.2)
    return f'каталог не удалён: {env["cwd"]}' if os.path.exists(env['cwd']) else None


def t_log_private(gw):
    """В журнале нет исходника и адреса клиента — только хэш."""
    marker = 'SECRET_SOURCE_MARKER_7f3a'
    gw.post(request(f'fn main() {{\n    let {marker} = 1\n}}\n'))
    gw.post(request(f'fn {marker}( {{'))
    # Строка журнала пишется после закрытия соединения — дождаться обеих.
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if len([ln for ln in gw.log_lines() if ln.get('event') == 'request']) >= 2:
            break
        time.sleep(0.05)
    text = gw.log_text()
    if marker in text:
        return 'исходник в журнале'
    if '127.0.0.1' in text.replace('"api":[["127.0.0.1"', '').replace('"metrics":[["127.0.0.1"', ''):
        return 'адрес клиента в журнале'
    reqs = [ln for ln in gw.log_lines() if ln.get('event') == 'request']
    return None if len(reqs) == 2 and len(reqs[0]['client']) == 16 else f'журнал {reqs}'


def t_metrics(gw):
    gw.post(request(''))
    r = gw.get('/metrics', port=gw.metrics_port)
    if r[0] != 200 or b'dplang_requests_total{code="result"} 1' not in r[2]:
        return f'метрики {r[0]} {r[2][:300]!r}'
    if gw.get('/metrics')[0] != 404:
        return 'метрики на адресе API'
    return None


def t_survives(gw):
    """После злоупотреблений шлюз жив и отвечает."""
    t_http_malformed(gw)
    t_header_limits(gw)
    t_payload_too_large(gw)
    for _ in range(50):
        gw.raw(b'\x00\xff' * 100, half_close=True)
    return expect(gw.get('/api/v1/info'), 200) or expect(gw.post(request('fn main() {\n}\n')), 200)


def t_config_strict(tmp, release_dplang):
    """Неверный профиль — отказ запуска с кодом 2, а не тихое значение."""
    with open(PROFILE, encoding='utf-8') as f:
        cfg = json.load(f)
    bad = [
        ('лишний ключ', lambda c: c['limits'].update(extra=1)),
        ('нет ключа', lambda c: c['queue'].pop('length')),
        ('ноль', lambda c: c['limits'].update(nesting=0)),
        ('строка вместо числа', lambda c: c['worker'].update(cpu_seconds='2')),
        ('http в CORS', lambda c: c['cors'].update(origins=['http://dpsmpx.com'])),
        ('неверный прокси', lambda c: c.update(trusted_proxies=['not-an-ip'])),
    ]
    for what, change in bad:
        c = json.loads(json.dumps(cfg))
        change(c)
        path = os.path.join(tmp, 'bad.json')
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(c, f)
        r = subprocess.run([sys.executable, '-I', GATEWAY, '--config', path, '--worker', release_dplang, '--listen',
                            '127.0.0.1:0'], capture_output=True, text=True, timeout=30)
        if r.returncode != 2 or 'error:' not in r.stderr:
            return f'{what}: код {r.returncode}: {r.stderr[-200:]}'
    with open(os.path.join(tmp, 'dup.json'), 'w', encoding='utf-8') as f:
        f.write('{"profile": "a", "profile": "b"}')
    r = subprocess.run([sys.executable, '-I', GATEWAY, '--config', os.path.join(tmp, 'dup.json'), '--worker',
                        release_dplang, '--listen', '127.0.0.1:0'], capture_output=True, text=True, timeout=30)
    return None if r.returncode == 2 else f'повторный ключ: код {r.returncode}'


def t_worker_relative_path(tmp, release_dplang):
    """Относительный --worker, как в docs/service/README.md: worker
    запускается из временного каталога, и путь должен остаться верным."""
    gw = Gateway(tmp, release_dplang, cwd=ROOT)
    try:
        return expect(gw.post(request('fn main() {\n}\n')), 200)
    finally:
        gw.stop()


def t_profile_shipped(tmp, release_dplang):
    """Поставляемый профиль: запуск выключен (Р49), шлюз с ним стартует."""
    with open(PROFILE, encoding='utf-8') as f:
        cfg = json.load(f)
    if cfg['available'] is not False:
        return 'available в поставляемом профиле должно быть false до решения автора (Р49)'
    if cfg['cors']['origins'] != ['https://dpsmpx.com']:
        return f'cors.origins {cfg["cors"]["origins"]}'
    gw = Gateway(tmp, release_dplang, available=False)
    try:
        return t_unavailable(gw)
    finally:
        gw.stop()


# Тест → (worker, замены профиля). Ключ замены — «раздел__ключ».
GATEWAY_CASES = {
    'compile_ok': (t_compile_ok, 'real', {}),
    'compile_errors': (t_compile_errors, 'real', {}),
    'deterministic': (t_deterministic, 'real', {}),
    'invalid_json': (t_invalid_json, 'real', {}),
    'unsupported_language': (t_unsupported_language, 'real', {}),
    'limit_source': (t_limit_source, 'real', {}),
    'limit_nesting': (t_limit_nesting, 'real', {}),
    'memory_bomb': (t_memory_bomb, 'real', {'worker__memory_bytes': 256 << 20, 'worker__cpu_seconds': 5}),
    'cpu_bomb': (t_cpu_bomb, 'real', {'worker__cpu_seconds': 1}),
    'info': (t_info, 'real', {}),
    'unavailable': (t_unavailable, 'real', {'available': False}),
    'security_headers': (t_security_headers, 'real', {}),
    'cors': (t_cors, 'real', {}),
    'routes': (t_routes, 'real', {}),
    'media_type': (t_media_type, 'real', {}),
    'payload_too_large': (t_payload_too_large, 'real', {}),
    'http_malformed': (t_http_malformed, 'real', {}),
    'header_limits': (t_header_limits, 'real', {}),
    'slow_headers': (t_slow_headers, 'real', {'http__read_timeout_ms': 300}),
    'slow_body': (t_slow_body, 'real', {'http__read_timeout_ms': 300}),
    'short_body': (t_short_body, 'real', {}),
    'connection_limit': (t_connection_limit, 'real', {'http__connections': 4}),
    'rate_limit_ip': (t_rate_limit_ip, 'real', {'rate_limit__per_ip_burst': 3, 'rate_limit__per_ip_per_minute': 1}),
    'rate_limit_global': (t_rate_limit_global, 'real', {
        'rate_limit__global_burst': 4, 'rate_limit__global_per_minute': 1, 'trusted_proxies': ['127.0.0.1/32']}),
    'rate_limit_ipv6': (t_rate_limit_ipv6, 'real', {
        'rate_limit__per_ip_burst': 1, 'rate_limit__per_ip_per_minute': 1, 'trusted_proxies': ['127.0.0.0/8']}),
    'forwarded_untrusted': (t_forwarded_untrusted, 'real', {
        'rate_limit__per_ip_burst': 1, 'rate_limit__per_ip_per_minute': 1}),
    'rate_table_bounded': (t_rate_table_bounded, 'real', {
        'rate_limit__per_ip_burst': 1, 'rate_limit__per_ip_per_minute': 1, 'rate_limit__table_size': 2,
        'trusted_proxies': ['127.0.0.1/32']}),
    'queue_full': (t_queue_full, 'slow:1p0', {'queue__concurrency': 1, 'queue__length': 1}),
    'queue_timeout': (t_queue_timeout, 'slow:1p5', {'queue__concurrency': 1, 'queue__wait_ms': 200}),
    'concurrency': (t_concurrency, 'slow:0p3', {'queue__concurrency': 2, 'queue__length': 32,
                                               'queue__wait_ms': 30000, 'rate_limit__per_ip_burst': 100}),
    'worker_crash': (t_worker_crash, 'crash', {}),
    'worker_hang': (t_worker_hang, 'hang', {'run__timeout_ms': 500}),
    'worker_flood': (t_worker_flood, 'flood', {}),
    'worker_garbage': (t_worker_garbage, 'garbage', {}),
    'worker_bad_code': (t_worker_bad_code, 'bad_code', {}),
    'worker_stderr_flood': (t_worker_stderr_flood, 'stderr_flood', {}),
    'worker_env': (t_worker_env, 'env', {}),
    'log_private': (t_log_private, 'real', {}),
    'metrics': (t_metrics, 'real', {}),
    'survives': (t_survives, 'real', {}),
}
# Тесты без готового шлюза: (tmp, release_dplang) → сбой или None.
PLAIN_CASES = {
    'config_strict': t_config_strict,
    'profile_shipped': t_profile_shipped,
    'worker_relative_path': t_worker_relative_path,
}
CASES = sorted(GATEWAY_CASES) + sorted(PLAIN_CASES)


def check_case(name, release_dplang):
    tmp = tempfile.mkdtemp(prefix='dpservice-')
    try:
        if name in PLAIN_CASES:
            return PLAIN_CASES[name](tmp, release_dplang)
        fn, worker, overrides = GATEWAY_CASES[name]
        gw = Gateway(tmp, release_dplang, worker, **overrides)
        try:
            err = fn(gw)
        finally:
            gw.stop()
        if gw.proc.returncode != 0:
            return f'шлюз завершился с кодом {gw.proc.returncode}: {gw.log_text()[-300:]}'
        return err
    except Exception as e:  # сбой теста — тоже сбой, с причиной
        return f'{type(e).__name__}: {e}'
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
