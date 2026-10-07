"""Пределы нагрузки шлюза: rate limit и очередь к worker-ам.

Rate limit — token bucket на адрес клиента и общий на весь шлюз.
Адрес IPv6 считается по сети /ipv6_prefix: у одного абонента обычно вся
сеть /64. Таблица адресов ограничена: при переполнении вытесняется
давно не виденный. Время — монотонные часы; функция часов задаётся
снаружи, чтобы тесты не зависели от настоящего времени.

Очередь: занято меньше concurrency — компилировать сразу; иначе ждать,
если ждущих меньше length; иначе отказ queue_full. Ожидание дольше
wait_ms — queue_timeout.
"""
import asyncio
import collections
import ipaddress


class Bucket:
    def __init__(self, per_minute, burst, now):
        self.rate = per_minute / 60.0
        self.burst = float(burst)
        self.tokens = float(burst)
        self.at = now

    def take(self, now):
        """True — токен взят. Иначе — секунды до следующего токена."""
        self.tokens = min(self.burst, self.tokens + (now - self.at) * self.rate)
        self.at = now
        if self.tokens >= 1.0:
            self.tokens -= 1.0
            return True
        return (1.0 - self.tokens) / self.rate


def client_key(addr, ipv6_prefix):
    """Ключ клиента: адрес IPv4 или сеть IPv6 /ipv6_prefix."""
    ip = ipaddress.ip_address(addr)
    if ip.version == 6 and ip.ipv4_mapped:
        ip = ip.ipv4_mapped
    if ip.version == 6:
        return str(ipaddress.ip_network(f'{ip}/{ipv6_prefix}', strict=False))
    return str(ip)


def client_address(peer, forwarded_for, trusted):
    """Адрес клиента. X-Forwarded-For учитывается, только если соединение
    пришло от доверенного прокси: тогда клиент — самый правый адрес цепочки,
    который не доверенный прокси."""
    try:
        ip = ipaddress.ip_address(peer)
    except ValueError:
        return peer
    if not forwarded_for or not any(ip in net for net in trusted):
        return str(ip)
    for part in reversed([p.strip() for p in forwarded_for.split(',')]):
        try:
            hop = ipaddress.ip_address(part)
        except ValueError:
            return str(ip)  # неразборчивая цепочка — верим только соединению
        if not any(hop in net for net in trusted):
            return str(hop)
    return str(ip)


class RateLimiter:
    def __init__(self, cfg, clock):
        self.cfg = cfg
        self.clock = clock
        self.table = collections.OrderedDict()
        self.all = Bucket(cfg['global_per_minute'], cfg['global_burst'], clock())

    def check(self, key):
        """None — можно; иначе секунды, через которые повторить."""
        now = self.clock()
        b = self.table.get(key)
        if b is None:
            b = Bucket(self.cfg['per_ip_per_minute'], self.cfg['per_ip_burst'], now)
            self.table[key] = b
            if len(self.table) > self.cfg['table_size']:
                self.table.popitem(last=False)
        else:
            self.table.move_to_end(key)
        r = b.take(now)
        if r is not True:
            return r
        r = self.all.take(now)
        if r is not True:
            b.tokens = min(b.burst, b.tokens + 1.0)  # общий отказ не тратит токен клиента
            return r
        return None


class QueueFull(Exception):
    pass


class QueueTimeout(Exception):
    pass


class Admission:
    """Допуск к worker-ам: не больше concurrency сразу, length ждущих.

    Ждущие — очередь FIFO. Освободившееся место передаётся первому ждущему
    сразу, без окна, в котором его занял бы опоздавший."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.active = 0
        self.queue = collections.deque()

    @property
    def waiting(self):
        return len(self.queue)

    async def __aenter__(self):
        if self.active < self.cfg['concurrency'] and not self.queue:
            self.active += 1
            return self
        if len(self.queue) >= self.cfg['length']:
            raise QueueFull()
        turn = asyncio.get_running_loop().create_future()
        self.queue.append(turn)
        try:
            await asyncio.wait_for(asyncio.shield(turn), self.cfg['wait_ms'] / 1000)
        except (asyncio.TimeoutError, asyncio.CancelledError) as e:
            if turn.done():  # место передано в тот же миг — вернуть его
                self._release()
            else:
                self.queue.remove(turn)
                turn.cancel()
            if isinstance(e, asyncio.CancelledError):
                raise
            raise QueueTimeout() from None
        return self

    async def __aexit__(self, *exc):
        self._release()
        return False

    def _release(self):
        while self.queue:
            turn = self.queue.popleft()
            if not turn.done():
                turn.set_result(None)  # место переходит ждущему, active не меняется
                return
        self.active -= 1
