#!/usr/bin/env python3
"""Генератор привязки Vulkan (Р58, Р62): bindings/vulkan/vk.xml →
bindings/vulkan/vulkan.dpm.

    python3 bindings/gen/vulkan.py           — записать vulkan.dpm
    python3 bindings/gen/vulkan.py --check   — сверить vulkan.dpm с генерацией

Объём — ядро 1.0–1.3 и расширения из bindings/vulkan/extensions.txt.
Правила перевода типов C в dpl — docs/bindings/README.md. Только
стандартная библиотека Python; результат зависит только от входа:
повторная генерация даёт те же байты.
"""
import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(os.path.dirname(HERE), 'vulkan')
XML = os.path.join(DIR, 'vk.xml')
EXTENSIONS = os.path.join(DIR, 'extensions.txt')
OUT = os.path.join(DIR, 'vulkan.dpm')

FEATURES = ['VK_VERSION_1_0', 'VK_VERSION_1_1', 'VK_VERSION_1_2', 'VK_VERSION_1_3']
LIB = 'vulkan'
# Ключевые слова dpl (compiler/syntax/tokens.def) и зарезервированное.
KEYWORDS = {'fn', 'struct', 'enum', 'const', 'import', 'pub', 'let', 'var', 'if', 'else', 'while', 'for', 'in', 'match',
            'return', 'break', 'continue', 'defer', 'errdefer', 'fail', 'try', 'parallel', 'none', 'true', 'false', 'mut',
            'extern', 'unsafe'}
BASIC = {'uint8_t': 'u8', 'uint16_t': 'u16', 'uint32_t': 'u32', 'uint64_t': 'u64', 'int8_t': 'i8', 'int16_t': 'i16',
         'int32_t': 'i32', 'int64_t': 'i64', 'float': 'f32', 'double': 'f64', 'char': 'u8', 'int': 'i32',
         # Цели dpl — 64-битные (docs/policy/PLATFORMS.md): size_t — u64.
         'size_t': 'u64'}
# void* без длины и вне pNext: дескриптор C без операций (pUserData, ppData).
VOID = 'VkVoid'
# Исключения правил — с причиной в комментарии поля.
OVERRIDES = {
    # SPIR-V читают из файла байтами: срез байт, счётчик — размер в байтах.
    ('VkShaderModuleCreateInfo', 'pCode'): ('[]u8', 'codeSize', 'SPIR-V байтами: code_size — длина в байтах'),
}


class Skip(Exception):
    """Объявление не выражается в dpl — пропуск с причиной."""


def api_ok(e):
    a = e.get('api')
    return a is None or 'vulkan' in a.split(',')


def snake(name):
    """camelCase → snake_case: pNext → p_next, deviceUUID → device_uuid."""
    out = []
    for i, c in enumerate(name):
        if c.isupper():
            prev = name[i - 1] if i else ''
            nxt = name[i + 1] if i + 1 < len(name) else ''
            if prev.islower() or (prev.isupper() and nxt.islower()):
                out.append('_')
            out.append(c.lower())
        else:
            out.append(c)
    s = ''.join(out)
    return s + '_' if s in KEYWORDS else s


def depends_ok(expr, enabled):
    """Выражение depends vk.xml: «+» — и, «,» — или, скобки."""
    toks = re.findall(r'[A-Za-z0-9_]+|[+,()]', expr)
    pos = 0

    def atom():
        nonlocal pos
        t = toks[pos]
        pos += 1
        if t == '(':
            v = alt()
            pos += 1  # ')'
            return v
        return t in enabled

    def conj():
        nonlocal pos
        v = atom()
        while pos < len(toks) and toks[pos] == '+':
            pos += 1
            v = atom() and v
        return v

    def alt():
        nonlocal pos
        v = conj()
        while pos < len(toks) and toks[pos] == ',':
            pos += 1
            v = conj() or v
        return v

    return alt()


