#!/usr/bin/env python3
"""Одноразовый переводчик вариантов синтаксиса dpl в C++ (v0.0, шаг 0.4).

Не компилятор (выведенное правило DECISIONS.md): разбирает подмножество
каждого варианта (A, B, C — docs/research/syntax/) в общее дерево,
печатает его в канонической записи любого варианта и выдаёт C++, чтобы
прогонять драйверы эталонного набора. Типы не проверяет: что пропустил
переводчик, ловят компилятор C++, санитайзеры и драйверы.

    cm.py check  -v A файл...            — разобрать
    cm.py fmt    -v A --to B файл...     — напечатать в записи варианта B
    cm.py cpp    -v A -o out.hpp файл... — перевести модули в C++
"""
import argparse
import os
import sys

# ---------------------------------------------------------------- варианты

VARIANTS = {
    # semis — «;» после простых инструкций; arrow — «->» перед типом
    # результата; colon — «имя: тип»; refs — «&T» и «&mut T» или режим
    # «mut»; err_type — тип ошибки в «E!T»; short_enum — «.X» вместо «E.X»;
    # walrus — «x := e» вместо let/var; attr — «#[det]» или «@det».
    'A': dict(semis=True, arrow=True, colon=True, refs=True, err_type=True, short_enum=False, walrus=False, attr='#'),
    'B': dict(semis=False, arrow=False, colon=True, refs=True, err_type=False, short_enum=True, walrus=False, attr='@'),
    'C': dict(semis=False, arrow=False, colon=False, refs=False, err_type=False, short_enum=True, walrus=True, attr='@'),
}


class CmError(Exception):
    def __init__(self, msg, line=0, col=0):
        super().__init__(f'{line}:{col}: {msg}' if line else msg)
        self.line = line


# ---------------------------------------------------------------- лексер

KEYWORDS = {'fn', 'struct', 'enum', 'const', 'import', 'pub', 'let', 'var', 'if', 'else', 'while', 'for',
            'in', 'match', 'return', 'break', 'continue', 'defer', 'fail', 'try', 'parallel', 'none',
            'true', 'false', 'mut'}
OPS = sorted(['..', '=>', '->', ':=', '==', '!=', '<=', '>=', '&&', '||', '??', '+=', '-=', '*=', '/=', '%=',
              '&=', '|=', '^=', '<<=', '>>=', '<<', '>>', '+%', '-%', '*%', '+', '-', '*', '/', '%', '&', '|',
              '^', '~', '!', '<', '>', '=', '(', ')', '[', ']', '{', '}', ',', ';', ':', '.', '?', '@', '#'],
             key=len, reverse=True)
ENDS_STMT_KW = {'return', 'break', 'continue', 'none', 'true', 'false'}


class Tok:
    __slots__ = ('kind', 'val', 'line', 'col')

    def __init__(self, kind, val, line, col):
        self.kind, self.val, self.line, self.col = kind, val, line, col

    def __repr__(self):
        return f'{self.kind}:{self.val!r}@{self.line}'


def lex(src, nl_mode):
    toks = []
    i, line, line_start, depth = 0, 1, 0, 0
    last = None
    nl_done = True
    n = len(src)
    while i < n:
        c = src[i]
        col = i - line_start + 1
        if c == '\n':
            if nl_mode and depth == 0 and not nl_done and last is not None and ends_stmt(last):
                toks.append(Tok('nl', '\n', line, col))
                nl_done = True
            i += 1
            line += 1
            line_start = i
            continue
        if c in ' \t\r':
            i += 1
            continue
        if src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j < 0 else j
            if src[line_start:i].strip() == '':
                toks.append(Tok('comment', src[i + 2:j].strip(), line, col))
            i = j
            continue
        if c.isdigit():
            j = i
            if src.startswith(('0x', '0X'), i):
                j = i + 2
                while j < n and (src[j] in '0123456789abcdefABCDEF_'):
                    j += 1
                kind = 'int'
            else:
                while j < n and (src[j].isdigit() or src[j] == '_'):
                    j += 1
                kind = 'int'
                if j + 1 < n and src[j] == '.' and src[j + 1].isdigit():
                    kind = 'float'
                    j += 1
                    while j < n and (src[j].isdigit() or src[j] == '_'):
                        j += 1
                if j < n and src[j] in 'eE' and (j + 1 < n and (src[j + 1].isdigit() or src[j + 1] in '+-')):
                    kind = 'float'
                    j += 2
                    while j < n and src[j].isdigit():
                        j += 1
            tok = Tok(kind, src[i:j], line, col)
            i = j
        elif c.isalpha() or c == '_':
            j = i
            while j < n and (src[j].isalnum() or src[j] == '_'):
                j += 1
            word = src[i:j]
            tok = Tok('kw' if word in KEYWORDS else 'id', word, line, col)
            i = j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and src[j] != c:
                if src[j] == '\\':
                    j += 1
                if j < n and src[j] == '\n':
                    raise CmError('строка не закрыта', line, col)
                j += 1
            if j >= n:
                raise CmError('строка не закрыта', line, col)
            tok = Tok('str' if c == '"' else 'byte', src[i + 1:j], line, col)
            i = j + 1
        else:
            for op in OPS:
                if src.startswith(op, i):
                    break
            else:
                raise CmError(f'непонятный символ {c!r}', line, col)
            tok = Tok('op', op, line, col)
            i += len(op)
            if op in '([':
                depth += 1
            elif op in ')]':
                depth = max(0, depth - 1)
        toks.append(tok)
        last = tok
        nl_done = False
    if nl_mode and not nl_done and last is not None and ends_stmt(last):
        toks.append(Tok('nl', '\n', line, 1))
    toks.append(Tok('eof', '', line, 1))
    return toks


def ends_stmt(t):
    if t.kind in ('id', 'int', 'float', 'str', 'byte'):
        return True
    if t.kind == 'kw':
        return t.val in ENDS_STMT_KW
    return t.kind == 'op' and t.val in (')', ']', '}')


# ---------------------------------------------------------------- дерево


class Node:
    _fields = ()

    def __init__(self, *args, **kw):
        for f, a in zip(self._fields, args):
            setattr(self, f, a)
        for f in self._fields[len(args):]:
            setattr(self, f, kw.get(f))

    def __repr__(self):
        return f'{type(self).__name__}(' + ', '.join(f'{f}={getattr(self, f)!r}' for f in self._fields) + ')'


def node(name, fields):
    return type(name, (Node,), {'_fields': tuple(fields.split())})


Module = node('Module', 'name items')
Comment = node('Comment', 'text')
Import = node('Import', 'name')
Fn = node('Fn', 'name params ret body pub attrs')
Param = node('Param', 'name type mode')  # mode: val | ref | mut
Struct = node('Struct', 'name fields pub')
Field = node('Field', 'name type default')
Enum = node('Enum', 'name variants pub')
Variant = node('Variant', 'name types fields')  # types — кортеж, fields — запись, оба None — без данных
Const = node('Const', 'name type value pub')

TName = node('TName', 'name mod')
TList = node('TList', 'elem')
TSlice = node('TSlice', 'elem')
TOpt = node('TOpt', 'inner')
TResult = node('TResult', 'err val')

Block = node('Block', 'stmts')
Let = node('Let', 'name type value mutable')
Assign = node('Assign', 'target op value')
ExprStmt = node('ExprStmt', 'expr')
If = node('If', 'cond then els')
IfLet = node('IfLet', 'name expr')
While = node('While', 'cond body')
For = node('For', 'var iter body mode parallel')
Range = node('Range', 'lo hi')
Match = node('Match', 'expr arms')
Arm = node('Arm', 'pats body')
Return = node('Return', 'expr')
Break = node('Break', '')
Continue = node('Continue', '')
Fail = node('Fail', 'expr')
Defer = node('Defer', 'stmt')

Int = node('Int', 'text')
Float = node('Float', 'text')
Str = node('Str', 'text')
Byte = node('Byte', 'text')
Bool = node('Bool', 'val')
NoneLit = node('NoneLit', '')
Name = node('Name', 'id')
FieldE = node('FieldE', 'obj name')
Index = node('Index', 'obj idx')
Slice = node('Slice', 'obj lo hi')
Call = node('Call', 'fn args')
Unary = node('Unary', 'op expr')
Binary = node('Binary', 'op left right')
Try = node('Try', 'expr')
RefArg = node('RefArg', 'expr mut')
StructLit = node('StructLit', 'path fields')  # path — Name, FieldE-цепочка или EnumLit
EnumLit = node('EnumLit', 'name')  # краткая запись «.X»
ListLit = node('ListLit', 'items')

