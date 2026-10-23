#!/usr/bin/env python3
"""Замер О4 (Р24): исключительный изменяемый доступ — сколько и чего
отвергли бы варианты А, Б, В и развилки 1, 2, 5, 6
(docs/research/notes/8_exclusive_access.md, разделы 4 и 5).

    python3 bench/o4/measure.py [--save NAME]

Каждая программа корпуса проходит `dpl debug access` (compiler/sema/
access.h): проверка пишет события доступа и программу не отвергает.
Наборы правил ниже (RULES) решают, какие события стали бы отказом.
Корпус:
- эталоны — bench/tasks/*/dp: решение и драйвер;
- модели — попытки моделей bench/results/k2-*/log/*/dp/attempt-N (N ≥ 1;
  attempt-0 — заготовка задачи) с драйвером задачи; события — только в
  файлах модели;
- тесты — tests/ без наборов с ожидаемыми отказами и разбором;
- примеры — examples/*/main.dpm;
- атаки — bench/o4/attacks.dpm: функция `a_<имя>` — атака (ATTACKS).

Разметка мест — bench/o4/labels.json: ошибка (настоящая: тихая запись,
ловушка, гонка, висячий вид), ложный отказ (программа корректна и
детерминирована) и правка, тест правила (тест компилятора, нарочно
задевающий правило). Неразмеченные места — в отчёте.

Компилятор — build/release/dpl (собирает tests/run.py). --save — данные и
отчёт в bench/results/NAME/ (events.jsonl, measure.json, summary.md).
"""
import argparse
import collections
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DPL = os.path.join(ROOT, 'build', 'release', 'dpl')
ATTACK_FILE = os.path.join(HERE, 'attacks.dpm')
LABELS = os.path.join(HERE, 'labels.json')
# Наборы tests/, где программы должны проходить проверку; fail, parse, lex,
# fmt, diag, print — отказы и разбор; access — тест самого режима.
TEST_SUITES = ['run', 'ffi', 'lib', 'ir', 'types', 'sema', 'test', 'summary', 'bindings']

# Атака → (семейство, что с ней сейчас).
ATTACKS = {
    'a_grow_in_mut_loop': ('обход', 'тихая запись не в тот элемент'),
    'a_shrink_in_mut_loop': ('обход', 'ловушка'),
    'a_clear_in_read_loop': ('обход', 'ловушка'),
    'a_push_in_read_loop': ('обход', 'рост до нехватки памяти'),
    'a_nested_mut_loops': ('обход', 'итог зависит от порядка'),
    'a_read_root_in_mut_loop': ('обход', 'итог зависит от порядка'),
    'a_map_insert_in_loop': ('обход', 'E0412'),
    'a_slice_loop_mutation': ('обход', 'E0412'),
    'a_par_accumulate': ('parallel for', 'гонка с потоками'),
    'a_par_push': ('parallel for', 'гонка с потоками'),
    'a_par_shared_index': ('parallel for', 'гонка с потоками'),
    'a_par_read_root': ('parallel for', 'гонка с потоками'),
    'a_par_computed_index': ('parallel for', 'гонка с потоками'),
    'a_call_same': ('вызов', 'E0502'),
    'a_call_part_whole': ('вызов', 'E0502'),
    'a_call_computed_alias': ('вызов', 'E0502'),
    'a_view_implicit': ('вид', 'снимок'),
    'a_view_explicit': ('вид', 'снимок'),
    'a_view_slice_of_slice': ('вид', 'снимок'),
    'a_view_method_arg': ('вид', 'снимок'),
    'a_view_in_and': ('вид', 'снимок'),
    'a_view_receiver': ('вид', 'снимок'),
    'a_view_index_of_slice': ('вид', 'снимок'),
    'a_view_field_root': ('вид', 'снимок'),
    'a_view_method_receiver': ('вид', 'снимок'),
    'a_view_mut_param': ('вид', 'снимок'),
    'a_view_ref_before_call': ('вид', 'снимок'),
    'a_view_addr_for_c': ('вид', 'снимок'),
}


# Связи путей, при которых места могут совпасть: одно место, часть и целое,
# элементы по вычисленным индексам (развилка 2 — отказ).
MAY_OVERLAP = ('overlap', 'computed_index', '')


