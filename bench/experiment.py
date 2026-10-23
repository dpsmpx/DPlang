#!/usr/bin/env python3
"""Измерительная обвязка опыта (v0.0, шаг 0.2, Р35; v0.1, шаг 13).

Модель (субагент с чистым контекстом) решает задачу эталонного набора на
C++, на варианте синтаксиса dpl (A, B, C — прототип v0.0) или на
dpl (dp — настоящий компилятор v0.1), зная язык только по
спецификации. Обвязка:

- prepare — песочница задания: PROMPT.md (правила, постановка, у t13 —
  целевой интерфейс, у dpl — спецификация варианта) и начальный код в
  src/ (заготовка, код с ошибками или модули до рефакторинга — в записи
  варианта); драйвер и ожидаемый вывод в песочницу не попадают;
- check — единственная проверка, доступная модели: перевод прототипом
  (варианты) или компилятор dpl (dp, bench/dp_check.py), сборка со
  скрытым драйвером, запуск, сверка с ожидаемым; итог — класс и первая
  ошибка; каждая попытка и снимок кода — в журнал;
- prepare --series — серия (Р-17): задачи по очереди в одной сессии
  субагента, спецификация читается один раз; задание серии —
  RUN/series/LANG/PROMPT.md;
- session — записать расход сессии субагента (из итога инструмента Agent;
  у серии — TASK = series);
- usage — разложить расход по транскрипту субагента: обвязка, задание,
  работа; по шагам — контекст, прочитанное и написанное (только числа);
- tokens — точный счёт токенов API Claude (count_tokens, ключ
  ANTHROPIC_API_KEY): прочитано на входе, прочитано диагностик, написано,
  итоговое решение;
- report — таблица прогона и сводка по языкам; --save — в bench/results/
  (журналы и задания ячеек);
- restore — вернуть сохранённый прогон из bench/results/ в рабочий каталог,
  чтобы досчитать токены или отчёт в другой сессии;
- selftest — проверка самой обвязки (шаг «обвязка» в tools/check/run.sh).

    python3 bench/experiment.py prepare RUN [--tasks t01,t05] [--langs cpp,A] [--series] [--doc spec|guide]
    python3 bench/experiment.py check DIR
    python3 bench/experiment.py session RUN TASK|series LANG --tokens N [--tool-uses K] [--seconds S]
    python3 bench/experiment.py usage RUN TASK|series LANG --transcript FILE
    python3 bench/experiment.py tokens RUN [--model M]
    python3 bench/experiment.py report RUN [--save]
    python3 bench/experiment.py restore RUN [--force]
    python3 bench/experiment.py selftest

Прогоны лежат в $DPL_EXP_WORK или build/experiment/. Подробно —
bench/README.md.
"""
import argparse
import concurrent.futures
import datetime
import difflib
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

BENCH = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BENCH)
TASKS = os.path.join(BENCH, 'tasks')
SYNTAX = os.path.join(BENCH, 'syntax')
RESULTS = os.path.join(BENCH, 'results')
sys.path.insert(0, BENCH)
sys.path.insert(0, SYNTAX)

import cm  # noqa: E402
import dp_check  # noqa: E402
import mkspec  # noqa: E402
import run_refs  # noqa: E402

LANGS = ('cpp', 'A', 'B', 'C', 'dp')
PROTO = ('A', 'B', 'C')
TITLE = {'cpp': 'C++', 'A': 'dpl, вариант A', 'B': 'dpl, вариант B', 'C': 'dpl, вариант C',
         'dp': 'dpl'}
# Спецификация для языка dp: docs/spec/core.md с первого раздела.
SPEC_DP = os.path.join(ROOT, 'docs', 'spec', 'core.md')
SPEC_DP_TITLE = "# Спецификация dpl (ядро v0.1)"
# Вместо спецификации задание может нести краткое руководство (--doc guide):
# приёмы с примерами, docs/guide/README.md.
GUIDE_DP = os.path.join(ROOT, 'docs', 'guide', 'README.md')
# Замер О4 (--doc o4): руководство серий k2-h с разделом о правиле
# исключительного доступа (Р66); контроль (--doc o4base) — то же
# руководство без этого раздела. Правило проверяет компилятор (до Р66 —
# прототип bench/o4/rule.py, коммит 2bfd91d).
GUIDE_O4 = os.path.join(BENCH, 'o4', 'guide.md')
O4_SECTION = '## Обход и изменение\n'
DOCS = ('spec', 'guide', 'o4', 'o4base')
DOC_WORD = {'spec': 'спецификации', 'guide': 'руководству', 'o4': 'руководству', 'o4base': 'руководству'}
ATTEMPTS = 5
RUN_TIMEOUT = 20
COUNT_MODEL = 'claude-opus-5-5'
MARK = '// --- driver ---'
ACCEPTED = 'принято'
# Классы итога проверки. «прототип» и «компилятор» — сбой переводчика или
# компилятора dpl, а не решения: такие попытки разбираются вручную.
CLASSES = ('разбор', 'перевод', 'сборка', 'интерфейс', 'ловушка', 'время', 'вывод', 'прототип', 'компилятор',
           ACCEPTED)
SRC_EXT = {'cpp': ('.cpp', '.hpp', '.h'), 'proto': ('.cm',), 'dp': ('.dpm',)}
MAX_SRC = 200_000


class HarnessError(Exception):
    pass


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)


def work_dir():
    return os.environ.get('DPL_EXP_WORK') or os.path.join(ROOT, 'build', 'experiment')


def is_dp(lang):
    """Язык — dpl: вариант прототипа или настоящий (dp)."""
    return lang != 'cpp'


def is_proto(lang):
    return lang in PROTO


def ext_key(lang):
    return 'proto' if is_proto(lang) else lang


def demote(text, by=1):
    """Заголовки Markdown на уровень ниже (вне блоков кода)."""
    out, fence = [], False
    for line in text.splitlines():
        if line.startswith('```'):
            fence = not fence
        elif not fence and line.startswith('#'):
            line = '#' * by + line
        out.append(line)
    return '\n'.join(out) + '\n'


# ---------------------------------------------------------------- задачи


class Task:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.id = self.name.split('_')[0]
        self.cm = os.path.join(path, 'cm')
        self.dp = os.path.join(path, 'dp')
        if os.path.isdir(os.path.join(path, 'before')):
            self.kind = 'refactor'
        elif os.path.exists(os.path.join(path, 'buggy.cpp')):
            self.kind = 'fix'
        else:
            self.kind = 'write'
        self.expected = read(os.path.join(path, 'expected.txt'))

    def langs(self):
        """Языки задачи: C++ всегда; варианты — если есть cm/, dp — если есть dp/."""
        return [lang for lang in LANGS if lang == 'cpp' or (is_proto(lang) and os.path.isdir(self.cm))
                or (lang == 'dp' and os.path.isdir(self.dp))]

    def dp_files(self, which):
        if self.kind == 'refactor':
            d = os.path.join(self.dp, 'before' if which == 'start' else 'ref')
            return {os.path.basename(p): read(p) for p in sorted(glob.glob(os.path.join(d, '*.dpm')))}
        name = 'sol.dpm' if which == 'ref' else ('buggy.dpm' if self.kind == 'fix' else 'stub.dpm')
        return {'sol.dpm': read(os.path.join(self.dp, name))}

    def cm_paths(self, which):
        if self.kind == 'refactor':
            return sorted(glob.glob(os.path.join(self.cm, 'before' if which == 'start' else 'ref', '*.cm')))
        if which == 'ref':
            return [os.path.join(self.cm, 'sol.cm')]
        return [os.path.join(self.cm, 'buggy.cm' if self.kind == 'fix' else 'stub.cm')]

    def files(self, lang, which):
        """Начальный код (which='start') или эталон ('ref'): имя в src/ → текст."""
        if lang == 'dp':
            return self.dp_files(which)
        if is_proto(lang):
            mods = cm.load(self.cm_paths(which), 'A')
            info = cm.Info(mods)
            texts = {m.name + '.cm': cm.print_module(m, lang, info) for m in mods}
            if self.kind == 'refactor':
                return texts
            return {'sol.cm': next(iter(texts.values()))}
        if self.kind == 'refactor':
            d = os.path.join(self.path, 'before' if which == 'start' else 'ref')
            return {os.path.basename(p): read(p) for p in sorted(glob.glob(os.path.join(d, '*.?pp')))
                    if os.path.basename(p) != 'main.cpp'}
        name = {'start': 'buggy.cpp' if self.kind == 'fix' else 'stub.cpp', 'ref': 'ref.cpp'}[which]
        text = read(os.path.join(self.path, name))
        if MARK in text:
            text = text.split(MARK)[0].rstrip('\n') + '\n'
        return {'sol.cpp': text}

    def cpp_driver(self):
        """Драйвер однофайловой задачи на C++: его #include и всё под чертой."""
        top, drv = read(os.path.join(self.path, 'ref.cpp')).split(MARK, 1)
        incs = [ln for ln in top.splitlines() if ln.startswith('#include')]
        return '\n'.join(incs) + '\n' + MARK + drv

    def interface(self, lang):
        """Целевой интерфейс задачи на рефакторинг: имя → текст (у остальных — пусто)."""
        if self.kind != 'refactor':
            return {}
        if lang == 'cpp':
            return {os.path.basename(p): read(p) for p in sorted(glob.glob(os.path.join(self.path, 'ref', '*.hpp')))}
        if lang == 'dp':
            return {name: dp_interface(text) for name, text in self.dp_files('ref').items()}
        mods = cm.load(self.cm_paths('ref'), 'A')
        info = cm.Info(mods)
        out = {}
        for m in mods:
            items, pending = [], []
            for it in m.items:
                if isinstance(it, cm.Comment):
                    pending.append(it)
                    continue
                if isinstance(it, cm.Import) or getattr(it, 'pub', False):
                    if isinstance(it, cm.Fn):
                        it = cm.Fn(it.name, it.params, it.ret, cm.Block([cm.Comment('...')]), it.pub, it.attrs)
                    items += pending + [it]
                pending = []
            out[m.name + '.cm'] = cm.print_module(cm.Module(m.name, items), lang, info)
        return out