PWild = node('PWild', '')
PLit = node('PLit', 'expr')
PNone = node('PNone', '')
PWrap = node('PWrap', 'kind pat')  # some | ok | err
PVariant = node('PVariant', 'path name args fields')  # path: None (краткая) или список имён
PBind = node('PBind', 'name')

PREC = {'||': 1, '&&': 2, '==': 3, '!=': 3, '<': 3, '<=': 3, '>': 3, '>=': 3, '??': 4,
        '+': 5, '-': 5, '|': 5, '^': 5, '+%': 5, '-%': 5,
        '*': 6, '/': 6, '%': 6, '<<': 6, '>>': 6, '&': 6, '*%': 6}
CMP = {'==', '!=', '<', '<=', '>', '>='}
ASSIGN_OPS = {'=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>='}

# ---------------------------------------------------------------- разбор


class Parser:
    def __init__(self, src, variant):
        self.v = VARIANTS[variant]
        self.vname = variant
        self.t = lex(src, nl_mode=not self.v['semis'])
        self.i = 0

    # --- токены
    def peek(self, k=0, skip_nl=False):
        j = self.i
        while True:
            while self.t[j].kind == 'comment' or (skip_nl and self.t[j].kind == 'nl'):
                j += 1
            if k == 0:
                return self.t[j]
            k -= 1
            j += 1

    def next(self):
        while self.t[self.i].kind == 'comment':
            self.i += 1
        tok = self.t[self.i]
        if tok.kind != 'eof':
            self.i += 1
        return tok

    def at(self, val, k=0, skip_nl=False):
        tok = self.peek(k, skip_nl)
        return tok.kind in ('op', 'kw') and tok.val == val

    def accept(self, val):
        if self.at(val):
            self.next()
            return True
        return False

    def err(self, msg, tok=None):
        tok = tok or self.peek()
        raise CmError(f'{msg} (встречено {tok.val!r})' if tok.kind != 'eof' else f'{msg} (конец файла)',
                      tok.line, tok.col)

    def expect(self, val):
        if not self.at(val):
            self.err(f'ожидалось {val!r}')
        return self.next()

    def ident(self):
        tok = self.peek()
        if tok.kind != 'id':
            self.err('ожидалось имя')
        return self.next().val

    def skip_nl(self):
        while self.peek().kind == 'nl':
            self.next()

    def comments(self):
        out = []
        while True:
            tok = self.t[self.i]
            if tok.kind == 'comment':
                out.append(Comment(tok.val))
                self.i += 1
            elif tok.kind == 'nl':
                self.i += 1
            else:
                return out

    def at_stmt_end(self):
        tok = self.peek()
        return tok.kind in ('nl', 'eof') or (tok.kind == 'op' and tok.val in (';', '}', ','))

    def end_stmt(self):
        if self.v['semis']:
            self.expect(';')
            return
        if self.accept(';'):
            return
        tok = self.peek()
        if tok.kind == 'nl':
            self.next()
        elif not (tok.kind == 'eof' or self.at('}')):
            self.err('ожидался конец строки')

    def sep(self, closer):
        """Разделитель в списке полей и вариантов: «,» в A, «,» или строка в B и C."""
        if self.accept(','):
            self.skip_nl()
            return
        if not self.v['semis'] and self.peek().kind == 'nl':
            self.skip_nl()
            return
        if not self.at(closer):
            self.err(f'ожидалось «,» или {closer!r}')

    # --- модуль
    def module(self, name):
        items = []
        while True:
            items += self.comments()
            if self.peek().kind == 'eof':
                return Module(name, items)
            items.append(self.item())

    def item(self):
        attrs = []
        while True:
            if self.v['attr'] == '#' and self.at('#'):
                self.next()
                self.expect('[')
                attrs.append(self.ident())
                self.expect(']')
            elif self.v['attr'] == '@' and self.at('@'):
                self.next()
                attrs.append(self.ident())
            else:
                break
            self.skip_nl()
        pub = self.accept('pub')
        if self.at('fn'):
            return self.fn(pub, attrs)
        if attrs:
            self.err('атрибут — только у функции')
        if self.at('struct'):
            return self.struct(pub)
        if self.at('enum'):
            return self.enum(pub)
        if self.at('const'):
            return self.const(pub)
        if self.at('import') and not pub:
            self.next()
            name = self.ident()
            self.end_stmt()
            return Import(name)
        self.err('ожидалось объявление: fn, struct, enum, const, import')

    def fn(self, pub, attrs):
        self.expect('fn')
        name = self.ident()
        self.expect('(')
        params = []
        while not self.at(')'):
            params.append(self.param())
            if not self.accept(','):
                break
        self.expect(')')
        ret = None
        if self.v['arrow']:
            if self.accept('->'):
                ret = self.type()
        elif not self.at('{'):
            ret = self.type()
        return Fn(name, params, ret, self.block(), pub, attrs)

    def param(self):
        name = self.ident()
        if self.v['colon']:
            self.expect(':')
        mode = 'val'
        if self.v['refs']:
            if self.accept('&'):
                mode = 'mut' if self.accept('mut') else 'ref'
        elif self.accept('mut'):
            mode = 'mut'
        return Param(name, self.type(), mode)

    def type(self):
        if self.accept('?'):
            return TOpt(self.type())
        if not self.v['err_type'] and self.accept('!'):
            return TResult(None, self.type())
        if self.accept('['):
            self.expect(']')
            return TSlice(self.type())
        name = self.ident()
        if name == 'List' and self.at('['):
            self.next()
            t = TList(self.type())
            self.expect(']')
        elif self.at('.') and self.peek(1).kind == 'id':
            self.next()
            t = TName(self.ident(), name)
        else:
            t = TName(name)
        if self.v['err_type'] and self.at('!'):
            self.next()
            return TResult(t, self.type())
        return t

    def field_decl(self):
        name = self.ident()
        if self.v['colon']:
            self.expect(':')
        t = self.type()
        default = self.expr() if self.accept('=') else None
        return Field(name, t, default)

    def struct(self, pub):
        self.expect('struct')
        name = self.ident()
        self.expect('{')
        fields = []
        while True:
            fields += self.comments()
            if self.at('}'):
                break
            fields.append(self.field_decl())
            self.sep('}')
        self.expect('}')
        return Struct(name, fields, pub)

    def enum(self, pub):
        self.expect('enum')
        name = self.ident()
        self.expect('{')
        variants = []
        while True:
            variants += self.comments()
            if self.at('}'):
                break
            vname = self.ident()
            if self.accept('('):
                types = []
                while not self.at(')'):
                    types.append(self.type())
                    if not self.accept(','):
                        break
                self.expect(')')
                variants.append(Variant(vname, types, None))
            elif self.at('{'):
                self.next()
                self.skip_nl()
                fields = []
                while not self.at('}'):
                    fields.append(self.field_decl())
                    self.sep('}')
                self.expect('}')
                variants.append(Variant(vname, None, fields))
            else:
                variants.append(Variant(vname, None, None))
            self.sep('}')
        self.expect('}')
        return Enum(name, variants, pub)

    def const(self, pub):
        self.expect('const')
        name = self.ident()
        if self.v['colon']:
            self.expect(':')
        t = self.type()
        self.expect('=')
        value = self.expr()
        self.end_stmt()
        return Const(name, t, value, pub)

    # --- инструкции
    def block(self):
        self.expect('{')
        stmts = []
        while True:
            stmts += self.comments()
            if self.at('}'):
                break
            stmts.append(self.stmt())
        self.expect('}')
        return Block(stmts)

    def stmt(self):
        tok = self.peek()
        if tok.kind == 'kw':
            if tok.val == 'if':
                return self.if_stmt()
            if tok.val == 'while':
                self.next()
                cond = self.expr(nostruct=True)
                return While(cond, self.block())
            if tok.val == 'for':
                return self.for_stmt(False)
            if tok.val == 'parallel':
                self.next()
                return self.for_stmt(True)
            if tok.val == 'match':
                return self.match_stmt()
            if tok.val == 'defer':
                self.next()
                if self.at('{'):
                    return Defer(self.block())
                s = self.simple()
                self.end_stmt()
                return Defer(s)
        if self.at('{'):
            return self.block()
        s = self.simple()
        self.end_stmt()
        return s

    def simple(self):
        """Инструкция без завершителя: объявление, присваивание, выражение, переходы."""
        tok = self.peek()
        if tok.kind == 'kw':
            if tok.val in ('let', 'var') and not self.v['walrus']:
                self.next()
                name = self.ident()
                t = None
                if self.accept(':'):
                    t = self.type()
                self.expect('=')
                return Let(name, t, self.expr(), tok.val == 'var')
            if tok.val == 'var' and self.v['walrus']:
                self.next()
                name = self.ident()
                t = self.type()
                self.expect('=')
                return Let(name, t, self.expr(), True)
            if tok.val == 'return':
                self.next()
                return Return(None if self.at_stmt_end() else self.expr())
            if tok.val == 'break':
                self.next()
                return Break()
            if tok.val == 'continue':
                self.next()
                return Continue()
            if tok.val == 'fail':
                self.next()
                return Fail(self.expr())
        if self.v['walrus'] and tok.kind == 'id' and self.at(':=', 1):
            name = self.next().val
            self.next()
            return Let(name, None, self.expr(), True)
        e = self.expr()
        tok = self.peek()
        if tok.kind == 'op' and tok.val in ASSIGN_OPS:
            self.next()
            return Assign(e, tok.val, self.expr())
        return ExprStmt(e)

    def if_stmt(self):
        self.expect('if')
        if not self.v['walrus'] and self.accept('let'):
            name = self.ident()
            self.expect('=')
            cond = IfLet(name, self.expr(nostruct=True))
        elif self.v['walrus'] and self.peek().kind == 'id' and self.at(':=', 1):
            name = self.next().val
            self.next()
            cond = IfLet(name, self.expr(nostruct=True))
        else:
            cond = self.expr(nostruct=True)
        then = self.block()
        save = self.i
        self.skip_nl()
        if self.accept('else'):
            return If(cond, then, self.if_stmt() if self.at('if') else self.block())
        self.i = save
        return If(cond, then, None)

    def for_stmt(self, parallel):
        self.expect('for')
        mode = 'val'
        if not self.v['refs'] and self.accept('mut'):
            mode = 'mut'
        var = self.ident()
        self.expect('in')
        if self.v['refs'] and self.accept('&'):
            mode = 'mut' if self.accept('mut') else 'ref'
        it = self.expr(nostruct=True)
        if self.accept('..'):
            it = Range(it, self.expr(nostruct=True))
        if parallel and not isinstance(it, Range):
            self.err('parallel for — только по диапазону a..b')
        return For(var, it, self.block(), mode, parallel)

    def match_stmt(self):
        self.expect('match')
        e = self.expr(nostruct=True)
        self.expect('{')
        arms = []
        while True:
            self.comments()
            if self.at('}'):
                break
            pats = [self.pattern()]
            while self.accept('|'):
                pats.append(self.pattern())
            self.expect('=>')
            if self.at('{'):
                body = self.block()
                if self.v['semis']:
                    self.accept(',')
            else:
                body = self.simple()
                if self.v['semis']:
                    if not self.at('}'):
                        self.expect(',')
            if not self.v['semis']:
                self.accept(',') or self.accept(';')
                if not (self.peek().kind == 'nl' or self.at('}')):
                    self.err('ветви match разделяются строками')
            arms.append(Arm(pats, body))
        self.expect('}')
        return Match(e, arms)

    def pattern(self):
        tok = self.peek()
        if tok.kind == 'id' and tok.val == '_':
            self.next()
            return PWild()
        if tok.kind in ('int', 'str', 'byte') or self.at('-'):
            return PLit(self.unary(True))
        if tok.kind == 'kw' and tok.val in ('true', 'false'):
            self.next()
            return PLit(Bool(tok.val == 'true'))
        if self.accept('none'):
            return PNone()
        if tok.kind == 'id' and tok.val in ('some', 'ok', 'err') and self.at('(', 1):
            self.next()
            self.next()
            p = self.pattern()
            self.expect(')')
            return PWrap(tok.val, p)
        if self.at('.'):
            if not self.v['short_enum']:
                self.err('в варианте A вариант перечисления пишется с типом: Тип.Вариант')
            self.next()
            return self.variant_pattern(None, self.ident())
        if tok.kind == 'id' and self.at('.', 1):
            path = [self.next().val]
            while self.accept('.'):
                path.append(self.ident())
            return self.variant_pattern(path[:-1], path[-1])
        if tok.kind == 'id':
            self.next()
            return PBind(tok.val)
        self.err('ожидался образец')

    def variant_pattern(self, path, name):
        if self.accept('('):
            args = []
            while not self.at(')'):
                args.append(self.pattern())
                if not self.accept(','):
                    break
            self.expect(')')
            return PVariant(path, name, args, None)
        if self.at('{'):
            self.next()
            fields = []
            while not self.at('}'):
                fname = self.ident()
                fields.append((fname, self.pattern() if self.accept(':') else PBind(fname)))
                if not self.accept(','):
                    break
            self.expect('}')
            return PVariant(path, name, None, fields)
        return PVariant(path, name, None, None)

    # --- выражения
    def expr(self, nostruct=False):
        return self.binary(0, nostruct)

    def binary(self, minp, ns):
        left = self.unary(ns)
        while True:
            tok = self.peek()
            op = tok.val if tok.kind == 'op' and tok.val in PREC else None
            if op is None or PREC[op] <= minp:
                return left
            self.next()
            right = self.binary(PREC[op], ns)
            if op in CMP and isinstance(right, Binary) and right.op in CMP:
                self.err('сравнения не сцепляются: a < b < c')
            if op in CMP and isinstance(left, Binary) and left.op in CMP:
                self.err('сравнения не сцепляются: a < b < c')
            left = Binary(op, left, right)

    def unary(self, ns):
        if self.accept('-'):
            return Unary('-', self.unary(ns))
        if self.accept('!'):
            return Unary('!', self.unary(ns))
        if self.accept('~'):
            return Unary('~', self.unary(ns))
        if self.accept('try'):
            return Try(self.unary(ns))
        if self.v['refs'] and self.at('&'):
            self.next()
            mut = self.accept('mut')
            return RefArg(self.unary(ns), mut)
        return self.postfix(self.primary(), ns)

    def postfix(self, e, ns):
        while True:
            if self.at('('):
                self.next()
                args = []
                while not self.at(')'):
                    args.append(self.expr())
                    if not self.accept(','):
                        break
                self.expect(')')
                e = Call(e, args)
            elif self.at('['):
                self.next()
                lo = None if self.at('..') else self.expr()
                if self.accept('..'):
                    hi = None if self.at(']') else self.expr()
                    e = Slice(e, lo, hi)
                else:
                    e = Index(e, lo)
                self.expect(']')
            elif self.at('.'):
                self.next()
                e = FieldE(e, self.ident())
            elif not ns and self.at('{') and is_path(e) and self.struct_lit_ahead():
                e = self.struct_lit(e)
            else:
                return e

    def struct_lit_ahead(self):
        a = self.peek(1, skip_nl=True)
        if a.kind == 'op' and a.val == '}':
            return True
        b = self.peek(2, skip_nl=True)
        return a.kind == 'id' and b.kind == 'op' and b.val in (':', ',', '}')

    def struct_lit(self, path):
        self.expect('{')
        self.skip_nl()
        fields = []
        while not self.at('}'):
            fname = self.ident()
            fields.append((fname, self.expr() if self.accept(':') else Name(fname)))
            self.skip_nl()
            if not self.accept(','):
                break
            self.skip_nl()
        self.skip_nl()
        self.expect('}')
        return StructLit(path, fields)

    def primary(self):
        tok = self.next()
        k, v = tok.kind, tok.val
        if k == 'int':
            return Int(v)
        if k == 'float':
            return Float(v)
        if k == 'str':
            return Str(v)
        if k == 'byte':
            return Byte(v)
        if k == 'id':
            return Name(v)
        if k == 'kw' and v in ('true', 'false'):
            return Bool(v == 'true')
        if k == 'kw' and v == 'none':
            return NoneLit()
        if k == 'op' and v == '(':
            e = self.expr()
            self.expect(')')
            return e
        if k == 'op' and v == '[':
            items = []
            while not self.at(']'):
                items.append(self.expr())
                if not self.accept(','):
                    break
            self.expect(']')
            return ListLit(items)
        if k == 'op' and v == '.':
            if not self.v['short_enum']:
                self.err('в варианте A вариант перечисления пишется с типом: Тип.Вариант', tok)
            return EnumLit(self.ident())
        self.err('ожидалось выражение', tok)


