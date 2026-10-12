#!/usr/bin/env python3
"""Повторная генерация привязок даёт те же байты (Р58): каждый генератор
bindings/gen/ сверяет свой модуль с файлом в репозитории. Шаг
tools/check/run.sh «привязки»."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GENERATORS = ['vulkan.py']


def main():
    names = []
    for g in GENERATORS:
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, g), '--check'], capture_output=True, text=True)
        if r.returncode != 0:
            print((r.stdout + r.stderr).strip())
            return 1
        names.append(g[:-len('.py')])
    print(f'привязки: совпадают с генерацией ({", ".join(names)})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