def dp_interface(text):
    """Открытый интерфейс модуля dpl в каноническом виде (dpl fmt):
    импорты и pub-объявления с комментариями над ними; тела функций — `// ...`."""
    lines = text.splitlines()
    out, pending, i = [], [], 0
    while i < len(lines):
        ln = lines[i]
        if not ln.strip():
            pending = []
            i += 1
            continue
        if ln.startswith('//'):
            pending.append(ln)
            i += 1
            continue
        j = i
        if ln.endswith('{'):
            while j < len(lines) and lines[j] != '}':
                j += 1
        item = lines[i:j + 1]
        keep = ln.startswith('import ') or ln.startswith('pub ')
        if keep and ln.startswith('pub fn ') and ln.endswith('{'):
            item = [ln, '    // ...', '}']
        if keep:
            if out and not (ln.startswith('import ') and out[-1].startswith('import ')):
                out.append('')
            out += pending + item
        pending = []
        i = j + 1
    return '\n'.join(out) + '\n'


def all_tasks():
    return [Task(os.path.join(TASKS, t)) for t in sorted(os.listdir(TASKS))
            if os.path.isdir(os.path.join(TASKS, t, 'cm')) or os.path.isdir(os.path.join(TASKS, t, 'dp'))]


def find_task(key):
    for t in all_tasks():
        if key in (t.id, t.name):
            return t
    raise HarnessError(f'нет задачи {key}')


# ---------------------------------------------------------------- задание


def cxx_line(flags):
    san = ', с ASan и UBSan' if run_refs.SAN_FLAGS[0] in flags else ''
    base = ' '.join(run_refs.BASE_FLAGS)
    return f'`{os.path.basename(run_refs.CXX)} {base}`{san}'


def prompt(task, lang, sandbox, attempts, flags, start, doc='spec'):
    exe = os.path.join(BENCH, 'experiment.py')
    dp = is_dp(lang)
    files = ', '.join(f'`src/{n}`' for n in start)
    lines = [
        f'# Задача {task.id} на {TITLE[lang]}', '',
        'Задание опыта: написать код по постановке'
        + (f', зная язык только по {DOC_WORD[doc] if lang == "dp" else "спецификации"} в конце этого файла.'
           if dp else '.'), '',
        '## Правила', '',
        f'- Работай только в папке `{sandbox}`: читай этот файл и `src/`, правь файлы в `src/`. '
        'Других файлов не открывай, по диску и в сети не ищи.',
        '- Компилятор и другие программы сам не запускай. Проверка — только командой', '',
        f'      python3 {exe} check {sandbox}', '',
        '  Она ' + {'proto': 'переводит код прототипом языка в C++, собирает его',
                    'dp': 'собирает код компилятором dpl',
                    'cpp': 'собирает его'}[ext_key(lang)]
        + ' со скрытым драйвером, запускает и сравнивает вывод с ожидаемым; '
        'печатает «принято» или первую ошибку.',
        f'- Попыток проверки — не больше {attempts}, каждая записывается. Проверяй, когда решение '
        'готово целиком, а не для разведки.',
        '- Работа кончается на «принято» или когда попытки кончились. Ответ — одна строка: '
        'номер последней попытки и её итог.', '',
        '## Код', '',
    ]
    if task.kind == 'write':
        lines.append(f'В {files} — заготовка: объявления, на которые опирается драйвер. Напиши тела '
                     'функций. Сигнатуры, перечисления и поля структур не меняй; поля структур с '
                     'пометкой «на ваш выбор» — свои. Вспомогательные функции и типы добавлять можно.')
    elif task.kind == 'fix':
        lines.append(f'В {files} — реализация с ошибками. Найди и исправь ошибки, не переписывая '
                     'решение заново; сигнатуры не меняй.')
    else:
        lines.append(f'В {files} — модули до рефакторинга. Измени их так, чтобы интерфейс стал как в '
                     'разделе «Целевой интерфейс»; поведение не меняется.')
    lines.append('')
    if is_proto(lang):
        lines.append('Ошибки типов прототип находит при сборке переведённого C++: сообщение называет '
                     'функцию, в которой ошибка, а текст — от компилятора C++.')
    elif lang == 'dp':
        mods = 'модули `src/<имя>.dpm`' if task.kind == 'refactor' else 'модуль `sol` (файл `src/sol.dpm`)'
        lines.append(f'Решение — {mods}. Скрытый драйвер — главный модуль `main`: он импортирует '
                     + ('эти модули' if task.kind == 'refactor' else '`sol`')
                     + ' и вызывает открытые (`pub`) функции. Ошибки — диагностика компилятора как есть: '
                     'файл, строка, код ошибки и место в коде.')
    elif task.kind == 'refactor':
        lines.append(f'Сборка — {cxx_line(flags)}. Драйвер `main.cpp` подключает заголовки модулей и '
                     'собирается вместе со всеми `src/*.cpp`.')
    else:
        lines.append(f'Сборка — {cxx_line(flags)}. Драйвер дописывается в конец `src/sol.cpp`.')
    lines.append('')
    text = '\n'.join(lines) + '\n' + demote(read(os.path.join(task.path, 'task.md')))
    iface = task.interface(lang)
    if iface:
        fence = 'dpl' if dp else 'cpp'
        text += '\n## Целевой интерфейс\n'
        for name, body in iface.items():
            text += f'\n`{name}`:\n\n```{fence}\n{body}```\n'
    if is_proto(lang):
        spec = read(mkspec.out_path(lang))
        spec = re.sub(r'\A<!--.*?-->\n+', '', spec, flags=re.S)
        text += '\n' + demote(spec)
    elif lang == 'dp':
        text += '\n' + demote(dp_spec(doc))
    return text


def dp_spec(doc='spec'):
    """Документ языка dp для задания: docs/spec/core.md с раздела 1 или
    краткое руководство docs/guide/README.md целиком."""
    if doc == 'guide':
        return read(GUIDE_DP)
    if doc == 'o4':
        return read(GUIDE_O4)
    if doc == 'o4base':
        text = read(GUIDE_O4)
        start = text.index(O4_SECTION)
        return text[:start] + text[text.index('\n## ', start + 1) + 1:]
    body = read(SPEC_DP)
    return SPEC_DP_TITLE + '\n\n' + body[body.index('\n## 1.') + 1:]