def is_path(e):
    if isinstance(e, (Name, EnumLit)):
        return True
    return isinstance(e, FieldE) and is_path(e.obj) and not isinstance(e.obj, EnumLit)


def parse(src, variant, name='main'):
    return Parser(src, variant).module(name)


# ---------------------------------------------------------------- сведения о программе

SCALARS = {'i8': 'int8_t', 'i16': 'int16_t', 'i32': 'int32_t', 'i64': 'int64_t', 'u8': 'uint8_t',
           'u16': 'uint16_t', 'u32': 'uint32_t', 'u64': 'uint64_t', 'int': 'int64_t', 'f32': 'float',
           'f64': 'double', 'bool': 'bool'}
BUILTIN_TYPES = dict(SCALARS, str='std::string', void='void')


class Info:
    """Имена программы: модули, типы, функции, варианты перечислений."""

    def __init__(self, modules):
        self.modules = {m.name for m in modules}
        self.structs, self.enums, self.fns, self.consts = {}, {}, {}, {}
        self.variant_of = {}
        for m in modules:
            for it in m.items:
                if isinstance(it, Struct):
                    self.structs[it.name] = it
                elif isinstance(it, Enum):
                    self.enums[it.name] = it
                    for v in it.variants:
                        if isinstance(v, Variant):
                            self.variant_of.setdefault(v.name, []).append(it.name)
                elif isinstance(it, Fn):
                    self.fns[it.name] = it
                elif isinstance(it, Const):
                    self.consts[it.name] = it

    def enum_path(self, e):
        """FieldE(Name E, X) или FieldE(FieldE(Name mod, E), X) → (E, X), если E — перечисление."""
        if isinstance(e, FieldE):
            o = e.obj
            if isinstance(o, Name) and o.id in self.enums:
                return o.id, e.name
            if isinstance(o, FieldE) and isinstance(o.obj, Name) and o.obj.id in self.modules and o.name in self.enums:
                return o.name, e.name
        return None

    def struct_path(self, e):
        if isinstance(e, Name) and e.id in self.structs:
            return e.id
        if isinstance(e, FieldE) and isinstance(e.obj, Name) and e.obj.id in self.modules and e.name in self.structs:
            return e.name
        return None


