#!/usr/bin/env python3
"""Собирает краткие спецификации вариантов синтаксиса (v0.0, шаг 0.3).

Источник — один шаблон bench/syntax/spec_template.md:
- вставка ⟦AB: текст⟧ попадает только в спецификации перечисленных
  вариантов;
- блок кода ```dpl пишется в варианте A и печатается в записи
  каждого варианта переводчиком cm.py, поэтому примеры во всех
  спецификациях одни и те же и всегда разбираются;
- в конце — образцовые программы bench/syntax/samples/*/cm/sol.cm в
  записи варианта.

Результат — docs/research/syntax/spec_A.md, spec_B.md, spec_C.md;
вручную их не править. Свежесть и правильность примеров проверяет
bench/syntax/run_variants.py.

    python3 bench/syntax/mkspec.py   — пересобрать спецификации
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TEMPLATE = os.path.join(HERE, 'spec_template.md')
OUT_DIR = os.path.join(ROOT, 'docs', 'research', 'syntax')
SAMPLES = os.path.join(HERE, 'samples')
sys.path.insert(0, HERE)

import cm  # noqa: E402

CHOICE = re.compile(r'⟦([ABC]+): ?(.*?)⟧', re.S)
CODE = re.compile(r'```dpl\n(.*?)```', re.S)
HEADER = ('<!-- Сгенерировано bench/syntax/mkspec.py из bench/syntax/spec_template.md '
          'и bench/syntax/samples/; вручную не править. -->\n\n')


def out_path(variant):
    return os.path.join(OUT_DIR, f'spec_{variant}.md')


def sample_paths():
    return sorted(glob.glob(os.path.join(SAMPLES, '*', 'cm', 'sol.cm')))


def example_modules():
    """Блоки кода шаблона как модули варианта A (spec1, spec2, …)."""
    with open(TEMPLATE, encoding='utf-8') as f:
        text = f.read()
    mods = []
    for k, m in enumerate(CODE.finditer(text), 1):
        try:
            mods.append(cm.parse(m.group(1), 'A', f'spec{k}'))
        except cm.CmError as e:
            raise cm.CmError(f'spec_template.md, пример {k}: {e}') from None
    return text, mods


def examples(variant):
    """Примеры спецификации в записи варианта: имя модуля → текст."""
    _, mods = example_modules()
    info = cm.Info(mods)
    return {m.name: cm.print_module(m, variant, info) for m in mods}


def render(variant):
    text, mods = example_modules()
    info = cm.Info(mods)
    printed = iter(cm.print_module(m, variant, info) for m in mods)
    text = CHOICE.sub(lambda m: m.group(2) if variant in m.group(1) else '', text)
    text = CODE.sub(lambda m: '```dpl\n' + next(printed) + '```', text)
    parts = [HEADER, text.rstrip('\n'), '\n\n## Приложение. Образцовые программы\n']
    for p in sample_paths():
        smods = cm.load([p], 'A')
        name = os.path.basename(os.path.dirname(os.path.dirname(p)))
        parts.append(f'\n### {name}\n\n```dpl\n' + cm.print_module(smods[0], variant, cm.Info(smods)) + '```\n')
    return ''.join(parts)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for v in sorted(cm.VARIANTS):
        body = render(v)
        with open(out_path(v), 'w', encoding='utf-8') as f:
            f.write(body)
        print(f'{os.path.relpath(out_path(v), ROOT)}: {len(body)} символов')
    return 0


if __name__ == '__main__':
    sys.exit(main())
