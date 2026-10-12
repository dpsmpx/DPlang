"""Разбор заголовков C для генераторов привязок (bindings/gen/): ветви
препроцессора по заданным макросам, объявления с документацией, целые
выражения C, типы деклараторов. Только стандартная библиотека Python."""
import os
import re


# --- выражения C: условия препроцессора и значения констант

def c_tokens(s):
    return re.findall(r'0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*|[A-Za-z_]\w*|<<|>>|&&|\|\||==|!=|<=|>=|[-+*/%&|^~!()<>,]', s)


def c_int(tok):
    t = tok.rstrip('uUlL')
    return int(t, 16) if t[:2] in ('0x', '0X') else int(t)


class Expr:
    """Целое выражение C: числа, имена из names, унарные и бинарные
    операторы, defined(X), SDL_UINT64_C(x). Нет имени — None."""

    def __init__(self, text, names, defined=False, macros=None):
        self.t = c_tokens(text)
        self.i = 0
        self.names = names
        self.defined = defined  # условие препроцессора: неизвестное имя — 0
        self.macros = macros or {}  # для defined(X)

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def take(self):
        tok = self.peek()
        self.i += 1
        return tok

    def value(self):
        v = self.binary(0)
        if self.peek() is not None:
            raise ValueError('лишнее в выражении')
        return v

    LEVELS = [['||'], ['&&'], ['|'], ['^'], ['&'], ['==', '!='], ['<', '>', '<=', '>='], ['<<', '>>'], ['+', '-'],
              ['*', '/', '%']]

    def binary(self, level):
        if level == len(self.LEVELS):
            return self.unary()
        v = self.binary(level + 1)
        while self.peek() in self.LEVELS[level]:
            op = self.take()
            r = self.binary(level + 1)
            v = {'||': lambda: int(bool(v) or bool(r)), '&&': lambda: int(bool(v) and bool(r)), '|': lambda: v | r,
                 '^': lambda: v ^ r, '&': lambda: v & r, '==': lambda: int(v == r), '!=': lambda: int(v != r),
                 '<': lambda: int(v < r), '>': lambda: int(v > r), '<=': lambda: int(v <= r),
                 '>=': lambda: int(v >= r), '<<': lambda: v << r, '>>': lambda: v >> r, '+': lambda: v + r,
                 '-': lambda: v - r, '*': lambda: v * r, '/': lambda: int(v / r), '%': lambda: v % r}[op]()
        return v

    def unary(self):
        tok = self.take()
        if tok is None:
            raise ValueError('пустое выражение')
        if tok == '-':
            return -self.unary()
        if tok == '~':
            return ~self.unary()
        if tok == '!':
            return int(not self.unary())
        if tok == '(':
            v = self.binary(0)
            if self.take() != ')':
                raise ValueError('нет )')
            return v
        if tok[0].isdigit():
            return c_int(tok)
        if tok == 'defined':
            paren = self.peek() == '('
            if paren:
                self.take()
            name = self.take()
            if paren:
                self.take()
            return int(name in self.macros)
        if tok in ('SDL_UINT64_C', 'SDL_SINT64_C'):
            self.take()
            v = self.binary(0)
            self.take()
            return v
        if tok in self.names:
            return self.names[tok]
        if self.defined:
            return 0
        raise ValueError(f'неизвестное имя {tok}')


# --- разбор заголовка