# ---------------------------------------------------------------- печать


class Printer:
    def __init__(self, variant, info):
        self.v = VARIANTS[variant]
        self.vname = variant
        self.info = info

    def module(self, m):
        out = []
        prev = None
        for it in m.items:
            if prev is not None and not (isinstance(prev, Comment) or (isinstance(prev, Import) and isinstance(it, Import))):
                out.append('')
            out += self.item(it)
            prev = it
        return '\n'.join(out) + '\n'

    def term(self):
        return ';' if self.v['semis'] else ''

    def item(self, it):
        if isinstance(it, Comment):
            return [f'// {it.text}' if it.text else '//']
        if isinstance(it, Import):
            return [f'import {it.name}{self.term()}']
        pub = 'pub ' if getattr(it, 'pub', False) else ''
        if isinstance(it, Fn):
            lines = [('#[' + a + ']') if self.v['attr'] == '#' else ('@' + a) for a in it.attrs or []]
            ps = ', '.join(self.param(p) for p in it.params)
            ret = ''
            if it.ret is not None:
                ret = (' -> ' if self.v['arrow'] else ' ') + self.type(it.ret)
            head = f'{pub}fn {it.name}({ps}){ret}'
            return lines + self.block(it.body, head, 0)
        if isinstance(it, Struct):
            lines = [f'{pub}struct {it.name} {{']
            for f in it.fields:
                lines.append('    ' + (f'// {f.text}' if isinstance(f, Comment) else self.field(f) + (',' if self.v['semis'] else '')))
            return lines + ['}']
        if isinstance(it, Enum):
            lines = [f'{pub}enum {it.name} {{']
            for vr in it.variants:
                if isinstance(vr, Comment):
                    lines.append(f'    // {vr.text}')
                    continue
                s = vr.name
                if vr.types is not None:
                    s += '(' + ', '.join(self.type(t) for t in vr.types) + ')'
                elif vr.fields is not None:
                    s += ' { ' + ', '.join(self.field(f) for f in vr.fields) + ' }'
                lines.append('    ' + s + (',' if self.v['semis'] else ''))
            return lines + ['}']
        if isinstance(it, Const):
            return [f'{pub}const {it.name}{self.colon()}{self.type(it.type)} = {self.expr(it.value)}{self.term()}']
        raise CmError(f'печать: неизвестное объявление {it!r}')

    def colon(self):
        return ': ' if self.v['colon'] else ' '

    def field(self, f):
        s = f'{f.name}{self.colon()}{self.type(f.type)}'
        if f.default is not None:
            s += ' = ' + self.expr(f.default)
        return s

    def param(self, p):
        t = self.type(p.type)
        if self.v['refs']:
            t = {'val': '', 'ref': '&', 'mut': '&mut '}[p.mode] + t
        elif p.mode == 'mut':
            t = 'mut ' + t
        return f'{p.name}{self.colon()}{t}'

    def type(self, t):
        if isinstance(t, TName):
            return f'{t.mod}.{t.name}' if t.mod else t.name
        if isinstance(t, TList):
            return f'List[{self.type(t.elem)}]'
        if isinstance(t, TSlice):
            return f'[]{self.type(t.elem)}'
        if isinstance(t, TOpt):
            return '?' + self.type(t.inner)
        if isinstance(t, TResult):
            if self.v['err_type']:
                return f'{self.type(t.err) if t.err else "Error"}!{self.type(t.val)}'
            return '!' + self.type(t.val)
        raise CmError(f'печать: неизвестный тип {t!r}')

    # --- инструкции
    def block(self, b, head, ind):
        pad = '    ' * ind
        if not b.stmts:
            return [f'{pad}{head} {{}}' if head else pad + '{}']
        lines = [f'{pad}{head} {{' if head else pad + '{']
        for s in b.stmts:
            lines += self.stmt(s, ind + 1)
        return lines + [pad + '}']

    def stmt(self, s, ind):
        pad = '    ' * ind
        if isinstance(s, Comment):
            return [f'{pad}// {s.text}' if s.text else pad + '//']
        if isinstance(s, Block):
            return self.block(s, '', ind)
        if isinstance(s, If):
            return self.if_stmt(s, ind, pad)
        if isinstance(s, While):
            return self.block(s.body, f'while {self.expr(s.cond)}', ind)
        if isinstance(s, For):
            return self.block(s.body, self.for_head(s), ind)
        if isinstance(s, Match):
            lines = [f'{pad}match {self.expr(s.expr)} {{']
            for a in s.arms:
                pats = ' | '.join(self.pattern(p) for p in a.pats)
                if isinstance(a.body, Block):
                    lines += self.block(a.body, f'{pats} =>', ind + 1)
                else:
                    lines.append(f'{pad}    {pats} => {self.simple(a.body)}' + (',' if self.v['semis'] else ''))
            return lines + [pad + '}']
        if isinstance(s, Defer):
            if isinstance(s.stmt, Block):
                return self.block(s.stmt, 'defer', ind)
            return [f'{pad}defer {self.simple(s.stmt)}{self.term()}']
        return [pad + self.simple(s) + self.term()]

    def if_stmt(self, s, ind, pad):
        lines = self.block(s.then, 'if ' + self.cond(s.cond), ind)
        e = s.els
        while e is not None:
            if isinstance(e, If):
                more = self.block(e.then, 'if ' + self.cond(e.cond), ind)
                lines[-1] += ' else ' + more[0].lstrip()
                lines += more[1:]
                e = e.els
            else:
                more = self.block(e, '', ind)
                lines[-1] += ' else ' + more[0].lstrip()
                lines += more[1:]
                e = None
        return lines

    def cond(self, c):
        if isinstance(c, IfLet):
            if self.v['walrus']:
                return f'{c.name} := {self.expr(c.expr)}'
            return f'let {c.name} = {self.expr(c.expr)}'
        return self.expr(c)

    def for_head(self, s):
        head = 'parallel for ' if s.parallel else 'for '
        it = s.iter
        src = f'{self.expr(it.lo)}..{self.expr(it.hi)}' if isinstance(it, Range) else self.expr(it)
        if self.v['refs']:
            amp = {'val': '', 'ref': '&', 'mut': '&mut '}[s.mode]
            return f'{head}{s.var} in {amp}{src}'
        return f'{head}{"mut " if s.mode == "mut" else ""}{s.var} in {src}'

    def simple(self, s):
        if isinstance(s, Let):
            if self.v['walrus']:
                if s.type is not None:
                    return f'var {s.name} {self.type(s.type)} = {self.expr(s.value, typed=True)}'
                return f'{s.name} := {self.expr(s.value, typed=False)}'
            kw = 'var' if s.mutable else 'let'
            t = f': {self.type(s.type)}' if s.type is not None else ''
            return f'{kw} {s.name}{t} = {self.expr(s.value, typed=s.type is not None)}'
        if isinstance(s, Assign):
            return f'{self.expr(s.target)} {s.op} {self.expr(s.value)}'
        if isinstance(s, ExprStmt):
            return self.expr(s.expr)
        if isinstance(s, Return):
            return 'return' if s.expr is None else 'return ' + self.expr(s.expr)
        if isinstance(s, Break):
            return 'break'
        if isinstance(s, Continue):
            return 'continue'
        if isinstance(s, Fail):
            return 'fail ' + self.expr(s.expr)
        raise CmError(f'печать: неизвестная инструкция {s!r}')

    # --- образцы
    def pattern(self, p):
        if isinstance(p, PWild):
            return '_'
        if isinstance(p, PLit):
            return self.expr(p.expr)
        if isinstance(p, PNone):
            return 'none'
        if isinstance(p, PWrap):
            return f'{p.kind}({self.pattern(p.pat)})'
        if isinstance(p, PBind):
            return p.name
        if isinstance(p, PVariant):
            if self.v['short_enum']:
                s = '.' + p.name
            else:
                path = p.path or [self.resolve_variant(p.name)]
                s = '.'.join(path + [p.name])
            if p.args is not None:
                s += '(' + ', '.join(self.pattern(a) for a in p.args) + ')'
            elif p.fields is not None:
                fs = []
                for fname, fp in p.fields:
                    fs.append(fname if isinstance(fp, PBind) and fp.name == fname else f'{fname}: {self.pattern(fp)}')
                s += ' { ' + ', '.join(fs) + ' }'
            return s
        raise CmError(f'печать: неизвестный образец {p!r}')

    def resolve_variant(self, name):
        owners = self.info.variant_of.get(name, [])
        if len(owners) != 1:
            raise CmError(f'печать в A: вариант .{name} не определяется однозначно ({owners})')
        return owners[0]

    # --- выражения
    def expr(self, e, typed=True):
        """typed=False — тип не следует из окружения: перечисление пишется с типом."""
        return self.ex(e, 0, typed)

    def prec(self, e):
        if isinstance(e, Binary):
            return PREC[e.op]
        if isinstance(e, (Unary, Try, RefArg)):
            return 7
        return 8

    def ex(self, e, minp, typed=True):
        s = self.ex_raw(e, typed)
        return f'({s})' if self.prec(e) < minp else s

    def enum_ref(self, enum, name, typed):
        if self.v['short_enum'] and typed:
            return '.' + name
        return f'{enum}.{name}'

    def ex_raw(self, e, typed):
        if isinstance(e, Int):
            return e.text
        if isinstance(e, Float):
            return e.text
        if isinstance(e, Str):
            return f'"{e.text}"'
        if isinstance(e, Byte):
            return f"'{e.text}'"
        if isinstance(e, Bool):
            return 'true' if e.val else 'false'
        if isinstance(e, NoneLit):
            return 'none'
        if isinstance(e, Name):
            return e.id
        if isinstance(e, EnumLit):
            return self.enum_ref(self.resolve_variant(e.name) if not self.v['short_enum'] or not typed else '', e.name, typed)
        if isinstance(e, FieldE):
            ep = self.info.enum_path(e)
            if ep:
                return self.enum_ref(self.qualified_enum(e.obj), ep[1], typed)
            return f'{self.ex(e.obj, 8)}.{e.name}'
        if isinstance(e, Index):
            return f'{self.ex(e.obj, 8)}[{self.expr(e.idx)}]'
        if isinstance(e, Slice):
            lo = self.expr(e.lo) if e.lo is not None else ''
            hi = self.expr(e.hi) if e.hi is not None else ''
            return f'{self.ex(e.obj, 8)}[{lo}..{hi}]'
        if isinstance(e, Call):
            args = ', '.join(self.expr(a) for a in e.args)
            if isinstance(e.fn, EnumLit) or self.info.enum_path(e.fn):
                return f'{self.ex_raw(e.fn, typed)}({args})'
            return f'{self.ex(e.fn, 8)}({args})'
        if isinstance(e, Unary):
            return e.op + self.ex(e.expr, 7)
        if isinstance(e, Try):
            return 'try ' + self.ex(e.expr, 7)
        if isinstance(e, RefArg):
            if not self.v['refs']:
                return self.ex(e.expr, 7)
            return ('&mut ' if e.mut else '&') + self.ex(e.expr, 7)
        if isinstance(e, Binary):
            p = PREC[e.op]
            right_min = p + 1
            left_min = p + 1 if e.op in CMP else p
            return f'{self.ex(e.left, left_min)} {e.op} {self.ex(e.right, right_min)}'
        if isinstance(e, StructLit):
            fs = ', '.join(n if isinstance(v, Name) and v.id == n else f'{n}: {self.expr(v)}' for n, v in e.fields)
            path = e.path
            if isinstance(path, EnumLit) or self.info.enum_path(path):
                head = self.ex_raw(path, typed)
            else:
                head = self.ex(path, 8)
            return f'{head} {{ {fs} }}' if fs else f'{head} {{}}'
        if isinstance(e, ListLit):
            return '[' + ', '.join(self.expr(x) for x in e.items) + ']'
        raise CmError(f'печать: неизвестное выражение {e!r}')

    def qualified_enum(self, obj):
        if isinstance(obj, Name):
            return obj.id
        return f'{obj.obj.id}.{obj.name}'


