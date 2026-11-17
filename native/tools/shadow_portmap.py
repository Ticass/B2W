# Shadow run of gscport.port_map: stage-bridge of a real map in process (Python
# steps); at port_map the Python step runs, its outputs and the native step's
# arguments are saved, then waw2bo2n port-map writes the same outputs into a
# separate folder and the two are compared.
# Usage: shadow_portmap.py der_riese|snn <outdir>
#        shadow_portmap.py <map.ff> <project> <outdir>   (any map, the user's settings)
#        shadow_portmap.py --again <outdir>   (rerun native on the saved inputs)
import filecmp
import json
import os
import runpy
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, 'C:/WawConverter/src')
os.environ['WAW2BO2_NATIVE'] = '0'
os.environ['WAW2BO2_BUILD_CACHE'] = '0'

NATIVE = Path(__file__).resolve().parents[1] / 'build/waw2bo2n.exe'


class Done(Exception):
    pass


def outputs(root: Path) -> dict:
    found = {}
    for sub in ('maps/mp/waw', 'aitype'):
        for p in (root / sub).rglob('*') if (root / sub).exists() else ():
            if p.is_file():
                found[p.relative_to(root).as_posix()] = p
    events = root / 'content_source/announcer_events.json'
    if events.is_file():
        found['content_source/announcer_events.json'] = events
    if (root / 'port_report.json').is_file():
        found['port_report.json'] = root / 'port_report.json'
    return found


def compare(shadow_dir: Path) -> None:
    native_root = shadow_dir / 'native'
    if native_root.exists():
        shutil.rmtree(native_root)
    (native_root / 'BSP').mkdir(parents=True)
    entities = shadow_dir / 'entities.json'
    if entities.is_file():
        shutil.copy2(entities, native_root / 'BSP/entities.json')
    args = json.loads((shadow_dir / 'args.json').read_text(encoding='utf-8'))
    args['out_root'] = str(native_root)
    args['report'] = str(native_root / 'port_report.json')
    (shadow_dir / 'args_run.json').write_text(json.dumps(args), encoding='utf-8')
    start = time.time()
    proc = subprocess.run([str(NATIVE), 'port-map', str(shadow_dir / 'args_run.json')], capture_output=True, text=True, errors='replace')
    seconds = time.time() - start
    print(f'SHADOW native {seconds:.1f}s, exit {proc.returncode}: {proc.stdout.strip()} {proc.stderr.strip()[-1500:]}', flush=True)
    if (native_root / 'port_report.json').is_file():
        report = json.loads((native_root / 'port_report.json').read_text(encoding='utf-8'))
        (native_root / 'port_report.json').write_text(json.dumps(report, indent=1), encoding='utf-8')
    a, b = outputs(shadow_dir / 'python'), outputs(native_root)
    different = [n for n in sorted(set(a) & set(b)) if not filecmp.cmp(a[n], b[n], shallow=False)]
    print(f'SHADOW {len(a)} python files, {len(b)} native; only python {len(set(a) - set(b))}, '
          f'only native {len(set(b) - set(a))}, different {len(different)}', flush=True)
    for n in (sorted(set(a) ^ set(b)) + different)[:40]:
        print('SHADOW   ', n, flush=True)


def main():
    if sys.argv[1] == '--again':
        compare(Path(sys.argv[2]).resolve())
        return
    from waw2bo2 import builddriver, cli, gscport, t6api  # noqa: E402
    custom = sys.argv[1].lower().endswith('.ff')
    out_arg = sys.argv[3] if custom else sys.argv[2]
    shadow_dir = Path(out_arg).resolve() / 'shadow_portmap'
    original_init = gscport.Sources.__init__

    def init(self, roots, iwds, stock_root, variant_roots=None):
        self._shadow_args = (list(roots), list(iwds), stock_root, list(variant_roots) if variant_roots else None)
        original_init(self, roots, iwds, stock_root, variant_roots)

    gscport.Sources.__init__ = init
    caches = []
    original_build = t6api.build

    def build(bo2_root, cache, unlinker=None, stock_dump=None):
        caches.append(cache)
        return original_build(bo2_root, cache, unlinker, stock_dump)

    t6api.build = build
    original_port_map = gscport.port_map

    def port_map(sources, api, map_name, out_root, fx_table=None, animtrees=None, core_animtrees=None, bo2_stock_perks=False,
                 actor_types=frozenset(), actor_template=None, stock_map=False, client_ported=False):
        roots, iwds, stock, variants = sources._shadow_args
        if actor_template is not None and not isinstance(actor_template, dict):
            actor_template = {'human': actor_template}
        args = {'roots': [str(r) for r in roots], 'iwds': [str(i) for i in iwds], 'stock': str(stock) if stock else None,
                'variant_roots': [str(v) for v in variants] if variants else None, 'snapshot': str(t6api.SNAPSHOT),
                'api_cache': str(caches[-1]), 'compat_dir': str(gscport.COMPAT_DIR), 'map_name': map_name,
                'animtrees': sorted(animtrees or ()), 'core_animtrees': {k: sorted(v) for k, v in (core_animtrees or {}).items()},
                'bo2_stock_perks': bo2_stock_perks, 'actor_types': sorted(actor_types), 'actor_template': actor_template,
                'stock_map': stock_map, 'client_ported': client_ported, 'bind': True}
        if shadow_dir.exists():
            shutil.rmtree(shadow_dir)
        shadow_dir.mkdir(parents=True)
        (shadow_dir / 'args.json').write_text(json.dumps(args), encoding='utf-8')
        if (Path(out_root) / 'BSP/entities.json').is_file():
            shutil.copy2(Path(out_root) / 'BSP/entities.json', shadow_dir / 'entities.json')
        start = time.time()
        report = original_port_map(sources, api, map_name, out_root, fx_table, animtrees, core_animtrees, bo2_stock_perks,
                                   actor_types, actor_template, stock_map, client_ported)
        print(f'SHADOW python {time.time() - start:.1f}s', flush=True)
        python_root = shadow_dir / 'python'
        for name, path in outputs(Path(out_root)).items():
            target = python_root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        (python_root / 'port_report.json').write_text(json.dumps(report.to_json(), indent=1), encoding='utf-8')
        compare(shadow_dir)
        raise Done

    gscport.port_map = port_map

    def run(command, log=None):
        if 'stage-bridge' not in command:
            raise Done
        cli.main(command[command.index('stage-bridge'):])
        raise Done

    builddriver.run = run
    builddriver.remember = lambda *a, **k: None
    try:
        if custom:
            from waw2bo2.launcher import Settings, discover
            main_root = Path('C:/WawConverter')
            s = discover(Settings.load())
            s.fastfile, s.project, s.menu_title = sys.argv[1], sys.argv[2], sys.argv[2]
            s.menu_blit = s.menu_large = s.menu_blur = ''
            s.t4 = str(main_root / 'vendor/OpenAssetTools/build/bin/Release_x86')
            s.t6 = str(main_root / 'vendor/OpenAssetToolsT6/build/bin/Release_x86')
            s.decoder = str(main_root / 'tools/bin/xaudio_wma_decoder.exe')
            s.source_fx = False
            s.resume = False
            builddriver.build(s, root=Path(out_arg).resolve())
        else:
            sys.argv = ['bench.py', sys.argv[1], sys.argv[2]]
            runpy.run_path('C:/WawConverter/work/perf/bench.py', run_name='__main__')
    except Done:
        pass


main()
