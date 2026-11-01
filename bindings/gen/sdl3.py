#!/usr/bin/env python3
"""Генератор привязки SDL3 (Р58, Р62): заголовки bindings/sdl3/include/SDL3
→ bindings/sdl3/sdl3.dpm.

    python3 bindings/gen/sdl3.py           — записать sdl3.dpm
    python3 bindings/gen/sdl3.py --check   — сверить sdl3.dpm с генерацией

Функции и константы — из заголовков списка bindings/sdl3/headers.txt;
типы — из всех закреплённых заголовков, по мере надобности. Смысл
указателей (ввод, вывод, NULL) — по типу и описанию параметра в
документации заголовка; что не выражается однозначно — пропускается с
причиной. Правила — docs/bindings/README.md. Только стандартная
библиотека Python; повторная генерация даёт те же байты.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cheader import CType, Expr, Header, param_docs  # noqa: E402 — bindings/gen/cheader.py
from common import Skip, main as gen_main, snake  # noqa: E402 — bindings/gen/common.py

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(os.path.dirname(HERE), 'sdl3')
INCLUDE = os.path.join(DIR, 'include', 'SDL3')
HEADERS = os.path.join(DIR, 'headers.txt')
OUT = os.path.join(DIR, 'sdl3.dpm')
LIB = 'SDL3'
VOID = 'SDL_Void'
# Заголовки с макросами платформы и встроенными функциями — не разбираются:
# их типы (Uint8 …) заданы ниже.
UNPARSED = {'SDL_stdinc.h', 'SDL_platform_defines.h', 'SDL_begin_code.h', 'SDL_close_code.h', 'SDL_endian.h',
            'SDL_atomic.h', 'SDL_mutex.h', 'SDL_thread.h'}
BASIC = {'Uint8': 'u8', 'Sint8': 'i8', 'Uint16': 'u16', 'Sint16': 'i16', 'Uint32': 'u32', 'Sint32': 'i32',
         'Uint64': 'u64', 'Sint64': 'i64', 'bool': 'bool', 'int': 'i32', 'unsigned int': 'u32', 'float': 'f32',
         'double': 'f64', 'char': 'u8', 'unsigned char': 'u8', 'signed char': 'i8',
         # Цели dpl — 64-битные (docs/policy/PLATFORMS.md).
         'size_t': 'u64', 'intptr_t': 'i64', 'uintptr_t': 'u64', 'SDL_Time': 'i64'}
# Типы Vulkan в SDL_vulkan.h — из привязки vulkan.
VULKAN = {'VkInstance', 'VkPhysicalDevice', 'VkSurfaceKHR', 'VkAllocationCallbacks'}
# Объекты SDL с открытыми полями: указатель на них — объект, которого в
# dpl не получить (функции с ним пропускаются).
OBJECTS = {'SDL_Surface', 'SDL_Palette'}
# Условия препроцессора: цели 64-битные, little-endian.
MACROS = {'__LP64__': 1, 'SDL_BYTEORDER': 1234, 'SDL_LIL_ENDIAN': 1234, 'SDL_BIG_ENDIAN': 4321}
ANNOTATIONS = re.compile(r'\b(SDL_PRINTF_FORMAT_STRING|SDL_SCANF_FORMAT_STRING|SDL_WPRINTF_FORMAT_STRING|'
                         r'SDL_(IN|OUT|INOUT)_(Z_)?(BYTE)?CAP\([^)]*\))\s*')


def read_list():
    out = []
    with open(HEADERS, encoding='utf-8') as f:
        for ln in f:
            ln = ln.split('#', 1)[0].strip()
            if ln:
                out.append(ln)
    return out


class Gen:
    def __init__(self):
        self.listed = read_list()
        names = sorted(f for f in os.listdir(INCLUDE) if f.endswith('.h'))
        order = self.listed + [h for h in names if h not in self.listed]
        self.headers = [Header(h, INCLUDE, MACROS) for h in order if h not in UNPARSED]
        self.types = {}       # имя → (вид, данные, заголовок, позиция)
        self.consts = {}      # имя → значение (для выражений)
        self.const_out = {}   # заголовок → [(имя, тип, значение)]
        self.enum_vals = {}   # перечисление → [(имя, значение)]
        self.fns = []         # (заголовок, имя, тип результата, параметры, документация)
        self.skipped = []
        self.used = set()
        self.collect()

    # --- сбор объявлений

    def collect(self):
        pos = 0
        for h in self.headers:
            group = None  # константы #define — типа предшествующего typedef целого
            for it in h.items:
                pos += 1
                if it[0] == 'define':
                    self.define(h.name, it[1], it[2], group)
                    continue
                decl, doc = it[1], it[2]
                group = None
                m = re.match(r'extern SDL_DECLSPEC (.*?)\s*SDLCALL (\w+)\s*\(', decl)
                if m:
                    if h.name in self.listed:
                        self.fns.append((h.name, m.group(2), m.group(1), self.paren(decl, m.end() - 1), doc))
                    continue
                m = re.fullmatch(r'typedef (struct|union) (\w+) \{(.*)\} (\w+)', decl)
                if m:
                    self.types[m.group(4)] = (m.group(1), m.group(3), h.name, pos)
                    continue
                m = re.fullmatch(r'(struct|union) (\w+) \{(.*)\}', decl)
                if m:
                    self.types[m.group(2)] = (m.group(1), m.group(3), h.name, pos)
                    continue
                m = re.fullmatch(r'typedef enum (\w+ )?\{(.*)\} (\w+)', decl)
                if m:
                    self.types[m.group(3)] = ('enum', None, h.name, pos)
                    self.enum(m.group(3), m.group(2))
                    continue
                m = re.fullmatch(r'typedef (struct|union) (\w+) (\w+)', decl)
                if m:
                    self.types.setdefault(m.group(3), ('opaque', None, h.name, pos))
                    continue
                # Дескриптор — указатель на скрытую структуру или void*: SDL_GLContext.
                m = re.fullmatch(r'typedef (?:struct \w+|void) ?\* ?(\w+)', decl)
                if m:
                    self.types[m.group(1)] = ('handle', None, h.name, pos)
                    continue
                m = re.fullmatch(r'typedef (.*?)\(SDLCALL \*(\w+)\)\s*\((.*)\)', decl)
                if m:
                    self.types[m.group(2)] = ('fnptr', (m.group(1), m.group(3)), h.name, pos)
                    continue
                m = re.fullmatch(r'typedef ([\w ]+?) (\w+)', decl)
                if m:
                    self.types[m.group(2)] = ('alias', m.group(1), h.name, pos)
                    if self.scalar(m.group(1)) and self.scalar(m.group(1)) != 'bool':
                        group = m.group(2)

    @staticmethod
    def paren(text, at):
        """Текст в скобках, открытых в позиции at."""
        depth = 0
        for j in range(at, len(text)):
            if text[j] == '(':
                depth += 1
            elif text[j] == ')':
                depth -= 1
                if depth == 0:
                    return text[at + 1:j]
        raise SystemExit(f'sdl3.py: нет ) в {text[:60]}')

    def define(self, header, name, value, group):
        if not value or name.endswith('_h_') or name.startswith('SDL_PLATFORM'):
            return
        out = self.const_out.setdefault(header, [])
        if re.fullmatch(r'"[^"\\]*"', value):
            if header in self.listed:
                out.append((name, 'str', value))
            return
        try:
            v = Expr(value, self.consts).value()
        except (ValueError, ZeroDivisionError, IndexError):
            return
        self.consts[name] = v
        if header not in self.listed:
            return
        ty = self.scalar(group) if group else None
        if ty is None:
            lits = re.findall(r'\b0[xX][0-9a-fA-F]+([uUlL]*)|\b\d+([uUlL]*)', value)
            suffix = ''.join(a + b for a, b in lits).lower()
            if 'SDL_UINT64_C' in value or 'll' in suffix:
                ty = 'u64'
            elif 'u' in suffix:
                ty = 'u32' if v < 1 << 32 else 'u64'
            else:
                ty = 'i32' if -(1 << 31) <= v < 1 << 31 else 'i64'
        out.append((name, ty, v))

    def enum(self, name, body):
        vals, nxt = [], 0
        for part in [p.strip() for p in body.split(',')]:
            if not part:
                continue
            m = re.fullmatch(r'(\w+)(?:\s*=\s*(.*))?', part)
            if not m:
                raise SystemExit(f'sdl3.py: значение перечисления {name}: {part}')
            v = Expr(m.group(2), self.consts).value() if m.group(2) else nxt
            self.consts[m.group(1)] = v
            vals.append((m.group(1), v))
            nxt = v + 1
        self.enum_vals[name] = vals

    # --- перевод типов

    def resolve(self, name):
        """Имя через typedef-псевдонимы до основы."""
        seen = 0
        while name in self.types and self.types[name][0] == 'alias' and seen < 16:
            name = self.types[name][1]
            seen += 1
        return name

    def scalar(self, name):
        name = self.resolve(name)
        if name in BASIC:
            return BASIC[name]
        if name in self.types and self.types[name][0] == 'enum':
            return 'i32'
        return None

    def kind(self, name):
        name = self.resolve(name)
        return self.types[name][0] if name in self.types else None

    def use(self, name):
        name = self.resolve(name)
        if name in self.used or name not in self.types:
            return
        self.used.add(name)
        k, data = self.types[name][0], self.types[name][1]
        if k in ('struct', 'union'):
            for _, ty, _, _ in self.fields(name):
                for t in re.findall(r'\b(SDL_\w+)', ty):
                    if t != VOID:
                        self.use(t)

    def fnptr(self, name, optional):
        ret, params = self.types[self.resolve(name)][1]
        ps = []
        if params.strip() != 'void':
            for p in self.split_params(params):
                ct = CType(self.strip_name(p))
                ps.append(self.callback_type(ct))
        r = CType(ret)
        rt = 'void' if r.base == 'void' and not r.stars else self.callback_type(r)
        s = 'extern fn(' + ', '.join(ps) + ')' + ('' if rt == 'void' else ' ' + rt)
        return ('?' if optional else '') + s

    def callback_type(self, ct):
        if ct.stars == 0:
            s = self.scalar(ct.base)
            if s:
                return s
            if self.kind(ct.base) == 'handle':
                self.use(ct.base)
                return '?' + self.resolve(ct.base)
            raise Skip(f'тип {ct.base} в указателе на функцию')
        if ct.stars == 1 and ct.base == 'void':
            return '?' + VOID
        if ct.stars == 1 and self.kind(ct.base) == 'opaque':
            self.use(ct.base)
            return '?' + self.resolve(ct.base)
        if ct.stars == 1 and self.kind(ct.base) in ('struct', 'union') and self.resolve(ct.base) not in OBJECTS:
            self.use(ct.base)
            return ('&' if ct.const else '&mut ') + self.resolve(ct.base)
        raise Skip(f'тип {ct.base}{"*" * ct.stars} в указателе на функцию')

    @staticmethod
    def split_params(params):
        return [p.strip() for p in params.split(',') if p.strip()]

    @staticmethod
    def strip_name(p):
        """Объявление параметра без имени: 'const char *title' → 'const char *'."""
        p = ANNOTATIONS.sub('', p).strip()
        m = re.match(r'(.*?[\s*])(\w+)(\[\d*\])?$', p)
        if m and m.group(1).strip() and m.group(2) not in ('void',):
            t = m.group(1)
            return t + ('*' if m.group(3) is not None else '')
        return p

    def fields(self, name):
        """Поля структуры: [(имя dpl, тип dpl, комментарий, имя C)]."""
        k, body = self.types[name][0], self.types[name][1]
        out = []
        for member in [m.strip() for m in body.split(';') if m.strip()]:
            fm = re.fullmatch(r'(.*?)\(SDLCALL \*(\w+)\)\s*\((.*)\)', member)
            if fm:
                out.append((snake(fm.group(2)), '?' + VOID, 'указатель на функцию', fm.group(2)))
                continue
            m = re.fullmatch(r'((?:const |struct |unsigned |signed )*\w+)\s*(.*)', member)
            base, decls = m.group(1), m.group(2)
            for d in [x.strip() for x in decls.split(',')]:
                dm = re.fullmatch(r'((?:\*\s*(?:const\s*)?)*)(\w+)((?:\[[^\]]+\])*)', d)
                if not dm:
                    raise Skip(f'поле {name}: {member}')
                stars, fname = dm.group(1).count('*'), dm.group(2)
                dims = re.findall(r'\[([^\]]+)\]', dm.group(3))
                out.append((snake(fname), self.field_type(name, base, stars, dims), '', fname))
        return out

    def field_type(self, sname, base, stars, dims):
        ct = CType(base + ' ' + '*' * stars)
        if stars:
            if self.kind(ct.base) == 'opaque' and stars == 1:
                return '?' + self.resolve(ct.base)
            return '?' + VOID  # указатель C без операций: строки и буферы событий
        if self.kind(ct.base) == 'fnptr':
            return self.fnptr(ct.base, True)
        if self.kind(ct.base) == 'handle':
            return '?' + self.resolve(ct.base)
        t = self.scalar(ct.base)
        if t is None:
            k = self.kind(ct.base)
            if k in ('struct', 'union'):
                t = self.resolve(ct.base)
            else:
                raise Skip(f'поле {sname}: тип {ct.base}')
        for d in reversed(dims):
            try:
                n = Expr(d, self.consts).value()
            except ValueError:
                raise Skip(f'поле {sname}: длина массива {d}')
            t = f'[{n}]{t}'
        return t

    # --- функции

    def param(self, p, docs, fname):
        p = ANNOTATIONS.sub('', p).strip()
        if p == '...':
            raise Skip('переменное число аргументов')
        m = re.match(r'(.*?)(\w+)((?:\[\d*\])?)$', p)
        tpart, pname, arr = m.group(1).strip(), m.group(2), m.group(3)
        if not tpart:  # параметр без имени
            tpart, pname = p, ''
        ct = CType(tpart + (' *' if arr else ''))
        doc = docs.get(pname, '')
        nullable = 'NULL' in doc and 'not be NULL' not in doc and 'must not' not in doc
        array = re.search(r'\barray\b', doc) is not None
        base = self.resolve(ct.base)
        name = snake(pname) if pname else f'a{len(docs)}'
        if ct.stars == 0:
            if base in VULKAN:
                return name, ('?' if nullable else '') + 'vulkan.' + base
            s = self.scalar(base)
            if s:
                return name, s
            k = self.kind(base)
            if k == 'fnptr':
                return name, self.fnptr(base, nullable)
            if k == 'handle':
                self.use(base)
                return name, ('?' if nullable else '') + base
            if k in ('struct', 'union') and base not in OBJECTS:
                self.use(base)
                return name, base
            raise Skip(f'параметр {pname}: {tpart}')
        if ct.stars == 1:
            if base == 'char':
                if not ct.const:
                    raise Skip(f'параметр {pname}: буфер строки')
                return name, '?str' if nullable else 'str'
            if base == 'void':
                if ct.const:
                    raise Skip(f'параметр {pname}: const void*')
                return name, '?' + VOID
            if base in VULKAN:
                if base == 'VkAllocationCallbacks':
                    return name, '?&vulkan.VkAllocationCallbacks'
                if not ct.const:
                    return name, '&mut ?vulkan.' + base
                raise Skip(f'параметр {pname}: const {base}*')
            k = self.kind(base)
            if k == 'opaque':
                self.use(base)
                return name, ('?' if nullable else '') + base
            if base in OBJECTS:
                raise Skip(f'параметр {pname}: объект {base}')
            s = self.scalar(base)
            if k in ('struct', 'union') or s:
                t = s or base
                if not s:
                    self.use(base)
                if ct.const:
                    if array:
                        return name, '[]' + t
                    return name, ('?&' if nullable else '&') + t
                if array:
                    raise Skip(f'параметр {pname}: массив вывода')
                return name, '&mut ' + t
        raise Skip(f'параметр {pname}: {tpart}')

    def result(self, ret, docs):
        ct = CType(ret)
        doc = docs.get('', '')
        base = self.resolve(ct.base)
        if ct.stars == 0:
            if base == 'void':
                return ''
            s = self.scalar(base)
            if s:
                return ' ' + s
            if self.kind(base) in ('struct', 'union') and base not in OBJECTS:
                self.use(base)
                return ' ' + base
            if self.kind(base) == 'handle':
                self.use(base)
                return ' ?' + base
            raise Skip(f'результат {ret}')
        if ct.stars == 1:
            if base == 'char':
                if not ct.const:
                    raise Skip('результат — строка, которую освобождает вызывающий (SDL_free)')
                return ' ?str' if 'NULL' in doc else ' str'
            if base == 'void':
                return ' ?' + VOID
            if self.kind(base) == 'opaque':
                self.use(base)
                return ' ?' + base
        raise Skip(f'результат {ret}: указатель на данные SDL')

    def function(self, header, name, ret, params, doc):
        docs = param_docs(doc)
        r = self.result(ret, docs)
        ps = []
        if params.strip() != 'void':
            for p in self.split_params(params):
                ps.append(self.param(p, docs, name))
        return f'pub extern fn {name}(' + ', '.join(f'{n}: {t}' for n, t in ps) + ')' + r

    # --- вывод

    def run(self):
        lines = []
        fn_lines = []
        for header, name, ret, params, doc in self.fns:
            try:
                fn_lines.append((header, self.function(header, name, ret, params, doc)))
            except Skip as s:
                self.skipped.append((name, str(s)))
        # Типы списка — все; остальные — по надобности.
        for n, (k, _, h, _) in self.types.items():
            if h in self.listed and k in ('struct', 'union', 'opaque', 'handle', 'enum'):
                self.use(n)
        structs = self.structs = {}
        for n in sorted(self.used, key=lambda x: self.types[x][3]):
            if self.types[n][0] in ('struct', 'union'):
                try:
                    structs[n] = self.fields(n)
                except Skip as s:
                    self.skipped.append((n, str(s)))
        version = '.'.join(str(self.consts[f'SDL_{p}_VERSION']) for p in ('MAJOR', 'MINOR', 'MICRO'))
        lines += ['// SPDX-License-Identifier: Zlib',
                  f'// Привязка SDL {version}: функции и константы заголовков из headers.txt.',
                  '// Сгенерировано bindings/gen/sdl3.py из include/SDL3 — не править (Р58).',
                  '// SDL: Copyright (C) 1997-2026 Sam Lantinga (LICENSE.txt).',
                  '// Правила перевода типов C — docs/bindings/README.md.',
                  'import vulkan',
                  '',
                  '// Указатель C без операций: userdata, строки и буферы в событиях.',
                  f'pub extern type {VOID}',
                  '']
        for h in self.listed:
            consts = self.const_out.get(h, [])
            enums = [n for n in sorted(self.used, key=lambda x: self.types[x][3])
                     if self.types[n][0] == 'enum' and self.types[n][2] == h]
            if not consts and not enums:
                continue
            lines.append(f'// {h}')
            for n, ty, v in consts:
                lines.append(f'pub const {n}: {ty} = {v}')
            for e in enums:
                lines.append(f'// {e}')
                for n, v in self.enum_vals[e]:
                    lines.append(f'pub const {n}: i32 = {v}')
            lines.append('')
        others = [n for n in sorted(self.used, key=lambda x: self.types[x][3])
                  if self.types[n][0] == 'enum' and self.types[n][2] not in self.listed]
        for e in others:
            lines.append(f'// {e} ({self.types[e][2]})')
            for n, v in self.enum_vals[e]:
                lines.append(f'pub const {n}: i32 = {v}')
            lines.append('')
        lines.append('// Непрозрачные типы SDL.')
        for n in sorted(self.used, key=lambda x: self.types[x][3]):
            if self.types[n][0] in ('opaque', 'handle') and n not in structs:
                lines.append(f'pub extern type {n}')
        lines.append('')
        for n, fields in structs.items():
            # Р76: структура C из заголовка — заголовок --lib включает его.
            lines.append(('@union' if self.types[n][0] == 'union' else '@c') + '("SDL3/SDL.h")')
            lines.append(f'pub struct {n} {{')
            for fn, ty, comment, _ in fields:
                lines.append(f'    {fn}: {ty}' + (f'  // {comment}' if comment else ''))
            lines.append('}')
            lines.append('')
        for header in self.listed:
            block = [ln for h, ln in fn_lines if h == header]
            if not block:
                continue
            lines.append(f'// {header}')
            for ln in block:
                lines.append(f'@link("{LIB}")')
                lines.append(ln)
            lines.append('')
        if self.skipped:
            lines.append('// Пропущено — не выражается в dpl v0.2:')
            for n, why in self.skipped:
                lines.append(f'// - {n}: {why}')
        return '\n'.join(lines).rstrip('\n') + '\n'


def generate():
    return Gen().run()


if __name__ == '__main__':
    sys.exit(gen_main(sys.argv[1:], generate, OUT, __doc__, 'sdl3.py'))
