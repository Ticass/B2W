# Byte compare two output folders; files only in the first that match an
# --ignore prefix are not reported. Usage: compare_dirs.py <a> <b> [--ignore prefix]...
import filecmp
import sys
from pathlib import Path

args = sys.argv[1:]
ignore = [args[i + 1] for i, a in enumerate(args) if a == '--ignore']
a_root, b_root = Path(args[0]), Path(args[1])
a = {p.relative_to(a_root).as_posix(): p for p in a_root.rglob('*') if p.is_file()}
b = {p.relative_to(b_root).as_posix(): p for p in b_root.rglob('*') if p.is_file()}
only_a = [n for n in sorted(set(a) - set(b)) if not any(n.startswith(i) for i in ignore)]
only_b = sorted(set(b) - set(a))
different = [n for n in sorted(set(a) & set(b)) if not filecmp.cmp(a[n], b[n], shallow=False)]
print(f'{len(a)} vs {len(b)} files; only first {len(only_a)}, only second {len(only_b)}, different {len(different)}')
for n in (only_a + only_b + different)[:20]:
    print('  ', n)
