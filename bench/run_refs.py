#!/usr/bin/env python3
"""Проверяет эталоны эталонного набора задач (bench/tasks).

Для каждой задачи: собирает эталон (C++20, без исключений и RTTI,
-Wall -Wextra -Werror; с ASan и UBSan, если компилятор их умеет), запускает
и сравнивает вывод с expected.txt. Эталон — ref.cpp или, у задачи из
нескольких модулей, все ref/*.cpp.

Задачи на правку проверяются ещё и на непустоту:
- buggy.cpp (исправление ошибки) НЕ должен проходить;
- before/ (рефакторинг) должен давать тот же вывод своим драйвером, а
  новый драйвер ref/main.cpp со старыми модулями — НЕ проходить.

    python3 bench/run_refs.py           — проверить
    python3 bench/run_refs.py --bless   — записать expected.txt по эталонам
                                          (только осознанно, с ревью диффа)

Компилятор — $CXX или g++. Вывод краткий: итог и первый сбой.
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
TASKS = os.path.join(ROOT, 'tasks')
CXX = os.environ.get('CXX', 'g++')
BASE_FLAGS = ['-std=c++20', '-O1', '-pthread', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti']
SAN_FLAGS = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']


def sanitizers_work(tmp):
    src = os.path.join(tmp, 'probe.cpp')
    with open(src, 'w') as f:
        f.write('int main() { return 0; }\n')
    r = subprocess.run([CXX] + BASE_FLAGS + SAN_FLAGS + [src, '-o', os.path.join(tmp, 'probe')],
                       capture_output=True)
    return r.returncode == 0


def build_run(srcs, exe, flags):
    """Собирает srcs (файл или список; заголовки ищутся рядом) и запускает."""
    if isinstance(srcs, str):
        srcs = [srcs]
    inc = ['-I', os.path.dirname(srcs[0])]
    r = subprocess.run([CXX] + flags + inc + srcs + ['-o', exe], capture_output=True, text=True)
    if r.returncode != 0:
        return None, 'сборка: ' + r.stderr.strip().splitlines()[0] if r.stderr.strip() else 'сборка'
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r, None


def check_before(t, before, ref_dir, exp_path, tmp, flags):
    """Рефакторинг: старое поведение то же, новый драйвер со старыми модулями не проходит."""
    with open(exp_path) as f:
        expected = f.read()
    out = []
    rb, err = build_run(sorted(glob.glob(os.path.join(before, '*.cpp'))), os.path.join(tmp, t + '_before'), flags)
    if err or rb.returncode != 0 or rb.stdout != expected:
        out.append(f'{t}: before/ не даёт ожидаемый вывод — поведение до и после расходится')
    mixed = os.path.join(tmp, t + '_mixed')
    shutil.copytree(before, mixed)
    shutil.copy(os.path.join(ref_dir, 'main.cpp'), os.path.join(mixed, 'main.cpp'))
    rm, errm = build_run(sorted(glob.glob(os.path.join(mixed, '*.cpp'))), mixed + '.exe', flags)
    if not errm and rm.returncode == 0 and rm.stdout == expected:
        out.append(f'{t}: новый драйвер проходит со старыми модулями — задача на рефакторинг пустая')
    return out


def main():
    bless = '--bless' in sys.argv
    tasks = sorted(d for d in os.listdir(TASKS) if os.path.isdir(os.path.join(TASKS, d)))
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        flags = BASE_FLAGS + (SAN_FLAGS if sanitizers_work(tmp) else [])
        for t in tasks:
            d = os.path.join(TASKS, t)
            ref_dir = os.path.join(d, 'ref')
            ref = sorted(glob.glob(os.path.join(ref_dir, '*.cpp'))) if os.path.isdir(ref_dir) \
                else os.path.join(d, 'ref.cpp')
            exp_path = os.path.join(d, 'expected.txt')
            r, err = build_run(ref, os.path.join(tmp, t), flags)
            if err or r.returncode != 0:
                failures.append(f'{t}: эталон: {err or "код возврата " + str(r.returncode)}')
                continue
            if bless:
                with open(exp_path, 'w') as f:
                    f.write(r.stdout)
            else:
                if not os.path.exists(exp_path):
                    failures.append(f'{t}: нет expected.txt')
                    continue
                with open(exp_path) as f:
                    if f.read() != r.stdout:
                        failures.append(f'{t}: вывод эталона расходится с expected.txt')
                        continue
            buggy = os.path.join(d, 'buggy.cpp')
            if os.path.exists(buggy):
                rb, errb = build_run(buggy, os.path.join(tmp, t + '_buggy'), flags)
                with open(exp_path) as f:
                    expected = f.read()
                if not errb and rb.returncode == 0 and rb.stdout == expected:
                    failures.append(f'{t}: buggy.cpp проходит — задача на исправление пустая')
            before = os.path.join(d, 'before')
            if os.path.isdir(before):
                failures += check_before(t, before, ref_dir, exp_path, tmp, flags)
    if failures:
        print('эталоны: сбоев ' + str(len(failures)))
        for line in failures:
            print('  ' + line)
        return 1
    san = 'с ASan/UBSan' if SAN_FLAGS[0] in flags else 'без санитайзеров'
    print(f'эталоны: {len(tasks)} задач {"записаны" if bless else "совпадают"} ({CXX}, {san})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