def frozen(e):
    """Сейчас E0412: изменение корня в обходе Map и среза (Р54, Р65) — по корню."""
    return e['rule'] == 'loop' and e['kind'] in ('structural', 'element') and e['detail'].split()[0] in ('map', 'slice')


def paths(f):
    """То же, но по путям (развилка 1): другое поле или постоянный индекс корня — не отказ."""
    return lambda e: f(e) and e.get('rel', '') in MAY_OVERLAP


def nested_inner_mut(e):
    return e['rule'] == 'loop' and e['kind'] == 'nested' and e['detail'].endswith('inner mut')


def call_kinds(*kinds):
    return lambda e: e['rule'] == 'call' and e['kind'] in kinds


def loop_kinds(*kinds):
    return lambda e: e['rule'] == 'loop' and e['kind'] in kinds


def par_kinds(*kinds):
    return lambda e: e['rule'] == 'par' and e['kind'] in kinds


def any_of(*fs):
    return lambda e: any(f(e) for f in fs)


# Набор правил → (описание, событие — отказ). Вызов: А — пути с полями
# (развилка 1: поля), А+ — ещё и разные постоянные индексы; два элемента по
# вычисленным индексам (развилка 2) — отказ в обоих. Обход (Б, развилка 5),
# по путям, как А+: Б1 — только длина обходимой коллекции, Б2 — любое
# изменение, Б3 — ещё и чтение в `&mut`-обходе; Б2 по корню — без путей.
# В — Б2 и вид левее действия. parallel for (развилка 6): 6а — только
# `out[i]` по переменной цикла, 6б — ещё вычисленный индекс с проверкой при
# выполнении, 6г — только свёртка библиотеки.
CALL_A_PLUS = call_kinds('overlap', 'computed_index')
B2 = any_of(CALL_A_PLUS, frozen, paths(loop_kinds('structural', 'element')), paths(nested_inner_mut))
RULES = collections.OrderedDict([
    ('сейчас', ('E0502 по корню; E0412 у Map и среза', any_of(call_kinds('overlap', 'fields', 'const_index',
                                                                          'computed_index'), frozen))),
    ('А', ('пути с полями', any_of(call_kinds('overlap', 'const_index', 'computed_index'), frozen))),
    ('А+', ('пути с полями и постоянными индексами', any_of(CALL_A_PLUS, frozen))),
    ('Б1', ('А+ и длина обходимой коллекции', any_of(CALL_A_PLUS, frozen, paths(loop_kinds('structural'))))),
    ('Б2', ('А+ и любое изменение обходимой коллекции', B2)),
    ('Б2 по корню', ('Б2 без путей: любое изменение корня', any_of(CALL_A_PLUS, frozen,
                                                                  loop_kinds('structural', 'element'),
                                                                  nested_inner_mut))),
    ('Б3', ('Б2 и чтение коллекции в обходе &mut', any_of(B2, paths(loop_kinds('nested', 'read'))))),
    ('В', ('Б2 и вид левее действия над его корнем', any_of(B2, lambda e: e['rule'] == 'view'))),
    ('6а', ('parallel for: только out[i] по переменной цикла', any_of(par_kinds('var', 'index_computed',
                                                                               'structural'),
                                                                     paths(par_kinds('read_root'))))),
    ('6б', ('parallel for: и вычисленный индекс с проверкой', any_of(par_kinds('var', 'structural'),
                                                                    paths(par_kinds('read_root'))))),
    ('6г', ('parallel for: только свёртка библиотеки', any_of(par_kinds('var', 'index_loopvar', 'index_computed',
                                                                        'structural'),
                                                              paths(par_kinds('read_root'))))),
])
# Составные варианты: правило вызова и обхода вместе с правилом parallel for.
B3 = RULES['Б3'][1]
VIEW = lambda e: e['rule'] == 'view'  # noqa: E731
for name, desc, f in [('Б3 + 6а', 'Б3 и 6а', any_of(B3, RULES['6а'][1])),
                      ('Б3 + 6б', 'Б3 и 6б', any_of(B3, RULES['6б'][1])),
                      ('В на Б3 + 6а', 'Б3, вид левее действия и 6а', any_of(B3, VIEW, RULES['6а'][1])),
                      ('В на Б3 + 6б', 'Б3, вид левее действия и 6б', any_of(B3, VIEW, RULES['6б'][1]))]:
    RULES[name] = (desc, f)


