"""Bring source-owned weapon client libraries onto the T6 client runtime."""
from pathlib import Path
import warnings
from . import gsc


def translate_library(source: str, effects: dict[str, str], weapons: dict[str, str],
                      sound_names: set[str]) -> str:
    tokens = gsc.tokenize(source)
    for token in tokens:
        if token.kind == gsc.IDENT and token.low == 'realwait':
            token.text = 'waitrealtime'
        elif token.kind == gsc.STRING:
            name = token.text[1:-1]
            if name in effects:
                token.text = '"' + effects[name] + '"'
            elif name in weapons and weapons[name]:
                token.text = '"' + weapons[name] + '"'
            elif name in sound_names:
                token.text = '"waw/' + name + '"'
    text = gsc.emit(tokens)
    for name in ('_utility', '_fx', '_music'):
        text = text.replace('clientscripts\\' + name, 'clientscripts\\mp\\' + name)
    # A legacy clientNotify is a T6 client-system command, whereas source
    # libraries wait on a level notification. Forward only their single-arg
    # notification listeners; native notetracks retain their event payload.
    script = gsc.parse(text, '<source client library>')
    events = set()
    for i, token in enumerate(script.tokens[:-4]):
        if token.low == 'waittill' and i and script.tokens[i - 1].low == 'level' \
                and script.tokens[i + 1].text == '(' and script.tokens[i + 2].kind == gsc.STRING \
                and script.tokens[i + 3].text == ')':
            events.add(script.tokens[i + 2].text[1:-1])
    if events:
        init = script.functions['init']
        script.tokens[init.body_open].text += ''.join(
            f'\n    registersystem( "{name}", ::waw_notify_{i} );' for i, name in enumerate(sorted(events)))
        text = gsc.emit(script.tokens)
        text += ''.join(f'\nwaw_notify_{i}( localclientnum, state )\n{{\n    level notify( "{name}" );\n}}\n'
                        for i, name in enumerate(sorted(events)))
    return text


def stage_tesla(roots: list[Path], project: Path, effects: dict[str, str],
                weapon_table: dict[str, str], sound_names: set[str]) -> str:
    core = project / 'maps/mp/waw/_waw2bo2_core.gsc'
    if not core.is_file() or 'waw_tesla_init()' not in core.read_text(encoding='utf-8'):
        return ''
    source = next((root / 'clientscripts/_zombiemode_tesla.csc' for root in roots
                   if (root / 'clientscripts/_zombiemode_tesla.csc').is_file()), None)
    if source is None:
        warnings.warn('source Tesla gameplay library has no matching client library; client effects were not staged',
                      RuntimeWarning)
        return ''
    target = project / 'clientscripts/mp/waw/_zombiemode_tesla.csc'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(translate_library(source.read_text(encoding='utf-8'), effects, weapon_table, sound_names),
                      encoding='utf-8')
    return '    clientscripts\\mp\\waw\\_zombiemode_tesla::init();\n'


# ---- the map's own client scripts ----------------------------------------
#
# WaW starts a map's client side from clientscripts\<map>::main. Its modules
# (and the WaW client framework they use: _fx, _lights, _utility, ...) are
# ported under clientscripts\mp\waw\. BO2 keeps only what T6 itself requires:
# the client VM start (BO2's own _load), engine callbacks (_callbacks) and the
# local-client connect, where WaW's _callbacks::localclientconnect starts
# _fx::fx_init through _load's effects_init thread.

PORT_HEADER = '// Translated from World at War by waw2bo2 (clientport).'
CLIENT_PREFIX = 'clientscripts\\'
PORTED_PREFIX = 'clientscripts\\mp\\waw\\'
CLIENT_GLUE = 'clientscripts\\mp\\waw\\_waw2bo2_client'
ENVIRONMENT = 'clientscripts\\mp\\waw\\_waw2bo2_environment'
# WaW client modules the port does not run as the map's code: T6 glue. Calls
# to them in the map's main() are dropped (reported); other references keep
# BO2's module of the same name.
CLIENT_RUNTIME = {
    'clientscripts\\_load': "BO2's clientscripts\\mp\\zombies\\_load starts the client VM",
    'clientscripts\\_callbacks': "engine callbacks are answered by BO2's clientscripts\\mp\\_callbacks",
}


