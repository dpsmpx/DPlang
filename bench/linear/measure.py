#!/usr/bin/env python3
"""Корпус правил линейных типов в циклах и errdefer (заметка
docs/research/notes/12_linear_loops.md; Р60).

    python3 bench/linear/measure.py [--save NAME]

Замер вариантов (31 октября 2026, bench/results/linear-2026-10-31/) шёл на
прототипе режимов DPL_LINEAR; автор выбрал Б — errdefer снимается, когда
ресурс поглощён, — прототип снят. Теперь корпус сверяется с принятым
правилом:
- задачи tN_<форма>.dpm (bench/linear/corpus/): форма base (обходной
  приём) и b (errdefer и try close) принимаются, a (закрыть внешний
  ресурс в цикле с выходом из него) — нет (E0513); у форм — токены
  (dpl debug lex, без комментариев и переводов строк) и строки кода;
- attacks.dpm: функции, разрезанные по строкам «// expect: …»: reject —
  отвергается, reject-base-a — безопасно и принимается.

Код возврата 1 — расхождение с ожиданием.
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
ACCEPT_FORM = {'base': True, 'a': False, 'b': True}


def check(path):
    r = subprocess.run([DPL, 'check', os.path.basename(path)], capture_output=True, text=True,
                       cwd=os.path.dirname(path))
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
    for f in sorted(os.listdir(CORPUS)):
        m = re.match(r'(t\d+)_(\w+)\.dpm$', f)
        if not m or m.group(2) not in ACCEPT_FORM:
            continue
        path = os.path.join(CORPUS, f)
        n, lines = tokens(path)
        ok, codes = check(path)
        if ok != ACCEPT_FORM[m.group(2)]:
            data['wrong'].append(f'{f[:-4]}: {"принята" if ok else "отвергнута"}')
        data['tasks'][f[:-4]] = {'tokens': n, 'lines': lines, 'result': 'ok' if ok else ' '.join(codes)}
    with tempfile.TemporaryDirectory() as tmp:
        for name, what, expect, path in attacks(tmp):
            ok, codes = check(path)
            if ok != (expect == 'reject-base-a'):
                data['wrong'].append(f'{name}: {"принята" if ok else "отвергнута"}')
            data['attacks'][name] = {'what': what, 'expect': expect, 'result': 'ok' if ok else ' '.join(codes)}
    report = summary(data)
    if args.save:
        d = os.path.join(ROOT, 'bench', 'results', args.save)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'measure.json'), 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        with open(os.path.join(d, 'summary.md'), 'w', encoding='utf-8') as f:
            f.write(report)
    print(report, end='')
    sys.exit(1 if data['wrong'] else 0)


def summary(data):
    lines = ['# Линейные типы: корпус против принятого правила (Р60)', '',
             '| задача | форма | токенов | строк | проверка |', '|---|---|---|---|---|']
    for name, c in data['tasks'].items():
        t, form = name.split('_', 1)
        lines.append(f'| {t} | {form} | {c["tokens"]} | {c["lines"]} | {c["result"]} |')
    lines += ['', '| случай | что | ждём | проверка |', '|---|---|---|---|']
    for name, c in data['attacks'].items():
        want = 'отвергнуть' if c['expect'] == 'reject' else 'принять'
        lines.append(f'| {name} | {c["what"]} | {want} | {c["result"]} |')
    lines += ['', 'Расхождения с ожиданием: ' + ('нет' if not data['wrong'] else '; '.join(data['wrong'])) + '.']
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    main()
