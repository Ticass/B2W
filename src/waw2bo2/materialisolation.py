"""Keep converted world/model materials out of cached BO2 asset identities."""
import json
import shutil
from pathlib import Path
from tempfile import TemporaryDirectory

from . import gsc, native, techsets


def stage(report, project: Path, stock: Path):
    if native.tool() is not None:
        fields = {'project': str(project), 'stock': str(stock),
                  'materials': [{k: entry[k] for k in ('name', 'file')} for entry in report.materials],
                  'hud': (getattr(report, 'scripts', None) or {}).get('hud_materials', [])}
        with TemporaryDirectory(prefix='waw2bo2_material_names_') as folder:
            request = Path(folder) / 'request.json'
            request.write_text(json.dumps(fields), encoding='utf-8')
            result = json.loads(native.run('isolate-material-names', str(request)))
        for update in result['updates']:
            report.materials[update.pop('index')].update(update)
        renamed = result['renamed']
        if renamed:
            report.warnings.append(f'MATERIAL_IDENTITIES_ISOLATED {len(renamed)} converted materials shared stock BO2 names')
        return renamed
    renamed = {}
    for entry in report.materials:
        name = entry['name']
        if name.startswith(('waw/', 'waw_world/', 'waw_material/', 'waw_fx/')):
            continue
        relative = techsets.oat_material_path(name)
        if not (stock / relative).is_file():
            continue
        source = project / entry['file']
        if not source.exists():
            continue
        data = json.loads(source.read_text(encoding='utf-8'))
        if not data.get('techniqueSet', '').startswith('waw/'):
            continue # explicitly native/fallback materials retain native identity
        output = 'waw_world/' + name.replace('*', '_')
        target = project / techsets.oat_material_path(output)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        entry.update(name=output, file=target.relative_to(project).as_posix(), source_name=name)
        renamed[name] = output
    if not renamed:
        return renamed
    for filename, field in (('lightmaps.json', 'wawMaterials'), ('constraints.json', 'constraints')):
        manifest = project / 'BSP' / filename
        if not manifest.is_file():
            continue
        data = json.loads(manifest.read_text())
        if filename == 'lightmaps.json':
            data[field] = [renamed.get(name, name) for name in data[field]]
        else:
            for constraint in data[field]:
                constraint['material'] = renamed.get(constraint['material'], constraint['material'])
        manifest.write_text(json.dumps(data, indent=2) + '\n')
    rewrite_hud(project, renamed, (getattr(report, 'scripts', None) or {}).get('hud_materials', []))
    for path in (project / 'model_export').rglob('*.gltf'):
        data = json.loads(path.read_text(encoding='utf-8'))
        changed = False
        for material in data.get('materials', []):
            if material.get('name') in renamed:
                material['name'] = renamed[material['name']]
                changed = True
        if changed:
            # A staged LOD can share its source through a cache hard link.
            path.unlink()
            path.write_text(json.dumps(data), encoding='utf-8')
    for path in (project / 'BSP').glob('*.fbx'):
        if path.name != 'map_gfx.fbx':
            continue
        text = path.read_text(encoding='utf-8')
        for old, new in renamed.items():
            text = text.replace('Material::' + old + '"', 'Material::' + new + '"')
        path.write_text(text, encoding='utf-8')
    for path in (project / 'fx').rglob('*.efx'):
        text = path.read_text(encoding='utf-8')
        for old, new in renamed.items():
            text = text.replace('"' + old + '"', '"' + new + '"')
        path.write_text(text, encoding='utf-8')
    report.warnings.append(f'MATERIAL_IDENTITIES_ISOLATED {len(renamed)} converted materials shared stock BO2 names')
    return renamed


def rewrite_hud(project, renamed, materials):
    hud = set(materials) & renamed.keys()
    if not hud:
        return
    # names scripts build at run time resolve through this table
    from .gscport import hud_material_table_source
    table = project / 'maps/mp/waw/_waw2bo2_hud_materials.gsc'
    table.parent.mkdir(parents=True, exist_ok=True)
    table.write_text(hud_material_table_source({name: renamed[name] for name in hud}), encoding='utf-8')
    for folder in ('maps', 'clientscripts'):
        for path in (project / folder).rglob('*'):
            if path.suffix not in ('.gsc', '.csc'):
                continue
            tokens = gsc.tokenize(path.read_text(encoding='utf-8'))
            changed = False
            for token in tokens:
                if token.kind == gsc.STRING and token.text[1:-1] in hud:
                    token.text = '"' + renamed[token.text[1:-1]] + '"'
                    changed = True
            if changed:
                path.write_text(gsc.emit(tokens), encoding='utf-8')