def print_module(m, variant, info):
    return Printer(variant, info).module(m)


# ---------------------------------------------------------------- C++

CPP_KEYWORDS = set('''alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t
char32_t class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield
decltype default delete do double dynamic_cast else enum explicit export extern false float for friend goto if
inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public
register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch
template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile
wchar_t while xor xor_eq main std cm'''.split())

BUILTIN_FNS = {'len': 'cm::len', 'repeat': 'cm::repeat', 'min': 'cm::min', 'max': 'cm::max', 'abs': 'cm::abs',
               'parse_int': 'cm::parse_int', 'is_digit': 'cm::is_digit', 'is_alpha': 'cm::is_alpha',
               'is_space': 'cm::is_space', 'is_alnum': 'cm::is_alnum', 'str': 'cm::to_str'}
METHODS = {'push', 'pop', 'insert', 'remove', 'clear', 'sort', 'contains', 'trim', 'find', 'starts_with', 'split'}
ARITH = {'+': 'cm::add', '-': 'cm::sub', '*': 'cm::mul', '/': 'cm::div', '%': 'cm::mod', '+%': 'cm::wadd',
         '-%': 'cm::wsub', '*%': 'cm::wmul', '<<': 'cm::shl', '>>': 'cm::shr'}


def mangle(name):
    return name + '_' if name in CPP_KEYWORDS else name


