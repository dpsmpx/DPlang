#!/usr/bin/env python3
"""Ищет секреты в файлах репозитория.

Репозиторий закрыт, а поиска секретов GitHub в закрытом репозитории
личного аккаунта нет — его заменяет эта проверка (CLAUDE.md, «Защита и
оригинальность»). Ловит токены распространённых служб, закрытые ключи и
файлы, которым в репозитории не место.

Проверяются файлы, которые отслеживает Git; без Git — все файлы, кроме
каталогов сборки.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

PATTERNS = [
    ('токен GitHub', re.compile(r'\bgh[pousr]_[A-Za-z0-9]{36,}\b')),
    ('токен GitHub', re.compile(r'\bgithub_pat_[A-Za-z0-9_]{60,}\b')),
    ('ключ AWS', re.compile(r'\bAKIA[0-9A-Z]{16}\b')),
    ('ключ Google API', re.compile(r'\bAIza[0-9A-Za-z_\-]{35}\b')),
    ('токен Slack', re.compile(r'\bxox[abprs]-[0-9A-Za-z\-]{10,}\b')),
    ('ключ Anthropic или OpenAI', re.compile(r'\bsk-(?:ant-)?[A-Za-z0-9_\-]{32,}\b')),
    ('закрытый ключ', re.compile(r'-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |ENCRYPTED )?PRIVATE KEY-----')),
]

FORBIDDEN = re.compile(r'(\.keystore|\.jks|\.p12|\.pfx|\.key|(^|/)\.env|(^|/)\.git-credentials)$')
SKIP_DIRS = ('build/', '.git/')


def tracked_files():
    try:
        out = subprocess.run(['git', '-C', ROOT, 'ls-files', '-z'], capture_output=True, check=True)
        return [f for f in out.stdout.decode('utf-8', 'replace').split('\0') if f]
    except (OSError, subprocess.CalledProcessError):
        found = []
        for root, dirs, names in os.walk(ROOT):
            rel = os.path.relpath(root, ROOT)
            dirs[:] = [d for d in dirs if not (os.path.join(rel, d) + '/').lstrip('./').startswith(SKIP_DIRS)]
            found += [os.path.normpath(os.path.join(rel, n)) for n in names]
        return found


def main():
    errors = []
    for rel in tracked_files():
        if rel.startswith(SKIP_DIRS):
            continue
        if FORBIDDEN.search(rel):
            errors.append(f'{rel}: такому файлу не место в репозитории')
            continue
        path = os.path.join(ROOT, rel)
        try:
            with open(path, 'rb') as f:
                data = f.read(2_000_000)
        except OSError:
            continue
        if b'\0' in data[:8192]:
            continue
        text = data.decode('utf-8', 'replace')
        for name, rx in PATTERNS:
            for m in rx.finditer(text):
                line = text.count('\n', 0, m.start()) + 1
                errors.append(f'{rel}:{line}: похоже на {name}')
    if errors:
        print('секреты: найдено ' + str(len(errors)))
        for e in errors:
            print('  ' + e)
        return 1
    print('секреты: нет')
    return 0


if __name__ == '__main__':
    sys.exit(main())
