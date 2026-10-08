"""Lossless GSC/CSC tokenizer and a light structural parser.

Shared by the WaW -> BO2 script translator (gscport.py). Every token keeps
the whitespace/comments that precede it, so ``emit(tokens)`` reproduces the
source byte for byte and translations only touch the tokens they rewrite.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

# token kinds
IDENT, PATH, STRING, NUMBER, PUNCT, DIRECTIVE, EOF = "ident", "path", "string", "number", "punct", "directive", "eof"

KEYWORDS = {"if", "else", "while", "for", "foreach", "in", "switch", "case", "default", "return", "break",
            "continue", "wait", "waittill", "waittillmatch", "waittillframeend", "notify", "endon", "thread",
            "self", "level", "game", "anim", "undefined", "true", "false", "do", "size", "isdefined",
            "prof_begin", "prof_end", "breakpoint", "vectorscale"}
# statements/operators that look like calls: `if (`, `while (` ...
CALL_LIKE_KEYWORDS = {"if", "while", "for", "foreach", "switch", "return", "wait", "waittill", "waittillmatch",
                      "notify", "endon", "thread", "case", "else", "do"}

_TOKEN_RE = re.compile(r"""
    (?P<ws>(?:\s+|//[^\n]*|/\*.*?\*/|/\*.*?(?m:^[\t ]*\*\\[\t ]*(?:\r?\n|$)))+)
  | (?P<directive>\#(?:include|using_animtree|insert|define)\b[^;\n]*;?)
  | (?P<string>[&#]?"(?:\\.|[^"\\\n])*")
  | (?P<path>[A-Za-z_]\w*(?:\\[A-Za-z_]\w*)+)
  | (?P<number>(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?)
  | (?P<ident>[A-Za-z_]\w*)
  | (?P<punct>::|\[\[|\]\]|\+\+|--|&&|\|\||==|!=|<=|>=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<|>>|/\#|\#/|[-+*/%<>=!&|^~?:;,.(){}\[\]\#@%])
""", re.VERBOSE | re.DOTALL)


class GscSyntaxError(ValueError):
    pass


@dataclass
class Token:
    kind: str
    text: str
    pre: str = ""   # whitespace/comments before the token
    line: int = 0

    @property
    def low(self) -> str:
        return self.text.lower()


def tokenize(source: str) -> list[Token]:
    tokens: list[Token] = []
    pos = 0
    line = 1
    pre = ""
    n = len(source)
    while pos < n:
        m = _TOKEN_RE.match(source, pos)
        if not m:
            raise GscSyntaxError(f"line {line + pre.count(chr(10))}: cannot tokenize {source[pos:pos + 20]!r}")
        kind = m.lastgroup
        text = m.group()
        if kind == "ws":
            pre += text
        else:
            line += pre.count("\n")
            tokens.append(Token(kind, text, pre, line))
            line += text.count("\n")
            pre = ""
        pos = m.end()
    tokens.append(Token(EOF, "", pre, line))
    return tokens


def emit(tokens: list[Token]) -> str:
    return "".join(t.pre + t.text for t in tokens)


def repair_block_comments(text: str) -> tuple[str, int]:
    """Repair the standalone *\\ terminator found in authored WaW comments.

    Normal comments win in the tokenizer. Never cross an existing */ or touch
    strings/code; callers pass only the whitespace/comment token prefixes.
    """
    pattern = r'//[^\n]*|/\*.*?\*/|(?P<broken>/\*.*?(?m:^[\t ]*\*\\(?=[\t ]*(?:\r?\n|$))))'
    count = 0
    def repair(match):
        nonlocal count
        if match.lastgroup == 'broken':
            count += 1
            return match[0][:-1] + '/'
        return match[0]
    return re.sub(pattern, repair, text, flags=re.DOTALL), count


@dataclass
class Function:
    name: str
    params: list[str]
    start: int          # index of the name token
    body_open: int      # index of '{'
    body_close: int     # index of matching '}'


@dataclass
class Script:
    path: str                      # e.g. maps\_access_bunker (no extension)
    tokens: list[Token]
    includes: list[str] = field(default_factory=list)
    functions: dict[str, Function] = field(default_factory=dict)   # lower-case name -> Function
    animtree: str | None = None


def _match(tokens: list[Token], i: int, open_: str, close: str) -> int:
    depth = 0
    for j in range(i, len(tokens)):
        t = tokens[j].text
        if tokens[j].kind == PUNCT:
            if t == open_:
                depth += 1
            elif t == close:
                depth -= 1
                if depth == 0:
                    return j
    raise GscSyntaxError(f"line {tokens[i].line}: unbalanced {open_}")


INCLUDE_RE = re.compile(r"#include\s+([\w\\/]+)\s*;?", re.IGNORECASE)
ANIMTREE_RE = re.compile(r'#using_animtree\s*\(\s*"([^"]+)"\s*\)', re.IGNORECASE)


def parse(source: str, path: str) -> Script:
    try:
        tokens = tokenize(source)
    except GscSyntaxError as error:
        raise GscSyntaxError(f'{path}: {error}') from error
    script = Script(path, tokens)
    i = 0
    depth = 0
    while i < len(tokens):
        t = tokens[i]
        if t.kind == DIRECTIVE:
            m = INCLUDE_RE.match(t.text)
            if m:
                script.includes.append(m.group(1).replace("/", "\\"))
            m = ANIMTREE_RE.match(t.text)
            if m:
                script.animtree = m.group(1)
        elif t.kind == PUNCT and t.text == "/#" and depth == 0:
            pass
        elif depth == 0 and t.kind == IDENT and tokens[i + 1].text == "(":
            close = _match(tokens, i + 1, "(", ")")
            if tokens[close + 1].text == "{":
                body_close = _match(tokens, close + 1, "{", "}")
                params = [tokens[k].text for k in range(i + 2, close) if tokens[k].kind == IDENT]
                script.functions.setdefault(t.low, Function(t.text, params, i, close + 1, body_close))
                i = body_close + 1
                continue
        if t.kind == PUNCT and t.text == "{":
            depth += 1
        elif t.kind == PUNCT and t.text == "}":
            depth -= 1
        i += 1
    return script


@dataclass
class Ref:
    """A reference to a function: a call ``f(``, a pointer ``::f`` or a
    qualified ``path::f``."""
    index: int          # index of the function-name token
    name: str
    qualifier: str | None   # script path for path::f
    qual_index: int | None  # index of the path token
    method: bool            # called on an object (``ent f()``)
    pointer: bool           # ``::f`` / ``path::f`` without a call


def references(tokens: list[Token], start: int = 0, end: int | None = None) -> list[Ref]:
    end = len(tokens) if end is None else end
    refs = []
    for i in range(start, end):
        t = tokens[i]
        if t.kind != IDENT:
            continue
        prev = tokens[i - 1] if i else None
        nxt = tokens[i + 1]
        if prev is not None and prev.text == "::":
            if i >= 2 and tokens[i - 2].kind == PATH:
                refs.append(Ref(i, t.text, tokens[i - 2].text, i - 2, False, nxt.text != "("))
            else:
                refs.append(Ref(i, t.text, None, None, False, nxt.text != "("))
            continue
        if nxt.text != "(" or t.low in CALL_LIKE_KEYWORDS:
            continue
        if prev is not None and prev.text == ".":
            continue
        method = prev is not None and (prev.kind in (IDENT, STRING) and prev.low not in ("thread", "return", "else",
                                                                                        "wait", "case", "do")
                                       or prev.text in (")", "]"))
        if method and prev.text == ")":
            # `if ( x ) f()` / `while (...) f()` is a statement, not `(expr) f()`
            depth = 0
            for k in range(i - 1, -1, -1):
                if tokens[k].text == ")":
                    depth += 1
                elif tokens[k].text == "(":
                    depth -= 1
                    if depth == 0:
                        if k and tokens[k - 1].low in ("if", "while", "for", "foreach", "switch"):
                            method = False
                        break
        refs.append(Ref(i, t.text, None, None, method, False))
    return refs


def load_scripts(root: Path, exts=(".gsc",)) -> dict[str, Script]:
    """Parse every script below ``root`` keyed by lower-case include path."""
    out = {}
    for ext in exts:
        for f in sorted(root.rglob(f"*{ext}")):
            rel = f.relative_to(root).with_suffix("").as_posix().replace("/", "\\").lower()
            try:
                out[rel] = parse(f.read_text(encoding="utf-8", errors="replace"), rel)
            except GscSyntaxError as exc:
                out[rel] = Script(rel, [])
                out[rel].error = str(exc)  # type: ignore[attr-defined]
    return out