def read_extensions():
    out = []
    with open(EXTENSIONS, encoding='utf-8') as f:
        for ln in f:
            ln = ln.split('#', 1)[0].strip()
            if ln:
                out.append(ln)
    return out


class Registry:
    def __init__(self, root, extensions):
        self.root = root
        self.types = {}      # имя → элемент <type>
        self.order = []      # имена типов в порядке vk.xml
        for t in root.find('types'):
            if t.tag != 'type' or not api_ok(t):
                continue
            n = t.get('name') or (t.find('name').text if t.find('name') is not None else None)
            if n and n not in self.types:
                self.types[n] = t
                self.order.append(n)
        self.enum_blocks = {e.get('name'): e for e in root.findall('enums')}
        self.commands = {}
        self.cmd_order = []
        for c in root.find('commands'):
            if not api_ok(c):
                continue
            n = c.get('name') if c.get('alias') else c.find('proto').find('name').text
            self.commands[n] = c
            self.cmd_order.append(n)
        self.header_version = None
        for t in root.find('types'):
            if t.get('category') == 'define' and api_ok(t) and t.find('name') is not None:
                if t.find('name').text == 'VK_HEADER_VERSION':
                    self.header_version = int(t.find('name').tail.strip())
        self.select(extensions)

    # --- что входит в привязку

    def select(self, extensions):
        enabled = set(FEATURES) | set(extensions)
        self.req_types, self.req_cmds = set(), []
        self.added = {}       # тип перечисления → [(имя, значение или None, alias)]
        self.loose = []       # константы расширений и API: (имя, элемент, номер расширения)
        self.versions = []    # (имя, major, minor)
        exts = {e.get('name'): e for e in self.root.find('extensions')}
        blocks = []
        for f in self.root.findall('feature'):
            if f.get('name') in FEATURES and api_ok(f):
                blocks.append((f, None))
                major, minor = f.get('number').split('.')
                self.versions.append((f.get('name').replace('VK_VERSION', 'VK_API_VERSION'), int(major), int(minor)))
        for name in extensions:
            e = exts.get(name)
            if e is None:
                sys.exit(f'vulkan.py: расширения {name} нет в vk.xml')
            if 'vulkan' not in e.get('supported', '').split(','):
                sys.exit(f'vulkan.py: расширение {name} не для vulkan')
            if e.get('depends') and not depends_ok(e.get('depends'), enabled):
                sys.exit(f'vulkan.py: расширению {name} нужно {e.get("depends")}')
            blocks.append((e, int(e.get('number'))))
        seen_cmd = set()
        for block, number in blocks:
            for r in block.findall('require'):
                if not api_ok(r) or (r.get('depends') and not depends_ok(r.get('depends'), enabled)):
                    continue
                for x in r:
                    if x.tag == 'type':
                        self.req_types.add(x.get('name'))
                    elif x.tag == 'command' and x.get('name') not in seen_cmd:
                        seen_cmd.add(x.get('name'))
                        self.req_cmds.append(x.get('name'))
                    elif x.tag == 'enum' and api_ok(x):
                        if x.get('extends'):
                            self.added.setdefault(x.get('extends'), []).append((x, number))
                        else:
                            self.loose.append((x, number))
        # Замыкание типов: поля, параметры, маски и их биты.
        self.used = set()
        for n in list(self.req_types):
            self.use(n)
        for c in self.req_cmds:
            cmd = self.commands[c]
            if cmd.get('alias'):
                continue
            for p in [cmd.find('proto')] + cmd.findall('param'):
                if api_ok(p):
                    self.use(p.find('type').text)

    def use(self, n):
        if n in self.used or n not in self.types:
            return
        self.used.add(n)
        t = self.types[n]
        if t.get('alias'):
            self.use(t.get('alias'))
            return
        for m in t.findall('member'):
            if api_ok(m):
                self.use(m.find('type').text)
        for x in t.findall('type'):
            self.use(x.text)
        if t.get('category') == 'bitmask':
            r = t.get('requires') or t.get('bitvalues')
            if r:
                self.use(r)

    # --- значения перечислений

    @staticmethod
    def enum_value(e, number):
        if e.get('value') is not None:
            return e.get('value')
        if e.get('bitpos') is not None:
            return str(1 << int(e.get('bitpos')))
        if e.get('offset') is not None:
            ext = int(e.get('extnumber') or number)
            v = 1000000000 + (ext - 1) * 1000 + int(e.get('offset'))
            return str(-v if e.get('dir') == '-' else v)
        return None

    def values(self, name):
        """Значения перечисления или маски: [(имя, значение | None, alias)]."""
        out, seen = [], set()
        block = self.enum_blocks.get(name)
        if block is not None:
            for e in block.findall('enum'):
                if api_ok(e) and e.get('name') not in seen:
                    seen.add(e.get('name'))
                    out.append((e.get('name'), self.enum_value(e, None), e.get('alias')))
        for e, number in self.added.get(name, []):
            if e.get('name') not in seen:
                seen.add(e.get('name'))
                out.append((e.get('name'), self.enum_value(e, number), e.get('alias')))
        return out


