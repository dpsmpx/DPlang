"""Общее генераторов привязок (bindings/gen/): имена dpl, запись и
сверка результата (Р58)."""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Ключевые слова dpl (compiler/syntax/tokens.def) и зарезервированное.
KEYWORDS = {'fn', 'struct', 'enum', 'const', 'import', 'pub', 'let', 'var', 'if', 'else', 'while', 'for', 'in', 'match',
            'return', 'break', 'continue', 'defer', 'errdefer', 'fail', 'try', 'parallel', 'none', 'true', 'false', 'mut',
            'extern', 'unsafe'}


class Skip(Exception):
    """Объявление не выражается в dpl — пропуск с причиной."""


def snake(name):
    """camelCase → snake_case: pNext → p_next, deviceUUID → device_uuid,
    windowID → window_id; совпадение с ключевым словом — с `_` в конце."""
    out = []
    for i, c in enumerate(name):
        if c.isupper():
            prev = name[i - 1] if i else ''
            nxt = name[i + 1] if i + 1 < len(name) else ''
            if prev.islower() or (prev.isupper() and nxt.islower()):
                out.append('_')
            out.append(c.lower())
        else:
            out.append(c)
    s = ''.join(out)
    return s + '_' if s in KEYWORDS else s


def main(argv, generate, out, doc, script):
    """Запись модуля out или (--check) сверка с генерацией; код выхода."""
    ap = argparse.ArgumentParser(description=doc.split('\n')[0])
    ap.add_argument('--check', action='store_true', help='сверить модуль с генерацией')
    a = ap.parse_args(argv)
    text = generate()
    rel = os.path.relpath(out, ROOT)
    if a.check:
        try:
            with open(out, encoding='utf-8') as f:
                old = f.read()
        except FileNotFoundError:
            old = None
        if old != text:
            print(f'{rel}: не совпадает с генерацией (python3 bindings/gen/{script})')
            return 1
        print(f'{rel}: совпадает с генерацией')
        return 0
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print(f'{rel}: записан, строк {text.count(chr(10))}')
    return 0


if __name__ == '__main__':
    sys.exit('common.py — модуль генераторов, не программа')
