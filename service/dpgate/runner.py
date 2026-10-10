"""Запуск `dpl worker` на один запрос (Р49, «Изоляция»).

Шлюз сам не компилирует. Worker — отдельный процесс в своей группе
(setsid), с пустым окружением и временным рабочим каталогом 0700;
пределы компиляции и процесса — аргументами из профиля (rlimits и
песочницу worker ставит себе сам до чтения входа). Шлюз ограничивает
время по часам и объём вывода; по таймауту или переполнению вывода
группа worker-а убивается SIGKILL.
"""
import asyncio
import os
import shutil
import signal
import tempfile
import time


class Outcome:
    """Итог запуска: kind — ok, timeout, crash, overflow, spawn_failed."""

    def __init__(self, kind, stdout=b'', stderr=b'', returncode=None, duration_ms=0):
        self.kind = kind
        self.stdout = stdout
        self.stderr = stderr
        self.returncode = returncode
        self.duration_ms = duration_ms


async def _read_capped(stream, cap, drain):
    """Читает до конца; хранит не больше cap байт. (данные, переполнено).
    drain=False — при переполнении чтение прекращается сразу (stdout:
    worker убивается); drain=True — остаток читается и отбрасывается
    (stderr: иначе worker встал бы на полном канале)."""
    chunks, n, over = [], 0, False
    while True:
        chunk = await stream.read(65536)
        if not chunk:
            return b''.join(chunks)[:cap], over
        if not over:
            chunks.append(chunk)
        n += len(chunk)
        if n > cap:
            over = True
            if not drain:
                return b''.join(chunks)[:cap], True


def _kill_group(proc):
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


async def _discard(stream):
    """Дочитывает канал до конца и отбрасывает данные."""
    while await stream.read(65536):
        pass


async def _reap(proc, timeout):
    """Убивает группу worker-а и ждёт его конца, не дольше timeout на шаг.

    proc.wait() возвращается, только когда закрыты все каналы. Канал,
    остановленный с непрочитанными данными (переполнение, таймаут),
    asyncio сам не закрывает — поэтому каналы дочитываются и
    отбрасываются. Если канал держит процесс вне группы worker-а, ждать
    дальше нельзя: запрос не должен висеть."""
    _kill_group(proc)
    try:
        await asyncio.wait_for(asyncio.gather(_discard(proc.stdout), _discard(proc.stderr)), timeout)
        await asyncio.wait_for(proc.wait(), timeout)
    except asyncio.TimeoutError:
        pass


async def run(argv, body, run_cfg):
    """Запускает worker с телом запроса в stdin; Outcome."""
    workdir = tempfile.mkdtemp(prefix='dpworker-')  # 0700
    start = time.monotonic()
    try:
        try:
            proc = await asyncio.create_subprocess_exec(
                *argv, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE, env={}, cwd=workdir, start_new_session=True, close_fds=True)
        except OSError as e:
            return Outcome('spawn_failed', stderr=str(e).encode())
        timeout = run_cfg['timeout_ms'] / 1000
        try:
            return await asyncio.wait_for(_exchange(proc, body, run_cfg, start), timeout)
        except asyncio.TimeoutError:
            await _reap(proc, timeout)
            return Outcome('timeout', returncode=proc.returncode, duration_ms=_ms(start))
        finally:
            if proc.returncode is None:
                await _reap(proc, timeout)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def _ms(start):
    return int((time.monotonic() - start) * 1000)


async def _exchange(proc, body, run_cfg, start):
    async def feed():
        try:
            proc.stdin.write(body)
            await proc.stdin.drain()
        except (BrokenPipeError, ConnectionResetError):
            pass  # worker закрыл вход сам (например, предел тела)
        finally:
            proc.stdin.close()

    feeding = asyncio.ensure_future(feed())
    out_task = asyncio.ensure_future(_read_capped(proc.stdout, run_cfg['stdout_bytes'], False))
    err_task = asyncio.ensure_future(_read_capped(proc.stderr, run_cfg['stderr_bytes'], True))
    try:
        out, out_over = await out_task
        if out_over:
            _kill_group(proc)  # сразу: иначе worker встанет на полном канале
            await _discard(proc.stdout)  # иначе канал не закроется и wait() повиснет (_reap)
        err, _ = await err_task
    finally:
        for t in (feeding, out_task, err_task):
            t.cancel()
        # Отменённые задачи дожидаются: читать канал после них может _reap.
        await asyncio.gather(feeding, out_task, err_task, return_exceptions=True)
    if out_over:
        await proc.wait()
        return Outcome('overflow', out, err, proc.returncode, _ms(start))
    await proc.wait()
    kind = 'ok' if proc.returncode in (0, 4) else 'crash'
    return Outcome(kind, out, err, proc.returncode, _ms(start))