def staged_client_modules(map_name: str, project: str) -> dict[str, str]:
    """The map's client modules another staging step ports (stage_tesla,
    t6bridge.write_amb_csc on BO2's audio runtime): WaW module -> staged one."""
    return {'clientscripts\\_zombiemode_tesla': 'clientscripts\\mp\\waw\\_zombiemode_tesla',
            'clientscripts\\_audio': 'clientscripts\\mp\\_audio',
            f'clientscripts\\{map_name.lower()}_amb': f'clientscripts\\mp\\{project}_amb'}


# WaW light intensity is gamma; T6 primary light colours are staged linear.
# haseyes: WaW's engine zombie-eye state, which T6 actors do not carry.
CLIENT_METHODS = {'setlightintensity': 'waw_setlightintensity', 'getlightintensity': 'waw_getlightintensity',
                  'haseyes': 'waw_haseyes'}
# WaW client builtins T6's client VM lacks (compat _waw2bo2_environment.csc
# says what each becomes).
CLIENT_BUILTINS = {'setvolfog': 'waw_client_setvolfog', 'syncsystemstates': 'waw_syncsystemstates',
                   'updatedvarsfromprofile': 'waw_updatedvarsfromprofile', 'getsoundvolume': 'waw_getsoundvolume',
                   'setclientdvar': 'waw_setclientdvar', 'prof_begin': 'waw_prof', 'prof_end': 'waw_prof'}


def client_norm(path: str) -> str:
    return path.replace('/', '\\').lower().removesuffix('.csc')


def _ported(path: str) -> str:
    return PORTED_PREFIX + path[len(CLIENT_PREFIX):]


def _client_deps(script: gsc.Script) -> set[str]:
    deps = {client_norm(p) for p in script.includes}
    deps |= {client_norm(r.qualifier) for r in gsc.references(script.tokens) if r.qualifier}
    return {d for d in deps if d.startswith(CLIENT_PREFIX) and not d.startswith('clientscripts\\mp\\')}


def _statement(tokens: list, index: int) -> tuple[int, int]:
    start = index
    while start > 0 and tokens[start - 1].text not in (';', '{', '}'):
        start -= 1
    end = index
    while tokens[end].text != ';':
        end += 1
    return start, end


