#!/usr/bin/env python3
"""Шлюз онлайн-компилятора dpl — временный, на Python 3 stdlib (Р49).

Заменяется шлюзом на dpl, когда язык сможет: FFI (v0.2), потоки
(v0.3), ввод-вывод в библиотеке. Приёмка замены — тесты tests/service.
Запуск и устройство — dpgate/main.py, docs/service/README.md.
"""
import os
import sys

# Пакет dpgate — рядом. Каталог добавляется явно: шлюз запускают с -I
# (без PYTHONPATH и без текущего каталога в пути импорта).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dpgate.main import main  # noqa: E402

if __name__ == '__main__':
    sys.exit(main())
