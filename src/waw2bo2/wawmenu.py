"""WaW's own in-game (pause) menus, translated for T6 LUI.

T4 loads two engine-named menulists in a running level: ``ui/ingame.txt``
(stock ``common.ff``) and ``ui/hud.txt`` (the map's zone; DLC maps re-ship it
in ``<map>_patch.ff``, so it wins). The compiled menus in those zones are built
from the Mod Tools ``raw/ui`` sources; the converter preprocesses those sources
as WaW's menu compiler does (``PC`` defined), parses every ``menuDef`` and keeps
the closure reachable from ``pausedmenu``. ``compat/waw_menu.lua`` interprets
the result: item rects in 640x480 with T4 alignment, shader/filled styles, text
in the original font glyphs, ``visible when()``/``exp`` and the item scripts
(``open``/``close``/``setdvar``/``exec``/``scriptMenuResponse``). Nothing here
chooses layout or wording; it all comes from the menu source.
"""
from __future__ import annotations

import csv
import io
import json
import re
from dataclasses import dataclass
from pathlib import Path

ROOT_MENU = 'pausedmenu'
# T4 engine menulists of a running level, oldest first (later wins by name).
MENULISTS = ('ui/ingame.txt', 'ui/hud.txt')
TABLE = 'zm/waw_menus.csv'
GLYPHS = 'zm/waw_menu_glyphs.csv'
MANIFEST = 'content_source/waw_menu.json'


class MenuSourceError(ValueError):
    pass


# ---------------------------------------------------------------- preprocessor

@dataclass
class Macro:
    params: list[str] | None
    body: str


_IDENT = re.compile(r'[A-Za-z_]\w*')


def _strip_comments(text: str) -> str:
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != '\n':
                j += 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            if j < 0:
                break
            # A // comment on a continued #define line keeps the continuation.
            if text[i:j].rstrip().endswith('\\'):
                out.append('\\')
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            if j < 0:
                break
            out.append('\n' * text.count('\n', i, j) or ' ')
            i = j + 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def _split_args(text: str, start: int) -> tuple[list[str], int]:
    """Arguments of a macro call; text[start] is '('. Returns args and end index."""
    depth, i, args, cur = 0, start, [], []
    while i < len(text):
        c = text[i]
        if c == '"':
            j = text.find('"', i + 1)
            j = len(text) - 1 if j < 0 else j
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c == '(':
            depth += 1
            if depth > 1:
                cur.append(c)
        elif c == ')':
            depth -= 1
            if depth == 0:
                args.append(''.join(cur).strip())
                return args, i + 1
            cur.append(c)
        elif c == ',' and depth == 1:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    raise MenuSourceError('Unterminated macro call')


def _open_parens(line: str) -> int:
    bare = re.sub(r'"[^"]*"', '', line)
    return bare.count('(') - bare.count(')')


