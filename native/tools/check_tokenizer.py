# Compare gsc.tokenize with waw2bo2n gsc-tokens on every script below the
# given folders (each file decoded as wawtext.read decodes it, then written
# as UTF-8 for the native side). Usage: check_tokenizer.py <folder>...
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, 'C:/WawConverter/src')
from waw2bo2 import gsc, wawtext  # noqa: E402

NATIVE = Path(__file__).resolve().parents[1] / 'build/waw2bo2n.exe'

files = sorted({p for folder in sys.argv[1:] for ext in ('*.gsc', '*.csc') for p in Path(folder).rglob(ext)})
with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    texts, listing = {}, []
    for i, path in enumerate(files):
        text = wawtext.read(path)
        copy = temp / f'{i}.txt'
        copy.write_bytes(text.encode('utf-8', errors='surrogatepass'))
        texts[str(copy)] = (path, text)
        listing.append(str(copy))
    (temp / 'list.txt').write_text('\n'.join(listing), encoding='utf-8')
    subprocess.run([str(NATIVE), 'gsc-tokens', str(temp / 'list.txt'), str(temp / 'out.jsonl')], check=True)
    native = {}
    for line in (temp / 'out.jsonl').read_text(encoding='utf-8').splitlines():
        row = json.loads(line)
        native[row['path']] = row
    bad = 0
    for copy, (path, text) in texts.items():
        row = native[copy]
        try:
            expected = [[t.kind, t.text, t.pre, t.line] for t in gsc.tokenize(text)]
            error = None
        except gsc.GscSyntaxError as exc:
            expected, error = None, str(exc)
        if error is not None or 'error' in row:
            if error != row.get('error'):
                bad += 1
                print('ERROR MISMATCH', path, error, '|', row.get('error'))
            continue
        if expected != row['tokens']:
            bad += 1
            first = next(i for i, (a, b) in enumerate(zip(expected, row['tokens'] + [None] * len(expected))) if a != b)
            print('DIFF', path, 'token', first, expected[first], row['tokens'][first] if first < len(row['tokens']) else None)
            continue
        repaired = list(gsc.repair_block_comments(text))
        if repaired != row.get('repair'):
            bad += 1
            print('REPAIR DIFF', path)
            continue
        if 'includes' not in row:
            continue  # the native parse raised (unbalanced); Python's parse must too
        script = gsc.parse(text, 'x')
        parsed = {'includes': script.includes, 'animtree': script.animtree,
                  'functions': [[low, f.name, f.params, f.start, f.body_open, f.body_close, f.animtree]
                                for low, f in script.functions.items()],
                  'refs': [[r.index, r.name, r.qualifier, r.qual_index, r.method, r.pointer]
                           for r in gsc.references(script.tokens)]}
        for key, value in parsed.items():
            if value != row[key]:
                bad += 1
                print('PARSE DIFF', key, path)
                break
    print(f'{len(files)} scripts, {bad} differ')
