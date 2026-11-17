import os
import sys
import zipfile

mods = os.path.join(os.environ['LOCALAPPDATA'], 'Plutonium', 'storage', 't6', 'mods')
project = sys.argv[1]
src = os.path.join(mods, project)
out = os.path.join(mods, project + '.zip')
tmp = out + '.tmp'
with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
    for dp, dn, fn in os.walk(src):
        rel_dir = os.path.relpath(dp, mods)
        if 'scriptdata' in rel_dir.split(os.sep):
            continue  # game-written player data, not part of the build
        for f in fn:
            if f.startswith('console_zm.log') or f == 'games_mp.log':
                continue
            p = os.path.join(dp, f)
            z.write(p, os.path.relpath(p, mods).replace(os.sep, '/'))
os.replace(tmp, out)
with zipfile.ZipFile(out) as z:
    print(len(z.namelist()), z.namelist())
print(out, os.path.getsize(out))
