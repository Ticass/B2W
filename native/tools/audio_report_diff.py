# Show where two audio.stage.json reports differ.
import json
import sys

a = json.load(open(sys.argv[1], encoding='utf-8'))
b = json.load(open(sys.argv[2], encoding='utf-8'))
print('audio', len(a['audio']), len(b['audio']), 'errors', len(a['errors']), len(b['errors']))
for x, y in zip(a['audio'], b['audio']):
    if x != y:
        print('A', json.dumps(x)[:900])
        print('B', json.dumps(y)[:900])
        break
for e in b['errors']:
    if e not in a['errors']:
        print('new error', e)
