#!/usr/bin/env python3
"""Генератор привязки JNI (Р58, Р63): bindings/jni/jni.h (AOSP) →
bindings/jni/jni.dpm.

    python3 bindings/gen/jni.py           — записать jni.dpm
    python3 bindings/gen/jni.py --check   — сверить jni.dpm с генерацией

Функции JNIEnv и JavaVM — записи таблиц JNINativeInterface и
JNIInvokeInterface: `@slot(N)`, N — место поля в таблице. Имена
параметров — из обёрток C++ того же заголовка. Правила —
docs/bindings/README.md. Только стандартная библиотека Python;
повторная генерация даёт те же байты.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cheader import CType, Header  # noqa: E402 — bindings/gen/cheader.py
from common import Skip, main as gen_main, snake  # noqa: E402 — bindings/gen/common.py

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(os.path.dirname(HERE), 'jni')
HEADER = 'jni.h'
OUT = os.path.join(DIR, 'jni.dpm')
VOID = 'JniVoid'
BASIC = {'jboolean': 'u8', 'jbyte': 'i8', 'jchar': 'u16', 'jshort': 'i16', 'jint': 'i32', 'jlong': 'i64',
         'jfloat': 'f32', 'jdouble': 'f64', 'jsize': 'i32', 'jobjectRefType': 'i32', 'char': 'u8'}
# Ссылки Java в C — один тип void* (jclass, jstring, массивы — его имена).
REFS = {'jobject', 'jclass', 'jstring', 'jarray', 'jobjectArray', 'jbooleanArray', 'jbyteArray', 'jcharArray',
        'jshortArray', 'jintArray', 'jlongArray', 'jfloatArray', 'jdoubleArray', 'jthrowable', 'jweak'}
HANDLES = {'jfieldID', 'jmethodID'}
TABLES = [('JNINativeInterface', 'JNIEnv'), ('JNIInvokeInterface', 'JavaVM')]
# Параметры void* с известным смыслом.
VOID_PARAMS = {
    ('GetEnv', 'env'): '&mut ?JNIEnv',
    ('AttachCurrentThread', 'thr_args'): '?&JavaVMAttachArgs',
    ('AttachCurrentThreadAsDaemon', 'thr_args'): '?&JavaVMAttachArgs',
    ('JNI_CreateJavaVM', 'vm_args'): '&JavaVMInitArgs',
    ('JNI_GetCreatedJavaVMs', 'vms'): '&mut List[?JavaVM]',
    ('JNI_GetCreatedJavaVMs', 'n_vms'): '&mut i32',
}
# Функции, которые экспортирует сама JVM (desktop — libjvm).
EXPORTED = {'JNI_CreateJavaVM': ['p_vm', 'p_env', 'vm_args'], 'JNI_GetCreatedJavaVMs': ['vms', 'len', 'n_vms']}
JVM_LIB = 'jvm'


class Gen:
    def __init__(self):
        self.header = Header(HEADER, DIR, {})
        with open(os.path.join(DIR, HEADER), encoding='utf-8') as f:
            self.raw = f.read()
        self.names = self.cpp_names()
        self.skipped = []

    def cpp_names(self):
        """Имена параметров по обёрткам C++: имя функции → [имена]."""
        out = {}
        for m in re.finditer(r'(\w+)\(([^()]*)\)\s*\{\s*(?:return\s+)?functions->(\w+)\(this', self.raw):
            if m.group(1) == m.group(3):
                out[m.group(1)] = self.param_names(m.group(2))
        # Обёртки из макросов: Call##_jname##MethodA(jobject obj, ...).
        for m in re.finditer(r'_jtype\s+(\w+(?:##\w+##)?\w*)\(([^()]*)\)', self.raw):
            pat = '^' + m.group(1).replace('##_jname##', r'\w+') + '$'
            out.setdefault(('re', pat), self.param_names(m.group(2)))
        return out

    @staticmethod
    def param_names(params):
        names = []
        for p in params.replace('\\', ' ').split(','):
            p = ' '.join(p.split())
            if p == '...':
                names.append('...')
            elif p:
                names.append(re.findall(r'(\w+)$', p)[0])
        return names

    def names_of(self, fn):
        if fn in self.names:
            return self.names[fn]
        # Шаблон макроса точнее, чем длиннее: CallStatic…MethodA прежде Call…MethodA.
        pats = sorted((k for k in self.names if isinstance(k, tuple)), key=lambda k: (-len(k[1]), k[1]))
        for k in pats:
            if re.match(k[1], fn):
                return self.names[k]
        return None

    # --- типы

    def value(self, ct, result):
        b = ct.base
        if ct.stars == 0:
            if b in BASIC:
                return BASIC[b]
            if b in REFS:
                return '?jobject'
            if b in HANDLES:
                return '?' + b
            if b == 'void' and result:
                return ''
        raise Skip(f'тип {b}{"*" * ct.stars}')

    def param(self, fn, ct, name, first):
        if first:
            return name, ct.base  # JNIEnv*, JavaVM* — объект с таблицей
        if (fn, name) in VOID_PARAMS:
            return name, VOID_PARAMS[(fn, name)]
        if ct.stars == 0:
            if ct.base == 'va_list':
                raise Skip('va_list')
            return name, self.value(ct, False)
        if ct.stars == 1:
            if ct.base == 'char':
                if ct.const:
                    return name, 'str'
                if name == 'buf':
                    return name, '&mut List[u8]'
            if ct.base == 'jvalue' and ct.const:
                return name, '[]jvalue'
            if ct.base in BASIC and ct.base != 'char':
                if ct.const:
                    return name, '[]' + BASIC[ct.base]
                if name == 'buf':
                    return name, f'&mut List[{BASIC[ct.base]}]'
            if ct.base == 'void' and not ct.const:
                return name, '?' + VOID
        if ct.stars == 2 and ct.base in ('JNIEnv', 'JavaVM'):
            return name, f'&mut ?{ct.base}'
        raise Skip(f'параметр {name}: {"const " if ct.const else ""}{ct.base}{"*" * ct.stars}')

    def signature(self, fn, ret, params, names):
        r = CType(ret)
        if r.stars:
            raise Skip('результат — указатель на данные JVM; копию дают функции …Region')
        rt = self.value(r, True)
        ps = [p.strip() for p in params.split(',') if p.strip()]
        if '...' in ps:
            raise Skip('переменное число аргументов (есть вариант …A)')
        if names is None or len(names) + (0 if fn in EXPORTED else 1) != len(ps):
            raise Skip('нет имён параметров')
        out = []
        for i, p in enumerate(ps):
            first = i == 0 and fn not in EXPORTED
            nm = ('env' if 'JNIEnv' in p else 'vm') if first else snake(names[i - (0 if fn in EXPORTED else 1)])
            out.append(self.param(fn, CType(p), nm, first))
        return ', '.join(f'{n}: {t}' for n, t in out) + ')' + (' ' + rt if rt else '')

    # --- вывод

    def tables(self):
        """[(таблица, тип объекта, [(номер, имя, результат, параметры)])]."""
        out = []
        for table, obj in TABLES:
            for it in self.header.items:
                if it[0] != 'decl':
                    continue
                m = re.fullmatch(r'struct ' + table + r' \{(.*)\}', it[1])
                if not m:
                    continue
                entries = []
                for slot, member in enumerate(x.strip() for x in m.group(1).split(';') if x.strip()):
                    fm = re.fullmatch(r'(.*?)\(\*(\w+)\)\s*\((.*)\)', member)
                    if fm:
                        entries.append((slot, fm.group(2), fm.group(1).strip(), fm.group(3)))
                    elif not re.fullmatch(r'void\s*\*\s*reserved\d+', member):
                        raise SystemExit(f'jni.py: запись таблицы {table}: {member}')
                out.append((table, obj, entries))
        return out

    def run(self):
        lines = ['// SPDX-License-Identifier: Apache-2.0',
                 '// Привязка JNI: функции JNIEnv и JavaVM — записи их таблиц (@slot, Р63).',
                 '// Сгенерировано bindings/gen/jni.py из jni.h (AOSP libnativehelper) — не править (Р58).',
                 '// jni.h: Copyright (C) 2006 The Android Open Source Project, Apache-2.0.',
                 '// Правила перевода типов C — docs/bindings/README.md.',
                 '',
                 '// Ссылка Java: jobject и его имена в C (jclass, jstring, jthrowable, массивы).',
                 'pub extern type jobject',
                 'pub extern type jfieldID',
                 'pub extern type jmethodID',
                 '// JNIEnv* и JavaVM* в C: объект, первое слово которого — адрес таблицы функций.',
                 'pub extern type JNIEnv',
                 'pub extern type JavaVM',
                 '// void* без длины: указатель C без операций.',
                 f'pub extern type {VOID}',
                 '']
        lines.append('// Константы')
        for it in self.header.items:
            if it[0] == 'define' and it[1].startswith('JNI_'):
                v = it[2].strip('()')
                if re.fullmatch(r'-?(0x[0-9a-fA-F]+|\d+)', v):
                    ty = 'u8' if it[1] in ('JNI_TRUE', 'JNI_FALSE') else 'i32'
                    lines.append(f'pub const {it[1]}: {ty} = {int(v, 0)}')
        for it in self.header.items:
            m = it[0] == 'decl' and re.fullmatch(r'typedef enum (\w+) \{(.*)\} \w+', it[1])
            if m:
                lines.append(f'// {m.group(1)}')
                for part in [x.strip() for x in m.group(2).split(',') if x.strip()]:
                    n, v = [x.strip() for x in part.split('=')]
                    lines.append(f'pub const {n}: i32 = {int(v, 0)}')
        lines += ['',
                  '@union("jni.h")',
                  'pub struct jvalue {',
                  '    z: u8',
                  '    b: i8',
                  '    c: u16',
                  '    s: i16',
                  '    i: i32',
                  '    j: i64',
                  '    f: f32',
                  '    d: f64',
                  '    l: ?jobject',
                  '}',
                  '']
        self.structs = {}
        for name, fields in self.struct_defs():
            self.structs[name] = fields
            lines.append('@c("jni.h")')  # Р76: структура C из заголовка
            lines.append(f'pub struct {name} {{')
            for dn, ty, count, _ in fields:
                lines.append(f'    {count}{dn}: {ty}')
            lines.append('}')
            lines.append('')
        self.slots = []
        for table, obj, entries in self.tables():
            lines.append(f'// {table}: {obj}')
            for slot, fn, ret, params in entries:
                try:
                    sig = self.signature(fn, ret, params, self.names_of(fn))
                except Skip as s:
                    self.skipped.append((fn, str(s)))
                    continue
                self.slots.append((table, fn, slot))
                lines.append(f'@slot({slot})')
                lines.append(f'pub extern fn {fn}({sig}')
            lines.append('')
        lines.append(f'// Функции JVM (на хосте — lib{JVM_LIB}).')
        for it in self.header.items:
            m = it[0] == 'decl' and re.fullmatch(r'(jint) (JNI_\w+)\((.*)\)', it[1])
            if not m:
                continue
            fn = m.group(2)
            if fn not in EXPORTED:
                why = ('библиотека пишет её сама (@export)' if fn.startswith('JNI_On')
                       else 'начальные параметры JNI 1.1')
                self.skipped.append((fn, why))
                continue
            try:
                sig = self.signature(fn, m.group(1), m.group(3), EXPORTED[fn])
            except Skip as s:
                self.skipped.append((fn, str(s)))
                continue
            lines.append(f'@link("{JVM_LIB}")')
            lines.append(f'pub extern fn {fn}({sig}')
        lines.append('')
        if self.skipped:
            lines.append('// Пропущено — не выражается в dpl v0.2:')
            for n, why in self.skipped:
                lines.append(f'// - {n}: {why}')
        return '\n'.join(lines).rstrip('\n') + '\n'

    def struct_defs(self):
        """Структуры вызова JVM: [(имя, [(поле dpl, тип, @count, поле C)])]."""
        out = []
        for it in self.header.items:
            if it[0] != 'decl':
                continue
            m = re.fullmatch(r'(?:typedef )?struct (JavaVM\w+) \{(.*)\}(?: \w+)?', it[1])
            if not m:
                continue
            members = [x.strip() for x in m.group(2).split(';') if x.strip()]
            fields = []
            cnames = [re.findall(r'(\w+)$', x)[0] for x in members]
            for member, cn in zip(members, cnames):
                ct = CType(member[:-len(cn)])
                count = ''
                if ct.stars == 0 and ct.base in BASIC:
                    ty = BASIC[ct.base]
                elif ct.stars == 0 and ct.base in REFS:
                    ty = '?jobject'
                elif ct.stars == 1 and ct.base == 'char':
                    ty = 'str'
                elif ct.stars == 1 and ct.base == 'void':
                    ty = '?' + VOID
                elif ct.stars == 1 and ct.base == 'JavaVMOption':
                    ty = '[]JavaVMOption'
                    count = '@count(n_options) '
                else:
                    raise SystemExit(f'jni.py: поле {m.group(1)}.{cn}')
                fields.append((snake(cn), ty, count, cn))
            out.append((m.group(1), fields))
        return out


def generate():
    return Gen().run()


if __name__ == '__main__':
    sys.exit(gen_main(sys.argv[1:], generate, OUT, __doc__, 'jni.py'))
