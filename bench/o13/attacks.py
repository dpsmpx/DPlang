#!/usr/bin/env python3
"""Атаки на варианты модели задач О13 (заметка 10, раздел 6, мера «атаки»).

Корпус bench/o13/attacks_<v>.dpm: программы между строками
«// === имя: что пробует | ожидание». Ожидание: race — гонка данных,
safe — без гонки, logic — логическая гонка без гонки данных. Каждая
программа проверяется `dpl check` с DPL_TASKS=<v>; принятая собирается
из `dpl debug c` с TSan и запускается RUNS раз (DPL_THREADS=4).

Итог по атаке: отвергнута (коды ошибок), принята и TSan молчит, принята и
TSan сообщил о гонке, принята и упала или не кончилась.

    python3 bench/o13/attacks.py [a b c] [--json OUT]
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DPL = os.environ.get('DPL') or os.path.join(ROOT, 'build', 'compiler', 'dpl')
RUNTIME = os.path.join(ROOT, 'runtime')
CC = os.environ.get('TSAN_CC', 'gcc')
RUNS = 3
TIMEOUT = 20
HEAD = re.compile(r'^// === (?P<name>\w+): (?P<what>.*?) \| (?P<expect>race|safe|logic)\s*$')
CODE = re.compile(r'error\[(E\d{4})\]')


def corpus(v):
    """Атаки варианта: [(имя, что пробует, ожидание, текст программы)]."""
    out, cur = [], None
    with open(os.path.join(HERE, f'attacks_{v}.dpm'), encoding='utf-8') as f:
        for ln in f:
            m = HEAD.match(ln.rstrip('\n'))
            if m:
                cur = [m.group('name'), m.group('what'), m.group('expect'), []]
                out.append(cur)
            elif cur is not None:
                cur[3].append(ln)
    return [(n, w, e, ''.join(body)) for n, w, e, body in out]


def run(argv, cwd, env=None, timeout=TIMEOUT):
    try:
        return subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True, errors='replace',
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def attack(v, text, tmp):
    """(итог, подробность): rejected, clean, race, crash, timeout, tool."""
    with open(os.path.join(tmp, 'main.dpm'), 'w', encoding='utf-8') as f:
        f.write(text)
    env = dict(os.environ, DPL_TASKS=v)
    r = run([DPL, 'check', 'main.dpm'], tmp, env)
    if r is None:
        return 'tool', 'check: время'
    if r.returncode == 1:
        return 'rejected', ' '.join(sorted(set(CODE.findall(r.stderr))))
    if r.returncode != 0:
        return 'tool', 'check: код ' + str(r.returncode)
    r = run([DPL, 'debug', 'c', 'main.dpm'], tmp, env)
    if r is None or r.returncode != 0:
        return 'tool', 'debug c'
    c_path = os.path.join(tmp, 'main.c')
    with open(c_path, 'w', encoding='utf-8') as f:
        f.write(r.stdout)
    exe = os.path.join(tmp, 'main.bin')
    r = run([CC, '-std=c11', '-O1', '-g', '-fsanitize=thread', '-I', os.path.join(RUNTIME, 'core'), c_path,
             os.path.join(RUNTIME, 'core', 'dp_rt.c'), os.path.join(RUNTIME, 'linux', 'dp_entry.c'), '-lm',
             '-lpthread', '-o', exe], tmp, timeout=120)
    if r is None or r.returncode != 0:
        return 'tool', 'сборка C: ' + (r.stderr.strip().splitlines()[0] if r and r.stderr.strip() else 'время')
    for _ in range(RUNS):
        p = run([exe], tmp, dict(os.environ, DPL_THREADS='4'))
        if p is None:
            return 'timeout', ''
        if 'WARNING: ThreadSanitizer' in p.stderr:
            return 'race', ''
        if p.returncode != 0:
            first = next((ln for ln in p.stderr.splitlines() if 'trap' in ln), '')
            return 'crash', f'код {p.returncode} {first}'.strip()
    return 'clean', ''


def main(argv):
    out_json = None
    if '--json' in argv:
        i = argv.index('--json')
        out_json = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    variants = argv or [v for v in 'abc' if os.path.exists(os.path.join(HERE, f'attacks_{v}.dpm'))]
    if not os.path.exists(DPL):
        print(f'атаки: нет компилятора {DPL}')
        return 1
    results = {}
    with tempfile.TemporaryDirectory() as tmp:
        for v in variants:
            rows = []
            for name, what, expect, text in corpus(v):
                d = os.path.join(tmp, v + name)
                os.makedirs(d)
                verdict, detail = attack(v, text, d)
                shutil.rmtree(d)
                rows.append({'name': name, 'what': what, 'expect': expect, 'verdict': verdict, 'detail': detail})
                print(f'{v} {name:4} {expect:5} {verdict:8} {detail:20} {what}')
            results[v] = rows
    print()
    print('вариант | атак | гонок | гонок отвергнуто | гонок принято (TSan) | безопасных принято | логических принято')
    for v, rows in results.items():
        race = [r for r in rows if r['expect'] == 'race']
        safe = [r for r in rows if r['expect'] == 'safe']
        logic = [r for r in rows if r['expect'] == 'logic']
        ok = lambda rs, k: sum(1 for r in rs if r['verdict'] in k)
        print(f'{v} | {len(rows)} | {len(race)} | {ok(race, ("rejected",))} | {ok(race, ("race",))} | '
              f'{ok(safe, ("clean",))}/{len(safe)} | {ok(logic, ("clean",))}/{len(logic)}')
    if out_json:
        with open(out_json, 'w', encoding='utf-8') as f:
            json.dump(results, f, ensure_ascii=False, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