class Emitter:
    def __init__(self, modules):
        self.mods = modules
        self.info = Info(modules)
        self.refs = False
        self.tmp = 0
        self.fn = None
        self.errnames = set()

    def fresh(self, base):
        self.tmp += 1
        return f'{base}{self.tmp}'

    # --- типы
    def ctype(self, t):
        if t is None:
            return 'void'
        if isinstance(t, TName):
            if t.mod is None and t.name in BUILTIN_TYPES:
                return BUILTIN_TYPES[t.name]
            if t.name not in self.info.structs and t.name not in self.info.enums:
                raise CmError(f'неизвестный тип {t.name}')
            return mangle(t.name)
        if isinstance(t, TList):
            return f'std::vector<{self.ctype(t.elem)}>'
        if isinstance(t, TSlice):
            return f'std::span<const {self.ctype(t.elem)}>'
        if isinstance(t, TOpt):
            return f'std::optional<{self.ctype(t.inner)}>'
        if isinstance(t, TResult):
            return f'cm::Result<{self.ctype(t.val)}>'
        raise CmError(f'неизвестный тип {t!r}')

    def param_ctype(self, p, variant_refs):
        t = self.ctype(p.type)
        if p.mode == 'mut':
            return f'{t}&'
        if p.mode == 'ref' or not variant_refs:
            return f'const {t}&'
        return f'const {t}'

    def type_deps(self, t, out):
        if isinstance(t, TName):
            if t.name in self.info.structs or t.name in self.info.enums:
                out.add(t.name)
        elif isinstance(t, (TList, TSlice)):
            self.type_deps(t.elem, out)
        elif isinstance(t, TOpt):
            self.type_deps(t.inner, out)
        elif isinstance(t, TResult):
            self.type_deps(t.val, out)

    # --- программа
    def program(self, variant_of_module):
        types = []
        for m in self.mods:
            types += [it for it in m.items if isinstance(it, (Struct, Enum))]
        deps = {}
        for t in types:
            d = set()
            if isinstance(t, Struct):
                for f in t.fields:
                    if isinstance(f, Field):
                        self.type_deps(f.type, d)
            else:
                for v in t.variants:
                    if isinstance(v, Variant):
                        for x in (v.types or []):
                            self.type_deps(x, d)
                        for f in (v.fields or []):
                            self.type_deps(f.type, d)
            d.discard(t.name)
            deps[t.name] = d
        order, seen = [], set()

        def visit(name, stack):
            if name in seen:
                return
            if name in stack:
                raise CmError(f'типы зависят друг от друга по кругу: {name}')
            for d in sorted(deps[name]):
                visit(d, stack | {name})
            seen.add(name)
            order.append(name)

        for t in types:
            visit(t.name, set())
        by_name = {t.name: t for t in types}
        out = ['// Сгенерировано bench/syntax/cm.py — не править.', '#include "cm_rt.hpp"', '']
        for name in order:
            t = by_name[name]
            out += self.struct(t) if isinstance(t, Struct) else self.enum(t)
            out.append('')
        for m in self.mods:
            refs = VARIANTS[variant_of_module[m.name]]['refs']
            for it in m.items:
                if isinstance(it, Const):
                    out.append(f'inline const {self.ctype(it.type)} {mangle(it.name)} = {self.ex(it.value, it.type)};')
        for m in self.mods:
            refs = VARIANTS[variant_of_module[m.name]]['refs']
            for it in m.items:
                if isinstance(it, Fn):
                    out.append(self.fn_head(it, refs) + ';')
        out.append('')
        for m in self.mods:
            refs = VARIANTS[variant_of_module[m.name]]['refs']
            for it in m.items:
                if isinstance(it, Fn):
                    out += self.fn_def(it, refs)
                    out.append('')
        return '\n'.join(out) + '\n'

    def struct(self, s):
        out = [f'struct {mangle(s.name)} {{']
        for f in s.fields:
            if isinstance(f, Field):
                init = f' = {self.ex(f.default, f.type)}' if f.default is not None else '{}'
                out.append(f'    {self.ctype(f.type)} {mangle(f.name)}{init};')
        out.append(f'    bool operator==(const {mangle(s.name)}&) const = default;')
        out.append(f'    auto operator<=>(const {mangle(s.name)}&) const = default;')
        out.append('};')
        return out

    def enum(self, e):
        name = mangle(e.name)
        vs = [v for v in e.variants if isinstance(v, Variant)]
        out = [f'struct {name} {{', '    enum class Tag : uint8_t { ' + ', '.join(v.name for v in vs) + ' };',
               '    Tag tag{};']
        for v in vs:
            fields = self.variant_fields(v)
            if fields:
                body = ' '.join(f'{self.ctype(t)} {mangle(n)}{{}};' for n, t in fields)
                out.append(f'    struct {v.name}_t {{ {body} bool operator==(const {v.name}_t&) const = default; '
                           f'auto operator<=>(const {v.name}_t&) const = default; }};')
                out.append(f'    {v.name}_t {v.name}_{{}};')
        for v in vs:
            fields = self.variant_fields(v)
            ps = ', '.join(f'{self.ctype(t)} a{i}' for i, (n, t) in enumerate(fields))
            sets = ''.join(f' r.{v.name}_.{mangle(n)} = a{i};' for i, (n, t) in enumerate(fields))
            out.append(f'    static {name} {v.name}({ps}) {{ {name} r; r.tag = Tag::{v.name};{sets} return r; }}')
        out.append(f'    bool operator==(const {name}&) const = default;')
        out.append(f'    auto operator<=>(const {name}&) const = default;')
        out.append('};')
        return out

    def variant_fields(self, v):
        if v.types is not None:
            return [(f'_{i}', t) for i, t in enumerate(v.types)]
        if v.fields is not None:
            return [(f.name, f.type) for f in v.fields]
        return []

    def fn_head(self, f, refs):
        ps = ', '.join(f'{self.param_ctype(p, refs)} {mangle(p.name)}' for p in f.params)
        return f'{self.ctype(f.ret)} {mangle(f.name)}({ps})'

    def fn_def(self, f, refs):
        self.fn = f
        self.refs = refs
        self.errnames = set()
        out = [self.fn_head(f, refs) + ' {']
        try:
            out += self.stmts(f.body.stmts, 1)
        except CmError as e:
            raise CmError(f'в функции {f.name}: {e}') from None
        if isinstance(f.ret, TResult) and isinstance(f.ret.val, TName) and f.ret.val.name == 'void':
            out.append('    return {};')
        out.append('}')
        return out

    # --- инструкции
    def stmts(self, stmts, ind):
        out = []
        for s in stmts:
            out += self.stmt(s, ind)
        return out

    def stmt(self, s, ind):
        pad = '    ' * ind
        if isinstance(s, Comment):
            return []
        if isinstance(s, Block):
            return [pad + '{'] + self.stmts(s.stmts, ind + 1) + [pad + '}']
        if isinstance(s, Let):
            return [pad + self.let(s) + ';']
        if isinstance(s, Assign):
            if isinstance(s.target, Name) and s.target.id == '_':
                return [f'{pad}(void)({self.ex(s.value)});']
            t = self.ex(s.target)
            v = self.ex(s.value)
            if s.op == '=':
                return [f'{pad}{t} = {v};']
            op = s.op[:-1]
            if op in ARITH:
                return [f'{pad}{t} = {ARITH[op]}({t}, {v});']
            return [f'{pad}{t} = ({t} {op} {v});']
        if isinstance(s, ExprStmt):
            return [f'{pad}{self.ex(s.expr)};']
        if isinstance(s, Return):
            ret = self.fn.ret
            if s.expr is None:
                if isinstance(ret, TResult):
                    return [pad + 'return {};']
                return [pad + 'return;']
            vt = ret.val if isinstance(ret, TResult) else ret
            return [f'{pad}return {self.ctype(ret)}({self.ex(s.expr, vt)});']
        if isinstance(s, Break):
            return [pad + 'break;']
        if isinstance(s, Continue):
            return [pad + 'continue;']
        if isinstance(s, Fail):
            return [f'{pad}return {self.err_value(s.expr)};']
        if isinstance(s, Defer):
            body = self.stmt(s.stmt, 0) if not isinstance(s.stmt, Block) else self.stmts(s.stmt.stmts, 0)
            return [f'{pad}cm::Defer {self.fresh("_defer")}([&] {{ ' + ' '.join(x.strip() for x in body) + ' });']
        if isinstance(s, If):
            return self.if_stmt(s, ind)
        if isinstance(s, While):
            return [f'{pad}while ({self.cond(s.cond)}) {{'] + self.stmts(s.body.stmts, ind + 1) + [pad + '}']
        if isinstance(s, For):
            return self.for_stmt(s, ind)
        if isinstance(s, Match):
            return self.match(s, ind)
        raise CmError(f'перевод: неизвестная инструкция {s!r}')

    def cond(self, e):
        """Условие if/while без лишней пары скобок: clang ругается на «if ((a == b))»."""
        s = self.ex(e)
        if isinstance(e, Binary) and e.op not in ARITH and e.op != '??' and s.startswith('(') and s.endswith(')'):
            return s[1:-1]
        return s

    def let(self, s):
        q = '' if s.mutable else 'const '
        if s.type is not None:
            return f'{q}{self.ctype(s.type)} {mangle(s.name)} = {self.ex(s.value, s.type)}'
        v = s.value
        if is_int_lit(v):
            return f'{q}int64_t {mangle(s.name)} = {self.ex(v)}'
        if isinstance(v, Float):
            return f'{q}double {mangle(s.name)} = {self.ex(v)}'
        return f'{q}auto {mangle(s.name)} = {self.ex(v)}'

    def err_tag(self, e, want=None):
        """Вариант ошибки .X или E.X → имя варианта (None — не вариант). Вариант должен
        существовать, а при известном типе ошибки want — принадлежать ему."""
        if isinstance(e, EnumLit):
            enum, name = None, e.name
            owners = self.info.variant_of.get(name, [])
            if not owners:
                raise CmError(f'нет варианта перечисления .{name}')
            if want is not None and want not in owners:
                raise CmError(f'в {want} нет варианта {name}')
            return name
        ep = self.info.enum_path(e)
        if not ep:
            return None
        enum, name = ep
        if not any(isinstance(v, Variant) and v.name == name for v in self.info.enums[enum].variants):
            raise CmError(f'в {enum} нет варианта {name}')
        if want is not None and enum != want:
            raise CmError(f'ошибка {enum}.{name} — не того типа: функция возвращает ошибку {want}')
        return name

    def err_value(self, e):
        ret = self.fn.ret
        want = ret.err.name if isinstance(ret, TResult) and isinstance(ret.err, TName) else None
        tag = self.err_tag(e, want)
        if tag is not None:
            return f'cm::Err{{"{tag}"}}'
        if isinstance(e, Name) and e.id in self.errnames:
            return mangle(e.id)
        raise CmError('fail: ожидался вариант ошибки .X или имя, связанное err(e)')

    def if_stmt(self, s, ind):
        pad = '    ' * ind
        if isinstance(s.cond, IfLet):
            o = self.fresh('_o')
            head = [f'{pad}if (auto&& {o} = {self.ex(s.cond.expr)}; {o}.has_value()) {{',
                    f'{pad}    const auto& {mangle(s.cond.name)} = *{o};']
        else:
            head = [f'{pad}if ({self.cond(s.cond)}) {{']
        out = head + self.stmts(s.then.stmts, ind + 1)
        if s.els is None:
            return out + [pad + '}']
        if isinstance(s.els, If):
            more = self.if_stmt(s.els, ind)
            return out + [pad + '} else {'] + ['    ' + x for x in more] + [pad + '}']
        return out + [pad + '} else {'] + self.stmts(s.els.stmts, ind + 1) + [pad + '}']

    def for_stmt(self, s, ind):
        pad = '    ' * ind
        var = mangle(s.var)
        if isinstance(s.iter, Range):
            lo, hi = self.ex(s.iter.lo), self.ex(s.iter.hi)
            i, e = self.fresh('_i'), self.fresh('_e')
            t = f'std::common_type_t<decltype({lo}), decltype({hi})>'
            loop = f'for ({t} {i} = {lo}, {e} = {hi}; {i} < {e}; ++{i})'
            body = self.stmts(s.body.stmts, ind + 2 if s.parallel else ind + 1)
            if s.parallel:
                th = self.fresh('_th')
                return ([f'{pad}{{', f'{pad}    std::vector<std::thread> {th};', f'{pad}    {loop} {{',
                         f'{pad}        {th}.emplace_back([&, {i}] {{', f'{pad}            const auto {var} = {i};']
                        + ['    ' + x for x in body] +
                        [f'{pad}        }});', f'{pad}    }}', f'{pad}    for (auto& t : {th}) t.join();', f'{pad}}}'])
            bind = [] if s.var == '_' else [f'{pad}    const auto {var} = {i};']
            return [f'{pad}{loop} {{'] + bind + body + [pad + '}']
        src = self.ex(s.iter)
        if s.mode == 'mut':
            head = f'{pad}for (auto& {var} : {src}) {{'
        else:
            head = f'{pad}for (const auto& {var} : cm::each({src})) {{'
        return [head] + self.stmts(s.body.stmts, ind + 1) + [pad + '}']

    def match(self, s, ind):
        pad = '    ' * ind
        m = self.fresh('_m')
        out = [f'{pad}{{', f'{pad}    auto&& {m} = {self.ex(s.expr)};']
        first = True
        has_default = False
        for a in s.arms:
            conds, binds = [], []
            if len(a.pats) == 1:
                c, b = self.pattern(a.pats[0], m)
                conds, binds = c, b
            else:
                alts = []
                for p in a.pats:
                    c, b = self.pattern(p, m)
                    if b:
                        raise CmError('в образцах через | нельзя связывать имена')
                    alts.append('(' + (' && '.join(c) or 'true') + ')')
                conds = [' || '.join(alts)]
            cond = ' && '.join(conds) or 'true'
            if cond == 'true':
                has_default = True
                out.append(f'{pad}    {{' if first else f'{pad}    }} else {{')
            else:
                out.append(f'{pad}    {"if" if first else "} else if"} ({cond}) {{')
            for name, expr in binds:
                out.append(f'{pad}        const auto& {mangle(name)} = {expr};')
            body = a.body.stmts if isinstance(a.body, Block) else [a.body]
            out += self.stmts(body, ind + 2)
            first = False
            if has_default:
                break
        if not has_default:
            out.append(f'{pad}    }} else {{')
            out.append(f'{pad}        cm::trap("match: нет подходящей ветви");')
        out.append(f'{pad}    }}')
        out.append(f'{pad}}}')
        return out

    def pattern(self, p, subj):
        """Образец → (условия, связывания)."""
        if isinstance(p, PWild):
            return [], []
        if isinstance(p, PBind):
            return [], [(p.name, subj)]
        if isinstance(p, PLit):
            return [f'{subj} == {self.ex(p.expr)}'], []
        if isinstance(p, PNone):
            return [f'!{subj}.has_value()'], []
        if isinstance(p, PWrap):
            if p.kind == 'some':
                c, b = self.pattern(p.pat, f'(*{subj})')
                return [f'{subj}.has_value()'] + c, b
            if p.kind == 'ok':
                c, b = self.pattern(p.pat, f'{subj}.value')
                return [f'{subj}.ok()'] + c, b
            inner = p.pat
            if isinstance(inner, PWild):
                return [f'!{subj}.ok()'], []
            if isinstance(inner, PBind):
                self.errnames.add(inner.name)
                return [f'!{subj}.ok()'], [(inner.name, f'cm::Err{{{subj}.err}}')]
            if isinstance(inner, PVariant) and inner.args is None and inner.fields is None:
                return [f'cm::tag_eq({subj}.err, "{inner.name}")'], []
            raise CmError('err(...): ожидалось имя, _ или вариант ошибки')
        if isinstance(p, PVariant):
            conds = [f'{subj}.tag == std::remove_cvref_t<decltype({subj})>::Tag::{p.name}']
            binds = []
            if p.args is not None:
                for i, a in enumerate(p.args):
                    c, b = self.pattern(a, f'{subj}.{p.name}_._{i}')
                    conds += c
                    binds += b
            elif p.fields is not None:
                for fname, fp in p.fields:
                    c, b = self.pattern(fp, f'{subj}.{p.name}_.{mangle(fname)}')
                    conds += c
                    binds += b
            return conds, binds
        raise CmError(f'перевод: неизвестный образец {p!r}')

    # --- выражения
    def enum_for(self, name, expect):
        t = expect
        while isinstance(t, (TOpt, TResult)):
            t = t.inner if isinstance(t, TOpt) else t.val
        if isinstance(t, TName) and t.name in self.info.enums:
            if any(isinstance(v, Variant) and v.name == name for v in self.info.enums[t.name].variants):
                return t.name
        owners = self.info.variant_of.get(name, [])
        if len(owners) == 1:
            return owners[0]
        if not owners:
            raise CmError(f'нет варианта перечисления .{name}')
        raise CmError(f'вариант .{name} есть в нескольких перечислениях ({", ".join(owners)}); укажите тип')

    def variant_decl(self, enum, name):
        for v in self.info.enums[enum].variants:
            if isinstance(v, Variant) and v.name == name:
                return v
        raise CmError(f'в {enum} нет варианта {name}')

    def ex(self, e, expect=None):
        if isinstance(e, Int):
            t = e.text.replace('_', '')
            v = int(t, 16) if t.lower().startswith('0x') else int(t)
            if v > 0x7FFFFFFFFFFFFFFF:
                return t + 'ULL'
            if v > 0x7FFFFFFF:
                return t + 'LL'
            return t
        if isinstance(e, Float):
            return e.text.replace('_', '')
        if isinstance(e, Str):
            return f'std::string("{e.text}")'
        if isinstance(e, Byte):
            return f"(uint8_t)'{e.text}'"
        if isinstance(e, Bool):
            return 'true' if e.val else 'false'
        if isinstance(e, NoneLit):
            return 'std::nullopt'
        if isinstance(e, Name):
            return mangle(e.id)
        if isinstance(e, EnumLit):
            enum = self.enum_for(e.name, expect)
            return f'{mangle(enum)}::{e.name}()'
        if isinstance(e, FieldE):
            ep = self.info.enum_path(e)
            if ep:
                return f'{mangle(ep[0])}::{ep[1]}()'
            if isinstance(e.obj, Name) and e.obj.id in self.info.modules:
                return mangle(e.name)
            return f'{self.ex(e.obj)}.{mangle(e.name)}'
        if isinstance(e, Index):
            return f'cm::at({self.ex(e.obj)}, {self.ex(e.idx)})'
        if isinstance(e, Slice):
            o = self.ex(e.obj)
            lo = self.ex(e.lo) if e.lo is not None else '0'
            hi = self.ex(e.hi) if e.hi is not None else f'cm::len({o})'
            return f'cm::slice({o}, {lo}, {hi})'
        if isinstance(e, Call):
            return self.call(e, expect)
        if isinstance(e, Unary):
            if e.op == '-':
                if isinstance(e.expr, (Int, Float)):
                    return '-' + self.ex(e.expr)
                return f'cm::neg({self.ex(e.expr)})'
            if e.op == '!':
                return f'(!{self.ex(e.expr)})'
            return f'cm::bnot({self.ex(e.expr)})'
        if isinstance(e, Try):
            t = self.fresh('_t')
            return (f'({{ auto {t} = {self.ex(e.expr)}; if (!{t}.ok()) return cm::Err{{{t}.err}}; '
                    f'cm_take({t}); }})')
        if isinstance(e, RefArg):
            return self.ex(e.expr, expect)
        if isinstance(e, Binary):
            return self.binary(e)
        if isinstance(e, StructLit):
            return self.struct_lit(e, expect)
        if isinstance(e, ListLit):
            elem = expect.elem if isinstance(expect, (TList, TSlice)) else None
            items = ', '.join(self.ex(x, elem) for x in e.items)
            if isinstance(expect, TList):
                return f'{self.ctype(expect)}{{{items}}}'
            if not e.items:
                return '{}'
            return f'std::vector{{{items}}}'
        raise CmError(f'перевод: неизвестное выражение {e!r}')

    def binary(self, e):
        op = e.op
        if op in ('==', '!=') and (self.is_err_name(e.left) or self.is_err_name(e.right)):
            name, lit = (e.left, e.right) if self.is_err_name(e.left) else (e.right, e.left)
            tag = self.err_tag(lit)
            if tag is None:
                raise CmError('ошибку сравнивают с вариантом: e == .X')
            s = f'cm::err_is({mangle(name.id)}, "{tag}")'
            return s if op == '==' else f'(!{s})'
        if op == '??':
            o = self.fresh('_o')
            return f'({{ auto&& {o} = {self.ex(e.left)}; {o}.has_value() ? *{o} : ({self.ex(e.right)}); }})'
        if op in ARITH:
            return f'{ARITH[op]}({self.ex(e.left)}, {self.ex(e.right)})'
        if op in CMP:
            hint = None
            if isinstance(e.right, EnumLit) or isinstance(e.left, EnumLit):
                hint = None
            return f'({self.ex(e.left, hint)} {op} {self.ex(e.right, hint)})'
        return f'({self.ex(e.left)} {op} {self.ex(e.right)})'

    def is_err_name(self, e):
        return isinstance(e, Name) and e.id in self.errnames

    def call(self, e, expect):
        f = e.fn
        args = e.args
        if isinstance(f, Name):
            if f.id in SCALARS and len(args) == 1:
                return f'cm::cast<{SCALARS[f.id]}>({self.ex(args[0])})'
            if f.id == 'assert':
                return f'cm::check({self.ex(args[0])}, "assert")'
            if f.id == 'repeat' and len(args) == 2:
                elem = expect.elem if isinstance(expect, TList) else None
                if elem is not None:
                    return f'cm::repeat<{self.ctype(elem)}>({self.ex(args[0], elem)}, {self.ex(args[1])})'
                if is_int_lit(args[0]):
                    return f'cm::repeat<int64_t>({self.ex(args[0])}, {self.ex(args[1])})'
            if f.id in BUILTIN_FNS and f.id not in self.info.fns:
                return f'{BUILTIN_FNS[f.id]}(' + ', '.join(self.ex(a) for a in args) + ')'
            return self.user_call(f.id, args)
        if isinstance(f, EnumLit):
            enum = self.enum_for(f.name, expect)
            return self.variant_call(enum, f.name, args)
        if isinstance(f, FieldE):
            ep = self.info.enum_path(f)
            if ep:
                return self.variant_call(ep[0], ep[1], args)
            if isinstance(f.obj, Name) and f.obj.id in SCALARS and f.name == 'wrap':
                return f'cm::wrap<{SCALARS[f.obj.id]}>({self.ex(args[0])})'
            if isinstance(f.obj, Name) and f.obj.id in self.info.modules:
                return self.user_call(f.name, args)
            if f.name in METHODS:
                return f'cm::m_{f.name}(' + ', '.join([self.ex(f.obj)] + [self.ex(a) for a in args]) + ')'
            raise CmError(f'нет метода .{f.name} (методы есть только у встроенных типов)')
        raise CmError('вызывать можно функцию по имени')

    def user_call(self, name, args):
        fn = self.info.fns.get(name)
        if fn is None:
            raise CmError(f'нет функции {name}')
        if len(args) != len(fn.params):
            raise CmError(f'{name}: ожидалось аргументов {len(fn.params)}, передано {len(args)}')
        if self.refs:
            for i, (a, p) in enumerate(zip(args, fn.params)):
                want = {'val': None, 'ref': False, 'mut': True}[p.mode]
                got = a.mut if isinstance(a, RefArg) else None
                if want != got:
                    mark = {None: 'без пометки', False: '&x', True: '&mut x'}
                    raise CmError(f'{name}: аргумент {i + 1} ({p.name}) пишется {mark[want]}, а не {mark[got]}')
        return f'{mangle(name)}(' + ', '.join(self.ex(a, p.type) for a, p in zip(args, fn.params)) + ')'

    def variant_call(self, enum, name, args):
        v = self.variant_decl(enum, name)
        fields = self.variant_fields(v)
        if len(args) != len(fields):
            raise CmError(f'{enum}.{name}: ожидалось значений {len(fields)}')
        return f'{mangle(enum)}::{name}(' + ', '.join(self.ex(a, t) for a, (n, t) in zip(args, fields)) + ')'

    def struct_lit(self, e, expect):
        path = e.path
        enum_variant = None
        if isinstance(path, EnumLit):
            enum_variant = (self.enum_for(path.name, expect), path.name)
        elif self.info.enum_path(path):
            enum_variant = self.info.enum_path(path)
        given = dict(e.fields)
        if len(given) != len(e.fields):
            raise CmError('поле указано дважды')
        if enum_variant:
            enum, name = enum_variant
            v = self.variant_decl(enum, name)
            if v.fields is None:
                raise CmError(f'{enum}.{name} — не вариант с полями')
            names = [f.name for f in v.fields]
            for n in given:
                if n not in names:
                    raise CmError(f'{enum}.{name}: нет поля {n}')
            missing = [n for n in names if n not in given]
            if missing:
                raise CmError(f'{enum}.{name}: не заданы поля {", ".join(missing)}')
            args = ', '.join(self.ex(given[f.name], f.type) for f in v.fields)
            return f'{mangle(enum)}::{name}({args})'
        sname = self.info.struct_path(path)
        if sname is None:
            raise CmError('литерал структуры: неизвестный тип')
        s = self.info.structs[sname]
        decl = [f for f in s.fields if isinstance(f, Field)]
        names = [f.name for f in decl]
        for n in given:
            if n not in names:
                raise CmError(f'{sname}: нет поля {n}')
        missing = [f.name for f in decl if f.name not in given and f.default is None]
        if missing:
            raise CmError(f'{sname}: не заданы поля {", ".join(missing)}')
        inits = ', '.join(f'.{mangle(f.name)} = {self.ex(given[f.name], f.type)}' for f in decl if f.name in given)
        return f'{mangle(sname)}{{{inits}}}'


