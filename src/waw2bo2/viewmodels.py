"""Keep first-person weapon surfaces out of T6's shadow-caster pass."""
import copy
import json
import re
from pathlib import Path
from . import native, weapons, techsets


def stage_shadow_variants(content: Path) -> dict:
    if native.tool() is not None:
        return json.loads(native.run('stage-viewmodel-shadows', str(content)))
    return _stage_shadow_variants_python(content)


def _stage_shadow_variants_python(content: Path) -> dict:
    stage_path = content / 'weapons.stage.json'
    visual_path = content / 'weapons.visuals.json'
    stage = json.loads(stage_path.read_text())
    visual = json.loads(visual_path.read_text())
    models = {entry['name']: entry for entry in stage['models']['models']}
    materials = {entry['name']: entry for entry in visual['materials']}
    replacements = {}
    material_clones = {}
    for entry in stage['weapons']:
        path = content / 'weapons' / entry['name']
        if not path.is_file():
            continue
        fields = weapons.read_info_file(path)
        changed = False
        for key, name in list(fields.items()):
            if name.startswith(weapons.output_name('xmodel', 'viewmodel_shadowless/')):
                continue
            if not re.fullmatch(r'gunModel(?:[2-9]|1[0-6])?', key) or name not in models:
                continue
            if name not in replacements:
                original = models[name]
                original_materials = [weapons.output_name('material', m) for m in original['materials']]
                if any(m not in materials or not (content / techsets.oat_material_path(m)).is_file()
                       for m in original_materials):
                    # Preserve the normal missing-material exclusion/report.
                    continue
                if not any('CASTS_SHADOW' in json.loads((content / techsets.oat_material_path(m)).read_text()).get('gameFlags', [])
                           for m in original_materials):
                    continue
                model = json.loads((content / f'xmodel/{name}.json').read_text())
                clone_key = 'viewmodel_shadowless/' + original['waw']
                clone_name = weapons.output_name('xmodel', clone_key)
                cloned_materials = set()
                for index, lod in enumerate(model['lods']):
                    geometry = json.loads((content / lod['file']).read_text())
                    for material in geometry.get('materials', []):
                        old = material['name']
                        definition = json.loads((content / techsets.oat_material_path(old)).read_text())
                        if 'CASTS_SHADOW' not in definition.get('gameFlags', []):
                            cloned_materials.add(old.removeprefix(weapons.PREFIXES['material']))
                            continue
                        new = weapons.output_name('material', 'viewmodel_shadowless/' +
                                                  old.removeprefix(weapons.PREFIXES['material']))
                        if new not in material_clones:
                            definition['gameFlags'].remove('CASTS_SHADOW')
                            target = content / techsets.oat_material_path(new)
                            target.parent.mkdir(parents=True, exist_ok=True)
                            target.write_text(json.dumps(definition, indent=2) + '\n')
                            metadata = copy.deepcopy(materials[old])
                            metadata.update(name=new, file=techsets.oat_material_path(new).as_posix())
                            metadata.setdefault('notes', []).append('first-person surface: no self-shadow casting')
                            visual['materials'].append(metadata)
                            visual['material_images'][new] = visual['material_images'].get(old, [])
                            material_clones[new] = old
                        material['name'] = new
                        cloned_materials.add(new.removeprefix(weapons.PREFIXES['material']))
                    relative = f'model_export/{clone_name}_lod{index}.gltf'
                    target = content / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(json.dumps(geometry, separators=(',', ':')))
                    lod['file'] = relative
                target = content / f'xmodel/{clone_name}.json'
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(json.dumps(model, indent=2) + '\n')
                stage['models']['models'].append(dict(original, name=clone_name, waw=clone_key,
                                                     materials=sorted(cloned_materials)))
                replacements[name] = clone_name
            fields[key] = replacements[name]
            changed = True
        if changed:
            # read_info_file maps code-page bytes it cannot decode to U+FFFD;
            # the locale code page (cp1252) cannot write that back.
            path.write_text(weapons.write_info(fields), encoding='utf-8')
    stage['viewmodel_shadow_variants'] = replacements
    visual['viewmodel_shadow_materials'] = material_clones
    stage_path.write_text(json.dumps(stage, indent=2) + '\n')
    visual_path.write_text(json.dumps(visual, indent=2) + '\n')
    return {'models': replacements, 'materials': material_clones}
