#!/usr/bin/env python3
"""Проверяет относительные ссылки в документах Markdown.

Документы ссылаются друг на друга (CLAUDE.md, «Где лежат не-кодовые
файлы»: при переносе обновить все ссылки). Ловит ссылку на файл или
каталог, которого нет. Внешние адреса (http, https, mailto) и якоря
внутри страницы не проверяются.
"""
import os
import re
import subprocess
import sys
from urllib.parse import unquote

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LINK = re.compile(r'\[[^\]]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)')
CODE_BLOCK = re.compile(r'```.*?```', re.S)
INLINE_CODE = re.compile(r'`[^`\n]*`')


def markdown_files():
    out = subprocess.run(['git', '-C', ROOT, 'ls-files', '-z', '--', '*.md'], capture_output=True, check=True)
    return [f for f in out.stdout.decode('utf-8', 'replace').split('\0') if f]


def main():
    errors = []
    files = markdown_files()
    for rel in files:
        with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
            text = f.read()
        text = CODE_BLOCK.sub('', text)
        text = INLINE_CODE.sub('', text)
        base = os.path.dirname(os.path.join(ROOT, rel))
        for m in LINK.finditer(text):
            target = m.group(1)
            if re.match(r'^[a-z]+:', target) or target.startswith('#'):
                continue
            path = unquote(target.split('#', 1)[0])
            if not path:
                continue
            if not os.path.exists(os.path.normpath(os.path.join(base, path))):
                line = text.count('\n', 0, m.start()) + 1
                errors.append(f'{rel}: нет цели ссылки «{target}» (строка ~{line})')
    if errors:
        print('ссылки: битых ' + str(len(errors)))
        for e in errors:
            print('  ' + e)
        return 1
    print(f'ссылки: целы ({len(files)} документов)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