def git_commit():
    r = subprocess.run(['git', '-C', ROOT, 'rev-parse', 'HEAD'], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ''


def cxx_version():
    r = subprocess.run([run_refs.CXX, '--version'], capture_output=True, text=True)
    return r.stdout.splitlines()[0] if r.returncode == 0 and r.stdout else run_refs.CXX


def build_flags():
    with tempfile.TemporaryDirectory() as tmp:
        return run_refs.BASE_FLAGS + (run_refs.SAN_FLAGS if run_refs.sanitizers_work(tmp) else [])


def cell_paths(run_dir, task_id, lang):
    return os.path.join(run_dir, task_id, lang), os.path.join(run_dir, '_log', task_id, lang)


def snapshot(files, dest):
    os.makedirs(dest, exist_ok=True)
    for name, text in files.items():
        write(os.path.join(dest, name), text)


def read_files(d):
    if not os.path.isdir(d):
        return {}
    return {n: read(os.path.join(d, n)) for n in sorted(os.listdir(d)) if os.path.isfile(os.path.join(d, n))}


def prepare(run, tasks, langs, attempts=ATTEMPTS, agent_model='', force=False, quiet=False, series=False,
            doc='spec'):
    run_dir = os.path.join(work_dir(), run)
    flags = build_flags()
    common = {'run': run, 'commit': git_commit(), 'cxx': cxx_version(), 'flags': flags,
              'attempts_max': attempts, 'agent_model': agent_model, 'doc': doc}
    made = []
    in_series = {lang: [] for lang in langs}
    for task in tasks:
        for lang in langs:
            if lang not in task.langs():
                continue
            sandbox, log = cell_paths(run_dir, task.id, lang)
            if os.path.exists(log) and not force:
                raise HarnessError(f'{task.id} {lang}: уже есть в прогоне {run} (--force — пересоздать)')
            for d in (sandbox, log):
                if os.path.exists(d):
                    shutil.rmtree(d)
            start = task.files(lang, 'start')
            snapshot(start, os.path.join(sandbox, 'src'))
            text = prompt(task, lang, sandbox, attempts, flags, start, doc)
            if series:
                in_series[lang].append((task, sandbox, text))
                text = (f'# Задача {task.id} на {TITLE[lang]} — часть серии\n\nЗадание — в '
                        f'`{series_paths(run_dir, lang)[0]}`.\n')
            write(os.path.join(sandbox, 'PROMPT.md'), text)
            snapshot(start, os.path.join(log, 'attempt-0'))
            meta = dict(common, task=task.name, id=task.id, lang=lang, kind=task.kind, series=series,
                        created=datetime.datetime.now().isoformat(timespec='seconds'))
            write(os.path.join(log, 'meta.json'), json.dumps(meta, ensure_ascii=False, indent=1) + '\n')
            made.append(sandbox)
    if series:
        for lang, items in in_series.items():
            if not items:
                continue
            path, meta_path = series_paths(run_dir, lang)
            text = series_prompt(items, lang, attempts, doc)
            write(path, text)
            write(meta_path[:-len('.json')] + '.PROMPT.md', text)
            meta = dict(common, lang=lang, tasks=[t.id for t, _, _ in items],
                        created=datetime.datetime.now().isoformat(timespec='seconds'))
            write(meta_path, json.dumps(meta, ensure_ascii=False, indent=1) + '\n')
    if not quiet:
        for s in made:
            print(s)
        if series:
            for lang in langs:
                if in_series[lang]:
                    print(series_paths(run_dir, lang)[0])
            print('серия: на язык — один субагент с сообщением «Прочитай <серия>/PROMPT.md и выполни '
                  'задание оттуда.» (bench/README.md)')
        else:
            print(f'песочниц: {len(made)}. Сообщение субагенту — «Прочитай <песочница>/PROMPT.md и выполни '
                  'задание оттуда.» (bench/README.md)')
    return made


# Серия (Р-17): один субагент решает по очереди несколько задач в одной
# сессии; спецификация читается один раз. Задание серии — RUN/series/LANG/
# PROMPT.md, его копия и расход сессии — в RUN/_log/_series-LANG.*.
DOC_MARKS = ['\n' + demote(SPEC_DP_TITLE).rstrip('\n') + '\n',
             '\n' + demote(read(GUIDE_DP).splitlines()[0]).rstrip('\n') + '\n']


def series_paths(run_dir, lang):
    return (os.path.join(run_dir, 'series', lang, 'PROMPT.md'),
            os.path.join(run_dir, '_log', f'_series-{lang}.json'))


def task_part(text):
    """Часть задания ячейки от раздела «Код» до спецификации."""
    body = text[text.index('## Код\n'):]
    for mark in DOC_MARKS:
        if mark in body:
            body = body[:body.index(mark)]
    return body.rstrip('\n') + '\n'


def series_prompt(items, lang, attempts, doc='spec'):
    """Задание серии: правила один раз, задачи по порядку, спецификация один раз."""
    exe = os.path.join(BENCH, 'experiment.py')
    dp = is_dp(lang)
    n = len(items)
    what = {'proto': 'переводит код прототипом языка в C++, собирает его',
            'dp': 'собирает код компилятором dpl', 'cpp': 'собирает его'}[ext_key(lang)]
    lines = [
        f'# Серия из {n} задач на {TITLE[lang]}', '',
        f'Задание опыта: решить по очереди {n} задач в одной работе'
        + (f', зная язык только по {DOC_WORD[doc] if lang == "dp" else "спецификации"} в конце этого файла.'
           if dp else '.'), '',
        '## Правила', '',
        '- Задачи решаются по порядку, следующая — после того, как кончилась предыдущая. У каждой '
        'задачи своя папка (в её разделе): читай этот файл и `src/` папки задачи, правь файлы в её '
        '`src/`. Других файлов не открывай, по диску и в сети не ищи.',
        '- Компилятор и другие программы сам не запускай. Проверка задачи — только командой из её '
        f'раздела. Она {what} со скрытым драйвером, запускает и сравнивает вывод с ожидаемым; '
        'печатает «принято» или первую ошибку.',
        f'- Попыток проверки на задачу — не больше {attempts}, каждая записывается. Проверяй, когда '
        'решение готово целиком, а не для разведки.',
        '- Задача кончается на «принято» или когда попытки кончились. Ответ в конце — по строке на '
        'задачу: задача, номер последней попытки и её итог.', '',
    ]
    text = '\n'.join(lines)
    for k, (task, sandbox, cell_text) in enumerate(items, 1):
        text += (f'\n## Задача {k} из {n}: {task.id}\n\nПапка — `{sandbox}`. Проверка:\n\n'
                 f'      python3 {exe} check {sandbox}\n\n' + demote(task_part(cell_text)))
    if lang == 'dp':
        text += '\n' + demote(dp_spec(doc))
    elif is_proto(lang):
        spec = read(mkspec.out_path(lang))
        text += '\n' + demote(re.sub(r'\A<!--.*?-->\n+', '', spec, flags=re.S))
    return text


# ---------------------------------------------------------------- проверка

ERR = re.compile(r'^(?P<file>[^:\s][^:]*):(?P<line>\d+):(?P<col>\d+): (?:fatal )?error: (?P<msg>.*)$')
FN_HEAD = re.compile(r'^\S[^(]*?(\w+)\(.*\) \{$')


def compile_errors(stderr):
    """Первая ошибка сборки: (строки до неё включительно, сама ошибка, её фрагмент кода, всего ошибок)."""
    lines = stderr.splitlines()
    idx = [i for i, ln in enumerate(lines) if ERR.match(ln)]
    if not idx:
        return lines, None, [], 1
    i = idx[0]
    snippet = []
    for ln in lines[i + 1:i + 3]:
        if re.match(r'^\s*\d*\s*\|', ln):
            snippet.append(ln)
        else:
            break
    return lines[:i + 1], ERR.match(lines[i]), snippet, len(idx)


def plain(stderr, build_dir):
    """Сообщение без путей каталога сборки."""
    return stderr.replace(build_dir + os.sep, '').replace(SYNTAX + os.sep, '')


def generated_fn(code_lines, line):
    """Функция dpl, в переведённом коде которой стоит строка line (с 1)."""
    for k in range(min(line, len(code_lines)) - 1, -1, -1):
        m = FN_HEAD.match(code_lines[k])
        if m:
            name = m.group(1)
            return name[:-1] if name.endswith('_') and name[:-1] in cm.CPP_KEYWORDS else name
        if code_lines[k].startswith('struct ') or code_lines[k] == '}' and k < line - 1:
            break
    return None


def diagnose_build(stderr, lang, build_dir, own, driver, sol_lines=0, code_lines=None):
    """Ошибка сборки → (класс, сообщение)."""
    text = plain(stderr, build_dir)
    head, err, snippet, total = compile_errors(text)
    more = f'\n(всего ошибок: {total})' if total > 1 else ''
    if err is None:
        first = next((ln for ln in head if 'error' in ln or 'undefined reference' in ln), head[0] if head else '')
        return 'сборка', first.strip() + more
    msg = 'error: ' + err.group('msg')
    # Место ошибки: сама строка ошибки или ближайшее «required from» в своих файлах.
    where = None
    for ln in reversed(head):
        m = re.match(r'^(?:\s*from\s+|In file included from\s+)?([^:\s]+):(\d+)(?::\d+)?', ln.strip())
        if m and (os.path.basename(m.group(1)) in own or os.path.basename(m.group(1)) == driver):
            where = (os.path.basename(m.group(1)), int(m.group(2)), ln)
            if 'included from' not in ln and not ln.strip().startswith('from '):
                break
    if where is None:
        return 'сборка', msg + more
    fname, line, _ = where
    if is_proto(lang):
        if fname == driver:
            return 'интерфейс', 'драйвер не собирается с решением: ' + msg + more
        fn = generated_fn(code_lines or [], line)
        place = f'в функции {fn}' if fn else 'в объявлениях'
        return 'сборка', f'{place}: {msg}\n' + '\n'.join(snippet) + more
    if fname == driver or (fname == 'sol.cpp' and sol_lines and line > sol_lines):
        return 'интерфейс', 'драйвер не собирается с решением: ' + msg + more
    return 'сборка', f'{fname}:{line}: {msg}\n' + '\n'.join(snippet) + more


def diagnose_run(proc, build_dir):
    err = plain(proc.stderr, build_dir)
    for key in ('ловушка:', 'runtime error:', 'ERROR: AddressSanitizer', 'ThreadSanitizer', 'LeakSanitizer'):
        ln = next((x for x in err.splitlines() if key in x), None)
        if ln:
            return ln.strip()
    first = err.strip().splitlines()[0] if err.strip() else ''
    return f'код возврата {proc.returncode}' + (f': {first}' if first else '')


def compare(expected, got):
    exp, out = expected.splitlines(), got.splitlines()
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


def source_files(files, lang):
    ext = SRC_EXT[ext_key(lang)]
    return {n: t for n, t in files.items() if n.endswith(ext)}


def evaluate(task, lang, files, build_dir, flags, build=True):
    """Проверка решения: (класс, сообщение). build=False — только разбор и перевод."""
    files = source_files(files, lang)
    if not files:
        return 'разбор', 'в src/ нет файлов ' + ' '.join('*' + e for e in SRC_EXT[ext_key(lang)])
    if sum(len(t) for t in files.values()) > MAX_SRC:
        return 'разбор', f'файлы в src/ больше {MAX_SRC} символов'
    os.makedirs(build_dir, exist_ok=True)
    exe = os.path.join(build_dir, 'driver')
    code_lines = None
    sol_lines = 0
    if lang == 'dp':
        if not build:
            return None, ''
        driver = read(os.path.join(task.dp, dp_check.DRIVER))
        return dp_check.evaluate(files, driver, dp_check.expected(task.path), build_dir)
    if is_proto(lang):
        mods = []
        for name, text in files.items():
            try:
                mods.append(cm.parse(text, lang, os.path.splitext(name)[0]))
            except cm.CmError as e:
                return 'разбор', f'{name}:{e}'
            except Exception as e:  # noqa: BLE001 — сбой прототипа, не решения
                return 'прототип', f'разбор {name}: {type(e).__name__}: {e}'
        try:
            code = cm.translate(mods, {m.name: lang for m in mods})
        except cm.CmError as e:
            return 'перевод', str(e)
        except Exception as e:  # noqa: BLE001
            return 'прототип', f'перевод: {type(e).__name__}: {e}'
        if not build:
            return None, ''
        write(os.path.join(build_dir, 'cm_out.hpp'), code)
        code_lines = code.splitlines()
        driver = 'driver.cpp'
        shutil.copy(os.path.join(task.cm, 'driver.cpp'), os.path.join(build_dir, driver))
        own = {'cm_out.hpp'}
        cmd = [run_refs.CXX] + flags + ['-I', SYNTAX, '-I', build_dir, os.path.join(build_dir, driver), '-o', exe]
    else:
        if not build:
            return None, ''
        own = set(files)
        for name, text in files.items():
            write(os.path.join(build_dir, name), text)
        if task.kind == 'refactor':
            driver = 'main.cpp'
            shutil.copy(os.path.join(task.path, 'ref', 'main.cpp'), os.path.join(build_dir, driver))
        else:
            driver = '-'
            sol = files.get('sol.cpp')
            if sol is None:
                return 'разбор', 'нет файла src/sol.cpp'
            if not sol.endswith('\n'):
                sol += '\n'
            sol_lines = sol.count('\n')
            write(os.path.join(build_dir, 'sol.cpp'), sol + task.cpp_driver())
        srcs = sorted(glob.glob(os.path.join(build_dir, '*.cpp')))
        cmd = [run_refs.CXX] + flags + ['-I', build_dir] + srcs + ['-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return diagnose_build(r.stderr, lang, build_dir, own, driver, sol_lines, code_lines)
    try:
        p = subprocess.run([exe], capture_output=True, text=True, timeout=RUN_TIMEOUT, cwd=build_dir)
    except subprocess.TimeoutExpired:
        return 'время', f'драйвер не закончил за {RUN_TIMEOUT} с'
    if p.returncode != 0:
        return 'ловушка', diagnose_run(p, build_dir)
    diff = compare(task.expected, p.stdout)
    if diff:
        return 'вывод', diff
    return ACCEPTED, ''


def read_attempts(log):
    path = os.path.join(log, 'attempts.jsonl')
    if not os.path.exists(path):
        return []
    with open(path, encoding='utf-8') as f:
        return [json.loads(ln) for ln in f if ln.strip()]


def locate(sandbox):
    sandbox = os.path.abspath(sandbox.rstrip('/'))
    lang = os.path.basename(sandbox)
    task_id = os.path.basename(os.path.dirname(sandbox))
    run_dir = os.path.dirname(os.path.dirname(sandbox))
    log = os.path.join(run_dir, '_log', task_id, lang)
    if not os.path.exists(os.path.join(log, 'meta.json')):
        raise HarnessError(f'{sandbox}: не песочница опыта (нет журнала {log})')
    return sandbox, log


def check(sandbox):
    sandbox, log = locate(sandbox)
    meta = json.loads(read(os.path.join(log, 'meta.json')))
    attempts = read_attempts(log)
    done = next((a for a in attempts if a['class'] == ACCEPTED), None)
    if done:
        print(f'уже принято на попытке {done["n"]}; больше проверять не нужно')
        return 0
    if len(attempts) >= meta['attempts_max']:
        print(f'попытки кончились ({len(attempts)} из {meta["attempts_max"]}) — задача не принята')
        return 2
    n = len(attempts) + 1
    files = source_files(read_files(os.path.join(sandbox, 'src')), meta['lang'])
    snapshot(files, os.path.join(log, f'attempt-{n}'))
    task = find_task(meta['task'])
    t0 = time.monotonic()
    with tempfile.TemporaryDirectory() as tmp:
        cls, msg = evaluate(task, meta['lang'], files, tmp, meta['flags'])
    out = f'попытка {n} из {meta["attempts_max"]}: {cls}' + (f'\n{msg.rstrip()}' if msg else '')
    entry = {'n': n, 'time': datetime.datetime.now().isoformat(timespec='seconds'),
             'seconds': round(time.monotonic() - t0, 1), 'class': cls, 'output': out,
             'files': {k: hashlib.sha256(v.encode()).hexdigest()[:16] for k, v in files.items()}}
    with open(os.path.join(log, 'attempts.jsonl'), 'a', encoding='utf-8') as f:
        f.write(json.dumps(entry, ensure_ascii=False) + '\n')
    print(out)
    return 0 if cls == ACCEPTED else 1


def session(run, task_id, lang, tokens, tool_uses=None, seconds=None):
    if task_id == 'series':
        path = series_paths(os.path.join(work_dir(), run), lang)[1]
        if not os.path.exists(path):
            raise HarnessError(f'в прогоне {run} нет серии на {lang}')
    else:
        _, log = cell_paths(os.path.join(work_dir(), run), find_task(task_id).id, lang)
        path = os.path.join(log, 'meta.json')
    meta = json.loads(read(path))
    meta['session'] = {'total_tokens': tokens, 'tool_uses': tool_uses, 'seconds': seconds}
    write(path, json.dumps(meta, ensure_ascii=False, indent=1) + '\n')


# ---------------------------------------------------------------- токены


def added_lines(before, after):
    """Строки, появившиеся или изменённые от снимка before к снимку after."""
    out = []
    for name, text in after.items():
        old = before.get(name, '').splitlines()
        new = text.splitlines()
        for op, _, _, j1, j2 in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
            if op in ('replace', 'insert'):
                out += new[j1:j2]
    return '\n'.join(out)


def joined(files):
    return ''.join(f'=== {n}\n{t}' for n, t in files.items())


def cell_texts(run_dir, task_id, lang):
    """Тексты ячейки для счёта: прочитано на входе, диагностики, написано, итоговое решение."""
    sandbox, log = cell_paths(run_dir, task_id, lang)
    attempts = read_attempts(log)
    snaps = [read_files(os.path.join(log, f'attempt-{k}')) for k in range(len(attempts) + 1)]
    written = [added_lines(snaps[k - 1], snaps[k]) for k in range(1, len(snaps))]
    return {
        'prompt': read(os.path.join(sandbox, 'PROMPT.md')) + joined(snaps[0]),
        'diagnostics': '\n'.join(a['output'] for a in attempts),
        'written': '\n'.join(w for w in written if w),
        'final': joined(snaps[-1]) if attempts else '',
    }


class TokenCounter:
    """Счёт токенов POST /v1/messages/count_tokens (Р35); кэш — по тексту и модели."""

    def __init__(self, model, cache_path):
        self.model = model
        self.key = os.environ.get('ANTHROPIC_API_KEY', '')
        base = os.environ.get('ANTHROPIC_BASE_URL') or 'https://api.anthropic.com'
        self.url = base.rstrip('/') + '/v1/messages/count_tokens'
        self.cache_path = cache_path
        self.cache = json.loads(read(cache_path)) if os.path.exists(cache_path) else {}

    def save(self):
        write(self.cache_path, json.dumps(self.cache, indent=0, sort_keys=True) + '\n')

    def count(self, text):
        if not text.strip():
            return 0
        h = hashlib.sha256((self.model + '\0' + text).encode()).hexdigest()
        if h not in self.cache:
            self.cache[h] = self.request(text)
        return self.cache[h]

    def request(self, text):
        body = json.dumps({'model': self.model, 'messages': [{'role': 'user', 'content': text}]}).encode()
        headers = {'x-api-key': self.key, 'anthropic-version': '2023-06-01', 'content-type': 'application/json'}
        for k in range(5):
            req = urllib.request.Request(self.url, data=body, headers=headers, method='POST')
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    return int(json.load(r)['input_tokens'])
            except urllib.error.HTTPError as e:
                if e.code in (429, 500, 502, 503, 529) and k < 4:
                    time.sleep(min(float(e.headers.get('retry-after') or 2 ** k), 60))
                    continue
                detail = e.read().decode('utf-8', 'replace')[:300]
                raise HarnessError(f'count_tokens: HTTP {e.code}: {detail}') from None
            except urllib.error.URLError as e:
                if k < 4:
                    time.sleep(2 ** k)
                    continue
                raise HarnessError(f'count_tokens: {e.reason}') from None
        raise HarnessError('count_tokens: попытки кончились')


def cells(run_dir):
    out = []
    for meta_path in sorted(glob.glob(os.path.join(run_dir, '_log', '*', '*', 'meta.json'))):
        log = os.path.dirname(meta_path)
        out.append((os.path.basename(os.path.dirname(log)), os.path.basename(log)))
    order = {lang: i for i, lang in enumerate(LANGS)}
    return sorted(out, key=lambda c: (c[0], order.get(c[1], 9)))


def transcript_steps(path):
    """Шаги сессии субагента по его транскрипту (JSONL инструмента Agent):
    контекст при каждом вызове модели и сколько символов перед ним прочитано
    (результаты инструментов) и написано (текст и входы инструментов).
    Содержимое не сохраняется — только числа и имена инструментов."""
    rows, seen = [], set()
    read_n = wrote_n = 0
    names = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            o = json.loads(line)
            m = o.get('message') or {}
            if o.get('type') == 'assistant':
                if m.get('id') not in seen:
                    seen.add(m.get('id'))
                    u = m.get('usage') or {}
                    ctx = u.get('input_tokens', 0) + u.get('cache_creation_input_tokens', 0) + \
                        u.get('cache_read_input_tokens', 0)
                    rows.append({'context': ctx, 'read': read_n, 'wrote': wrote_n, 'tools': names})
                    read_n = wrote_n = 0
                    names = []
                for b in m.get('content') or []:
                    if b.get('type') == 'tool_use':
                        inp = b.get('input', {})
                        wrote_n += len(json.dumps(inp, ensure_ascii=False))
                        cmd = inp.get('command', '') if isinstance(inp, dict) else ''
                        names.append('check' if 'experiment.py check' in cmd else b.get('name'))
                    elif b.get('type') == 'text':
                        wrote_n += len(b.get('text', ''))
            elif o.get('type') == 'user' and isinstance(m.get('content'), list):
                for b in m['content']:
                    if b.get('type') == 'tool_result':
                        c = b.get('content')
                        read_n += len(c) if isinstance(c, str) else \
                            sum(len(x.get('text', '')) for x in c or [] if isinstance(x, dict))
    return rows


def usage(run, task_id, lang, transcript):
    """Разложение расхода сессии по транскрипту: обвязка субагента (контекст
    первого вызова), задание (рост контекста после его чтения) и работа
    (остальное: прочитанный код, вывод проверок, написанное и рассуждения)."""
    rows = transcript_steps(transcript)
    if len(rows) < 2:
        raise HarnessError(f'{transcript}: в транскрипте меньше двух вызовов модели')
    base, after_prompt, final = rows[0]['context'], rows[1]['context'], rows[-1]['context']
    # Работа по задачам серии: рост контекста до каждой проверки (у ячейки — одна часть).
    parts, start = [], after_prompt
    for k, r in enumerate(rows):
        if 'check' in r['tools'] and k + 1 < len(rows):
            parts.append(rows[k + 1]['context'] - start)
            start = rows[k + 1]['context']
    data = {'base': base, 'prompt': after_prompt - base, 'work': final - after_prompt, 'final': final,
            'calls': len(rows), 'per_check': parts, 'steps': rows}
    if task_id == 'series':
        path = series_paths(os.path.join(work_dir(), run), lang)[1]
    else:
        _, log = cell_paths(os.path.join(work_dir(), run), find_task(task_id).id, lang)
        path = os.path.join(log, 'meta.json')
    if not os.path.exists(path):
        raise HarnessError(f'нет журнала {path}')
    meta = json.loads(read(path))
    meta['usage'] = data
    write(path, json.dumps(meta, ensure_ascii=False, indent=1) + '\n')
    print(f'{run} {task_id} {lang}: обвязка {base}, задание {data["prompt"]}, работа {data["work"]}, '
          f'итог {final} (вызовов {len(rows)}); работа до каждой проверки: {parts}')
    return 0


def tokens(run, model=COUNT_MODEL):
    run_dir = os.path.join(work_dir(), run)
    counter = TokenCounter(model, os.path.join(work_dir(), 'token_cache.json'))
    if not counter.key:
        print('нет ключа ANTHROPIC_API_KEY: точный счёт токенов недоступен (Р35, TODO.md); '
              'report покажет символы')
        return 2
    n = 0
    try:
        for task_id, lang in cells(run_dir):
            texts = cell_texts(run_dir, task_id, lang)
            counts = {k: counter.count(v) for k, v in texts.items()}
            _, log = cell_paths(run_dir, task_id, lang)
            write(os.path.join(log, 'tokens.json'), json.dumps(dict(counts, model=model), indent=1) + '\n')
            n += 1
    finally:
        counter.save()
    print(f'токены: {n} ячеек прогона {run} сосчитаны ({model})')
    return 0


# ---------------------------------------------------------------- отчёт

MEASURES = ('prompt', 'diagnostics', 'written', 'final')
MEASURE_TITLE = {'prompt': 'прочитано на входе', 'diagnostics': 'диагностик', 'written': 'написано',
                 'final': 'решение'}


def collect(run_dir):
    rows = []
    for task_id, lang in cells(run_dir):
        _, log = cell_paths(run_dir, task_id, lang)
        meta = json.loads(read(os.path.join(log, 'meta.json')))
        attempts = read_attempts(log)
        accepted = next((a['n'] for a in attempts if a['class'] == ACCEPTED), None)
        texts = cell_texts(run_dir, task_id, lang)
        tok_path = os.path.join(log, 'tokens.json')
        toks = json.loads(read(tok_path)) if os.path.exists(tok_path) else None
        rows.append({
            'task': task_id, 'lang': lang, 'kind': meta['kind'],
            'status': ACCEPTED if accepted else ('не начато' if not attempts else 'не принято'),
            'attempts': len(attempts), 'accepted_at': accepted, 'first_try': accepted == 1,
            'path': [a['class'] for a in attempts],
            'chars': {k: len(v) for k, v in texts.items()},
            'tokens': {k: toks[k] for k in MEASURES} if toks else None,
            'token_model': toks.get('model') if toks else None,
            'session': meta.get('session'),
            'meta': {k: meta.get(k) for k in ('commit', 'cxx', 'agent_model', 'attempts_max', 'created')},
        })
    return rows


def summary(rows):
    langs = [lang for lang in LANGS if any(r['lang'] == lang for r in rows)]
    tasks = sorted({r['task'] for r in rows})
    by = {(r['task'], r['lang']): r for r in rows}
    common = [t for t in tasks if all(by.get((t, lang), {}).get('status') == ACCEPTED for lang in langs)]
    unit = 'tokens' if rows and all(r['tokens'] for r in rows) else 'chars'
    out = {'langs': langs, 'tasks': tasks, 'common': common, 'unit': unit, 'by_lang': {}}
    for lang in langs:
        rs = [r for r in rows if r['lang'] == lang]
        acc = [r for r in rs if r['status'] == ACCEPTED]
        com = [by[(t, lang)] for t in common]
        sess = [r['session']['total_tokens'] for r in com if r['session'] and r['session'].get('total_tokens')]
        out['by_lang'][lang] = {
            'cells': len(rs), 'accepted': len(acc), 'first_try': sum(r['first_try'] for r in rs),
            'mean_attempts': round(sum(r['accepted_at'] for r in acc) / len(acc), 2) if acc else None,
            'classes': {c: sum(r['path'].count(c) for r in rs) for c in CLASSES if c != ACCEPTED},
            'common': {k: sum(r[unit][k] for r in com) for k in MEASURES},
            'session': sum(sess) if len(sess) == len(com) and com else None,
        }
    return out


def render(run, rows, s):
    unit = 'токены' if s['unit'] == 'tokens' else 'символы (токены не сосчитаны — нет ключа, Р35)'
    key = s['unit']
    lines = [f'# Опыт: прогон {run}', '']
    if rows:
        m = rows[0]['meta']
        lines += [f'Коммит `{m["commit"][:12]}`, {m["cxx"]}; модель субагентов — {m["agent_model"] or "как у сессии"}; '
                  f'попыток на задачу — {m["attempts_max"]}. Меры — {unit}'
                  + (f', модель счёта `{rows[0]["token_model"]}`' if key == 'tokens' else '') + '.', '']
    lines += ['## Сводка по языкам', '',
              '| язык | принято | с первой попытки | попыток на принятую | ' + ' | '.join(MEASURE_TITLE[k] for k in MEASURES)
              + ' | сессия |', '|---|---|---|---|' + '---|' * (len(MEASURES) + 1)]
    for lang in s['langs']:
        b = s['by_lang'][lang]
        sess = str(b['session']) if b['session'] is not None else '—'
        lines.append(f'| {TITLE[lang]} | {b["accepted"]} из {b["cells"]} | {b["first_try"]} | {b["mean_attempts"] or "—"} | '
                     + ' | '.join(str(b['common'][k]) for k in MEASURES) + f' | {sess} |')
    lines += ['', f'Меры в сводке — суммы по задачам, принятым на всех языках прогона ({len(s["common"])}: '
              + (', '.join(s['common']) or 'нет') + '); сравниваются только решения, равные по корректности '
              '(GOAL.md, раздел 2). «Написано» — оценка снизу: изменённые строки между попытками.', '',
              '## Сбои по классам', '', '| язык | ' + ' | '.join(c for c in CLASSES if c != ACCEPTED) + ' |',
              '|---|' + '---|' * (len(CLASSES) - 1)]
    for lang in s['langs']:
        cl = s['by_lang'][lang]['classes']
        lines.append(f'| {TITLE[lang]} | ' + ' | '.join(str(cl[c]) for c in CLASSES if c != ACCEPTED) + ' |')
    lines += ['', '## Задачи', '', '| задача | язык | итог | путь | ' + ' | '.join(MEASURE_TITLE[k] for k in MEASURES)
              + ' | сессия |', '|---|---|---|---|' + '---|' * (len(MEASURES) + 1)]
    for r in rows:
        vals = r[key] or r['chars']
        sess = r['session']['total_tokens'] if r['session'] and r['session'].get('total_tokens') else '—'
        lines.append(f'| {r["task"]} | {r["lang"]} | {r["status"]} | {" → ".join(r["path"]) or "—"} | '
                     + ' | '.join(str(vals[k]) for k in MEASURES) + f' | {sess} |')
    return '\n'.join(lines) + '\n'


def report(run, save=False):
    run_dir = os.path.join(work_dir(), run)
    if not os.path.isdir(os.path.join(run_dir, '_log')):
        raise HarnessError(f'нет прогона {run_dir}')
    rows = collect(run_dir)
    s = summary(rows)
    text = render(run, rows, s) + render_series(run_dir, rows)
    sys.stdout.write(text)
    if save:
        dest = os.path.join(RESULTS, run)
        if os.path.exists(dest):
            shutil.rmtree(dest)
        shutil.copytree(os.path.join(run_dir, '_log'), os.path.join(dest, 'log'))
        for task_id, lang in cells(run_dir):
            sandbox, _ = cell_paths(run_dir, task_id, lang)
            shutil.copy(os.path.join(sandbox, 'PROMPT.md'), os.path.join(dest, 'log', task_id, lang, 'PROMPT.md'))
        write(os.path.join(dest, 'summary.md'), text)
        write(os.path.join(dest, 'summary.json'),
              json.dumps({'rows': rows, 'summary': s}, ensure_ascii=False, indent=1) + '\n')
        print(f'\nзаписано: {os.path.relpath(dest, ROOT)}')
    return 0


def series_rows(run_dir, rows):
    """Серии прогона: язык, задачи, принято, задание, написано, решение, расход сессии."""
    out = []
    for lang in LANGS:
        path, meta_path = series_paths(run_dir, lang)
        if not os.path.exists(meta_path):
            continue
        meta = json.loads(read(meta_path))
        cells_ = [r for r in rows if r['lang'] == lang and r['task'] in meta['tasks']]
        sess = (meta.get('session') or {}).get('total_tokens')
        acc = sum(1 for r in cells_ if r['status'] == ACCEPTED)
        use = meta.get('usage') or {}
        out.append({'lang': lang, 'tasks': meta['tasks'], 'accepted': acc,
                    'prompt_tokens': use.get('prompt'), 'work_tokens': use.get('work'),
                    'first_try': sum(r['first_try'] for r in cells_),
                    'prompt': len(read(meta_path[:-len('.json')] + '.PROMPT.md')),
                    'written': sum(r['chars']['written'] for r in cells_),
                    'final': sum(r['chars']['final'] for r in cells_),
                    'session': sess, 'per_change': round(sess / acc) if sess and acc else None})
    return out


def render_series(run_dir, rows):
    sr = series_rows(run_dir, rows)
    if not sr:
        return ''
    lines = ['', '## Серии (Р-17)', '',
             'Один субагент на язык решает задачи по очереди в одной сессии; спецификация читается один '
             'раз. «Задание» — символы задания серии; «сессия» — расход всей сессии по итогу инструмента '
             'Agent; «на изменение» — сессия на одну принятую задачу. По транскрипту (команда `usage`): '
             '«задание, ток.» — рост контекста от чтения задания, «работа, ток.» — весь рост после него '
             '(прочитанный код, вывод проверок, написанное и рассуждения).', '',
             '| язык | задачи | принято | с первой попытки | задание | написано | решение | сессия | на изменение '
             '| задание, ток. | работа, ток. |',
             '|---|---|---|---|---|---|---|---|---|---|---|']
    for r in sr:
        lines.append(f'| {TITLE[r["lang"]]} | {", ".join(r["tasks"])} | {r["accepted"]} из {len(r["tasks"])} | '
                     f'{r["first_try"]} | {r["prompt"]} | {r["written"]} | {r["final"]} | {r["session"] or "—"} | '
                     f'{r["per_change"] or "—"} | {r["prompt_tokens"] or "—"} | {r["work_tokens"] or "—"} |')
    return '\n'.join(lines) + '\n'


def restore(run, force=False):
    """Сохранённый прогон bench/results/RUN → рабочий каталог (журналы, задания, итоговый код)."""
    src = os.path.join(RESULTS, run, 'log')
    if not os.path.isdir(src):
        raise HarnessError(f'нет сохранённого прогона {src}')
    run_dir = os.path.join(work_dir(), run)
    if os.path.exists(run_dir):
        if not force:
            raise HarnessError(f'{run_dir} уже есть (--force — заменить)')
        shutil.rmtree(run_dir)
    shutil.copytree(src, os.path.join(run_dir, '_log'))
    n = 0
    for task_id, lang in cells(run_dir):
        sandbox, log = cell_paths(run_dir, task_id, lang)
        os.makedirs(sandbox, exist_ok=True)
        shutil.move(os.path.join(log, 'PROMPT.md'), os.path.join(sandbox, 'PROMPT.md'))
        last = len(read_attempts(log))
        snapshot(read_files(os.path.join(log, f'attempt-{last}')), os.path.join(sandbox, 'src'))
        n += 1
    for lang in LANGS:
        path, meta_path = series_paths(run_dir, lang)
        if os.path.exists(meta_path):
            write(path, read(meta_path[:-len('.json')] + '.PROMPT.md'))
    print(f'восстановлено: {n} ячеек прогона {run} в {run_dir}')
    return 0


# ---------------------------------------------------------------- самопроверка

START_CLASS = {'write': {'сборка'}, 'fix': {'вывод', 'ловушка'}, 'refactor': {'интерфейс'}}
FULL = ('cpp', 'A', 'dp')


def selftest_cell(task, lang, sandbox, tmp, flags):
    fails = []
    tag = f'{task.id} {lang}'
    p = read(os.path.join(sandbox, 'PROMPT.md'))
    if f'check {sandbox}' not in p:
        fails.append(f'{tag}: в PROMPT.md нет команды проверки')
    if MARK in p or task.expected.strip() in p:
        fails.append(f'{tag}: в PROMPT.md попал драйвер или ожидаемый вывод')
    if is_proto(lang) and f'вариант {lang} — краткая спецификация' not in p:
        fails.append(f'{tag}: в PROMPT.md нет спецификации варианта')
    if lang == 'dp' and (SPEC_DP_TITLE not in p or '[lex.source]' not in p):
        fails.append(f'{tag}: в PROMPT.md нет спецификации dpl')
    if lang == 'dp' and task.kind == 'refactor' and '// ...' not in p:
        fails.append(f'{tag}: в PROMPT.md нет целевого интерфейса')
    start = read_files(os.path.join(sandbox, 'src'))
    if start != task.files(lang, 'start'):
        fails.append(f'{tag}: src/ не совпадает с начальным кодом')
    build = lang in FULL
    cls, msg = evaluate(task, lang, start, os.path.join(tmp, tag.replace(' ', '_') + '_start'), flags, build)
    if build and cls not in START_CLASS[task.kind]:
        fails.append(f'{tag}: начальный код — «{cls}», ожидалось {"/".join(sorted(START_CLASS[task.kind]))}: '
                     f'{msg.splitlines()[0] if msg else ""}')
    if not build and cls is not None:
        fails.append(f'{tag}: начальный код не переводится: {cls}: {msg}')
    cls, msg = evaluate(task, lang, task.files(lang, 'ref'), os.path.join(tmp, tag.replace(' ', '_') + '_ref'),
                        flags, build)
    if cls != (ACCEPTED if build else None):
        fails.append(f'{tag}: эталон не принят: {cls}: {msg}')
    return fails


def selftest_stub_cpp(task, tmp):
    """Заготовка C++ собирается с драйвером (без тел) и называет те же функции, что заготовка dpl."""
    stub = os.path.join(task.path, 'stub.cpp')
    if not os.path.exists(stub):
        return []
    src = os.path.join(tmp, task.id + '_stub.cpp')
    write(src, read(stub) + task.cpp_driver())
    flags = [f for f in run_refs.BASE_FLAGS if f != '-Werror'] + ['-fsyntax-only', '-Wno-return-type']
    r = subprocess.run([run_refs.CXX] + flags + [src], capture_output=True, text=True)
    if r.returncode != 0:
        first = next((ln for ln in r.stderr.splitlines() if 'error' in ln), r.stderr[:200])
        return [f'{task.id}: stub.cpp не собирается с драйвером: {first}']
    dp = set()
    if os.path.isdir(task.cm):
        dp = {it.name for it in cm.load([os.path.join(task.cm, 'stub.cm')], 'A')[0].items if isinstance(it, cm.Fn)}
    cpp = set(re.findall(r'^\S[^(\n]*?(\w+)\([^;\n]*\) \{$', read(stub), re.M))
    fails = []
    if os.path.isdir(task.cm) and dp != cpp:
        fails.append(f'{task.id}: функции stub.cpp ({", ".join(sorted(cpp))}) и stub.cm ({", ".join(sorted(dp))}) расходятся')
    if os.path.isdir(task.dp):
        real = set(re.findall(r'^pub fn (\w+)\(', read(os.path.join(task.dp, 'stub.dpm')), re.M))
        if real != cpp:
            fails.append(f'{task.id}: функции stub.cpp ({", ".join(sorted(cpp))}) и dp/stub.dpm '
                         f'({", ".join(sorted(real))}) расходятся')
    return fails


def selftest_log(tmp):
    """Путь модели: проверка заготовки, эталона, повтор; журнал и отчёт."""
    task = find_task('t01')
    (sandbox,) = prepare('log', [task], ['cpp'], attempts=2, quiet=True)
    fails = []
    quiet = open(os.devnull, 'w')
    old = sys.stdout
    sys.stdout = quiet
    try:
        rc1 = check(sandbox)
        snapshot(task.files('cpp', 'ref'), os.path.join(sandbox, 'src'))
        rc2 = check(sandbox)
        rc3 = check(sandbox)
        report('log')
        has_key = bool(os.environ.get('ANTHROPIC_API_KEY'))
        rc4 = None if has_key else tokens('log')
    finally:
        sys.stdout = old
        quiet.close()
    _, log = cell_paths(os.path.join(work_dir(), 'log'), task.id, 'cpp')
    att = read_attempts(log)
    if (rc1, rc2, rc3) != (1, 0, 0) or [a['class'] for a in att] != ['сборка', ACCEPTED]:
        fails.append(f'журнал: коды {rc1, rc2, rc3}, попытки {[a["class"] for a in att]}')
    if rc4 not in (None, 2):
        fails.append('tokens без ключа не сообщает об отсутствии ключа')
    texts = cell_texts(os.path.join(work_dir(), 'log'), task.id, 'cpp')
    if not texts['written'] or not texts['diagnostics'] or 'ring_push' not in texts['final']:
        fails.append('журнал: тексты для счёта токенов пусты')
    fails += selftest_restore(tmp, task, texts)
    return fails


def selftest_restore(tmp, task, texts):
    """Сохранение в results и восстановление дают те же тексты для счёта."""
    global RESULTS
    saved = RESULTS
    RESULTS = os.path.join(tmp, 'results')
    quiet = open(os.devnull, 'w')
    old = sys.stdout
    sys.stdout = quiet
    try:
        report('log', save=True)
        shutil.rmtree(os.path.join(work_dir(), 'log'))
        restore('log')
    except HarnessError as e:
        return [f'сохранение и восстановление: {e}']
    finally:
        sys.stdout = old
        quiet.close()
        RESULTS = saved
    back = cell_texts(os.path.join(work_dir(), 'log'), task.id, 'cpp')
    return [] if back == texts else ['восстановленный прогон даёт другие тексты для счёта']


def selftest_series(tmp):
    """Серия: задание с правилами и спецификацией один раз, проверки всех задач, отчёт с серией."""
    tasks = [find_task('t15'), find_task('t16')]
    quiet = open(os.devnull, 'w')
    old = sys.stdout
    sys.stdout = quiet
    fails = []
    try:
        prepare('series', tasks, ['cpp', 'dp'], attempts=2, quiet=True, series=True)
        run_dir = os.path.join(work_dir(), 'series')
        for lang in ('cpp', 'dp'):
            path, meta_path = series_paths(run_dir, lang)
            p = read(path)
            for t in tasks:
                sandbox, _ = cell_paths(run_dir, t.id, lang)
                if f'check {sandbox}' not in p:
                    fails.append(f'серия {lang}: нет проверки {t.id}')
                if t.expected.strip() in p or MARK in p:
                    fails.append(f'серия {lang}: в задание попал драйвер или ожидаемый вывод')
                if 'часть серии' not in read(os.path.join(sandbox, 'PROMPT.md')):
                    fails.append(f'серия {lang}: PROMPT.md ячейки {t.id} — не ссылка на серию')
            if p.count('## Правила') != 1:
                fails.append(f'серия {lang}: правила не один раз')
            if p.count('[lex.source]') != (1 if lang == 'dp' else 0):
                fails.append(f'серия {lang}: спецификация не один раз')
        prepare('series-guide', tasks[:1], ['dp'], attempts=2, quiet=True, series=True, doc='guide')
        g = read(series_paths(os.path.join(work_dir(), 'series-guide'), 'dp')[0])
        if read(GUIDE_DP).splitlines()[0][2:] not in g or '[lex.source]' in g or 'по руководству' not in g:
            fails.append('серия с руководством: в задании нет руководства или осталась спецификация')
        sandbox, _ = cell_paths(run_dir, 't15', 'dp')
        snapshot(tasks[0].files('dp', 'ref'), os.path.join(sandbox, 'src'))
        rc = check(sandbox)
        session('series', 'series', 'dp', 1000)
        report('series')
        rows = collect(run_dir)
        sr = {r['lang']: r for r in series_rows(run_dir, rows)}
    except HarnessError as e:
        return [f'серия: {e}']
    finally:
        sys.stdout = old
        quiet.close()
    if rc != 0:
        fails.append('серия: эталон t15 в серии не принят')
    if sr.get('dp', {}).get('per_change') != 1000 or sr.get('cpp', {}).get('session') is not None:
        fails.append(f'серия: в отчёте не тот расход ({sr})')
    return fails


def selftest():
    tasks = all_tasks()
    if not os.path.exists(dp_check.dpl()):
        print(f'обвязка: нет компилятора {dp_check.dpl()} — сначала python3 tests/run.py')
        return 1
    saved = os.environ.get('DPL_EXP_WORK')
    with tempfile.TemporaryDirectory() as tmp:
        os.environ['DPL_EXP_WORK'] = os.path.join(tmp, 'work')
        flags = build_flags()
        sandboxes = prepare('self', tasks, LANGS, quiet=True)
        jobs = [(t, lang, s) for (t, lang), s in zip([(t, lang) for t in tasks for lang in t.langs()], sandboxes)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
            results = list(pool.map(lambda j: selftest_cell(j[0], j[1], j[2], tmp, flags), jobs))
            results += list(pool.map(lambda t: selftest_stub_cpp(t, tmp), tasks))
        results.append(selftest_log(tmp))
        results.append(selftest_series(tmp))
    if saved is None:
        del os.environ['DPL_EXP_WORK']
    else:
        os.environ['DPL_EXP_WORK'] = saved
    fails = [f for r in results for f in r]
    if fails:
        print('обвязка: сбоев ' + str(len(fails)))
        for f in fails:
            print('  ' + f)
        return 1
    print(f'обвязка: {len(tasks)} задач, ячеек {len(jobs)} — задания собираются, начальный код не проходит, '
          f'эталоны принимаются ({"/".join(FULL)} — со сборкой), журнал, отчёт, восстановление и серия работают')
    return 0


# ---------------------------------------------------------------- командная строка


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('run')
    p.add_argument('--tasks', default='')
    p.add_argument('--langs', default=','.join(LANGS))
    p.add_argument('--attempts', type=int, default=ATTEMPTS)
    p.add_argument('--agent-model', default='')
    p.add_argument('--force', action='store_true')
    p.add_argument('--series', action='store_true')
    p.add_argument('--doc', choices=DOCS, default='spec')
    p = sub.add_parser('check')
    p.add_argument('dir')
    p = sub.add_parser('session')
    p.add_argument('run')
    p.add_argument('task')
    p.add_argument('lang', choices=LANGS)
    p.add_argument('--tokens', type=int, required=True)
    p.add_argument('--tool-uses', type=int)
    p.add_argument('--seconds', type=float)
    p = sub.add_parser('usage')
    p.add_argument('run')
    p.add_argument('task')
    p.add_argument('lang', choices=LANGS)
    p.add_argument('--transcript', required=True)
    p = sub.add_parser('tokens')
    p.add_argument('run')
    p.add_argument('--model', default=COUNT_MODEL)
    p = sub.add_parser('report')
    p.add_argument('run')
    p.add_argument('--save', action='store_true')
    p = sub.add_parser('restore')
    p.add_argument('run')
    p.add_argument('--force', action='store_true')
    sub.add_parser('selftest')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'prepare':
            tasks = [find_task(t) for t in a.tasks.split(',') if t] or all_tasks()
            langs = [x for x in a.langs.split(',') if x]
            bad = [x for x in langs if x not in LANGS]
            if bad:
                raise HarnessError(f'неизвестный язык: {", ".join(bad)} (есть: {", ".join(LANGS)})')
            prepare(a.run, tasks, langs, a.attempts, a.agent_model, a.force, series=a.series, doc=a.doc)
            return 0
        if a.cmd == 'check':
            return check(a.dir)
        if a.cmd == 'session':
            session(a.run, a.task, a.lang, a.tokens, a.tool_uses, a.seconds)
            return 0
        if a.cmd == 'usage':
            return usage(a.run, a.task, a.lang, a.transcript)
        if a.cmd == 'tokens':
            return tokens(a.run, a.model)
        if a.cmd == 'report':
            return report(a.run, a.save)
        if a.cmd == 'restore':
            return restore(a.run, a.force)
        return selftest()
    except HarnessError as e:
        print(f'ошибка: {e}', file=sys.stderr)
        return 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