def port_map_client(sources, map_name: str, out_root: Path, staged: dict[str, str] | None = None,
                    builtins: set[str] | None = None) -> dict:
    """Translate clientscripts\\<map> and every client module it reaches into
    out_root/clientscripts/mp/waw. String literals keep their WaW names until
    finish_map_client (effects convert later). ``staged``: modules another step
    ports (staged_client_modules); the map main's calls to them are dropped
    since BO2's client main starts them. ``builtins``: T6 builtin names the map's
    function names must not take. Returns the port report, empty when
    the map has no client main."""
    main_path = CLIENT_PREFIX + map_name.lower()
    if sources.get(main_path, '.csc') is None:
        return {}
    report = {'entry': _ported(main_path), 'modules': [], 'events': [], 'fx': [], 'dropped': [],
              'missing': [], 'runtime': [], 'compat': [], 'renamed': []}
    staged = staged or {}
    skip = {*CLIENT_RUNTIME, *staged}
    order, queue, seen = [], [main_path], set()
    while queue:
        path = queue.pop(0)
        if path in seen:
            continue
        seen.add(path)
        script = sources.get(path, '.csc')
        if script is None:
            report['missing'].append(path)
            continue
        order.append(path)
        queue += sorted(d for d in _client_deps(script) if d not in skip and d not in seen)

    def target(path: str) -> str:
        path = client_norm(path)
        if not path.startswith(CLIENT_PREFIX) or path.startswith('clientscripts\\mp\\'):
            return path
        if path in staged:
            return staged[path]
        if path in CLIENT_RUNTIME or path in report['missing']:
            if path not in report['runtime']:
                report['runtime'].append(path)
            return 'clientscripts\\mp\\' + path[len(CLIENT_PREFIX):]
        return _ported(path)

    defined = {path: set(sources.get(path, '.csc').functions) for path in order}
    shadowed = {name for names in defined.values() for name in names} & set(builtins or ())
    events: set[str] = set()
    for path in order:
        script = sources.get(path, '.csc')
        tokens = gsc.tokenize(gsc.emit(script.tokens))
        refs = gsc.references(tokens)
        main = script.functions.get('main') if path == main_path else None
        statements = set()
        for ref in refs:
            if main is None or not main.body_open < ref.index < main.body_close or ref.pointer:
                continue
            qualifier = client_norm(ref.qualifier) if ref.qualifier else None
            if qualifier in skip or (qualifier is None and ref.name.lower() == 'waitforclient'):
                # BO2's client main runs these: its _load, the separately
                # staged tesla/amb libraries and the wait for client 0
                statements.add(_statement(tokens, ref.qual_index if ref.qual_index is not None else ref.index))
        for start, end in sorted(statements):
            dropped = ' '.join(t.text for t in tokens[start:end + 1])
            report['dropped'].append(dropped)
            tokens[start].pre += f'// waw2bo2: the BO2 client main or a separate port runs this: {dropped}\n\t'
            for t in tokens[start:end + 1]:
                t.text = ''
        # A WaW function named like a T6 builtin (_utility's vectorScale): the
        # T6 compiler rejects the definition, so it and its callers take the
        # waw_ name, as gscport.shadowed_name does for server scripts.
        visible = defined[path].union(*(defined.get(client_norm(i), set()) for i in script.includes))
        for ref in refs:
            t = tokens[ref.index]
            low = ref.name.lower()
            owner = defined.get(client_norm(ref.qualifier), set()) if ref.qualifier else visible
            if t.text and low in shadowed and low in owner and not ref.method:
                t.text = 'waw_' + t.text
                if low not in report['renamed']:
                    report['renamed'].append(low)
        for ref in refs:
            t = tokens[ref.index]
            if not t.text or ref.qualifier is not None or ref.pointer or t.text.startswith('waw_'):
                continue
            low = ref.name.lower()
            if ref.method and low in CLIENT_METHODS:
                t.text = f'{ENVIRONMENT}::{CLIENT_METHODS[low]}'
            elif not ref.method and low in CLIENT_BUILTINS and low not in script.functions:
                t.text = f'{ENVIRONMENT}::{CLIENT_BUILTINS[low]}'
            else:
                continue
            if low not in report['compat']:
                report['compat'].append(low)
        for i, t in enumerate(tokens):
            if t.kind == gsc.PATH and client_norm(t.text).startswith(CLIENT_PREFIX):
                t.text = target(t.text)
            elif t.kind == gsc.DIRECTIVE:
                m = gsc.INCLUDE_RE.match(t.text)
                if m:
                    t.text = f'#include {target(m.group(1))};'
            elif t.low == 'loadfx' and tokens[i + 1].text == '(' and tokens[i + 2].kind == gsc.STRING:
                report['fx'].append(tokens[i + 2].text[1:-1])
            elif t.low == 'waittill' and i > 1 and tokens[i - 1].low == 'level' and tokens[i + 1].text == '(' \
                    and tokens[i + 2].kind == gsc.STRING:
                events.add(tokens[i + 2].text[1:-1])
        dest = out_root / (_ported(path).replace('\\', '/') + '.csc')
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(f'{PORT_HEADER} Source: {path}.csc\n'
                        + gsc.emit(tokens), encoding='utf-8')
        report['modules'].append(_ported(path))
    report['events'] = sorted(events)
    report['fx'] = sorted(set(report['fx']))
    return report


