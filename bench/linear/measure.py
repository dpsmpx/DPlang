#!/usr/bin/env python3
"""Замер правил линейных типов в циклах (заметка
docs/research/notes/12_linear_loops.md; только для замера).

    python3 bench/linear/measure.py [--save NAME]

Режимы проверки — переменная DPL_LINEAR прототипа (compiler/sema/linear.cpp):
- base — правила Р60: поглотить внешний ресурс в цикле нельзя (E0513),
  errdefer и поглощение ресурса дальше на пути ошибки — E0512;
- a — поглощение внешнего ресурса в цикле, если путь из цикла выходит;
- b — errdefer снимается, когда ресурс поглощён;
- ab — оба.

Корпус bench/linear/corpus/:
- задачи tN_<форма>.dpm: форма base принимается во всех режимах, форма a —
  в режимах с a, форма b — в режимах с b; у форм — токены (dpl debug lex,
  без комментариев и переводов строк) и строки кода;
- attacks.dpm: функции, разрезанные по строкам «// expect: …»: reject —
  отвергается во всех режимах, reject-base-a — безопасно, принимается в
  режимах с b.

Код возврата 1 — режим принял атаку или отверг программу, которую должен
принять.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DPL = os.path.join(ROOT, 'build', 'compiler', 'dpl')
CORPUS = os.path.join(ROOT, 'bench', 'linear', 'corpus')
MODES = ('base', 'a', 'b', 'ab')


def rules(mode):
    """Правки режима: у base — никаких."""
    return '' if mode == 'base' else mode


def check(path, mode):
    env = dict(os.environ)
    env.pop('DPL_LINEAR', None)
    if mode != 'base':
        env['DPL_LINEAR'] = mode
    r = subprocess.run([DPL, 'check', os.path.basename(path)], capture_output=True, text=True,
                       cwd=os.path.dirname(path), env=env)
    codes = sorted(set(re.findall(r'error\[(E\d+)\]', r.stderr)))
    return r.returncode == 0, codes


def tokens(path):
    r = subprocess.run([DPL, 'debug', 'lex', os.path.basename(path)], capture_output=True, text=True,
                       cwd=os.path.dirname(path))
    n = sum(1 for ln in r.stdout.splitlines() if ' Comment ' not in ln and ' Newline ' not in ln)
    with open(path, encoding='utf-8') as f:
        lines = sum(1 for ln in f if ln.strip() and not ln.strip().startswith('//'))
    return n, lines


def attacks(tmp):
    """Атаки — по программе на функцию: импорт и main общие."""
    with open(os.path.join(CORPUS, 'attacks.dpm'), encoding='utf-8') as f:
        text = f.read()
    parts = re.split(r'^// expect: (\S+)\n', text, flags=re.M)
    head, out = parts[0], []
    imports = '\n'.join(ln for ln in head.splitlines() if ln.startswith('import ')) + '\n\n'
    for k in range(1, len(parts), 2):
        expect, body = parts[k], parts[k + 1].replace('fn main() {}\n', '')
        name = re.search(r'^// (x\d+) — (.*)$', body, flags=re.M)
        d = os.path.join(tmp, name.group(1))
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, 'attack.dpm')
        with open(path, 'w', encoding='utf-8') as f:
            f.write(imports + body + '\nfn main() {}\n')
        out.append((name.group(1), name.group(2), expect, path))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--save', default='')
    args = ap.parse_args()
    if not os.path.exists(DPL):
        sys.exit(f'нет {DPL}: соберите компилятор (python3 tests/run.py)')
    data = {'tasks': {}, 'attacks': {}, 'wrong': []}
    tasks = sorted({f.split('_')[0] for f in os.listdir(CORPUS) if re.match(r't\d+_', f)})
    for t in tasks:
        for form in ('base', 'a', 'b'):
            path = os.path.join(CORPUS, f'{t}_{form}.dpm')
            if not os.path.exists(path):
                continue
            n, lines = tokens(path)
            cell = {'tokens': n, 'lines': lines, 'modes': {}}
            for mode in MODES:
                ok, codes = check(path, mode)
                want = form == 'base' or form in rules(mode)
                cell['modes'][mode] = 'ok' if ok else ' '.join(codes)
                if ok != want:
                    data['wrong'].append(f'{t}_{form} в режиме {mode}: {"принята" if ok else "отвергнута"}')
            data['tasks'][f'{t}_{form}'] = cell
    with tempfile.TemporaryDirectory() as tmp:
        for name, what, expect, path in attacks(tmp):
            cell = {'what': what, 'expect': expect, 'modes': {}}
            for mode in MODES:
                ok, codes = check(path, mode)
                want = expect == 'reject-base-a' and 'b' in rules(mode)
                cell['modes'][mode] = 'ok' if ok else ' '.join(codes)
                if ok != want:
                    data['wrong'].append(f'{name} в режиме {mode}: {"принята" if ok else "отвергнута"}')
            data['attacks'][name] = cell
    report = summary(data, tasks)
    if args.save:
        d = os.path.join(ROOT, 'bench', 'results', args.save)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        with open(os.path.join(d, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(report)
    print(report, end='')
    sys.exit(1 if data['wrong'] else 0)


def summary(data, tasks):
    lines = ['# Линейные типы в циклах: режимы проверки', '',
             'Режимы: base — Р60; a — поглощение в цикле с выходом из него; b — errdefer снимается при '
             'поглощении; ab — оба.', '',
             '## Задачи: формы, токены, приём', '',
             '| задача | форма | токенов | строк | base | a | b | ab |', '|---|---|---|---|---|---|---|---|']
    for name, c in data['tasks'].items():
        t, form = name.split('_', 1)
        m = c['modes']
        lines.append(f'| {t} | {form} | {c["tokens"]} | {c["lines"]} | {m["base"]} | {m["a"]} | {m["b"]} | {m["ab"]} |')
    lines += ['', '| задача | base → a | base → b |', '|---|---|---|']
    for t in tasks:
        b = data['tasks'].get(f'{t}_base')
        a = data['tasks'].get(f'{t}_a')
        bb = data['tasks'].get(f'{t}_b')
        if b and a and bb:
            lines.append(f'| {t} | {a["tokens"] - b["tokens"]:+d} токенов ({(a["tokens"] / b["tokens"] - 1) * 100:+.0f}%) | '
                         f'{bb["tokens"] - b["tokens"]:+d} токенов ({(bb["tokens"] / b["tokens"] - 1) * 100:+.0f}%) |')
    lines += ['', '## Атаки', '', '| случай | что | ждём | base | a | b | ab |', '|---|---|---|---|---|---|---|']
    for name, c in data['attacks'].items():
        m = c['modes']
        want = 'отвергнуть' if c['expect'] == 'reject' else 'принять в b, ab'
        lines.append(f'| {name} | {c["what"]} | {want} | {m["base"]} | {m["a"]} | {m["b"]} | {m["ab"]} |')
    lines += ['', 'Расхождения с ожиданием: ' + ('нет' if not data['wrong'] else '; '.join(data['wrong'])) + '.']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    main()