def run_access(cwd, root):
    """События `dpl debug access` программы с главным модулем root (в cwd)."""
    r = subprocess.run([DPL, 'debug', 'access', root], cwd=cwd, capture_output=True, text=True, timeout=120)
    if r.returncode not in (0, 1):
        sys.exit(f'{os.path.join(cwd, root)}: dpl debug access: код {r.returncode}\n{r.stderr[-2000:]}')
    events = [json.loads(ln) for ln in r.stdout.splitlines() if ln.strip()]
    codes = sorted(set(re.findall(r'error\[(E\d{4})\]', r.stderr)))
    return r.returncode == 0, events, codes


def programs(tmp):
    """(сегмент, имя, каталог, главный модуль, свои файлы или None — все, класс попытки)."""
    out = []
    for dp in sorted(glob.glob(os.path.join(ROOT, 'bench', 'tasks', 't*', 'dp'))):
        task = os.path.basename(os.path.dirname(dp))
        d = os.path.join(tmp, 'ref', task)
        os.makedirs(d)
        ref = os.path.join(dp, 'ref')
        srcs = glob.glob(os.path.join(ref, '*.dpm')) if os.path.isdir(ref) else [os.path.join(dp, 'sol.dpm')]
        for s in srcs + [os.path.join(dp, 'main.dpm')]:
            shutil.copy(s, d)
        out.append(('эталоны', task, d, 'main.dpm', None, ''))
    tasks = {os.path.basename(t).split('_')[0]: t for t in glob.glob(os.path.join(ROOT, 'bench', 'tasks', 't*'))}
    for att in sorted(glob.glob(os.path.join(ROOT, 'bench', 'results', 'k2-*', 'log', 't*', 'dp', 'attempt-*'))):
        n = int(att.rsplit('-', 1)[1])
        if n == 0:
            continue
        log = os.path.dirname(att)
        tid = os.path.basename(os.path.dirname(log))
        run = os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(log))))
        own = sorted(os.path.basename(f) for f in glob.glob(os.path.join(att, '*.dpm')))
        if not own or tid not in tasks:
            continue
        cls = ''
        try:
            with open(os.path.join(log, 'attempts.jsonl'), encoding='utf-8') as f:
                for ln in f:
                    a = json.loads(ln)
                    if a.get('n') == n:
                        cls = a.get('class', '')
        except OSError:
            pass
        name = f'{run}/{tid}/attempt-{n}'
        d = os.path.join(tmp, 'model', name.replace('/', '_'))
        os.makedirs(d)
        shutil.copy(os.path.join(tasks[tid], 'dp', 'main.dpm'), d)
        for f in own:
            shutil.copy(os.path.join(att, f), d)
        out.append(('модели', name, d, 'main.dpm', set(own), cls))
    for suite in TEST_SUITES:
        base = os.path.join(ROOT, 'tests', suite)
        if not os.path.isdir(base):
            continue
        for entry in sorted(os.listdir(base)):
            p = os.path.join(base, entry)
            if entry.startswith(('err_', 'bad-')):  # тест ожидаемого отказа
                continue
            if entry.endswith('.dpm') and os.path.isfile(p):
                out.append(('тесты', f'{suite}/{entry}', base, entry, None, ''))
            elif os.path.isfile(os.path.join(p, 'main.dpm')):
                out.append(('тесты', f'{suite}/{entry}', p, 'main.dpm', None, ''))
    for main in sorted(glob.glob(os.path.join(ROOT, 'examples', '*', 'main.dpm'))):
        out.append(('примеры', os.path.basename(os.path.dirname(main)), os.path.dirname(main), 'main.dpm', None, ''))
    out.append(('атаки', 'attacks', HERE, 'attacks.dpm', None, ''))
    return out


def attack_ranges():
    """Функция-атака → (первая строка, последняя)."""
    with open(ATTACK_FILE, encoding='utf-8') as f:
        lines = f.read().splitlines()
    starts = [(i + 1, m.group(1)) for i, ln in enumerate(lines) if (m := re.match(r'fn (\w+)', ln))]
    ranges = {}
    for k, (line, name) in enumerate(starts):
        end = starts[k + 1][0] - 1 if k + 1 < len(starts) else len(lines)
        if name.startswith('a_'):
            ranges[name] = (line, end)
    return ranges