class Gen:
    def __init__(self, reg):
        self.reg = reg
        self.out = []
        self.skipped = []          # (имя, причина)
        self.int_of = {}           # имя типа C → целое dpl (перечисления, маски, базовые)
        self.kind = {}             # имя типа C → категория
        self.borrowing = {}        # структура → заимствующая (Р62)
        self.structs = {}          # имя → (поля dpl) после перевода
        for n in reg.order:
            if n not in reg.used:
                continue
            t = reg.types[n]
            cat = t.get('category')
            self.kind[n] = cat
        for n in reg.order:
            if n in reg.used:
                self.classify(n)

    def target(self, n):
        t = self.reg.types.get(n)
        while t is not None and t.get('alias'):
            n = t.get('alias')
            t = self.reg.types.get(n)
        return n

    def classify(self, n):
        t = self.reg.types[n]
        cat = t.get('category')
        if t.get('alias'):
            return
        if cat == 'basetype':
            base = t.find('type')
            if base is not None and base.text in BASIC:
                self.int_of[n] = BASIC[base.text]
        elif cat == 'bitmask':
            base = t.find('type').text
            self.int_of[n] = 'u64' if base == 'VkFlags64' else 'u32'
            bits = t.get('requires') or t.get('bitvalues')
            if bits:
                self.int_of[bits] = self.int_of[n]
        elif cat == 'enum':
            block = self.reg.enum_blocks.get(n)
            if block is not None and block.get('type') == 'bitmask':
                self.int_of.setdefault(n, 'u64' if block.get('bitwidth') == '64' else 'u32')
            else:
                self.int_of.setdefault(n, 'i32')

    def scalar(self, n):
        """Тип C без указателя → dpl для числа, или None."""
        n = self.target(n)
        if n in BASIC:
            return BASIC[n]
        if n in self.int_of:
            v = self.int_of[n]
            return self.scalar(v) if v in self.reg.types else v
        return None

    def cat(self, n):
        return self.kind.get(self.target(n))

    # --- указатели на функции C (funcpointer) — тип extern fn(...)

    def funcpointer(self, n, optional):
        t = self.reg.types[self.target(n)]
        text = ''.join(t.itertext())
        m = re.match(r'typedef\s+(.*?)\(VKAPI_PTR \*\s*(\w+)\)\((.*)\);', text, re.S)
        if not m:
            raise Skip(f'указатель на функцию {n} не разобран')
        ret = self.c_decl_type(m.group(1).strip(), result=True)
        params = []
        body = m.group(3).strip()
        if body != 'void':
            for p in body.split(','):
                p = ' '.join(p.split())
                pm = re.match(r'(.*?)(\w+)$', p)
                params.append(self.c_decl_type(pm.group(1).strip(), result=False))
        s = 'extern fn(' + ', '.join(params) + ')' + ('' if ret == 'void' else ' ' + ret)
        return ('?' if optional else '') + s

    def c_decl_type(self, decl, result):
        """Тип из текста C в сигнатуре указателя на функцию."""
        const = decl.startswith('const ')
        base = decl.replace('const ', '').replace('struct ', '').strip()
        stars = base.count('*')
        base = base.replace('*', '').strip()
        if stars == 0:
            if base == 'void':
                return 'void'
            s = self.scalar(base)
            if s:
                return s
            raise Skip(f'тип {decl} указателя на функцию')
        if stars == 1 and base == 'void':
            return '?' + VOID
        if stars == 1 and base == 'char' and const and not result:
            raise Skip('строка в указателе на функцию')
        if stars == 1 and const and self.cat(base) == 'struct' and not result:
            return '&' + base
        raise Skip(f'тип {decl} указателя на функцию')

    # --- поля структур

    def member(self, sname, m, returned, fields):
        """Поле структуры → (имя, тип, атрибут @count или '', значение по умолчанию, комментарий)."""
        tname = m.find('type').text
        nm = m.find('name')
        cname = nm.text
        prefix = (m.text or '').strip()
        stars = (m.find('type').tail or '').count('*')
        const = prefix.startswith('const')
        dims = re.findall(r'\[(\w+)\]', (nm.tail or '') + ''.join((e.text or '') + (e.tail or '') for e in m
                                                                    if e.tag == 'enum'))
        optional = (m.get('optional') or '').split(',')[0] == 'true'
        length = m.get('len')
        name = snake(cname)
        comment = ''
        if (sname, cname) in OVERRIDES:
            t, counter, why = OVERRIDES[(sname, cname)]
            return name, t, f'@count({snake(counter)}) ', None, why
        if cname == 'sType' and m.get('values'):
            return name, 'i32', '', m.get('values'), ''
        if cname == 'pNext':
            if returned:  # Р62: цепочка вывода не выражается — только NULL
                return name, '?' + VOID, '', None, 'цепочка вывода — только none'
            return name, '?&' + ('VkBaseOutStructure' if sname == 'VkBaseOutStructure' else 'VkBaseInStructure'), '', None, ''
        if ':' in (nm.tail or ''):
            raise Skip('битовое поле')
        if stars == 0:
            elem = self.value_type(tname, optional or returned, returned)
            for d in reversed(dims):
                n = self.array_len(d)
                elem = f'[{n}]' + (elem if not elem.startswith('extern') else elem)
            if self.cat(tname) in ('enum', 'bitmask'):
                comment = self.target(tname)
            return name, elem, '', None, comment
        count = ''
        counted = False
        if length and length != 'null-terminated' and not m.get('altlen'):
            counter = length.split(',')[0]
            names = [f.find('name').text for f in fields]
            if counter in names and not optional and m.get('noautovalidity') != 'true':
                count = f'@count({snake(counter)}) '
                counted = True
        if stars == 1:
            if tname == 'char' and const and length and 'null-terminated' in length:
                return name, 'str', '', None, ''
            if tname == 'void':
                if const and length:
                    return name, '[]u8', count, None, ''
                if not const:
                    return name, '?' + VOID, '', None, ''
                raise Skip(f'поле {cname}: const void* без длины')
            if not const:  # вывод C внутри входной структуры: только NULL
                return name, '?' + VOID, '', None, 'вывод C — только none'
            if length:
                return name, '[]' + self.slice_elem(tname), count, None, ''
            if self.cat(tname) in ('struct', 'union'):
                return name, ('?&' if optional else '&') + self.target(tname), '', None, ''
            s = self.scalar(tname)
            if s:
                return name, ('?&' if optional else '&') + s, '', None, ''
            raise Skip(f'поле {cname}')
        if stars == 2 and tname == 'char' and const and length and 'null-terminated' in length:
            return name, '[]str', count, None, ''
        raise Skip(f'поле {cname}: указатель на указатель')

    def array_len(self, d):
        if d.isdigit():
            return int(d)
        for e in self.reg.enum_blocks['API Constants'].findall('enum'):
            if e.get('name') == d:
                if e.get('alias'):
                    return self.array_len(e.get('alias'))
                return int(e.get('value'))
        raise Skip(f'длина массива {d}')

    def value_type(self, tname, optional, returned):
        s = self.scalar(tname)
        if s:
            return s
        c = self.cat(tname)
        t = self.target(tname)
        if c == 'handle':
            return ('?' if optional else '') + t
        if c in ('struct', 'union'):
            return t
        if c == 'funcpointer':
            return self.funcpointer(tname, optional)
        raise Skip(f'тип {tname}')

    def slice_elem(self, tname):
        s = self.scalar(tname)
        if s:
            return s
        c = self.cat(tname)
        if c in ('handle', 'struct', 'union'):
            return self.target(tname)
        raise Skip(f'элемент среза {tname}')

    # --- вывод

    def emit(self, s=''):
        self.out.append(s)

    def run(self):
        reg = self.reg
        self.emit('// SPDX-License-Identifier: Apache-2.0 OR MIT')
        self.emit(f'// Привязка Vulkan {self.version()}: ядро 1.0–1.3 и расширения из extensions.txt.')
        self.emit('// Сгенерировано bindings/gen/vulkan.py из vk.xml — не править (Р58).')
        self.emit('// vk.xml: Copyright 2015-2024 The Khronos Group Inc.')
        self.emit('// Правила перевода типов C — docs/bindings/README.md.')
        self.emit()
        self.emit('// void* без длины: указатель C без операций (pUserData, отображённая память).')
        self.emit(f'pub extern type {VOID}')
        self.emit()
        self.versions_and_constants()
        self.enums()
        self.handles()
        self.struct_defs()
        self.command_defs()
        if self.skipped:
            self.emit('// Пропущено — не выражается в dpl v0.2:')
            for n, why in self.skipped:
                self.emit(f'// - {n}: {why}')
        return '\n'.join(self.out).rstrip('\n') + '\n'

    def version(self):
        return f'1.3.{self.reg.header_version}'

    def versions_and_constants(self):
        reg = self.reg
        self.emit('// Версии API: VK_MAKE_API_VERSION(variant, major, minor, patch).')
        self.emit('pub fn VK_MAKE_API_VERSION(variant: u32, major: u32, minor: u32, patch: u32) u32 {')
        self.emit('    return (variant << 29) | (major << 22) | (minor << 12) | patch')
        self.emit('}')
        self.emit()
        for name, major, minor in reg.versions:
            self.emit(f'pub const {name}: u32 = {(major << 22) | (minor << 12)}')
        self.emit(f'pub const VK_HEADER_VERSION: u32 = {reg.header_version}')
        self.emit()
        api = {e.get('name'): e for e in reg.enum_blocks['API Constants'].findall('enum')}
        wanted = []
        for e, number in reg.loose:
            n = e.get('name')
            if n in api:
                wanted.append((n, api[n], None))
            else:
                wanted.append((n, e, number))
        # Длины массивов в полях — тоже константы API.
        self.emit('// Константы API и расширений.')
        seen = set()
        for n, e, number in wanted:
            if n in seen:
                continue
            seen.add(n)
            self.constant(n, e, number, api)
        self.emit()

    def constant(self, n, e, number, api):
        if e.get('alias'):
            target = api.get(e.get('alias'))
            ty = self.const_type(target) if target is not None else None
            if ty:
                self.emit(f'pub const {n}: {ty} = {e.get("alias")}')
            return
        v = e.get('value')
        if v is None:
            v = Registry.enum_value(e, number)
        if v is None:
            return
        if v.startswith('"'):
            self.emit(f'pub const {n}: str = {v}')
            return
        ty = self.const_type(e) or 'u32'
        self.emit(f'pub const {n}: {ty} = {self.c_number(v, ty)}')

    def const_type(self, e):
        t = e.get('type')
        return BASIC.get(t) if t else None

    @staticmethod
    def c_number(v, ty):
        v = v.strip()
        m = re.fullmatch(r'\(~(\d+)U(LL)?\)', v)
        if m:
            bits = 64 if m.group(2) else 32
            return str(((1 << bits) - 1) ^ int(m.group(1)))
        if v.endswith('F'):
            return v[:-1]
        if v.startswith('0x'):
            return str(int(v, 16))
        return v

    def enums(self):
        reg = self.reg
        for n in reg.order:
            if n not in reg.used or reg.types[n].get('alias'):
                continue
            cat = reg.types[n].get('category')
            if cat not in ('enum', 'bitmask'):
                continue
            # У маски значения — у её битов (requires/bitvalues).
            if cat == 'bitmask':
                continue
            ty = self.int_of.get(n, 'i32')
            vals = reg.values(n)
            if not vals:
                continue
            self.emit(f'// {n}')
            known = set()
            for vn, v, alias in vals:
                if alias:
                    if alias in known:
                        self.emit(f'pub const {vn}: {ty} = {alias}')
                        known.add(vn)
                    continue
                if v is None:
                    continue
                self.emit(f'pub const {vn}: {ty} = {self.c_number(v, ty)}')
                known.add(vn)
            self.emit()

    def handles(self):
        reg = self.reg
        self.emit('// Дескрипторы. Неотправляемые (VK_DEFINE_NON_DISPATCHABLE_HANDLE) на 64-битных')
        self.emit('// целях — тоже указатели.')
        for n in reg.order:
            if n in reg.used and reg.types[n].get('category') == 'handle' and not reg.types[n].get('alias'):
                self.emit(f'pub extern type {n}')
        self.emit()

    def struct_defs(self):
        reg = self.reg
        done = {}
        for n in reg.order:
            t = reg.types[n]
            if n not in reg.used or t.get('alias') or t.get('category') not in ('struct', 'union'):
                continue
            returned = t.get('returnedonly') == 'true'
            members = [m for m in t.findall('member') if api_ok(m)]
            try:
                fields = [self.member(n, m, returned, members) for m in members]
            except Skip as s:
                self.skipped.append((n, str(s)))
                continue
            done[n] = fields
        # Заимствующие структуры (Р62): поле str, []T, &T, ?&T или такая структура.
        def borrows(n, depth=0):
            if n in self.borrowing:
                return self.borrowing[n]
            if depth > 64 or n not in done:
                return False
            b = False
            for _, ty, _, _, _ in done[n]:
                base = ty
                while base.startswith('['):
                    base = base[base.index(']') + 1:]
                if ty == 'str' or ty.startswith('[]') or ty.startswith('&') or ty.startswith('?&') or borrows(base, depth + 1):
                    b = True
            self.borrowing[n] = b
            return b
        for n in done:
            borrows(n)
        self.structs = done
        for n in reg.order:
            if n not in done:
                continue
            t = reg.types[n]
            union = t.get('category') == 'union'
            self.emit('@union' if union else '@c')
            self.emit(f'pub struct {n} {{')
            for name, ty, count, default, comment in done[n]:
                line = f'    {count}{name}: {ty}' + (f' = {default}' if default else '')
                if comment:
                    line += f'  // {comment}'
                self.emit(line)
            self.emit('}')
            self.emit()

    def command_defs(self):
        reg = self.reg
        for n in reg.req_cmds:
            c = reg.commands[n]
            if c.get('alias'):
                continue
            try:
                line = self.command(n, c)
            except Skip as s:
                self.skipped.append((n, str(s)))
                continue
            self.emit(f'@link("{LIB}")')
            self.emit(line)
        self.emit()

    def command(self, n, c):
        proto = c.find('proto')
        rt = proto.find('type').text
        rstars = (proto.find('type').tail or '').count('*')
        if rstars or self.cat(rt) == 'funcpointer':
            raise Skip('результат — указатель на функцию: адрес берут через загрузчик')
        ret = '' if rt == 'void' else ' ' + (self.scalar(rt) or self.fail(f'результат {rt}'))
        params = [p for p in c.findall('param') if api_ok(p)]
        out = []
        for p in params:
            out.append(f'{snake(p.find("name").text)}: {self.param(p)}')
        return f'pub extern fn {n}(' + ', '.join(out) + ')' + ret

    @staticmethod
    def fail(why):
        raise Skip(why)

    def param(self, p):
        tname = p.find('type').text
        nm = p.find('name')
        prefix = (p.text or '').strip()
        stars = (p.find('type').tail or '').count('*')
        const = prefix.startswith('const')
        optional = (p.get('optional') or '').split(',')[0] == 'true'
        length = p.get('len')
        array = re.search(r'\[(\w+)\]', nm.tail or '')
        if array:  # const float blendConstants[4] — указатель на элементы
            return '[]' + (self.scalar(tname) or self.fail(f'массив {tname}'))
        cat = self.cat(tname)
        if stars == 0:
            s = self.scalar(tname)
            if s:
                return s
            if cat == 'handle':
                return ('?' if optional else '') + self.target(tname)
            if cat in ('struct', 'union'):
                if self.borrowing.get(self.target(tname)):
                    raise Skip(f'заимствующая структура {tname} по значению')
                return self.target(tname)
            raise Skip(f'параметр {tname}')
        if stars == 1 and const:
            if tname == 'char':
                return '?str' if optional else 'str'
            if tname == 'void':
                return '[]u8'
            if length:
                return '[]' + self.slice_elem(tname)
            if cat in ('struct', 'union'):
                self.need_struct(tname)
                return ('?&' if optional else '&') + self.target(tname)
            s = self.scalar(tname)
            if s:
                return '&' + s
            raise Skip(f'параметр const {tname}*')
        if stars == 1:  # вывод C
            if tname == 'void':
                if length:
                    return '&mut List[u8]'
                raise Skip('void* без длины')
            if cat == 'handle':
                elem = '?' + self.target(tname)
            elif cat in ('struct', 'union'):
                self.need_struct(tname)
                if self.borrowing.get(self.target(tname)):
                    raise Skip(f'вывод в заимствующую структуру {self.target(tname)} (Р62: её нельзя &mut)')
                elem = self.target(tname)
            else:
                elem = self.scalar(tname) or self.fail(f'вывод {tname}')
            return f'&mut List[{elem}]' if length else f'&mut {elem}'
        if stars == 2 and tname == 'void' and not const:
            return '&mut ?' + VOID
        raise Skip(f'параметр {prefix} {tname}{"*" * stars}')

    def need_struct(self, tname):
        if self.target(tname) not in self.structs:
            raise Skip(f'структура {tname} пропущена')


def generate():
    root = ET.parse(XML).getroot()
    reg = Registry(root, read_extensions())
    return Gen(reg).run()


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--check', action='store_true', help='сверить vulkan.dpm с генерацией')
    a = ap.parse_args(argv)
    text = generate()
    rel = os.path.relpath(OUT, os.path.dirname(os.path.dirname(HERE)))
    if a.check:
        try:
            with open(OUT, encoding='utf-8') as f:
                old = f.read()
        except FileNotFoundError:
            old = None
        if old != text:
            print(f'{rel}: не совпадает с генерацией (python3 bindings/gen/vulkan.py)')
            return 1
        print(f'{rel}: совпадает с генерацией')
        return 0
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print(f'{rel}: записан, строк {text.count(chr(10))}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
