"""Профиль онлайн-сервиса: service/config/online-v1.json (Р49).

Профиль — единственное место всех пределов: шлюз берёт из него свои, а
пределы компиляции и процесса передаёт worker-у аргументами. Загрузка
строгая: лишний или пропущенный ключ, неверный тип или ноль — ошибка
запуска, а не тихое значение по умолчанию. Сообщения — по-английски,
как у компилятора (Р47).
"""
import ipaddress
import json

POSITIVE = 'positive'

# Раздел → ключ → тип. POSITIVE — целое больше нуля.
SCHEMA = {
    'profile': str,
    'available': bool,
    'limits': {
        'request_bytes': POSITIVE,
        'source_bytes': POSITIVE,
        'lines': POSITIVE,
        'identifier_bytes': POSITIVE,
        'nesting': POSITIVE,
        'diagnostics': POSITIVE,
        'output_bytes': POSITIVE,
    },
    'worker': {
        'memory_bytes': POSITIVE,
        'cpu_seconds': POSITIVE,
        'stack_bytes': POSITIVE,
    },
    'run': {
        'timeout_ms': POSITIVE,
        'stdout_bytes': POSITIVE,
        'stderr_bytes': POSITIVE,
    },
    'queue': {
        'concurrency': POSITIVE,
        'length': POSITIVE,
        'wait_ms': POSITIVE,
    },
    'rate_limit': {
        'per_ip_per_minute': POSITIVE,
        'per_ip_burst': POSITIVE,
        'global_per_minute': POSITIVE,
        'global_burst': POSITIVE,
        'ipv6_prefix': POSITIVE,
        'table_size': POSITIVE,
    },
    'http': {
        'header_bytes': POSITIVE,
        'headers': POSITIVE,
        'read_timeout_ms': POSITIVE,
        'connections': POSITIVE,
    },
    'cors': {
        'origins': list,
        'max_age_s': POSITIVE,
    },
    'trusted_proxies': list,
}

# Пределы, которые worker получает аргументами: --request-bytes=… и т. д.
WORKER_SECTIONS = ('limits', 'worker')


class ConfigError(Exception):
    pass


def _no_duplicates(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise ConfigError(f'duplicate key {k!r}')
        out[k] = v
    return out


def _check(value, schema, where):
    if isinstance(schema, dict):
        if not isinstance(value, dict):
            raise ConfigError(f'{where}: expected an object')
        extra = sorted(set(value) - set(schema))
        missing = sorted(set(schema) - set(value))
        if extra:
            raise ConfigError(f'{where}: unknown keys {extra}')
        if missing:
            raise ConfigError(f'{where}: missing keys {missing}')
        for k, sub in schema.items():
            _check(value[k], sub, f'{where}.{k}' if where else k)
        return
    if schema is POSITIVE:
        if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
            raise ConfigError(f'{where}: expected a positive integer')
        return
    if not isinstance(value, schema) or (schema is not bool and isinstance(value, bool)):
        raise ConfigError(f'{where}: expected {schema.__name__}')


def load(path):
    """Профиль из файла; ConfigError — если он неверен."""
    try:
        with open(path, encoding='utf-8') as f:
            cfg = json.load(f, object_pairs_hook=_no_duplicates)
    except (OSError, ValueError) as e:
        raise ConfigError(f'{path}: {e}') from e
    _check(cfg, SCHEMA, '')
    for origin in cfg['cors']['origins']:
        if not isinstance(origin, str) or not origin.startswith('https://') or origin.endswith('/'):
            raise ConfigError(f'cors.origins: {origin!r} must be an https:// origin without a path')
    try:
        cfg['trusted_proxies'] = [ipaddress.ip_network(p) for p in cfg['trusted_proxies']]
    except (TypeError, ValueError) as e:
        raise ConfigError(f'trusted_proxies: {e}') from e
    if not 1 <= cfg['rate_limit']['ipv6_prefix'] <= 128:
        raise ConfigError('rate_limit.ipv6_prefix must be from 1 to 128')
    if cfg['run']['stdout_bytes'] <= cfg['limits']['output_bytes']:
        raise ConfigError('run.stdout_bytes must be greater than limits.output_bytes')
    return cfg


def worker_flags(cfg):
    """Аргументы `dplang worker` из профиля: --request-bytes=262144 …"""
    flags = []
    for section in WORKER_SECTIONS:
        for k, v in cfg[section].items():
            flags.append(f'--{k.replace("_", "-")}={v}')
    return flags


def public_limits(cfg):
    """Пределы для /api/v1/info: то, что видит пользователь сайта."""
    return dict(cfg['limits'])
