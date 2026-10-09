"""Проверка JSON по схеме — подмножество JSON Schema 2020-12, которого
хватает схемам компилятора (docs/compiler/diagnostics.schema.json):
type, enum, required, properties, additionalProperties: false, items,
pattern, minimum, dependentRequired и $ref на #/$defs/…. Иное в схеме —
ошибка, а не молчаливый пропуск.
"""
import json
import re

KNOWN = {'$schema', '$id', 'title', 'description', '$defs', '$ref', 'type', 'enum', 'required', 'properties',
         'additionalProperties', 'items', 'pattern', 'minimum', 'dependentRequired'}
TYPES = {
    'object': lambda v: isinstance(v, dict),
    'array': lambda v: isinstance(v, list),
    'string': lambda v: isinstance(v, str),
    'integer': lambda v: isinstance(v, int) and not isinstance(v, bool),
    'boolean': lambda v: isinstance(v, bool),
}


def validate(value, schema, root=None, where='$'):
    """Список нарушений; пусто — значение подходит к схеме."""
    root = root or schema
    unknown = set(schema) - KNOWN
    if unknown:
        return [f'{where}: в схеме неизвестные ключи {sorted(unknown)}']
    if '$ref' in schema:
        ref = schema['$ref']
        if not ref.startswith('#/$defs/'):
            return [f'{where}: ссылка {ref} не поддерживается']
        return validate(value, root['$defs'][ref[len('#/$defs/'):]], root, where)
    errs = []
    t = schema.get('type')
    if t and not TYPES[t](value):
        return [f'{where}: ожидался {t}, а не {type(value).__name__}']
    if 'enum' in schema and value not in schema['enum']:
        errs.append(f'{where}: {value!r} не из {schema["enum"]}')
    if 'pattern' in schema and isinstance(value, str) and not re.search(schema['pattern'], value):
        errs.append(f'{where}: {value!r} не подходит к {schema["pattern"]}')
    if 'minimum' in schema and isinstance(value, int) and value < schema['minimum']:
        errs.append(f'{where}: {value} меньше {schema["minimum"]}')
    if isinstance(value, dict):
        props = schema.get('properties', {})
        for key in schema.get('required', []):
            if key not in value:
                errs.append(f'{where}: нет поля {key}')
        if schema.get('additionalProperties') is False:
            for key in value:
                if key not in props:
                    errs.append(f'{where}: лишнее поле {key}')
        for key, deps in schema.get('dependentRequired', {}).items():
            if key in value:
                errs += [f'{where}: есть {key}, но нет {d}' for d in deps if d not in value]
        for key, sub in props.items():
            if key in value:
                errs += validate(value[key], sub, root, f'{where}.{key}')
    if isinstance(value, list) and 'items' in schema:
        for i, item in enumerate(value):
            errs += validate(item, schema['items'], root, f'{where}[{i}]')
    return errs


def check_text(text, schema_path):
    """Нарушения схемы у документа JSON в тексте text."""
    with open(schema_path, encoding='utf-8') as f:
        schema = json.load(f)
    try:
        value = json.loads(text)
    except ValueError as e:
        return [f'не JSON: {e}']
    return validate(value, schema)
