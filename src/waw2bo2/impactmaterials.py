"""Keep the source map's impact materials on native engine-created marks."""
from __future__ import annotations

import json
import shutil
from pathlib import Path


def source_names(roots: list[Path]) -> set[str]:
    return {p.relative_to(root/'materials').as_posix().removesuffix('.json')
            for root in roots for family in ('mc', 'wc')
            for p in (root/'materials'/family).glob('gfx_impact_*.json')}


def stage(project: Path, roots: list[Path], stock: Path, source_stock=None, wavelets=None) -> dict:
    from . import assetresolve, t6bridge
    names = source_names(roots)
    if not names:
        return {}
    resolver = assetresolve.Resolver(roots, source_stock)
    closure = resolver.expand({('material', name) for name in names})
    output = project/'content_source/impact_materials'
    output.mkdir(parents=True, exist_ok=True)
    report = t6bridge.StageReport('source_impact_materials')
    t6bridge.stage_materials(report, names, resolver.roots, stock/'materials', output, stock)
    t6bridge.stage_images(report, [r/'images' for r in resolver.roots], output, wavelets=wavelets)
    if report.errors:
        raise ValueError('source impact materials: ' + '; '.join(report.errors[:8]))
    data = {'materials': report.materials, 'images': report.images}
    (output/'stage.json').write_text(json.dumps(data, indent=1), encoding='utf-8')
    return {'materials': len(report.materials), 'report': 'content_source/impact_materials/stage.json',
            'missing_dependencies': closure['missing']}


def bind(project: Path, dump: Path, overlay: Path, stock: Path) -> dict[str, str]:
    """Route matching stock mark identities to their original WaW materials.

    T6's metal05_blend identity is the engine's blend variant of metal05.
    Both rigid-model and world variants keep their own original programs.
    """
    root = project/'content_source/impact_materials'
    manifest = root/'stage.json'
    if not manifest.exists():
        return {}
    entries = {m.get('source', m['name']): m for m in json.loads(manifest.read_text())['materials']
               if m.get('shader_runtime', {}).get('active')}
    candidates = {p.relative_to(r/'materials').as_posix().removesuffix('.json')
                  for r in (dump, stock) for family in ('mc', 'wc')
                  for p in (r/'materials'/family).glob('gfx_impact_*.json')}
    changed = {}
    for name in sorted(candidates):
        original = name if name in entries else name.removesuffix('_blend')
        if original not in entries:
            continue
        material = json.loads((root/entries[original]['file']).read_text())
        relative = 'materials/'+name+'.json'
        target = overlay/relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(material), encoding='utf-8')
        for texture in material.get('textures', []):
            image = texture.get('image', '')
            if not image or image.startswith((',', '$')):
                continue
            source = root/'images'/f'{image}.iwi'
            destination = overlay/'images'/f'{image}.iwi'
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        changed[relative] = original
    if changed:
        for folder in ('techniquesets', 'shader_bin'):
            shutil.copytree(root/folder, overlay/folder, dirs_exist_ok=True)
    return changed
