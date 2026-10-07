"""HTTP/1.1 шлюза: строгий разбор запроса и сборка ответа.

Поддержано ровно то, что нужно контракту /api/v1: запрос с телом по
Content-Length, без chunked и без keep-alive — после ответа соединение
закрывается. Всё неожиданное — ошибка 400, а не догадка: так у запроса
одно толкование и у шлюза, и у прокси перед ним (без request smuggling).
"""
import asyncio
import re

REASONS = {
    200: 'OK', 204: 'No Content', 400: 'Bad Request', 404: 'Not Found', 405: 'Method Not Allowed',
    408: 'Request Timeout', 411: 'Length Required', 413: 'Content Too Large', 415: 'Unsupported Media Type',
    422: 'Unprocessable Content', 429: 'Too Many Requests', 431: 'Request Header Fields Too Large',
    500: 'Internal Server Error', 503: 'Service Unavailable',
}

TOKEN = re.compile(rb'[!#$%&\'*+\-.^_`|~0-9A-Za-z]+')
REQUEST_LINE = re.compile(rb'([A-Z]+) (/[\x21-\x7e]*) HTTP/1\.([01])')
# Значение заголовка: видимые ASCII, пробел и табуляция.
VALUE = re.compile(rb'[\x20-\x7e\t]*')
# Заголовки, у которых повтор меняет смысл запроса.
SINGLE = {b'content-length', b'content-type', b'transfer-encoding', b'host', b'origin', b'x-forwarded-for'}


class HttpError(Exception):
    """Ответ-ошибка до разбора тела: статус, код контракта, сообщение."""

    def __init__(self, status, code, message):
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message


class Request:
    def __init__(self, method, path, headers):
        self.method = method
        self.path = path
        self.headers = headers  # имя в нижнем регистре → значение (str)
        self.body = b''

    def header(self, name, default=None):
        return self.headers.get(name, default)


async def read_head(reader, http_cfg):
    """Строка запроса и заголовки. None — клиент закрыл соединение молча."""
    timeout = http_cfg['read_timeout_ms'] / 1000
    try:
        head = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), timeout)
    except asyncio.IncompleteReadError:
        return None
    except asyncio.LimitOverrunError:
        raise HttpError(431, 'invalid_request', 'request headers are too large')
    except asyncio.TimeoutError:
        raise HttpError(408, 'invalid_request', 'request headers were not received in time')
    lines = head[:-4].split(b'\r\n')
    m = REQUEST_LINE.fullmatch(lines[0])
    if not m:
        raise HttpError(400, 'invalid_request', 'malformed request line')
    method = m.group(1).decode('ascii')
    target = m.group(2).decode('ascii')
    path = target.split('?', 1)[0]
    if len(lines) - 1 > http_cfg['headers']:
        raise HttpError(431, 'invalid_request', 'too many request headers')
    headers = {}
    for line in lines[1:]:
        name, sep, value = line.partition(b':')
        if not sep or not TOKEN.fullmatch(name):
            raise HttpError(400, 'invalid_request', 'malformed request header')
        value = value.strip(b' \t')
        if not VALUE.fullmatch(value):
            raise HttpError(400, 'invalid_request', 'malformed request header')
        key = name.lower()
        if key in headers and key in SINGLE:
            raise HttpError(400, 'invalid_request', f'repeated header {key.decode()}')
        headers[key] = value
    req = Request(method, path, {k.decode('ascii'): v.decode('ascii') for k, v in headers.items()})
    if 'transfer-encoding' in req.headers:
        raise HttpError(400, 'invalid_request', 'Transfer-Encoding is not supported; send Content-Length')
    return req


def content_length(req):
    """Длина тела; HttpError, если её нет или она неверна."""
    v = req.header('content-length')
    if v is None:
        raise HttpError(411, 'invalid_request', 'Content-Length is required')
    if not v.isdigit() or len(v) > 12:
        raise HttpError(400, 'invalid_request', 'malformed Content-Length')
    return int(v)


async def read_body(reader, req, n, http_cfg):
    timeout = http_cfg['read_timeout_ms'] / 1000
    try:
        req.body = await asyncio.wait_for(reader.readexactly(n), timeout)
    except asyncio.IncompleteReadError:
        raise HttpError(400, 'invalid_request', 'request body is shorter than Content-Length')
    except asyncio.TimeoutError:
        raise HttpError(408, 'invalid_request', 'request body was not received in time')


def response(status, body, headers):
    """Байты ответа: статус, заголовки, тело; соединение закрывается."""
    out = [f'HTTP/1.1 {status} {REASONS[status]}']
    for k, v in headers:
        out.append(f'{k}: {v}')
    out.append(f'Content-Length: {len(body)}')
    out.append('Connection: close')
    return ('\r\n'.join(out) + '\r\n\r\n').encode('ascii') + body