CLIENT_GLUE_SOURCE = PORT_HEADER + ''' T6 glue around the map's own client scripts.
#include clientscripts\\mp\\_utility;

init()
{
    // A server clientnotify reaches the T6 client as the state change of a
    // client system named after the message; WaW's _load answered it through
    // its levelNotify system with a level notify carrying the local client.
@FORWARDS@    @ENTRY@::main();
    // BO2's connect path (zombies\\_players::on_connect) runs its own
    // _fx::fx_init before level.onplayerconnect: its light mixer would take
    // the map's lights and its createfx playback would double the map's
    // effects. Both are handed to the map's own _fx at the same connect.
    level.waw_createfxent = level.createfxent;
    level.createfxent = undefined;
    // The map's light entities (classname "light", "pl#" -> .pl), collected
    // as WaW's _utility_code::struct_class_init indexes them by classname.
    level.waw_light_structs = [];
    for ( i = 0; i < level.struct.size; i++ )
    {
        if ( isdefined( level.struct[i].classname ) && level.struct[i].classname == "light" )
            level.waw_light_structs[level.waw_light_structs.size] = level.struct[i];
    }
    if ( isdefined( level.struct_class_names ) )
        level.struct_class_names["classname"]["light"] = undefined;
    level.waw_client_connect = level.onplayerconnect;
    level.onplayerconnect = ::on_connect;
}

// What clientscripts\\mp\\_utility::registersystem stores, without its
// script-side limit of 32 systems (BO2's own systems already use many).
forward_notify( name, callback )
{
    if ( !isdefined( level._systemstates ) )
        level._systemstates = [];
    if ( !isdefined( level._systemstates[name] ) )
        level._systemstates[name] = spawnstruct();
    level._systemstates[name].callback = callback;
}

on_connect( localclientnum )
{
    if ( isdefined( level.waw_client_connect ) )
        level thread [[ level.waw_client_connect ]]( localclientnum );
    if ( isdefined( level.waw_createfxent ) )
        level.createfxent = level.waw_createfxent;
    // No allocatemixerlights: BO2's mixer then owns every light's colour, and
    // WaW's lights are never made mixer lights (they drew with colour 0).
    if ( level.waw_light_structs.size )
        level.struct_class_names["classname"]["light"] = level.waw_light_structs;
    // WaW _callbacks::localclientconnect -> _load effects_init -> _fx::fx_init
@CONNECT@}
@HANDLERS@'''


LOOKUPS = {'getstruct', 'getstructarray', 'getent', 'getentarray'}


def _lookup_key(tokens: list, index: int) -> bool:
    """The string at ``index`` is an argument of a struct/entity lookup: a key
    such as the classname "light", never an asset name (Der Riese ships a
    sound alias called "light")."""
    depth = 0
    for j in range(index - 1, -1, -1):
        text = tokens[j].text
        if text in (')', ']'):
            depth += 1
        elif text in ('(', '['):
            if depth == 0:
                return text == '(' and j > 0 and tokens[j - 1].low in LOOKUPS
            depth -= 1
        elif text in (';', '{', '}'):
            return False
    return False


def finish_map_client(project_root: Path, report: dict, effects: dict[str, str], weapons: dict[str, str],
                      sound_names: set[str]) -> str:
    """Bind the ported client modules' literals to converted assets and write
    the connect glue. Returns the line the BO2 client main calls; the map's
    effects no converted asset answers land in report['unbound_fx']."""
    report['unbound_fx'] = sorted(set(report.get('fx', [])) - set(effects))
    for module in report.get('modules', []):
        dest = project_root / (module.replace('\\', '/') + '.csc')
        tokens = gsc.tokenize(dest.read_text(encoding='utf-8'))
        for i, token in enumerate(tokens):
            if token.kind == gsc.STRING and token.text.startswith('"') and not _lookup_key(tokens, i):
                name = token.text[1:-1]
                if name in effects:
                    token.text = '"' + effects[name] + '"'
                elif name in weapons and weapons[name]:
                    token.text = '"' + weapons[name] + '"'
                elif name in sound_names:
                    token.text = '"waw/' + name + '"'
        dest.write_text(gsc.emit(tokens), encoding='utf-8')
    events = report.get('events', [])
    fx_module = PORTED_PREFIX + '_fx'
    glue = (CLIENT_GLUE_SOURCE
            .replace('@FORWARDS@', ''.join(f'    forward_notify( "{name}", ::waw_notify_{i} );\n'
                                           for i, name in enumerate(events)))
            .replace('@ENTRY@', report['entry'])
            .replace('@CONNECT@', f'    {fx_module}::fx_init( localclientnum );\n'
                     if fx_module in report.get('modules', []) else '')
            .replace('@HANDLERS@', ''.join(f'\nwaw_notify_{i}( localclientnum, state )\n{{\n'
                                           f'    level notify( "{name}", localclientnum );\n}}\n'
                                           for i, name in enumerate(events))))
    dest = project_root / (CLIENT_GLUE.replace('\\', '/') + '.csc')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(glue, encoding='utf-8')
    return f'    {CLIENT_GLUE}::init();\n'