class Header:
    """Объявления заголовка по порядку: ('define', имя, текст),
    ('decl', текст без комментариев, документация)."""

    def __init__(self, name, directory, macros):
        self.name = name
        with open(os.path.join(directory, name), encoding='utf-8') as f:
            text = f.read()
        lines, defines = self.preprocess(text, macros)
        self.items = self.scan(lines, defines)

    @staticmethod
    def preprocess(text, macros):
        """Ветви препроцессора по macros; #define — меткой @@define N."""
        raw = text.split('\n')
        out, defines, stack = [], [], []  # stack: [активна, была ли истинной]
        i = 0
        while i < len(raw):
            ln = raw[i]
            while ln.endswith('\\') and i + 1 < len(raw):
                i += 1
                ln = ln[:-1] + ' ' + raw[i]
            i += 1
            s = ln.strip()
            active = all(a for a, _ in stack)
            if not s.startswith('#'):
                out.append(ln if active else '')
                continue
            d = re.match(r'#\s*(\w+)\s*(.*)', s)
            kw, rest = d.group(1), d.group(2)
            rest = re.sub(r'/\*.*?\*/|//.*', '', rest).strip()
            if kw in ('if', 'ifdef', 'ifndef'):
                if kw == 'ifdef':
                    c = rest in macros
                elif kw == 'ifndef':
                    c = rest not in macros
                else:
                    c = bool(Expr(rest, macros, defined=True, macros=macros).value())
                stack.append([c, c])
            elif kw == 'elif':
                c = not stack[-1][1] and bool(Expr(rest, macros, defined=True, macros=macros).value())
                stack[-1] = [c, stack[-1][1] or c]
            elif kw == 'else':
                stack[-1] = [not stack[-1][1], True]
            elif kw == 'endif':
                stack.pop()
            elif kw == 'define' and active:
                m = re.match(r'(\w+)(\(?)\s*(.*)', rest)
                if m and not m.group(2):
                    defines.append((m.group(1), m.group(3).strip()))
                    out.append(f'@@define {len(defines) - 1}')
                    continue
            out.append('')
        return out, defines

    @staticmethod
    def scan(lines, defines):
        text = '\n'.join(lines)
        items, doc, i, n = [], '', 0, len(text)
        while i < n:
            c = text[i]
            if c.isspace():
                i += 1
            elif text.startswith('/**', i) and not text.startswith('/**<', i):
                j = text.index('*/', i)
                doc = text[i + 3:j]
                i = j + 2
            elif text.startswith('/*', i):
                i = text.index('*/', i) + 2
            elif text.startswith('//', i):
                j = text.find('\n', i)
                i = n if j < 0 else j
            elif text.startswith('@@define ', i):
                j = text.find('\n', i)
                j = n if j < 0 else j
                name, value = defines[int(text[i + 9:j])]
                items.append(('define', name, value))
                i = j
            else:  # объявление до ; вне скобок; тело встроенной функции — до }
                depth, j, body = 0, i, False
                while j < n:
                    if text.startswith('/*', j):
                        j = text.index('*/', j) + 2
                        continue
                    if text.startswith('//', j):
                        j = text.find('\n', j)
                        continue
                    ch = text[j]
                    if ch in '({':
                        if ch == '{' and depth == 0 and ')' in text[i:j] and not text[i:j].lstrip().startswith('typedef'):
                            body = True
                        depth += 1
                    elif ch in ')}':
                        depth -= 1
                        if body and depth == 0 and ch == '}':
                            j += 1
                            break
                    elif ch == ';' and depth == 0:
                        j += 1
                        break
                    j += 1
                decl = text[i:j]
                if not body:
                    clean = re.sub(r'/\*.*?\*/|//[^\n]*|@@define \d+', ' ', decl, flags=re.S)
                    items.append(('decl', ' '.join(clean.split()).rstrip(';').strip(), doc))
                doc = ''
                i = j
        return items


def param_docs(doc):
    """\\param имя текст → {имя: текст}; '\\returns' → ключ ''."""
    out = {}
    lines = [re.sub(r'^\s*\*\s?', '', ln) for ln in doc.split('\n')]
    cur = None
    for ln in lines:
        m = re.match(r'\\param\s+(\w+)\s*(.*)', ln)
        r = re.match(r'\\returns?\s*(.*)', ln)
        if m:
            cur = m.group(1)
            out[cur] = m.group(2)
        elif r:
            cur = ''
            out[cur] = r.group(1)
        elif ln.startswith('\\') or not ln.strip():
            cur = None
        elif cur is not None:
            out[cur] += ' ' + ln.strip()
    return out


class CType:
    """Тип C: имя основы, число *, const у указуемого, const указателя."""

    def __init__(self, text):
        t = ' '.join(text.replace('*', ' * ').split())
        words = t.split(' ')
        self.stars = words.count('*')
        first = words.index('*') if '*' in words else len(words)
        base = [w for w in words[:first] if w not in ('const', 'struct', 'enum', 'union')]
        self.const = 'const' in words[:first + 1] if self.stars else False
        self.base = ' '.join(base)
