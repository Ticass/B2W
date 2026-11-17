# Compare core/pyre with Python's re on every literal pattern the converter's
# Python uses (found with ast) against sample texts: real scripts and short
# synthetic strings. Usage: check_regex.py <scripts folder> [max scripts]
import ast
import json
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

NATIVE = Path(__file__).resolve().parents[1] / 'build/waw2bo2n.exe'
SRC = Path('C:/WawConverter/src/waw2bo2')
FUNCS = {'compile', 'search', 'match', 'fullmatch', 'findall', 'finditer', 'sub', 'subn', 'split'}

patterns = set()
for path in SRC.rglob('*.py'):
    tree = ast.parse(path.read_text(encoding='utf-8'))
    for node in ast.walk(tree):
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr in FUNCS
                and isinstance(node.func.value, ast.Name) and node.func.value.id == 're' and node.args
                and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str)):
            flags = 0
            for kw in node.keywords:
                if kw.arg == 'flags':
                    try:
                        flags = eval(compile(ast.Expression(kw.value), 'f', 'eval'), {'re': re})
                    except Exception:  # noqa: BLE001
                        flags = 0
            if node.func.attr == 'compile' and len(node.args) > 1:
                try:
                    flags = eval(compile(ast.Expression(node.args[1]), 'f', 'eval'), {'re': re})
                except Exception:  # noqa: BLE001
                    pass
            patterns.add((node.args[0].value, int(flags)))

folder = Path(sys.argv[1])
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 40
random.seed(7)
scripts = sorted(folder.rglob('*.gsc'))
texts = [p.read_text(encoding='utf-8', errors='replace')[:20000] for p in random.sample(scripts, min(limit, len(scripts)))]
texts += ['', 'abc', 'a\nb\n', '  x = 1;\r\n', 'level.foo = 1;', 'hud_chalk_5 // c\\\n', 'Foo_Bar baz123', 'x*\\y', '\u00e9t\u00e9 \u00a0z']

cases, expected = [], []
for pattern, flags in sorted(patterns):
    try:
        compiled = re.compile(pattern, flags)
    except re.error:
        continue
    for text in texts:
        try:
            found = [[list(m.span(g)) for g in range(compiled.groups + 1)] for m in compiled.finditer(text)]
            m, f = compiled.match(text), compiled.fullmatch(text)
            result = {'finditer': found, 'sub': compiled.sub(r'<\g<0>>', text),
                      'split': [s if s is not None else '' for s in compiled.split(text)],
                      'match': list(m.span()) if m else None, 'fullmatch': list(f.span()) if f else None}
        except Exception as exc:  # noqa: BLE001
            result = {'error': str(exc)}
        cases.append({'pattern': pattern, 'flags': flags, 'text': text, 'repl': r'<\g<0>>'})
        expected.append(result)

with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    (temp / 'cases.jsonl').write_text('\n'.join(json.dumps(c) for c in cases), encoding='utf-8')
    subprocess.run([str(NATIVE), 'regex-test', str(temp / 'cases.jsonl'), str(temp / 'out.jsonl')], check=True)
    got = [json.loads(line) for line in (temp / 'out.jsonl').read_text(encoding='utf-8').splitlines()]

bad, failing = 0, {}
for case, want, have in zip(cases, expected, got):
    if 'error' in want and 'error' in have:
        continue
    if want != have:
        bad += 1
        failing.setdefault((case['pattern'], case['flags']), (case['text'][:80], want, have))
print(f'{len(patterns)} patterns x {len(texts)} texts = {len(cases)} cases; {bad} differ in {len(failing)} patterns')
for (pattern, flags), (text, want, have) in list(failing.items())[:15]:
    keys = [k for k in want if want.get(k) != have.get(k)] if 'error' not in have else ['error']
    print(f'  {pattern!r} flags={flags}: {keys} text={text!r}')
    if 'error' in have:
        print('     native error:', have['error'])