class Preprocessor:
    """The C preprocessor subset WaW's menu sources use."""

    def __init__(self, root: Path, defines: dict[str, str] | None = None):
        self.root = root
        self.macros: dict[str, Macro] = {k: Macro(None, v) for k, v in (defines or {}).items()}

    def read(self, name: str) -> str:
        path = self.root / name
        if not path.is_file():
            raise MenuSourceError(f'WaW menu source missing: {path}')
        return path.read_text(encoding='latin-1')

    def run(self, name: str) -> str:
        return self.directives(self.read(name))

    def directives(self, text: str) -> str:
        text = _strip_comments(text.replace('\r\n', '\n').replace('\r', '\n'))
        joined, buf = [], ''
        for line in text.split('\n'):
            if line.rstrip().endswith('\\'):
                buf += line.rstrip()[:-1] + ' '
                continue
            joined.append(buf + line)
            buf = ''
        if buf:
            joined.append(buf)
        out, stack, pending = [], [], ''

        def active():
            return all(s[0] for s in stack)
        for line in joined:
            if pending:
                line, pending = pending + ' ' + line, ''
            s = line.strip()
            if not s.startswith('#'):
                if active():
                    if _open_parens(line) > 0:
                        pending = line  # a macro call continues on the next line
                        continue
                    # Expanded in source order, so later #undef/#define pairs
                    # apply only to what follows them, as in the compiler.
                    out.append(self.expand(line))
                continue
            word, rest = re.match(r'#\s*(\w*)\s*(.*)', s).groups()
            rest = rest.strip()
            if word in ('ifdef', 'ifndef'):
                name = rest.split()[0] if rest else ''
                cond = (name in self.macros) == (word == 'ifdef')
                stack.append([cond and active(), cond])
            elif word == 'if':
                cond = self.condition(rest)
                stack.append([cond and active(), cond])
            elif word == 'else':
                parent = all(x[0] for x in stack[:-1])
                stack[-1][0] = parent and not stack[-1][1]
            elif word == 'endif':
                stack.pop()
            elif not active():
                continue
            elif word == 'include':
                out.append(self.directives(self.read(rest.strip('"<> '))))
            elif word == 'define':
                m = _IDENT.match(rest)
                if not m:
                    continue
                name, tail = m.group(), rest[m.end():]
                if tail.startswith('('):
                    close = tail.index(')')
                    params = [p.strip() for p in tail[1:close].split(',') if p.strip()]
                    self.macros[name] = Macro(params, tail[close + 1:].strip())
                else:
                    self.macros[name] = Macro(None, tail.strip())
            elif word == 'undef':
                self.macros.pop(rest.split()[0] if rest else '', None)
        return '\n'.join(out)

    def condition(self, expr: str) -> bool:
        expr = re.sub(r'defined\s*\(\s*(\w+)\s*\)', lambda m: '1' if m.group(1) in self.macros else '0', expr)
        expr = self.expand(expr).replace('&&', ' and ').replace('||', ' or ')
        expr = re.sub(r'!(?!=)', ' not ', expr)
        expr = re.sub(r'[A-Za-z_]\w*', lambda m: m.group() if m.group() in ('and', 'or', 'not') else '0', expr)
        try:
            return bool(eval(expr, {'__builtins__': {}}, {}))
        except Exception:
            return False

    def expand(self, text: str, hide: frozenset = frozenset()) -> str:
        out, i, n = [], 0, len(text)
        while i < n:
            c = text[i]
            if c == '"':
                j = text.find('"', i + 1)
                j = n - 1 if j < 0 else j
                out.append(text[i:j + 1])
                i = j + 1
                continue
            m = _IDENT.match(text, i) if (c.isalpha() or c == '_') else None
            if not m:
                out.append(c)
                i += 1
                continue
            name = m.group()
            macro = self.macros.get(name)
            if macro is None or name in hide:
                out.append(name)
                i = m.end()
                continue
            if macro.params is None:
                out.append(self.expand(macro.body, hide | {name}))
                i = m.end()
                continue
            j = m.end()
            while j < n and text[j] in ' \t\n':
                j += 1
            if j >= n or text[j] != '(':
                out.append(name)
                i = m.end()
                continue
            args, end = _split_args(text, j)
            args = [self.expand(a, hide) for a in args]
            values = dict(zip(macro.params, args + [''] * (len(macro.params) - len(args))))
            body = re.sub(r'"[^"]*"|[A-Za-z_]\w*',
                          lambda t: t.group() if t.group().startswith('"') else values.get(t.group(), t.group()),
                          macro.body)
            out.append(self.expand(body, hide | {name}))
            i = end
        return ''.join(out)


# ---------------------------------------------------------------- parser

_TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|[{}();,]|\|\||&&|[!=<>]=|[-+*/%!<>()=?:]|[^\s{}();,"!=<>+*/%?:-]+')


def tokenize(text: str) -> list[str]:
    return _TOKEN.findall(text)


SCRIPT_KEYS = ('onopen', 'onclose', 'onesc', 'onfocus', 'leavefocus', 'action', 'mouseenter', 'mouseexit',
               'mouseentertext', 'mouseexittext')
VECTOR_ARITY = {'origin': 2, 'forecolor': 4, 'backcolor': 4, 'bordercolor': 4, 'outlinecolor': 4,
                'focuscolor': 4, 'disablecolor': 4, 'glowcolor': 4}

# T4 script commands and their argument counts (None: up to ';'). The script
# runner consumes each command's arguments, so ';' between them is optional.
COMMAND_ARGS = {'fadein': 1, 'fadeout': 1, 'show': 1, 'hide': 1, 'showmenu': 1, 'hidemenu': 1,
                'setcolor': 5, 'open': 1, 'close': 1, 'closeforallplayers': 1, 'ingameopen': 1,
                'ingameclose': 1, 'setbackground': 2, 'setitemcolor': 6, 'focusfirst': 0, 'setfocus': 1,
                'setfocusbydvar': 1, 'setdvar': 2, 'exec': 1, 'execnow': 1, 'execondvarstringvalue': 3,
                'execondvarintvalue': 3, 'execondvarfloatvalue': 3, 'execnowondvarstringvalue': 3,
                'execnowondvarintvalue': 3, 'execnowondvarfloatvalue': 3, 'play': 1, 'scriptmenuresponse': 1,
                'scriptmenurespondondvarstringvalue': 3, 'scriptmenurespondondvarintvalue': 3,
                'scriptmenurespondondvarfloatvalue': 3, 'uiscript': None, 'setlocalvarint': 2,
                'setlocalvarstring': 2, 'setlocalvarfloat': 2, 'setlocalvarbool': 2, 'nosavehide': 1,
                'saveavailablehide': 1, 'profilehide': 1, 'setsaveexeconsuccess': 1, 'closeingame': 1,
                'stopmusic': 0, 'setfromdvar': 2, 'setmenuowner': 1}

# Expression functions that share a name with a menu keyword.
_FUNCTIONS = {'text', 'visible', 'rect', 'name'}


def split_commands(commands: list[list[str]]) -> list[list[str]]:
    out = []
    for command in commands:
        i = 0
        while i < len(command):
            count = COMMAND_ARGS.get(command[i].lower())
            if count is None:
                out.append(command[i:])
                break
            out.append(command[i:i + 1 + count])
            i += 1 + count
    return out


class Parser:
    def __init__(self, tokens: list[str]):
        self.t = tokens
        self.i = 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else None

    def take(self):
        tok = self.t[self.i]
        self.i += 1
        return tok

    def expect(self, value):
        tok = self.take()
        if tok != value:
            raise MenuSourceError(f'Expected {value!r}, found {tok!r} near token {self.i}')

    def block(self) -> list[list[str]]:
        """A script block body as commands (token lists)."""
        self.expect('{')
        commands, cur, depth = [], [], 1
        while True:
            tok = self.take()
            if tok == '{':
                depth += 1
            elif tok == '}':
                depth -= 1
                if depth == 0:
                    break
            if tok == ';' and depth == 1:
                if cur:
                    commands.append(cur)
                cur = []
            elif tok not in ('{', '}') or depth > 1:
                cur.append(tok)
        if cur:
            commands.append(cur)
        return split_commands(commands)

    def expression(self) -> list[str]:
        """Expression tokens up to ';' (or the next keyword) at depth 0."""
        depth, out = 0, []
        while self.peek() is not None:
            tok = self.peek()
            if depth == 0 and (tok in (';', '}') or (out and _is_keyword(tok) and tok.lower() not in _FUNCTIONS)):
                break
            self.take()
            depth += (tok == '(') - (tok == ')')
            out.append(tok)
        if self.peek() == ';':
            self.take()
        return out

    def number(self) -> str:
        """One numeric operand, a parenthesised constant expression included."""
        tok = self.take()
        if tok == '-':
            return '-' + self.number()
        if tok != '(':
            return tok
        depth, out = 1, ['(']
        while depth:
            tok = self.take()
            depth += (tok == '(') - (tok == ')')
            out.append(tok)
        return ' '.join(out)

    def value_line(self) -> list[str]:
        out = []
        while self.peek() not in (None, '}', ';') and not (_is_keyword(self.peek()) and out):
            out.append(self.take())
        if self.peek() == ';':
            self.take()
        return out

    def definition(self) -> dict:
        self.expect('{')
        d: dict = {'items': []}
        while self.peek() != '}':
            key = self.take()
            low = key.lower()
            if low == ';':
                continue
            if low == 'itemdef':
                d['items'].append(self.definition())
            elif low in SCRIPT_KEYS:
                d[low] = self.block()
            elif low in ('visible', 'disabled') and self.peek() and self.peek().lower() == 'when':
                self.take()
                d[low] = self.expression()
            elif low == 'exp':
                prop = self.take().lower()
                if prop in ('rect', 'forecolor', 'backcolor'):
                    prop += ' ' + self.take().lower()
                if self.peek() and self.peek().lower() == 'when':
                    self.take()
                d.setdefault('exp', {})[prop] = self.expression()
            elif low == 'decoration':
                d['decoration'] = True
            elif low in VECTOR_ARITY:
                d[low] = [self.number() for _ in range(VECTOR_ARITY[low])]
            elif low == 'rect':
                vals = [self.number() for _ in range(4)]
                while self.peek() and re.fullmatch(r'\d+', self.peek()) and len(vals) < 6:
                    vals.append(self.take())
                d['rect'] = vals
            else:
                d[low] = self.value_line()
        self.expect('}')
        return d


_KEYWORDS = {'name', 'rect', 'style', 'background', 'forecolor', 'backcolor', 'bordercolor', 'border', 'bordersize',
             'text', 'textfont', 'textscale', 'textstyle', 'textalign', 'textalignx', 'textaligny', 'visible',
             'decoration', 'type', 'exp', 'ownerdraw', 'itemdef', 'fullscreen', 'focuscolor', 'blurworld',
             'allowedbinding', 'origin', 'group', 'dvar', 'dvartest', 'showdvar', 'hidedvar', 'textfile',
             'feeder', 'elementwidth', 'elementheight', 'elementtype', 'disablecolor', 'outlinecolor', 'popup',
             'legacysplitscreenscale', 'execkey', 'execkeyint', 'soundloop', 'disabled', 'ownerdrawflag',
             'autowrapped', 'textsavegame', 'outofbounds', 'menudef', 'maxchars', 'notselectable',
             'noscrollbars', 'usepaging', 'columns'} | set(SCRIPT_KEYS)


def _is_keyword(tok: str | None) -> bool:
    return tok is not None and tok.lower() in _KEYWORDS


def parse(text: str) -> list[dict]:
    p = Parser(tokenize(text))
    menus = []
    while p.peek() is not None:
        if p.take().lower() == 'menudef':
            menus.append(p.definition())
    return menus


# ---------------------------------------------------------------- source set

def menu_name(menu: dict) -> str:
    return ''.join(menu.get('name', [''])).strip('"')


def load_sources(raw: Path) -> dict[str, dict]:
    """Every menuDef of the in-game menulists, later lists overriding by name."""
    menus: dict[str, dict] = {}
    for menulist in MENULISTS:
        if not (raw / menulist).is_file():
            raise MenuSourceError(f'WaW menulist missing: {raw / menulist}')
        listing = Preprocessor(raw).run(menulist)
        for file in re.findall(r'loadMenu\s*\{\s*"([^"]+)"', listing):
            if not (raw / file).is_file():
                continue  # listed but not shipped in raw/: not an in-game menu we draw
            for menu in parse(Preprocessor(raw, {'PC': '1'}).run(file)):
                menus[menu_name(menu).lower()] = menu
    return menus


def references(menu: dict) -> set[str]:
    out = set()

    def scripts(d):
        for key in SCRIPT_KEYS:
            for command in d.get(key, []):
                if command and command[0].lower() in ('open', 'showmenu') and len(command) > 1:
                    out.add(command[1].strip('"').lower())
        for item in d.get('items', []):
            scripts(item)
    scripts(menu)
    return out


def closure(menus: dict[str, dict], root: str = ROOT_MENU, stop=frozenset()) -> list[str]:
    seen, todo = [], [root]
    while todo:
        name = todo.pop(0)
        if name in seen or name not in menus or name in stop:
            continue
        seen.append(name)
        todo.extend(sorted(references(menus[name])))
    return seen


# ---------------------------------------------------------------- expressions

_PREC = {'||': 1, '&&': 2, '==': 3, '!=': 3, '<': 4, '>': 4, '<=': 4, '>=': 4, '+': 5, '-': 5,
         '*': 6, '/': 6, '%': 6}


def _q(text: str) -> str:
    return re.sub(r'[%\s,"|;=]', lambda m: '%%%02X' % ord(m.group()), text)


def rpn(tokens: list[str]) -> str:
    """Infix menu expression -> space separated RPN for the LUI evaluator.

    Literals ``n:<number>`` and ``s:<escaped string>``; functions
    ``f:<name>:<argc>``; operators ``op:<op>``. A bare word is a string, as
    T4's expression parser reads it."""
    out: list[str] = []
    pos = 0

    def primary():
        nonlocal pos
        tok = tokens[pos]
        pos += 1
        if tok == '(':
            expr(0)
            pos += 1  # ')'
        elif tok == '!':
            primary()
            out.append('op:!')
        elif tok == '-':
            primary()
            out.append('op:neg')
        elif tok.startswith('"'):
            out.append('s:' + _q(tok[1:-1]))
        elif re.fullmatch(r'\d+(\.\d*)?|\.\d+', tok):
            out.append('n:' + tok)
        elif pos < len(tokens) and tokens[pos] == '(':
            pos += 1
            argc = 0
            if tokens[pos] != ')':
                while True:
                    expr(0)
                    argc += 1
                    if tokens[pos] == ',':
                        pos += 1
                        continue
                    break
            pos += 1
            out.append(f'f:{tok.lower()}:{argc}')
        else:
            out.append('s:' + _q(tok))

    def expr(level):
        nonlocal pos
        primary()
        while pos < len(tokens) and tokens[pos] in _PREC and _PREC[tokens[pos]] >= level:
            op = tokens[pos]
            pos += 1
            expr(_PREC[op] + 1)
            out.append('op:' + op)

    if not tokens:
        return ''
    expr(0)
    if pos != len(tokens):
        raise MenuSourceError('Unparsed expression tail: ' + ' '.join(tokens[pos:]))
    return ' '.join(out)


def script(commands: list[list[str]]) -> str:
    """Commands as ';'-separated, '%'-escaped tokens."""
    return ';'.join(' '.join(_q(t.strip('"')) for t in command) for command in commands)


# ---------------------------------------------------------------- encoding

ITEM_COLUMNS = ('rect', 'style', 'background', 'forecolor', 'backcolor', 'bordercolor', 'border', 'bordersize',
                'text', 'textfont', 'textscale', 'textstyle', 'textalign', 'textalignx', 'textaligny',
                'visible', 'exp', 'type', 'name', 'onfocus', 'leavefocus', 'action', 'decoration', 'ownerdraw',
                'origin', 'autowrapped')
MENU_COLUMNS = ('rect', 'onopen', 'onclose', 'onesc', 'blurworld', 'items', 'visible', 'fullscreen', 'focuscolor')
SCRIPTS = ('onfocus', 'leavefocus', 'action', 'onopen', 'onclose', 'onesc')
NUMBERS = ('style', 'border', 'bordersize', 'textfont', 'textscale', 'textstyle', 'textalign', 'textalignx',
           'textaligny', 'type', 'ownerdraw', 'blurworld', 'fullscreen')
VECTORS = ('rect', 'forecolor', 'backcolor', 'bordercolor', 'focuscolor', 'origin')


def _num(value: str):
    """A numeric field; the menu compiler folds constant arithmetic."""
    value = value.strip()
    if not re.fullmatch(r'[\d.\s()+*/%-]+', value):
        raise MenuSourceError(f'Expected a number, found {value!r}')
    number = float(eval(value, {'__builtins__': {}}, {}))
    return int(number) if number == int(number) else round(number, 6)


def _value(item: dict, key: str) -> str:
    v = item.get(key)
    if key == 'visible':
        return 'n:1' if v is None else rpn(v)
    if v is None:
        return ''
    if key in SCRIPTS:
        return script(v)
    if key == 'exp':
        return '|'.join(f'{prop}={rpn(tokens)}' for prop, tokens in sorted(v.items()))
    if key in ('decoration', 'autowrapped'):
        return '1'
    if key in VECTORS:
        return ' '.join(str(_num(x)) for x in v)
    text = ' '.join(t.strip('"') for t in v)
    return str(_num(text)) if key in NUMBERS and text else text


def encode(menus: dict[str, dict], names: list[str], context: dict[str, str]) -> list[list[str]]:
    """Rows keyed ``menu:<name>`` / ``item:<name>:<n>`` plus ``context`` rows."""
    rows = [[key, value] for key, value in sorted(context.items())]
    for name in names:
        menu = menus[name]
        rows.append([f'menu:{name}', *[str(len(menu['items'])) if key == 'items' else _value(menu, key)
                                       for key in MENU_COLUMNS]])
        for i, item in enumerate(menu['items'], 1):
            rows.append([f'item:{name}:{i}', *[_value(item, key) for key in ITEM_COLUMNS]])
    return rows


def _literals(tokens) -> list[str]:
    return [t[1:-1] for t in tokens if len(t) > 1 and t.startswith('"') and t.endswith('"')]


def requirements(menus: dict[str, dict], names: list[str], mapname: str) -> dict:
    """Materials, localized strings and literal text the translated menus draw.

    An ``exp material`` concatenation is folded with the source map name, the
    only dynamic part the in-game menus use (``"menu_map_" + dvarString(mapname)``)."""
    materials, strings, literal = set(), {}, {}

    def text_of(d):
        if d.get('text'):
            yield ' '.join(d['text']).strip('"')
        yield from _literals(d.get('exp', {}).get('text', []))

    def walk(d):
        if d.get('background'):
            materials.add(' '.join(d['background']).strip('"'))
        exp = d.get('exp', {})
        if 'material' in exp:
            tokens = exp['material']
            name = ''.join(_literals(tokens))
            if any(t.lower() == 'mapname' for t in tokens):
                name += mapname
            materials.add(name)
        enum = int(_num(' '.join(d['textfont']).strip('"'))) if d.get('textfont') else 0
        for value in text_of(d):
            if value.startswith('@'):
                strings.setdefault(value[1:], set()).add(enum)
            elif value:
                literal.setdefault(enum, set()).update(value)
        for item in d.get('items', []):
            walk(item)
    for name in names:
        walk(menus[name])
    return {'materials': sorted(materials - {''}),
            'strings': {k: sorted(v) for k, v in sorted(strings.items())},
            'literal_text': {str(k): ''.join(sorted(v)) for k, v in sorted(literal.items())}}


# T4 UI_GetFontHandle (t4sp 0x5B6400): SMALL, NORMAL, CONSOLE and OBJECTIVE are
# fixed; BOLD picks small/bold/normal and every other enum small/extraBig/big/
# normal from the on-screen text size, so each of those may be drawn.
FONT_ROLES = {0: ('small', 'extrabig', 'big', 'normal'), 1: ('normal',), 3: ('small',),
              4: ('small', 'bold', 'normal'), 5: ('console',), 6: ('objective',)}
FONT_FILES = {'small': 'fonts/smallFont', 'normal': 'fonts/normalFont', 'big': 'fonts/bigFont',
              'extrabig': 'fonts/extraBigFont', 'bold': 'fonts/boldFont', 'console': 'fonts/consoleFont',
              'objective': 'fonts/objectiveFont'}


def font_roles(enum: int) -> tuple[str, ...]:
    return FONT_ROLES.get(enum, FONT_ROLES[0])


def glyph_sets(required: dict, localized: dict[str, str]) -> dict[str, set[str]]:
    """Characters each font role must carry: the menus' resolved text."""
    out: dict[str, set[str]] = {}
    for key, enums in required['strings'].items():
        value = re.sub(r'\^\d', '', localized.get(key, key))
        for enum in enums:
            for role in font_roles(enum):
                out.setdefault(role, set()).update(value)
    for enum, chars in required['literal_text'].items():
        for role in font_roles(int(enum)):
            out.setdefault(role, set()).update(chars)
    for chars in out.values():
        chars.add('?')  # an unknown glyph draws as '?'
        chars.discard('\n')
    return out


def write_table(path: Path, rows) -> None:
    data = io.StringIO(newline='')
    csv.writer(data, lineterminator='\n').writerows(rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data.getvalue(), encoding='utf-8')


def glue() -> dict:
    return json.loads((Path(__file__).parent / 'compat/waw_menu_glue.json').read_text(encoding='utf-8'))


def translate(raw: Path) -> tuple[dict[str, dict], list[str]]:
    menus = load_sources(raw)
    names = closure(menus, stop=set(glue()['engine_menus']))
    if ROOT_MENU not in names:
        raise MenuSourceError(f'{ROOT_MENU} is not defined by {", ".join(MENULISTS)}')
    return menus, names


def find_raw(raw_roots: list[Path]) -> Path | None:
    return next((r for r in raw_roots if all((r / m).is_file() for m in MENULISTS)), None)


def stage_source(project: Path, raw_roots: list[Path], mapname: str) -> dict:
    """Translate the in-game menus; record what they need before localization."""
    raw = find_raw(raw_roots)
    if raw is None:
        raise MenuSourceError('WaW menu sources (raw/ui/ingame.txt, raw/ui/hud.txt) not found; '
                              'the WaW Mod Tools raw folder is required')
    menus, names = translate(raw)
    required = requirements(menus, names, mapname)
    engine = glue()['engine_menus']
    context = {'mapname': mapname, **{f'engine:{k}': v['t6_menu'] for k, v in engine.items()}}
    write_table(project / TABLE, encode(menus, names, context))
    result = {'source': str(raw), 'menus': names, 'mapname': mapname, **required,
              'engine_menus': sorted(engine)}
    path = project / MANIFEST
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def requested_strings(project: Path) -> set[str]:
    path = project / MANIFEST
    return set(json.loads(path.read_text(encoding='utf-8'))['strings']) if path.is_file() else set()


def stage_assets(project: Path, roots: list[Path], stock, localized: dict[str, str], recovery=None) -> dict:
    """The menus' original materials and the glyphs their resolved text uses."""
    from PIL import Image
    from . import wawhud
    manifest = json.loads((project / MANIFEST).read_text(encoding='utf-8'))
    source = wawhud.SourceFinder(roots, stock, recovery)
    materials, missing = [], []

    def sprite(name, image):
        materials.append(wawhud.write_sprite(project, 'waw_menu/' + name, image))
        return materials[-1]
    for name in manifest['materials']:
        path = source.material_image(name)
        if path is None:
            missing.append(name)
            continue
        with Image.open(path) as pixels:
            sprite(name, pixels)
    rows = []
    for role, chars in sorted(glyph_sets(manifest, localized).items()):
        try:
            rows += wawhud.font_glyphs(source, FONT_FILES[role], role, sprite, chars)
        except ValueError as error:
            if str(error).startswith('Invalid'):
                raise
            missing.append(str(error))
    write_table(project / GLYPHS, rows)
    manifest.update(staged_materials=materials, missing=missing, glyphs=len(rows))
    (project / MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return {'menus': len(manifest['menus']), 'materials': len(materials), 'glyphs': len(rows), 'missing': missing}
