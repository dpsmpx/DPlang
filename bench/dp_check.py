#!/usr/bin/env python3
"""Проверка решения задачи набора на настоящем компиляторе DPlang (v0.1, шаг 13).

Решение — модули .dpm (имя файла — имя модуля). Рядом кладётся скрытый
драйвер задачи `main.dpm` из bench/tasks/<задача>/dp/, программа
собирается `dplang build`, запускается и её вывод сверяется с ожидаемым:
dp/expected.txt, если он есть, иначе expected.txt задачи.

Итог — класс и сообщение (классы — как у bench/experiment.py):
- разбор — синтаксическая ошибка (коды E01xx и E02xx) в файлах решения
  или неверное имя файла;
- сборка — другая ошибка компилятора в файлах решения;
- интерфейс — ошибки только в драйвере: решение не даёт того, на что он
  опирается;
- ловушка — программа завершилась с кодом не 0 (ловушка — код 101);
- время — не закончила за RUN_TIMEOUT секунд;
- вывод — вывод не совпал с ожидаемым;
- компилятор — внутренняя ошибка компилятора или окружения (коды 2 и 3):
  сбой обвязки, а не решения, — разбирается вручную;
- принято.

Компилятор — $DPLANG или build/compiler/dplang. Модуль служит и проверке
обвязки (bench/experiment.py, язык dp), и проходу bench в tests/run.py.
"""
import os
import re
import subprocess

BENCH = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BENCH)
DRIVER = 'main.dpm'
RUN_TIMEOUT = 20
BUILD_TIMEOUT = 120
ACCEPTED = 'принято'
HEAD = re.compile(r'^(?P<file>[^:\s][^:]*):(?P<line>\d+):(?P<col>\d+): error\[(?P<code>E\d{4})\]: ')
MODULE_FILE = re.compile(r'^[a-z_][a-z0-9_]*\.dpm$')


def dplang():
    return os.environ.get('DPLANG') or os.path.join(ROOT, 'build', 'compiler', 'dplang')


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def expected(task_path):
    own = os.path.join(task_path, 'dp', 'expected.txt')
    return read(own if os.path.exists(own) else os.path.join(task_path, 'expected.txt'))


def errors(stderr):
    """Блоки ошибок компилятора: (файл, код, текст блока) по порядку."""
    out = []
    for ln in stderr.splitlines():
        m = HEAD.match(ln)
        if m:
            out.append([m.group('file'), m.group('code'), [ln]])
        elif out and ln.strip():
            out[-1][2].append(ln)
    return [(f, c, '\n'.join(b)) for f, c, b in out]


def diagnose(stderr):
    """Отказ компилятора (код 1) → (класс, сообщение)."""
    errs = errors(stderr)
    if not errs:
        first = stderr.strip().splitlines()[0] if stderr.strip() else 'нет сообщения'
        return 'компилятор', 'компилятор отказал без диагностики: ' + first
    more = f'\n(всего ошибок: {len(errs)})' if len(errs) > 1 else ''
    own = [e for e in errs if os.path.basename(e[0]) != DRIVER]
    if not own:
        return 'интерфейс', 'драйвер не собирается с решением:\n' + errs[0][2] + more
    _, code, block = own[0]
    return ('разбор' if code[:3] in ('E01', 'E02') else 'сборка'), block + more


def compare(want, got):
    exp, out = want.splitlines(), got.splitlines()
    if exp == out:
        return None
    for k in range(max(len(exp), len(out))):
        e = exp[k] if k < len(exp) else None
        g = out[k] if k < len(out) else None
        if e != g:
            fe = f'«{e}»' if e is not None else 'конец вывода'
            fg = f'«{g}»' if g is not None else 'конец вывода'
            return f'строка {k + 1}: ожидалось {fe}, получено {fg} (строк: ожидалось {len(exp)}, получено {len(out)})'
    return None


def evaluate(files, driver_text, want, build_dir, compiler=None):
    """Собирает файлы решения (имя → текст) с драйвером в build_dir,
    запускает и сверяет с want: (класс, сообщение)."""
    compiler = compiler or dplang()
    for name in files:
        if name == DRIVER:
            return 'разбор', f'имя {DRIVER} занято драйвером: переименуйте модуль'
        if not MODULE_FILE.match(name):
            return 'разбор', f'{name}: имя модуля — строчные латинские буквы, цифры и _, файл — <имя>.dpm'
    os.makedirs(build_dir, exist_ok=True)
    for name, text in list(files.items()) + [(DRIVER, driver_text)]:
        with open(os.path.join(build_dir, name), 'w', encoding='utf-8') as f:
            f.write(text)
    exe = os.path.join(build_dir, 'program')
    try:
        r = subprocess.run([compiler, 'build', '-o', exe, DRIVER], capture_output=True, text=True,
                           errors='backslashreplace', timeout=BUILD_TIMEOUT, cwd=build_dir)
    except subprocess.TimeoutExpired:
        return 'компилятор', f'сборка не закончилась за {BUILD_TIMEOUT} с'
    except OSError as e:
        return 'компилятор', f'не запускается {compiler}: {e}'
    if r.returncode == 1:
        return diagnose(r.stderr)
    if r.returncode != 0:
        lines = r.stderr.strip().splitlines()
        return 'компилятор', f'код {r.returncode}: ' + (lines[0] if lines else 'нет сообщения')
    try:
        p = subprocess.run([exe], capture_output=True, text=True, errors='backslashreplace',
                           timeout=RUN_TIMEOUT, cwd=build_dir)
    except subprocess.TimeoutExpired:
        return 'время', f'драйвер не закончил за {RUN_TIMEOUT} с'
    if p.returncode != 0:
        lines = p.stderr.strip().splitlines()
        first = next((ln for ln in lines if ': trap: ' in ln), lines[0] if lines else '')
        return 'ловушка', f'код возврата {p.returncode}' + (f': {first}' if first else '')
    diff = compare(want, p.stdout)
    if diff:
        return 'вывод', diff
    return ACCEPTED, ''
