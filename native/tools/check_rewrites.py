# Apply each gscport rewrite in Python and natively to every script below the
# given folders (texts as wawtext.read gives them) and compare count and text.
# Usage: check_rewrites.py <folder>... [--only name]
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, 'C:/WawConverter/src')
from waw2bo2 import gsc, gscport, wawtext  # noqa: E402

NATIVE = Path(__file__).resolve().parents[1] / 'build/waw2bo2n.exe'
COMPAT = Path('C:/WawConverter/src/waw2bo2/compat')

REWRITES = {
    'neutralize_animations': lambda t: gscport.neutralize_animations(t, 'tree') or 0,
    'bridge_contact_kill_attacks': lambda t: gscport.bridge_contact_kill_attacks(t, gsc.parse(gsc.emit(t), 'x')),
    'fix_syntax': gscport.fix_syntax, 'split_call_receivers': gscport.split_call_receivers,
    'rename_level_fields': gscport.rename_level_fields,
    'bridge_poi_record_comparisons': gscport.bridge_poi_record_comparisons,
    'bridge_revive_reads': gscport.bridge_revive_reads, 'bridge_drinking_flag': gscport.bridge_drinking_flag,
    'bridge_score_writes': gscport.bridge_score_writes, 'bridge_player_stat_reads': gscport.bridge_player_stat_reads,
    'bridge_dynamic_hud_materials': gscport.bridge_dynamic_hud_materials,
    'mirror_team_zombie_vars': gscport.mirror_team_zombie_vars, 'bridge_font_scales': gscport.bridge_font_scales,
    'keep_bo2_deathanims': gscport.keep_bo2_deathanims,
    'bridge_intermission_camera': gscport.bridge_intermission_camera,
    'bridge_launch_triggers': gscport.bridge_launch_triggers,
    'lower_maxhealth_assignments': gscport.lower_maxhealth_assignments,
    'lower_health_assignments': gscport.lower_health_assignments, 'lower_hud_fields': gscport.lower_hud_fields,
    'lower_zombie_weapon_keys': gscport.lower_zombie_weapon_keys,
    'lower_undefined_size_loops': gscport.lower_undefined_size_loops,
    'wrap_integer_literals': gscport.wrap_integer_literals,
    'lower_stationary_fx_carriers': gscport.lower_stationary_fx_carriers,
}

args = sys.argv[1:]
only = args[args.index('--only') + 1] if '--only' in args else None
folders = [a for i, a in enumerate(args) if a != '--only' and (i == 0 or args[i - 1] != '--only')]
files = sorted({p for f in folders for ext in ('*.gsc', '*.csc') for p in Path(f).rglob(ext)})
with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    texts = []
    for i, path in enumerate(files):
        text = wawtext.read(path)
        copy = temp / f'{i}.txt'
        copy.write_bytes(text.encode('utf-8', errors='surrogatepass'))
        texts.append((str(copy), path, text))
    (temp / 'list.txt').write_text('\n'.join(c for c, _, _ in texts), encoding='utf-8')
    total = 0
    for name, function in REWRITES.items():
        if only and name != only:
            continue
        subprocess.run([str(NATIVE), 'gsc-rewrite', name, str(temp / 'list.txt'), str(temp / 'out.jsonl'), str(COMPAT)],
                       check=True)
        native = {}
        for line in (temp / 'out.jsonl').read_text(encoding='utf-8').splitlines():
            row = json.loads(line)
            native[row['path']] = row
        bad = changed = 0
        for copy, path, text in texts:
            row = native[copy]
            try:
                tokens = gsc.tokenize(text)
                count = function(tokens)
                result = {'count': count, 'text': gsc.emit(tokens)}
            except Exception as exc:  # noqa: BLE001
                result = {'error': type(exc).__name__}
            if 'error' in result or 'error' in row:
                if ('error' in result) != ('error' in row):
                    bad += 1
                    print(f'  {name}: error mismatch {path}: python {result.get("error")} native {row.get("error")}')
                continue
            changed += result['count'] > 0
            if result['count'] != row['count'] or result['text'] != row['text']:
                bad += 1
                if bad <= 3:
                    a, b = result['text'], row['text']
                    at = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
                    print(f'  {name}: DIFF {path} count {result["count"]}/{row["count"]} at {at}: '
                          f'{a[max(0, at - 60):at + 60]!r} | {b[max(0, at - 60):at + 60]!r}')
        total += bad
        print(f'{name}: {len(texts)} scripts, {changed} changed, {bad} differ')
    print('TOTAL differ', total)