def site(e):
    return (e['segment'], e['program'], e['file'], e['line'], e['col'])


def load_labels():
    if not os.path.exists(LABELS):
        return []
    with open(LABELS, encoding='utf-8') as f:
        return json.load(f)


def label_of(e, labels):
    """Разметка места: первая подходящая запись (поля match — подмножество события)."""
    for lab in labels:
        m = lab['match']
        if all(str(e.get(k)) == str(v) if k != 'program' else re.fullmatch(v, e['program']) for k, v in m.items()):
            return lab
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--save', default='')
    args = ap.parse_args()
    if not os.path.exists(DPL):
        sys.exit(f'нет {DPL}: соберите компилятор (python3 tests/run.py)')
    labels = load_labels()
    events, progs = [], []
    with tempfile.TemporaryDirectory() as tmp:
        for seg, name, d, root, own, cls in programs(tmp):
            ok, evs, codes = run_access(d, root)
            mine = [e for e in evs if own is None or e['file'] in own]
            for e in mine:
                e.update(segment=seg, program=name)
            progs.append({'segment': seg, 'program': name, 'ok': ok, 'codes': codes, 'class': cls,
                          'events': len(mine)})
            if ok:
                events.extend(mine)
    for e in events:
        lab = label_of(e, labels)
        e['label'] = lab['label'] if lab else ''
        e['fix'] = lab.get('fix', '') if lab else ''
    report, data = summarize(progs, events)
    if args.save:
        d = os.path.join(ROOT, 'bench', 'results', args.save)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'events.jsonl'), 'w', encoding='utf-8') as f:
            for e in events:
                f.write(json.dumps(e, ensure_ascii=False) + '\n')
        with open(os.path.join(d, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        with open(os.path.join(d, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(report)
    print(report, end='')


SEGMENTS = ['эталоны', 'модели', 'тесты', 'примеры']


def summarize(progs, events):
    commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True,
                            text=True).stdout.strip()
    lines = ['# Замер О4: исключительный изменяемый доступ', '',
             f'Коммит {commit}, `dpl debug access` (build/release/dpl), `bench/o4/measure.py`. Наборы правил — '
             '`RULES` там же; разметка — `bench/o4/labels.json`.', '', '## Корпус', '',
             '| сегмент | программ | проходят проверку | не проходят по другим правилам | событий |',
             '|---|---|---|---|---|']
    data = {'commit': commit, 'programs': progs, 'rules': {}, 'attacks': {}}
    for seg in SEGMENTS + ['атаки']:
        ps = [p for p in progs if p['segment'] == seg]
        ok = [p for p in ps if p['ok']]
        lines.append(f'| {seg} | {len(ps)} | {len(ok)} | {len(ps) - len(ok)} | '
                     f'{sum(1 for e in events if e["segment"] == seg)} |')
    classes = collections.Counter(p['class'] or '?' for p in progs if p['segment'] == 'модели')
    lines += ['', 'Попытки моделей по итогу проверки задачи: ' +
              ', '.join(f'{k} — {v}' for k, v in classes.most_common()) + '.']
    corpus = [e for e in events if e['segment'] != 'атаки']
    # События по видам.
    lines += ['', '## События корпуса по видам', '',
              '| событие | ' + ' | '.join(SEGMENTS) + ' |', '|---|' + '---|' * len(SEGMENTS)]
    kinds = sorted({(e['rule'], e['kind']) for e in corpus})
    for rule, kind in kinds:
        cnt = [len({site(e) for e in corpus if e['segment'] == s and e['rule'] == rule and e['kind'] == kind})
               for s in SEGMENTS]
        lines.append(f'| {rule}/{kind} | ' + ' | '.join(map(str, cnt)) + ' |')
    # Отказы по наборам правил.
    lines += ['', '## Отказы по наборам правил', '',
              'Место — строка и столбец события; одно место с несколькими событиями — один отказ. Разметка: '
              'ошибка / ложный отказ / тест правила / не размечено.', '',
              '| правило | что отвергает | ' + ' | '.join(SEGMENTS) + ' | программ | ошибка | ложный | тест правила '
              '| не размечено |', '|---|---|' + '---|' * (len(SEGMENTS) + 5)]
    ranges = attack_ranges()
    attack_events = [e for e in events if e['segment'] == 'атаки']
    for name, (desc, rejects) in RULES.items():
        hit = [e for e in corpus if rejects(e)]
        sites = {}
        for e in hit:
            sites.setdefault(site(e), e)
        per_seg = [sum(1 for s in sites if s[0] == seg) for seg in SEGMENTS]
        n_prog = len({(s[0], s[1]) for s in sites})
        lab = collections.Counter(e['label'] or 'не размечено' for e in sites.values())
        caught = [a for a, (lo, hi) in ranges.items()
                  if any(rejects(e) and lo <= e['line'] <= hi for e in attack_events)]
        data['rules'][name] = {'desc': desc, 'sites': per_seg, 'programs': n_prog, 'labels': dict(lab),
                               'caught': caught}
        lines.append(f'| {name} | {desc} | ' + ' | '.join(map(str, per_seg)) +
                     f' | {n_prog} | {lab.get("ошибка", 0)} | {lab.get("ложный отказ", 0)} | '
                     f'{lab.get("тест правила", 0)} | {lab.get("не размечено", 0)} |')
    # Атаки: пойманные по семействам, затем каждая.
    families = list(dict.fromkeys(f for f, _ in ATTACKS.values()))
    lines += ['', '## Атаки', '', 'Поймано ошибкой компиляции по семействам (атак в семействе):', '',
              '| правило | ' + ' | '.join(f'{f} ({sum(1 for v in ATTACKS.values() if v[0] == f)})' for f in families) +
              ' |', '|---|' + '---|' * len(families)]
    for r in RULES:
        caught = data['rules'][r]['caught']
        lines.append(f'| {r} | ' + ' | '.join(str(sum(1 for a in caught if ATTACKS.get(a, ('?',))[0] == f))
                                             for f in families) + ' |')
    lines += ['', '| атака | семейство | что с ней сейчас | ' + ' | '.join(RULES) + ' |',
              '|---|---|---|' + '---|' * len(RULES)]
    for a, (lo, hi) in ranges.items():
        fam, now = ATTACKS.get(a, ('?', '?'))
        marks = ['да' if a in data['rules'][r]['caught'] else '—' for r in RULES]
        data['attacks'][a] = {'family': fam, 'now': now}
        lines.append(f'| `{a}` | {fam} | {now} | ' + ' | '.join(marks) + ' |')
    # Решения моделей: что сейчас отвергает E0502 и что принял бы вариант А.
    model_calls = [e for e in corpus if e['segment'] == 'модели' and e['rule'] == 'call']
    lines += ['', '## E0502 у моделей', '']
    if model_calls:
        lines += ['| связь путей | мест |', '|---|---|']
        for k, n in collections.Counter(e['kind'] for e in {site(e): e for e in model_calls}.values()).most_common():
            lines.append(f'| {k} | {n} |')
    else:
        lines.append('Ни в одной попытке моделей нет двух ссылок в один корень в одном вызове: E0502 моделей не '
                     'останавливал, и пути вариантов А и А+ им ничего не дают.')
    # Места.
    lines += ['', '## Места корпуса', '', '| сегмент | программа | место | событие | текст | разметка | правка |',
              '|---|---|---|---|---|---|---|']
    seen = set()
    for e in sorted(corpus, key=lambda e: (SEGMENTS.index(e['segment']), e['program'], e['file'], e['line'],
                                           e['col'])):
        key = site(e) + (e['rule'], e['kind'])
        if key in seen:
            continue
        seen.add(key)
        text = e['text'].replace('|', '\\|')
        rel = f' ({e["rel"]})' if e.get('rel') and e['rule'] != 'call' else ''
        lines.append(f'| {e["segment"]} | {e["program"]} | {e["file"]}:{e["line"]}:{e["col"]} | '
                     f'{e["rule"]}/{e["kind"]}{rel} {e["detail"]} | `{text}` | {e["label"] or "—"} | '
                     f'{e["fix"] or ""} |')
    return '\n'.join(lines) + '\n', data


if __name__ == '__main__':
    main()
