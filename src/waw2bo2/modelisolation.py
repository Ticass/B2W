"""Keep source map models distinct from same-named BO2 engine assets."""
import json
from pathlib import Path

from . import gsc

PREFIX = 'waw_world_model/'


def stage(project: Path) -> dict[str, str]:
    renamed = {}
    folder = project/'xmodel'
    for path in list(folder.rglob('*.json')):
        name = path.relative_to(folder).as_posix().removesuffix('.json')
        if name.startswith((PREFIX, 'waw_xmodel/', 'waw_fx_model/')):
            continue
        output = PREFIX + name
        target = folder/f'{output}.json'
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(path.read_bytes())
        renamed[name] = output
    if not renamed:
        return renamed

    # Render placements, collision placements, map entities and dynamic pieces
    # must all name the same source model. Brush model '*N' names are untouched.
    def references(value, manifest):
        if isinstance(value, dict):
            for key, item in value.items():
                if isinstance(item, str) and (key in ('model', 'xModel', 'skyboxmodel', 'modelName')
                        or key == 'name' and manifest in ('models.json', 'staticmodels.json')):
                    value[key] = renamed.get(item, item)
                else:
                    references(item, manifest)
        elif isinstance(value, list):
            for item in value:
                references(item, manifest)
    for path in (project/'BSP').glob('*.json'):
        data = json.loads(path.read_text())
        references(data, path.name)
        path.write_text(json.dumps(data))
    # Replace source values, including array entries and model comparisons.
    # Dynamic names additionally resolve through the model builtin adapters.
    for folder_name in ('maps', 'clientscripts'):
        for path in (project/folder_name).rglob('*'):
            if path.suffix not in ('.gsc', '.csc'):
                continue
            tokens = gsc.tokenize(path.read_text(encoding='utf-8'))
            changed = False
            for token in tokens:
                if token.kind == gsc.STRING and token.text[1:-1] in renamed:
                    token.text = '"' + renamed[token.text[1:-1]] + '"'
                    changed = True
            if changed:
                path.write_text(gsc.emit(tokens), encoding='utf-8')
    for path in (project/'fx').rglob('*.efx'):
        text = path.read_text()
        for old, new in renamed.items():
            text = text.replace('"'+old+'"', '"'+new+'"')
        path.write_text(text)
    table = project/'maps/mp/waw/_waw2bo2_models.gsc'
    table.parent.mkdir(parents=True, exist_ok=True)
    lines = ['// Source model identities; engine assets keep their own names.', 'table()', '{', '    models = [];']
    for old, new in sorted(renamed.items()):
        # Split keys so later literal identity rewrites cannot change them.
        lines.append(f'    models["{old[:1]}" + "{old[1:]}"] = "{new}";')
    table.write_text('\n'.join(lines+['    return models;', '}', '']))
    return renamed
