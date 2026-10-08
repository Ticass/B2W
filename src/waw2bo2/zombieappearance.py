"""Preserve each source spawner's WaW character selection on T6 actors."""
from pathlib import Path
import re

from . import gsc


def actor_name(source: str) -> str:
    return 'waw_' + source


def appearance_script(source: str) -> str:
    """Keep source character logic; the T6 actor owns engine AI settings."""
    script = gsc.parse(source, '<WaW actor>')
    tokens = script.tokens
    for name in ('main', 'precache'):
        fn = script.functions.get(name)
        if fn is None:
            raise ValueError(f'WaW actor has no {name}()')
        start = fn.body_open + 1
        depth = 0
        for i in range(start, fn.body_close):
            depth += tokens[i].text in ('(', '[', '{')
            depth -= tokens[i].text in (')', ']', '}')
            if tokens[i].text != ';' or depth:
                continue
            stmt = tokens[start:i + 1]
            words = [t.low for t in stmt]
            # Authored character selection (including switch blocks) survives.
            engine_assignment = len(words) > 3 and words[:2] == ['self', '.'] and words[3] == '=' and words[2] in {
                'animtree', 'animstatedef', 'team', 'type', 'accuracy', 'health', 'weapon',
                'secondaryweapon', 'sidearm', 'grenadeweapon', 'grenadeammo', 'subclass'}
            engine_call = len(words) > 1 and words[0] == 'self' and words[1] in (
                'setengagementmindist', 'setengagementmaxdist')
            item_precache = words and words[0] == 'precacheitem'
            if engine_assignment or engine_call or item_precache:
                for token in stmt:
                    token.text = token.pre = ''
            start = i + 1
    return gsc.emit(tokens)


def native_actor(template: str, source_path: str) -> str:
    """Retain T6 animation/state registration, replace donor appearance."""
    script = gsc.parse(template, '<T6 actor>')
    tokens = script.tokens
    main = script.functions['main']
    refs = list(gsc.references(tokens, main.body_open, main.body_close))
    random = next(r for r in refs if r.name.lower() == 'get_random_character')
    start = random.qual_index
    while start > main.body_open and tokens[start - 1].text not in (';', '{'):
        start -= 1
    tokens[start].text = f'{source_path}::main();\n    self setcharacterindex( 0 );'
    for token in tokens[start + 1:main.body_close]:
        token.text = token.pre = ''
    # Character includes and precaches are donor content too.
    for token in tokens:
        if token.kind == gsc.DIRECTIVE and re.match(r'#include\s+character\\', token.text, re.I):
            token.text = ''
    fn = script.functions['precache']
    for ref in gsc.references(tokens, fn.body_open, fn.body_close):
        if ref.qualifier and ref.qualifier.lower().startswith('character\\'):
            end = ref.index
            while tokens[end].text != ';':
                end += 1
            for token in tokens[ref.qual_index:end + 1]:
                token.text = token.pre = ''
    tokens[fn.body_close].text = f'    {source_path}::precache();\n}}'
    return '// waw2bo2: source WaW zombie appearance, T6 actor state support.\n' + gsc.emit(tokens)


def native_client_actor(name: str) -> str:
    # Appearance is replicated from the server's source character scripts.
    # Register the same T6 animation state on both sides without donor skins.
    return f'''// waw2bo2: replicated WaW appearance, T6 client AI state support.
#using_animtree("zm_nuked_basic");
main()
{{
    self._aitype = "{actor_name(name)}";
}}
precache( ai_index )
{{
    usefootsteptable( ai_index, "default_ai" );
    precacheanimstatedef( ai_index, #animtree, "zm_nuked_basic" );
    setdemolockonvalues( ai_index, 100, 60, -15, 60, 30, -5, 60 );
}}
'''


def source_actor_types(project_root: Path) -> set[str]:
    import json
    path = project_root / 'BSP/entities.json'
    if not path.exists():
        return set()
    names = set()
    for ent in json.loads(path.read_text())['entities']:
        if 'waw_actor_type' in ent:
            names.add(ent['waw_actor_type'])
        elif ent.get('classname', '').startswith('actor_waw_'):
            names.add(ent['classname'].removeprefix('actor_waw_'))
    return names


def bind_actor_types(project_root: Path, names: set[str]) -> None:
    import json
    path = project_root / 'BSP/entities.json'
    data = json.loads(path.read_text())
    for ent in data['entities']:
        name = ent.pop('waw_actor_type', None)
        if name in names:
            ent['classname'] = 'actor_' + actor_name(name)
    path.write_text(json.dumps(data, indent=1), encoding='utf-8')
