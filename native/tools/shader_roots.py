# Recover the shader stage's roots (in first-use order) from a stage.json.
# Usage: shader_roots.py <stage.json> <out.txt>
import json
import sys

report = json.load(open(sys.argv[1], encoding='utf-8'))
roots = []
for row in report['shaders']:
    source = row['source']
    for sub in ('\\content_source\\shader_bin\\', '\\shader_bin\\'):
        if sub in source:
            root = source.split(sub)[0]
            if root not in roots:
                roots.append(root)
            break
open(sys.argv[2], 'w', encoding='utf-8').write('\n'.join(roots))
print(len(roots), 'roots')
