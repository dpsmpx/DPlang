"""Прототип правила О4 для серии сессий моделей (выбор автора 23 октября
2026: «В на Б3 + 6а», docs/research/notes/8_exclusive_access.md, раздел 7).

Компилятор правила ещё не проверяет. Прототип прогоняет программу через
`dpl debug access` и события, которые отвергает набор RULE из measure.py,
отдаёт ошибками в виде диагностики компилятора: заголовок, строка, метка.
Сообщения — по-английски, с правкой (Р47). Отличие от будущего правила:
`dpl build` после прототипа по-прежнему отвергает пары `&mut p.x, &p.y`
по корню (E0502), а правило А+ их примет.
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import measure  # noqa: E402

RULE = 'В на Б3 + 6а'


def message(e):
    """(код, сообщение) события."""
    root, text = e['root'], e['text']
    rule, kind = e['rule'], e['kind']
    mut = e['detail'].split(',')[0].endswith(' mut')
    loop = "'for ... in &mut'" if mut else "'for ... in &'"
    if rule == 'loop' and kind == 'read':
        return 'E0412', (f"'{root}' is read while {loop} at {e['other']} changes it; read the value before the "
                         f"loop: 'let v = {text}'")
    if rule == 'loop' and kind == 'nested':
        return 'E0412', (f"'{root}' is iterated again while {loop} at {e['other']} iterates over it; iterate "
                         f"over indexes: 'for i in 0..len({root})'")
    if rule == 'loop':
        return 'E0412', (f"cannot change '{root}' while {loop} at {e['other']} iterates over it; change it "
                         f"through the loop variable, or iterate over indexes: 'for i in 0..len({root})'")
    if rule == 'view':
        return 'E0502', (f"'{text}' is a view into a value that is changed later in this expression; make a "
                         f"copy before the call: 'let old = {text.split('[')[0]}'")
    if rule == 'par' and kind == 'index_computed':
        return 'E0412', (f"'{text}' is written inside 'parallel for' at {e['other']} by an index other than the "
                         "loop variable; iterations may write the same element. Index by the loop variable")
    if rule == 'par' and kind == 'read_root':
        return 'E0412', (f"'{root}' is read inside 'parallel for' at {e['other']} while other iterations change "
                         "it; read the value before the loop")
    if rule == 'par':
        return 'E0412', (f"cannot change '{root}' inside 'parallel for' at {e['other']}: iterations run at the "
                         "same time. Write each result to 'out[i]' by the loop variable and combine them after "
                         "the loop")
    return 'E0502', (f"'{root}' is passed as '&mut' together with another reference into it; pass a copy of the "
                     "value instead")


def errors(dpl, cwd, root, own):
    """Блоки ошибок правила в файлах own программы с главным модулем root
    (в cwd). [] — правило не отвергает или в программе другие ошибки
    (их покажет сборка)."""
    r = subprocess.run([dpl, 'debug', 'access', root], cwd=cwd, capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        return []
    rejects = measure.RULES[RULE][1]
    seen, out = set(), []
    events = [json.loads(ln) for ln in r.stdout.splitlines() if ln.strip()]
    for e in sorted(events, key=lambda e: (e['file'], e['line'], e['col'])):
        site = (e['file'], e['line'], e['col'])
        if e['file'] not in own or site in seen or not rejects(e):
            continue
        seen.add(site)
        code, msg = message(e)
        try:
            with open(os.path.join(cwd, e['file']), encoding='utf-8') as f:
                src = f.read().splitlines()[e['line'] - 1]
        except (OSError, IndexError):
            src = ''
        n = str(e['line'])
        pad = ' ' * len(n)
        width = max(1, len(e['text']))
        out.append(f"{e['file']}:{e['line']}:{e['col']}: error[{code}]: {msg}\n  {n} | {src}\n  {pad} | "
                   f"{' ' * (e['col'] - 1)}^{'~' * (width - 1)}")
    return out
