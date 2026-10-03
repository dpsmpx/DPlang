#!/usr/bin/env python3
"""Проверяет решения эталонного набора на вариантах синтаксиса (v0.0, шаг 0.4).

Для каждой задачи с каталогом cm/ (bench/tasks/tNN/cm):
- исходники в варианте A — в каноническом виде (печать A → A ничего не
  меняет);
- для каждого варианта A, B, C: исходник печатается в записи варианта,
  разбирается снова, печать идемпотентна, переводится в C++ (cm.py),
  собирается с драйвером cm/driver.cpp и даёт вывод, равный expected.txt;
- задача на исправление (cm/buggy.cm) — неисправленная версия НЕ
  проходит;
- задача на рефакторинг (cm/before/, cm/ref/) — старые модули со старым
  драйвером cm/before_driver.cpp проходят, новые модули с новым драйвером
  проходят, старые модули с новым драйвером — НЕ проходят.

Флаги сборки — как у эталонов (bench/run_refs.py): C++20, -Werror, без
исключений и RTTI, с ASan и UBSan, если компилятор их умеет.

    python3 bench/syntax/run_variants.py     — проверить (входит в run.sh)

Вывод краткий: итог и сбои.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
TASKS = os.path.join(BENCH, 'tasks')
sys.path.insert(0, HERE)
sys.path.insert(0, BENCH)

import cm  # noqa: E402
import run_refs  # noqa: E402

VARIANTS = sorted(cm.VARIANTS)


def to_variant(paths, variant):
    """Модули из файлов варианта A → тексты в записи варианта (имя → текст)."""
    mods = cm.load(paths, 'A')
    info = cm.Info(mods)
    return {m.name: cm.print_module(m, variant, info) for m in mods}


def translate(texts, variant, out_dir):
    """Тексты варианта → cm_out.hpp в out_dir. Проверяет идемпотентность печати."""
    mods = []
    for name, src in texts.items():
        try:
            mods.append(cm.parse(src, variant, name))
        except cm.CmError as e:
            raise cm.CmError(f'{name} ({variant}): разбор: {e}') from None
    info = cm.Info(mods)
    for m in mods:
        again = cm.print_module(m, variant, info)
        if again != texts[m.name]:
            raise cm.CmError(f'{m.name} ({variant}): печать не идемпотентна')
    code = cm.translate(mods, {m.name: variant for m in mods})
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'cm_out.hpp'), 'w', encoding='utf-8') as f:
        f.write(code)


def build_run(driver, out_dir, flags):
    """Собирает драйвер с cm_out.hpp из out_dir; (вывод или None, ошибка)."""
    exe = os.path.join(out_dir, 'driver')
    r = subprocess.run([run_refs.CXX] + flags + ['-I', HERE, '-I', out_dir, driver, '-o', exe],
                       capture_output=True, text=True)
    if r.returncode != 0:
        first = next((ln for ln in r.stderr.splitlines() if 'error' in ln), r.stderr.strip()[:200])
        return None, 'сборка: ' + first
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        return None, f'код возврата {r.returncode}: {r.stderr.strip()[:200]}'
    return r.stdout, None


def case(name, srcs, driver, expected, out_dir, flags, must_pass=True):
    """Один прогон: модули (вариант A) → каждый вариант → сборка → сверка."""
    fails = []
    for v in VARIANTS:
        d = os.path.join(out_dir, v)
        try:
            translate(to_variant(srcs, v), v, d)
        except cm.CmError as e:
            if must_pass:
                fails.append(f'{name} {v}: {e}')
            continue
        out, err = build_run(driver, d, flags)
        ok = err is None and out == expected
        if must_pass and not ok:
            fails.append(f'{name} {v}: {err or "вывод расходится с expected.txt"}')
        if not must_pass and ok:
            fails.append(f'{name} {v}: проходит, а не должен — задача пустая')
    return fails


def canonical(paths):
    """Исходники варианта A в каноническом виде."""
    fails = []
    for p in paths:
        with open(p, encoding='utf-8') as f:
            src = f.read()
        mods = cm.load([p], 'A')
        if cm.print_module(mods[0], 'A', cm.Info(mods)) != src:
            fails.append(f'{os.path.relpath(p, TASKS)}: не в каноническом виде (cm.py fmt -v A)')
    return fails


def check_task(t, tmp, flags):
    d = os.path.join(TASKS, t)
    cmd = os.path.join(d, 'cm')
    with open(os.path.join(d, 'expected.txt')) as f:
        expected = f.read()
    out = os.path.join(tmp, t)
    fails = []
    try:
        if os.path.isdir(os.path.join(cmd, 'ref')):
            before = sorted(glob.glob(os.path.join(cmd, 'before', '*.cm')))
            ref = sorted(glob.glob(os.path.join(cmd, 'ref', '*.cm')))
            fails += canonical(before + ref)
            fails += case(t + ' ref', ref, os.path.join(cmd, 'driver.cpp'), expected, out + '/ref', flags)
            fails += case(t + ' before', before, os.path.join(cmd, 'before_driver.cpp'), expected,
                          out + '/before', flags)
            fails += case(t + ' before+новый драйвер', before, os.path.join(cmd, 'driver.cpp'), expected,
                          out + '/mixed', flags, must_pass=False)
        else:
            sol = [os.path.join(cmd, 'sol.cm')]
            fails += canonical(sol)
            fails += case(t, sol, os.path.join(cmd, 'driver.cpp'), expected, out + '/sol', flags)
            buggy = os.path.join(cmd, 'buggy.cm')
            if os.path.exists(buggy):
                fails += canonical([buggy])
                fails += case(t + ' buggy', [buggy], os.path.join(cmd, 'driver.cpp'), expected,
                              out + '/buggy', flags, must_pass=False)
    except (cm.CmError, OSError) as e:
        fails.append(f'{t}: {e}')
    return fails


def main():
    tasks = sorted(t for t in os.listdir(TASKS) if os.path.isdir(os.path.join(TASKS, t, 'cm')))
    with tempfile.TemporaryDirectory() as tmp:
        flags = run_refs.BASE_FLAGS + (run_refs.SAN_FLAGS if run_refs.sanitizers_work(tmp) else [])
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
            results = list(pool.map(lambda t: check_task(t, tmp, flags), tasks))
    fails = [f for r in results for f in r]
    if fails:
        print('варианты: сбоев ' + str(len(fails)))
        for line in fails:
            print('  ' + line)
        return 1
    san = 'с ASan/UBSan' if run_refs.SAN_FLAGS[0] in flags else 'без санитайзеров'
    print(f'варианты: {len(tasks)} задач × {len(VARIANTS)} варианта совпадают ({run_refs.CXX}, {san})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
