"""Keep source map models distinct from same-named BO2 engine assets."""
import json
from pathlib import Path

from . import gsc

PREFIX = 'waw_world_model/'

# A model and a bone can have the same name. These arguments always name
# bones, even when a source xmodel happens to share their literal value.
TAG_ARGUMENTS = {
    'gettagorigin': {0}, 'gettagangles': {0}, 'hidepart': {0}, 'showpart': {0},
    'attach': {1}, 'detach': {1}, 'linkto': {1}, 'linktoblendtotag': {1},
    'playfxontag': {2}, 'waw_gettagorigin': {0}, 'waw_gettagangles': {0},
    'waw_attach': {1}, 'waw_detach': {1}, 'waw_linkto': {1},
    'waw_playfxontag': {2},
    # client: playviewmodelfx( localclientnum, fx, tag ) (the Wunderwaffe's
    # rail and tube effects on its viewmodel's tag_flash / tag_brass)
    'playviewmodelfx': {2},
}


def rewrite_script(source: str, renamed: dict[str, str]) -> str:
    tokens = gsc.tokenize(source)
    tags = set()
    for ref in gsc.references(tokens):
        if ref.pointer or ref.name.lower() not in TAG_ARGUMENTS:
            continue
        wanted = TAG_ARGUMENTS[ref.name.lower()]
        argument, depth = 0, 0
        for i in range(ref.index + 2, len(tokens)):
            value = tokens[i].text
            if value == ')' and depth == 0:
                break
            if value == ',' and depth == 0:
                argument += 1
            elif value in ('(', '[', '{'):
                depth += 1
            elif value in (')', ']', '}'):
                depth -= 1
            if argument in wanted:
                tags.add(i)
    # A bone name held in a variable first (tag = "tag_origin"; then
    # playfxontag( fx, ent, tag )) is a bone too: renaming it to the model's
    # isolated name made the power-ups' glow carrier play on a missing tag.
    tag_variables = {tokens[i].text.lower() for i in tags
                     if tokens[i].kind == gsc.IDENT and tokens[i - 1].text in ('(', ',')
                     and i + 1 < len(tokens) and tokens[i + 1].text in (')', ',')}
    for i in range(2, len(tokens)):
        if (tokens[i].kind == gsc.STRING and tokens[i - 1].text == '=' and tokens[i - 2].kind == gsc.IDENT
                and tokens[i - 2].text.lower() in tag_variables
                and (i < 3 or tokens[i - 3].text not in ('.', ']'))):
            tags.add(i)
    for i, token in enumerate(tokens):
        if i not in tags and token.kind == gsc.STRING and token.text[1:-1] in renamed:
            token.text = '"' + renamed[token.text[1:-1]] + '"'
    return gsc.emit(tokens)


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
            source = path.read_text(encoding='utf-8')
            rewritten = rewrite_script(source, renamed)
            if source != rewritten:
                path.write_text(rewritten, encoding='utf-8')
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