def is_int_lit(e):
    return isinstance(e, Int) or (isinstance(e, Unary) and e.op == '-' and isinstance(e.expr, Int))


TAKE = '''
template <class T> T cm_take(cm::Result<T>& r) { return std::move(r.value); }
inline void cm_take(cm::Result<void>&) {}
'''


def translate(modules, variant_of_module):
    """Модули (в порядке зависимостей не важно) → текст C++."""
    em = Emitter(modules)
    code = em.program(variant_of_module)
    return code.replace('#include "cm_rt.hpp"\n', '#include "cm_rt.hpp"\n' + TAKE, 1)


# ---------------------------------------------------------------- командная строка


def load(paths, variant):
    mods = []
    for p in paths:
        with open(p, encoding='utf-8') as f:
            src = f.read()
        name = os.path.splitext(os.path.basename(p))[0]
        try:
            mods.append(parse(src, variant, name))
        except CmError as e:
            raise CmError(f'{p}:{e}') from None
    return mods


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('cmd', choices=['check', 'fmt', 'cpp'])
    ap.add_argument('-v', '--variant', required=True, choices=sorted(VARIANTS))
    ap.add_argument('--to', choices=sorted(VARIANTS))
    ap.add_argument('-o', '--out')
    ap.add_argument('files', nargs='+')
    a = ap.parse_args(argv)
    try:
        mods = load(a.files, a.variant)
        if a.cmd == 'check':
            print(f'разобрано: {len(mods)}')
        elif a.cmd == 'fmt':
            info = Info(mods)
            for m in mods:
                sys.stdout.write(print_module(m, a.to or a.variant, info))
        else:
            code = translate(mods, {m.name: a.variant for m in mods})
            if a.out:
                with open(a.out, 'w', encoding='utf-8') as f:
                    f.write(code)
            else:
                sys.stdout.write(code)
    except CmError as e:
        print(f'ошибка: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
