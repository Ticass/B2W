"""WaW speed selection and plain spawner locations on the BO2 AI runtime."""
from pathlib import Path
import re
import shutil
import copy
from . import gsc

RUNTIME = r'maps\mp\waw\_waw2bo2_zombie_behavior'
# The converted zombie plays WaW AnimScripted clips through this animation
# state (one substate per clip of its WaW animtree, SCRIPTED_TREE).
SCRIPTED_STATE = 'waw_scripted'
SCRIPTED_NOTIFY = 'waw_scripted_anim'
# substates per state (T6's BG_AnimStateDef_Parse stops at 64)
SCRIPTED_CHUNK = 60
SCRIPTED_TREE = 'waw2bo2_generic_human'
ACTOR_TREE = 'waw2bo2_zombie_actor'
SELECTION = r'maps\_waw2bo2_source_spawns'
SPEED_STATE = r'maps\_waw2bo2_source_speed_state'
CHASE_POLICY = r'maps\_waw2bo2_source_chase_policy'


def _rise_result(script, fn, event: str) -> str | None:
    """Find the local receiving the shared spawn-completion event payload."""
    results = set()
    for i in range(fn.body_open + 1, fn.body_close - 5):
        tokens = script.tokens[i:i + 6]
        if ([t.low for t in tokens[:3]] == ['self', 'waittill', '('] and
                tokens[3].text.lower() == '"' + event.lower() + '"' and
                tokens[4].text == ',' and tokens[5].kind == gsc.IDENT):
            results.add(tokens[5].text)
    if len(results) > 1:
        raise ValueError(f'{script.path}::{fn.name}: ambiguous {event} event result')
    return next(iter(results), None)


def source_chase_policy(sources, definition: dict) -> str | None:
    """Retain the source's ordered choice between forced goals and chasing."""
    from .gscport import top_level_statements
    contract = definition['source_chase']
    source = sources.get(contract['script'])
    fn = source.functions.get(contract['function']) if source else None
    if fn is None:
        return None
    tokens = source.tokens
    for a, b in top_level_statements(tokens, fn.body_open, fn.body_close):
        if tokens[a].low != 'if':
            continue
        branches, i, found = [], a, False
        while i < b:
            header_start = i
            if tokens[i].low == 'else':
                i += 1
            if tokens[i].low == 'if':
                if tokens[i + 1].text != '(':
                    break
                i = gsc._match(tokens, i + 1, '(', ')') + 1
            if tokens[i].text != '{':
                break
            close = gsc._match(tokens, i, '{', '}')
            refs = list(gsc.references(tokens, i + 1, close))
            chase = any(r.name.lower() == contract['chase_function'] for r in refs) and any(
                r.name.lower() == contract['setup_function'] for r in refs)
            found |= chase
            branches.append(gsc.emit(tokens[header_start:i]) + '{ return ' + ('true' if chase else 'false') + '; }')
            i = close + 1
            if i >= b or tokens[i].low != 'else':
                break
        if found:
            result = _rise_result(source, fn, contract['rise_event']) or 'rise_result'
            return '\n'.join(['// Source zombie_think chase/entrance decision.',
                              f'should_chase( {result} )', '{', *branches, 'return false;', '}'])
    return None


def source_speed_state(sources, definition: dict, rounds: bool = True) -> str | None:
    """Keep initialization and round inputs read by the actual source selector.

    Round-spawn helpers may initialize speed bounds as a side effect of choosing
    the round population. Preserve those calls before choosing a zombie's speed,
    without starting a second WaW spawning loop on the native AI runtime.
    With rounds=False the map's own round loop and spawning run and update the
    state themselves: only the selector script's initial values are kept.
    """
    from .gscport import top_level_statements
    contract = definition['source_speed']
    round_contract = definition['source_round_state']
    def owner(script, ref):
        if ref.qualifier:
            return sources.get(ref.qualifier)
        if ref.name.lower() in script.functions:
            return script
        return next((dep for path in script.includes if (dep := sources.get(path)) and
                     ref.name.lower() in dep.functions), None)
    def fields(script, name, seen=None):
        seen = set() if seen is None else seen
        key = (script.path, name)
        if key in seen or name not in script.functions:
            return set(), set()
        seen.add(key)
        fn = script.functions[name]
        tokens = script.tokens
        reads, writes = set(), set()
        for i in range(fn.body_open + 1, fn.body_close - 3):
            if [t.low for t in tokens[i:i + 2]] == ['level', '.']:
                field = tokens[i + 2].low
                if tokens[i + 3].text in ('=', '+=', '-=', '++', '--'):
                    writes.add(field)
                if tokens[i + 3].text != '=':
                    reads.add(field)
        for ref in gsc.references(tokens, fn.body_open + 1, fn.body_close):
            dep = owner(script, ref)
            if dep:
                r, w = fields(dep, ref.name.lower(), seen)
                reads |= r
                writes |= w
        return reads, writes
    speed = sources.get(contract['script'])
    if not speed or contract['function'] not in speed.functions:
        return None
    reads, _ = fields(speed, contract['function'])
    reads -= {'round_number', 'zombie_vars'}
    initial, start, end = [], [], []
    initializer = speed.functions.get('init')
    if initializer:
        for a, b in top_level_statements(speed.tokens, initializer.body_open, initializer.body_close):
            stmt = speed.tokens[a:b]
            if len(stmt) > 3 and [t.low for t in stmt[:2]] == ['level', '.'] and stmt[2].low in reads:
                initial.append(gsc.emit(stmt))
    framework = sources.get(round_contract['script']) if rounds else None
    begin = framework.functions.get(round_contract['start_function']) if framework else None
    if begin:
        for a, b in top_level_statements(framework.tokens, begin.body_open, begin.body_close):
            stmt = framework.tokens[a:b]
            if stmt[0].low == 'while':
                break
            if stmt[0].low in ('for', 'foreach'):
                continue
            direct = len(stmt) > 3 and [t.low for t in stmt[:2]] == ['level', '.'] and stmt[2].low in reads
            writers = [ref for ref in gsc.references(stmt) if (dep := owner(framework, ref)) and
                       fields(dep, ref.name.lower())[1] & reads]
            if direct or writers:
                start.append(gsc.emit(stmt))
    finish = framework.functions.get(round_contract['end_function']) if framework else None
    if finish:
        tokens = framework.tokens
        # End-of-round scalar updates belong to the previous source round.
        # A conditional update requires a control-flow slice, not a guessed rate.
        depth = 0
        guards = []
        for i in range(finish.body_open + 1, finish.body_close - 3):
            depth += tokens[i].text == '{'
            depth -= tokens[i].text == '}'
            if tokens[i].text == '{':
                header = None
                if tokens[i - 1].text == ')':
                    nesting, opening = 1, i - 2
                    while opening > finish.body_open and nesting:
                        nesting += tokens[opening].text == ')'
                        nesting -= tokens[opening].text == '('
                        opening -= 1
                    if tokens[opening].low == 'if':
                        header = copy.deepcopy(tokens[opening:i])
                guards.append(header)
            elif tokens[i].text == '}' and guards:
                guards.pop()
            if [t.low for t in tokens[i:i + 2]] != ['level', '.'] or tokens[i + 2].low not in reads:
                continue
            if tokens[i + 3].text not in ('=', '+=', '-=', '++', '--'):
                continue
            boundary = max((j for j in range(finish.body_open, i) if tokens[j].text in (';', '{', '}')),
                           default=finish.body_open)
            prefix = [t.low for t in tokens[boundary + 1:i]]
            if (any(word in ('if', 'else', 'for', 'switch', 'case') for word in prefix)
                    or (depth > 1 and any(header is None for header in guards[1:]))):
                raise ValueError(f'{framework.path}::{finish.name}: conditional source speed state needs a control-flow slice')
            j = next(j for j in range(i, finish.body_close) if tokens[j].text == ';')
            stmt = copy.deepcopy(tokens[i:j + 1])
            if depth == 0:
                initial.append(gsc.emit(stmt))
                continue
            headers = [header for header in guards if header is not None]
            if headers:
                guarded = []
                for header in headers:
                    guarded += header + [gsc.Token(gsc.PUNCT, '{')]
                stmt = guarded + stmt + [gsc.Token(gsc.PUNCT, '}') for _ in headers]
            for k in range(len(stmt) - 2):
                if [t.low for t in stmt[k:k + 3]] == ['level', '.', 'round_number']:
                    stmt[k].text, stmt[k + 1].text, stmt[k + 2].text = 'source_round', '', ''
            end.append(gsc.emit(stmt))
    if not initial and not start and not end:
        return None
    includes = {contract['script'], round_contract['script']}
    if framework:
        includes.update(framework.includes)
    return ('\n'.join('#include ' + path + ';' for path in sorted(includes)) +
            '\ninit()\n{\n' + '\n'.join(initial) + '\n}\n' +
            '\nprepare_round()\n{\n' + ('if (level.round_number > 1) {\nsource_round = level.round_number - 1;\n' +
             '\n'.join(end) + '\n}\n' if end else '') + '\n'.join(start) + '\n}\n')


def source_selection(sources, definition: dict) -> str | None:
    """Extract the source loop's selection statements through its history update."""
    c = definition['source_selection']
    script = sources.get(c['script'])
    fn = script.functions.get(c['function']) if script else None
    if fn is None:
        return None
    tokens = script.tokens
    depth, start, base, end = 0, None, None, None
    for i in range(fn.body_open + 1, fn.body_close):
        depth += tokens[i].text == '{'
        depth -= tokens[i].text == '}'
        words = [t.low for t in tokens[i:i + 5]]
        if start is None and words == [c['candidate'], '=', 'level', '.', c['array_field']]:
            start, base = i, depth
        if start is not None and depth == base and [t.low for t in tokens[i:i + 4]] == [c['previous'], '=', c['candidate'], ';']:
            end = i + 4
            break
    if start is not None and end is None:
        # DLC2 selects once without maintaining selection history. Accept only
        # that complete random-index statement, not an unknown selection body.
        close = next((i for i in range(start, fn.body_close) if tokens[i].text == ';'), None)
        if close is not None:
            words = [t.low for t in tokens[start:close + 1]]
            expected = [c['candidate'], '=', 'level', '.', c['array_field'], '[',
                        'randomint', '(', 'level', '.', c['array_field'], '.', 'size', ')', ']', ';']
            if words == expected:
                end = close + 1
    if start is None or end is None:
        return None
    body = copy.deepcopy(tokens[start:end])
    for i in range(len(body) - 2):
        if [t.low for t in body[i:i + 3]] == ['level', '.', c['array_field']]:
            body[i].text = 'candidates'
            body[i + 1].text = body[i + 2].text = ''
    includes = '#include ' + c['script'] + ';'
    return (includes + f'\nchoose(candidates, {c["previous"]})\n{{\n' + gsc.emit(body) +
            f'\nreturn {c["candidate"]};\n}}\n')


def reachable_variants(sources, definition: dict) -> dict[str, set[int] | None]:
    """Movement variants the source actually selects: set_run_anim( "walk" + var )
    after var = randomintrange( a, b ) picks a..b-1. A table can define more
    (Verruckt: walk8 and sprint4 are never chosen). None: the variant is not a
    plain randomintrange, so every authored entry stays reachable."""
    contract = definition['source_movement']
    script = sources.get(contract.get('variant_script', ''))
    if script is None:
        return {}
    toks, result = script.tokens, {}
    for i in range(len(toks) - 6):
        words = [t.low for t in toks[i:i + 7]]
        if words[:2] != [contract['variant_function'], '('] or words[3] != '+' or words[5] != ')':
            continue
        match = re.fullmatch(r'"(walk|run|sprint)"', words[2])
        if not match or toks[i + 4].kind != gsc.IDENT:
            continue
        var, picked = words[4], None
        for j in range(i - 1, 9, -1):
            if toks[j].text in ('{', '}'):
                break
            span = [t.low for t in toks[j:j + 9]]
            if span[:4] == [var, '=', 'randomintrange', '('] and span[5] == ',' and span[7:9] == [')', ';'] \
                    and toks[j + 4].kind == gsc.NUMBER and toks[j + 6].kind == gsc.NUMBER:
                picked = set(range(int(float(span[4])), int(float(span[6]))))
                break
        speed = match[1]
        if picked is None or result.get(speed, set()) is None:
            result[speed] = None
        else:
            result[speed] = result.get(speed, set()) | picked
    return result


def source_last_zombie(sources, definition: dict) -> dict | None:
    """The source's last-zombie speed rule from its round spawning loop, e.g.
    Shi No Numa: if ( level.round_number > 3 ) ... if ( zombies.size == 1 &&
    zombies[0].has_legs ) zombies[0] set_run_anim( "sprint" + var ).
    None when the map has no such rule (Verruckt: the last zombie keeps its
    speed); BO2's own zombie_speed_up then must not apply either."""
    contract = definition['source_last_zombie']
    script = sources.get(contract['script'])
    fn = script.functions.get(contract['function']) if script else None
    if fn is None:
        return None
    toks = script.tokens
    for i in range(fn.body_open, fn.body_close):
        words = [t.low for t in toks[i:i + 8]]
        if len(words) < 8 or words[:5] != ['if', '(', 'level', '.', 'round_number'] or words[5] not in ('>', '>=') \
                or toks[i + 6].kind != gsc.NUMBER or words[7] != ')' or toks[i + 8].text != '{':
            continue
        close = gsc._match(toks, i + 8, '{', '}')
        body = [t.low for t in toks[i + 8:close]]
        last = any(body[k:k + 4] == ['.', 'size', '==', '1'] for k in range(len(body)))
        speeds = [re.fullmatch(r'"(walk|run|sprint)"', body[k + 2])
                  for k in range(len(body) - 3) if body[k:k + 2] == [contract['anim_function'], '(']]
        speeds = {m[1] for m in speeds if m}
        if last and len(speeds) == 1:
            threshold = int(float(words[6]))
            return {'after_round': threshold if words[5] == '>' else threshold - 1, 'speed': speeds.pop()}
    return None


def movement_sets(sources, definition: dict, map_name: str | None = None) -> tuple[str | None, dict[str, list[str]]]:
    """Read flat, authored movement table assignments, retaining variant weights."""
    contract = definition['source_movement']
    script = sources.get(contract['script'])
    fn = script.functions.get(contract['function']) if script else None
    if script and fn is None:
        # Older/renamed frameworks declare the same movement table in another
        # initializer. Identify the role from its authored table assignments.
        candidates = []
        for candidate in script.functions.values():
            if candidate.params:
                continue
            tokens = script.tokens
            for i in range(candidate.body_open + 1, candidate.body_close - 12):
                words = [t.low for t in tokens[i:i + 13]]
                if words[:5] == ['level', '.', contract['field'], '[', '"' + contract['row'] + '"'] and \
                        words[5:7] == [']', '['] and words[8:11] == [']', '=', '%'] and \
                        re.fullmatch(r'"(?:walk|run|sprint)\d+"', words[7]):
                    candidates.append(candidate)
                    break
        if len(candidates) > 1:
            raise ValueError(f'{script.path}: ambiguous movement table initializers')
        fn = candidates[0] if candidates else None
    if fn is None:
        return None, {}
    # level.script is the immutable input map name. Resolve only that known
    # condition; dynamic conditions retain their braces and diagnostics.
    from .gscport import top_level_statements
    def fold(opening, closing):
        result = []
        for a, b in top_level_statements(script.tokens, opening, closing):
            original = script.tokens[a:b]
            if map_name is None or script.tokens[a].low != 'if':
                result.extend(original)
                continue
            guard_end = gsc._match(script.tokens, a + 1, '(', ')')
            words = [t.low for t in script.tokens[a + 2:guard_end]]
            if (len(words) != 5 or words[:3] != ['level', '.', 'script'] or
                    words[3] not in ('==', '!=') or not words[4].startswith('"')):
                result.extend(original)
                continue
            yes = guard_end + 1
            if script.tokens[yes].text != '{':
                result.extend(original)
                continue
            yes_end = gsc._match(script.tokens, yes, '{', '}')
            no = no_end = None
            if yes_end + 1 < b and script.tokens[yes_end + 1].low == 'else':
                no = yes_end + 2
                if script.tokens[no].text != '{':
                    result.extend(original)
                    continue
                no_end = gsc._match(script.tokens, no, '{', '}')
            matched = map_name.lower() == words[4].strip('"')
            if words[3] == '!=':
                matched = not matched
            if matched:
                result.extend(fold(yes, yes_end))
            elif no is not None:
                result.extend(fold(no, no_end))
        return result
    tokens = [script.tokens[fn.body_open], *fold(fn.body_open, fn.body_close), script.tokens[fn.body_close]]
    found = {}
    depth = 0
    for i in range(1, len(tokens) - 1):
        depth += tokens[i].text == '{'
        depth -= tokens[i].text == '}'
        words = [t.low for t in tokens[i:i + 13]]
        if len(words) < 13 or words[:5] != ['level', '.', contract['field'], '[', '"' + contract['row'] + '"']:
            continue
        if words[5:7] != [']', '['] or words[8:11] != [']', '=', '%'] or words[12] != ';':
            continue
        alias = words[7].strip('"')
        match = re.fullmatch(r'(walk|run|sprint)(\d+)', alias)
        if not match:
            continue
        if depth:
            raise ValueError(f'{contract["script"]}:{tokens[i].line}: conditional movement table requires runtime translation')
        found.setdefault(match[1], {})[int(match[2])] = words[11]
    reachable = reachable_variants(sources, definition)
    for speed, entries in found.items():
        picked = reachable.get(speed)
        if picked and picked & set(entries):
            found[speed] = {i: entries[i] for i in entries if i in picked}
    return fn.animtree, {speed: [entries[i] for i in sorted(entries)] for speed, entries in found.items()}


def melee_sets(sources, definition: dict) -> dict[str, list[str]]:
    """The source's attack tables per native melee state, in authored order.

    WaW animscripts/melee::pick_zombie_melee_anim picks randomly from the
    combined arrays (standing + walk or run attacks, crawl, stumpy), the
    same split as the native states; a clip listed twice keeps its weight."""
    contract = definition['source_melee']
    script = sources.get(contract['script'])
    if script is None:
        return {}
    fields = {f for names in contract['states'].values() for f in names}
    fn = script.functions.get(contract['function'])
    if fn is None:
        # older/renamed frameworks: the parameterless initializer assigning the tables
        tokens = script.tokens
        fn = next((c for c in script.functions.values() if not c.params and any(
            tokens[i].low == 'level' and tokens[i + 2].low in fields
            for i in range(c.body_open + 1, c.body_close - 3) if tokens[i + 1].text == '.')), None)
    if fn is None:
        return {}
    tables: dict[str, dict[int, str]] = {}
    depth = 0
    tokens = script.tokens
    for i in range(fn.body_open + 1, fn.body_close):
        depth += tokens[i].text == '{'
        depth -= tokens[i].text == '}'
        words = [t.low for t in tokens[i:i + 13]]
        if len(words) < 13 or words[:2] != ['level', '.'] or words[2] not in fields:
            continue
        if words[3:7] != ['[', '"' + contract['row'] + '"', ']', '['] or words[8:11] != [']', '=', '%'] \
                or words[12] != ';' or not words[7].isdigit():
            continue
        if depth:
            # a conditional table needs runtime selection: keep the native states
            return {}
        tables.setdefault(words[2], {})[int(words[7])] = words[11]
    states = {}
    for state, names in contract['states'].items():
        clips = [clip for name in names for _, clip in sorted(tables.get(name, {}).items())]
        if clips:
            states[state] = clips
    return states


def _replace_state(asd: str, state: str, assets: list[str]) -> str:
    pattern = re.compile(r'(^' + re.escape(state) + r'\s*:[^\n]*\n\{).*?^\}', re.M | re.S)
    asd, count = pattern.subn(lambda m: m[1] + '\n' + ''.join('\t' + a + '\n' for a in assets) + '}', asd)
    if count != 1:
        raise ValueError(f'Native animation state {state}: expected one definition')
    return asd


# The native zombie's traversal states hold WaW's traversal tables under
# WaW's clip names: zm_traverse_barrier(_crawl) is wall_hop_zombie's window
# hop (walk v1/v2, run v5, sprint v6/v7, crawl), zm_traverse(_crawl) the
# zombie_jump_* clips. BO2 plays its own copies of those clips; the map's
# WaW xanims of the same names replace them.
TRAVERSAL_STATES = ('zm_traverse', 'zm_traverse_crawl', 'zm_traverse_barrier', 'zm_traverse_barrier_crawl')


def source_traversal_clips(asd: str, find_xanim, out_root: Path) -> tuple[str, list[str], list[str]]:
    """Point the traversal states' clips at the WaW xanims ``find_xanim``
    (name -> Path | None) finds, copied to ``out_root``/xanim.
    Returns (asd, staged assets, clips left BO2's)."""
    staged, native = [], []
    for state in TRAVERSAL_STATES:
        block = re.search(r'^' + re.escape(state) + r'\s*:[^\n]*\n\{(.*?)^\}', asd, re.M | re.S)
        if block is None:
            continue
        lines = []
        for line in block[1].split('\n'):
            words = line.split()
            if len(words) != 2 or line.lstrip().startswith('//'):
                lines.append(line)
                continue
            clip = words[1]
            src = find_xanim(clip)
            if src is None:
                native.append(clip)
                lines.append(line)
                continue
            asset = 'waw_anim_' + clip.lower()
            dst = out_root / 'xanim' / asset
            if not dst.is_file():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
            staged.append(asset)
            lines.append(line[:line.rindex(clip)] + asset + line[line.rindex(clip) + len(clip):])
        asd = asd[:block.start(1)] + '\n'.join(lines) + asd[block.end(1):]
    return asd, sorted(set(staged)), sorted(set(native))


def stage_actor_movement(sources, out_root: Path, bo2_root: Path, definition: dict, map_name: str | None = None,
                         find_xanim=None) -> dict:
    """``find_xanim``: name -> WaW xanim path (None: absent); the traversal
    states then play the map's clips (source_traversal_clips)."""
    _, sets = movement_sets(sources, definition, map_name)
    asd = (bo2_root / 'raw/animstatedefs/zm_nuked_basic.asd').read_text(encoding='utf-8')
    traversal, traversal_native = [], []
    if find_xanim is not None:
        asd, traversal, traversal_native = source_traversal_clips(asd, find_xanim, out_root)
    if not sets and not traversal:
        return {}
    clips, missing, native = list(traversal), [], []
    for speed, names in list(sets.items()):
        # A clip absent from every zone dump was already reported by the
        # animtree stage (UNSUPPORTED_XANIM); the native state cannot list it.
        # Substates are picked at random, so dropping it shifts no index.
        kept = [name for name in names if (out_root / 'xanim' / ('waw_anim_' + name)).is_file()]
        missing += [name for name in names if name not in kept]
        if not kept:
            native.append(speed)    # keeps the native BO2 clips: BO2 instead of nothing
            del sets[speed]
            continue
        sets[speed] = kept
        assets = ['waw_anim_' + name for name in kept]
        asd = _replace_state(asd, definition['source_movement']['states'][speed], assets)
        clips.extend(assets)
    # The source's attacks, timing and notetracks: BO2's melee clips carry
    # "no pain" notes that keep a hit from interrupting the swing, and lack
    # the source's walk/run attacks entirely.
    melee, melee_native = {}, []
    for state, names in melee_sets(sources, definition).items():
        absent = [name for name in names if not (out_root / 'xanim' / ('waw_anim_' + name)).is_file()]
        if absent:
            missing += absent
            melee_native.append(state)    # the native clips: BO2 instead of nothing
            continue
        assets = ['waw_anim_' + name for name in names]
        asd = _replace_state(asd, state, assets)
        clips.extend(assets)
        melee[state] = names
    # WaW AnimScripted on the zombie (the map's rise, taunts...): every clip of
    # its WaW animtree as substate c<index> of SCRIPTED_STATE_<index // CHUNK>
    # (compat waw_animscripted; T6 parses at most 64 substates per state)
    tree = out_root / 'animtrees' / (SCRIPTED_TREE + '.atr')
    scripted = sorted({line.strip() for line in tree.read_text(encoding='utf-8').splitlines()
                       if line.strip() and not line.startswith('//')} if tree.is_file() else ())
    for start in range(0, len(scripted), SCRIPTED_CHUNK):
        asd = (asd.rstrip('\n') + f'\n\n{SCRIPTED_STATE}_{start // SCRIPTED_CHUNK} : aliased restart notify '
               f'{SCRIPTED_NOTIFY}\n{{\n' +
               ''.join(f'\tc{k}\t{c}\n' for k, c in enumerate(scripted[start:start + SCRIPTED_CHUNK], start)) +
               '}\n')
    clips.extend(scripted)
    atr =(bo2_root / 'raw/animtrees/zm_nuked_basic.atr').read_text(encoding='utf-8')
    atr, count = re.subn(r'(^body\s*\n\{)', lambda m: m[1] + '\n' +
                        ''.join('\t' + c + '\n' for c in sorted(set(clips))), atr, count=1, flags=re.M)
    if count != 1:
        raise ValueError('Native zombie animation tree has no body group')
    for directory, suffix, text in [('animtrees', '.atr', atr), ('animstatedefs', '.asd', asd)]:
        target = out_root / directory / (ACTOR_TREE + suffix)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding='utf-8')
    for actor in (out_root / 'aitype').rglob('waw_*.?sc'):
        text = actor.read_text(encoding='utf-8')
        if 'zm_nuked_basic' not in text and ACTOR_TREE not in text:
            continue    # another native family (the T6 dog AI) keeps its own tree
        text = text.replace('zm_nuked_basic', ACTOR_TREE)
        text = re.sub(r'\n// BEGIN SOURCE MOVEMENT REFS.*?// END SOURCE MOVEMENT REFS\n', '', text, flags=re.S)
        # T6 keeps only animation-tree nodes referenced by compiled scripts.
        # ASD substate names are strings and do not pin those nodes themselves.
        text += ('\n// BEGIN SOURCE MOVEMENT REFS\nwaw_reference_movement_anims()\n{\n' +
                 ''.join(f'    dummy_anim_ref = %{clip};\n' for clip in sorted(set(clips))) +
                 '}\n// END SOURCE MOVEMENT REFS\n')
        actor.write_text(text, encoding='utf-8')
    result = {'states': sets, 'melee': melee, 'tree': ACTOR_TREE, 'clips': sorted(set(clips))}
    if traversal:
        result['traversal_clips'] = traversal
    if traversal_native:
        result['native_traversal_clips'] = traversal_native
    if missing:
        result['missing'] = sorted(set(missing))
    if native:
        result['native_states'] = native
    if melee_native:
        result['native_melee_states'] = melee_native
    return result


def runtime_source(definition: dict, speed_function: str | None, selection_function: str | None = None,
                   speed_state_function: str | None = None, source_membership: bool = False,
                   respawn_function: str | None = None, chase_function: str | None = None,
                   last_zombie: dict | None = None, scripted_clips: list[str] = ()) -> str:
    native = definition['native_spawner']
    # WaW AnimScripted on a zombie (compat waw_animscripted): the WaW clip's
    # index k = substate c<k> of SCRIPTED_STATE_<k // SCRIPTED_CHUNK>
    clips = sorted(scripted_clips)
    scripted = (f'\n#using_animtree( "{SCRIPTED_TREE}" );\n\n' if clips else '\n') + \
        'waw_scripted_index( clip )\n{\n' + \
        ''.join(f'    if ( clip == %{c} )\n        return {k};\n' for k, c in enumerate(clips)) + \
        '    return undefined;\n}\n'
    # WaW drives the zombie AI: the map's _zombiemode_spawner is ported whole
    # (gsc_api preserved_scripts) and its init adds its own zombie_spawn_init;
    # BO2's is dropped from the spawners. Otherwise BO2's runs (owned copy).
    from .gscport import API
    source_ai = definition['source_chase']['script'] in API.get('preserved_scripts', [])
    bo2_spawn_init = ('continue;  // the map\'s own zombie_spawn_init runs (WaW drives the zombie AI)' if source_ai
                      else f'spawner.spawn_funcs[i]["function"] = {definition["owned_spawner"]}::{definition["native_spawn_init"]};')
    owned = definition['owned_spawner']
    native_round = definition['native_round_runtime']
    owned_round = definition['owned_round_runtime']
    entry = definition['native_spawn_init']
    result_field = definition['source_speed']['owned_result_field']
    callback_field = definition['native_movement']['callback']
    member_field = definition['source_selection']['array_field']
    membership = definition['source_membership']
    rise_field = membership['riser_array_field']
    respawn = definition['source_respawn']
    chase = f'return self {chase_function}( rise_result );' if chase_function else 'return false;'
    respawn_init = (f"level.{respawn['native_callback']} = ::source_respawn;" if respawn_function else '')
    respawn_body = (f'''source_respawn( player, return_struct )
{{
    origin = {respawn_function}( player );
    if ( !isdefined( origin ) || !isdefined( return_struct ) || !return_struct )
        return origin;
    point = spawnstruct();
    point.origin = origin;
    point.angles = ( 0, 0, 0 );
    return point;
}}
''' if respawn_function else '')
    speed = (f'self {speed_function}();\n'
             f'    self refresh_speed();\n'
             '    if ( !isdefined( self.waw_speed_callback_bound ) )\n'
             '    {\n'
             '        self.waw_speed_callback_bound = 1;\n'
             f'        self.waw_previous_speed_callback = self.{callback_field};\n'
             f'        self.{callback_field} = ::source_animstate;\n'
             '    }'
             if speed_function else 'self maps\\mp\\zombies\\_zm_utility::init_zombie_run_cycle();')
    state_init = f'{speed_state_function}::init();' if speed_state_function else ''
    state_round = f'{speed_state_function}::prepare_round();' if speed_state_function else ''
    selection = (f'{selection_function}( candidates, level.waw_previous_spawner )'
                 if selection_function else 'common_scripts\\utility::random( candidates )')
    speed_up = definition['source_last_zombie']['native_callback']
    if last_zombie:
        # WaW repeats set_run_anim every 0.5 s with a new variant; one change
        # keeps the chosen clip, as the native callback is also polled.
        last_rule = (f'''    if ( level.round_number <= {last_zombie['after_round']} || level.zombie_total > 0 )
        return;
    if ( isdefined( self.{result_field} ) && self.{result_field} == "{last_zombie['speed']}" )
        return;
    self.{result_field} = "{last_zombie['speed']}";
    self refresh_speed();''')
    else:
        last_rule = '    // the source keeps the last zombie at its own speed'
    return f'''// waw2bo2: source WaW speed rules and plain spawner origin ownership.
init()
{{
    level.waw_source_spawn_membership = {int(source_membership)};
    // BO2's {speed_up} sprints the last zombie every round past 3, then the
    // source speed state restores it: its clip re-rolled every 0.5 s.
    level.{speed_up} = ::source_last_zombie;
    {respawn_init}
    {state_init}
    // The map's own spawn loop when the port runs it (gscport extract_round_loop);
    // else native initialization can register a cached stock round-spawn pointer:
    // keep authored custom providers, route the native default to our copy.
    if ( isdefined( level.waw2bo2_source_round_spawning ) )
        level.round_spawn_func = level.waw2bo2_source_round_spawning;
    else if ( !isdefined( level.round_spawn_func ) || level.round_spawn_func == {native_round}::round_spawning )
        level.round_spawn_func = {owned_round}::round_spawning;
    bound = 0;
    spawners = getentarray( "zombie_spawner", "script_noteworthy" );
    foreach ( spawner in spawners )
    {{
        if ( !isdefined( spawner.spawn_funcs ) )
            continue;
        kept = [];
        for ( i = 0; i < spawner.spawn_funcs.size; i++ )
        {{
            if ( spawner.spawn_funcs[i]["function"] == {native}::{entry} ||
                 spawner.spawn_funcs[i]["function"] == {owned}::{entry} )
            {{
                bound++;
                {bo2_spawn_init}
            }}
            kept[kept.size] = spawner.spawn_funcs[i];
        }}
        spawner.spawn_funcs = kept;
    }}
    println( "WAW2BO2 zombie spawn callbacks adapted: " + bound );
}}

// T6 actor setup the map's zombie_spawn_init leaves to WaW's engine (gsc_api
// zombie_runtime actor_glue runs it first there). No game rules here.
waw2bo2_zombie_actor_glue()
{{
    self.team = level.zombie_team;
    // T6 reaction/pain animscripts; WaW's zombie AI had none
    self.a.disablepain = 1;
    self.allowpain = 0;
    self maps\\mp\\zombies\\_zm_utility::disable_react();
    // the template's actor capsule (72 tall broke upstairs windows)
    self setphysparams( 15, 0, 48 );
    // WaW engine zombie melee: 60 (ai melee weapon)
    self.meleedamage = 60;
    // per-hit player points through T6's actor damage plumbing
    self thread maps\\mp\\zombies\\_zm_spawner::enemy_death_detection();
    self thread waw2bo2_zombie_animstate();
}}

// T6 picks a zombie's movement and death from its animation states
// (zm_move_<speed>, zm_death); its move animscript re-reads them only when
// told. WaW's engine read the run cycle the map's set_zombie_run_cycle set.
waw2bo2_zombie_animstate()
{{
    self endon( "death" );
    speed = undefined;
    legs = undefined;
    for ( ;; )
    {{
        if ( !isdefined( speed ) || !isdefined( self.zombie_move_speed ) || speed != self.zombie_move_speed ||
             !isdefined( legs ) || legs != self.has_legs )
        {{
            speed = self.zombie_move_speed;
            legs = self.has_legs;
            self.deathanim = self maps\\mp\\animscripts\\zm_utility::append_missing_legs_suffix( "zm_death" );
            if ( isdefined( speed ) )
                self maps\\mp\\animscripts\\zm_run::needsupdate();
        }}
        wait 0.05;
    }}
}}

bind_spawn_location()
{{
    if ( isdefined( self.spawn_point_override ) )
        return;
    // Plain WaW spawners use their own origin.
    if ( isdefined( self.script_string ) && self.script_string == "{membership['riser_actor_value']}" )
    {{
        // WaW do_zombie_rise: a riser rises at random( level.{rise_field} )
        // when the map keeps that list (Shi No Numa: the rise structs of the
        // zones players are in), as choose_spawn_location does for round
        // spawns. Source scripts also spawn risers themselves (Shi No Numa's
        // <hut>_initial_zombies when a door opens): with no point chosen, T6
        // would take any active zone location. With the list empty or absent
        // WaW's riser stays at its spawner.
        if ( isdefined( level.{rise_field} ) && level.{rise_field}.size )
        {{
            self.spawn_point_override = rise_point( level.{rise_field}[randomint( level.{rise_field}.size )], self );
            return;
        }}
        point = spawnstruct();
        point.origin = self.origin;
        point.angles = self.angles;
        point.script_noteworthy = "{definition['plain_spawn_location']}";
        point.target = self.target;
        point.targetname = self.targetname;
        self.spawn_point_override = point;
        return;
    }}
    // WaW DoSpawn: the zombie appears at its spawner (the map's own zone
    // manager chose the spawner); not a location from BO2's zone lists.
    point = spawnstruct();
    point.origin = self.origin;
    point.angles = self.angles;
    point.script_noteworthy = "{definition['plain_spawn_location']}";
    point.target = self.target;
    point.targetname = self.targetname;
    // T6 reads any other string as a barrier id (choose_spawn_location)
    if ( isdefined( self.script_string ) && self.script_string == "{membership['source_chaser_value']}" )
        point.script_string = "{membership['native_chaser_value']}";
    self.spawn_point_override = point;
}}

rise_point( anchor, actor )
{{
    point = spawnstruct();
    point.origin = anchor.origin;
    point.angles = anchor.angles;
    point.script_noteworthy = "{membership['native_riser_location']}";
    // T6 reads any other string as a barrier id; WaW riser_door goes to the
    // nearest windows, the native no-string default.
    if ( isdefined( anchor.script_noteworthy ) && anchor.script_noteworthy == "{membership['native_chaser_value']}" )
        point.script_string = "{membership['native_chaser_value']}";
    point.target = actor.target;
    point.targetname = actor.targetname;
    return point;
}}

spawn_zombie( spawner, target_name, spawn_point, round_number )
{{
    // T6's helper accepts spawn_point but does not use it. Its callbacks run
    // after a frame boundary, so bind the chosen active location first.
    ai = maps\\mp\\zombies\\_zm_utility::spawn_zombie( spawner, target_name, spawn_point, round_number );
    if ( isdefined( ai ) && isdefined( spawn_point ) )
    {{
        ai.spawn_point_override = spawn_point;
        ai forceteleport( spawn_point.origin );
    }}
    return ai;
}}

select_speed()
{{
    if ( !isdefined( level.waw_speed_round ) || level.waw_speed_round != level.round_number )
    {{
        {state_round}
        level.waw_speed_round = level.round_number;
    }}
    {speed}
}}

source_last_zombie()
{{
{last_rule}
}}

refresh_speed( native_request )
{{
    if ( isdefined( self.{result_field} ) )
        native_request = self.{result_field};
    self maps\\mp\\zombies\\_zm_utility::set_zombie_run_cycle( native_request );
}}

source_animstate( animstate, substate )
{{
    // Cached native scripts can still request another speed. Restore the source
    // field and its animation state when native movement consumes that request.
    if ( isdefined( self.{result_field} ) && self.zombie_move_speed != self.{result_field} )
    {{
        self refresh_speed();
        animstate = self maps\\mp\\animscripts\\zm_utility::append_missing_legs_suffix( "zm_move_" + self.{result_field} );
        if ( isdefined( self.a.gib_ref ) && self.a.gib_ref == "no_legs" )
            animstate = "zm_move_stumpy";
        substate = undefined;
        self setanimstatefromasd( animstate );
    }}
    if ( isdefined( self.waw_previous_speed_callback ) )
        self [[ self.waw_previous_speed_callback ]]( animstate, substate );
}}

choose_spawner( candidates, spawn_point )
{{
    if ( isdefined( spawn_point ) && isdefined( spawn_point.waw_source_spawner ) )
        return spawn_point.waw_source_spawner;
    // Synthesized locations retain each WaW actor's origin. Choose its source
    // actor when available instead of an actor in a closed or distant zone.
    if ( isdefined( spawn_point ) )
    {{
        matching = [];
        foreach ( candidate in candidates )
            if ( distancesquared( candidate.origin, spawn_point.origin ) < 1.0 )
                matching[matching.size] = candidate;
        if ( matching.size )
            candidates = matching;
    }}
    if ( !isdefined( level.waw_selection_round ) || level.waw_selection_round != level.round_number )
    {{
        level.waw_previous_spawner = undefined;
        level.waw_selection_round = level.round_number;
    }}
    chosen = {selection};
    level.waw_previous_spawner = chosen;
    return chosen;
}}

spawn_locations_available()
{{
    if ( isdefined( level.waw_source_spawn_membership ) && level.waw_source_spawn_membership )
        return isdefined( level.{member_field} ) && level.{member_field}.size > 0;
    return level.zombie_spawn_locations.size > 0;
}}

choose_spawn_location()
{{
    if ( !isdefined( level.waw_source_spawn_membership ) || !level.waw_source_spawn_membership )
        return level.zombie_spawn_locations[randomint( level.zombie_spawn_locations.size )];
    actor = choose_spawner( level.{member_field}, undefined );
    anchor = actor;
    noteworthy = "spawn_location";
    if ( isdefined( actor.script_string ) && actor.script_string == "{membership['riser_actor_value']}" )
    {{
        if ( !isdefined( level.{rise_field} ) || !level.{rise_field}.size )
            return undefined;
        anchor = level.{rise_field}[randomint( level.{rise_field}.size )];
        noteworthy = "{membership['native_riser_location']}";
    }}
    point = spawnstruct();
    point.origin = anchor.origin;
    point.angles = anchor.angles;
    point.script_noteworthy = noteworthy;
    // T6 interprets any other string as a specific barrier id. WaW's
    // riser_door and ordinary actors use the nearest-window path instead.
    if ( noteworthy == "{membership['native_riser_location']}" )
    {{
        if ( isdefined( anchor.script_noteworthy ) && anchor.script_noteworthy == "{membership['native_chaser_value']}" )
            point.script_string = "{membership['native_chaser_value']}";
    }}
    else if ( isdefined( actor.script_string ) && actor.script_string == "{membership['source_chaser_value']}" )
        point.script_string = "{membership['native_chaser_value']}";
    point.target = actor.target;
    point.targetname = actor.targetname;
    point.waw_source_spawner = actor;
    return point;
}}

source_chases( rise_result )
{{
    {chase}
}}
{respawn_body}
{scripted}'''


# WaW _zombiemode_spawner::zombie_death_points rolls a power-up for every
# death (score-gated by zombie_drop_item) before it looks at the attacker, so
# kills without one (the flogger) can drop; only zombies marked for death
# (electric traps) never do. BO2 also requires a player attacker and its own
# "emerged into the playable area" flag. Drops then keep WaW's playable_area
# check (compat waw_powerup_drop).
_DROP_GATE = ('    if ( !isdefined( attacker ) || !isplayer( attacker ) )\n        return;\n\n'
              '    if ( zombie_can_drop_powerups( zombie ) )')
_KILL_VOX = ('    level thread maps\\mp\\zombies\\_zm_audio::player_zombie_kill_vox( hit_location, attacker, mod, zombie );')
_DROP_CALL = '    level thread maps\\mp\\zombies\\_zm_powerups::powerup_drop( origin );'
_WAW_DROP_CHECK = '''
waw_zombie_can_drop_powerups( zombie )
{
    if ( is_tactical_grenade( zombie.damageweapon ) || !flag( "zombie_drop_powerups" ) )
        return false;

    // waw2bo2: WaW gives no points or power-ups for zombies marked for death
    if ( isdefined( zombie.marked_for_death ) && zombie.marked_for_death )
        return false;

    return true;
}
'''


# The map's zombie_death_event runs beside BO2's (gscport quote_hooks); BO2's
# copies of its work go: the death vocal, the close-call line (BO2's vox
# categories) and the killer's zom_kill, which the map's kill-streak timer
# counts once per kill.
_DEATH_EVENT = '    level thread zombie_death_event( self );\n'
_DEATH_VOCALS = '    zombie thread maps\\mp\\zombies\\_zm_audio::do_zombies_playvocals( "death", name );\n'
_KILL_NOTIFY = '        zombie.attacker notify( "zom_kill", zombie );\n'
_CLOSE_DAMAGE_VOX = '''        if ( isdefined( zombie.sound_damage_player ) && zombie.sound_damage_player == zombie.attacker )
        {
            chance = get_response_chance( "damage" );

            if ( chance != 0 )
            {
                if ( chance > randomintrange( 1, 100 ) )
                    zombie.attacker maps\\mp\\zombies\\_zm_audio::create_and_play_dialog( "kill", "damage" );
            }
            else
                zombie.attacker maps\\mp\\zombies\\_zm_audio::create_and_play_dialog( "kill", "damage" );
        }
'''


def waw_quote_rules(text: str, native: str, core_text: str) -> str:
    """Thread the map's zombie_death_event where core holds it, in place of
    BO2's copies of its work."""
    if 'waw2bo2_zombie_death_event(' not in core_text:
        return text
    for anchor in (_DEATH_EVENT, _DEATH_VOCALS, _KILL_NOTIFY, _CLOSE_DAMAGE_VOX):
        if text.count(anchor) != 1:
            raise ValueError(f'{native}: zombie death event contract changed near {anchor.strip()[:40]!r}')
    text = text.replace(_DEATH_EVENT, _DEATH_EVENT +
                        '    maps\\mp\\waw\\_waw2bo2_core::waw2bo2_zombie_death_event( self );\n')
    for anchor in (_DEATH_VOCALS, _KILL_NOTIFY, _CLOSE_DAMAGE_VOX):
        text = text.replace(anchor, '')
    return text


def waw_drop_rules(text: str, native: str) -> str:
    for anchor in (_DROP_GATE, _KILL_VOX, _DROP_CALL):
        if text.count(anchor) != 1:
            raise ValueError(f'{native}: power-up drop contract changed near {anchor.strip()[:40]!r}')
    text = text.replace(_DROP_GATE, '    if ( waw_zombie_can_drop_powerups( zombie ) )')
    text = text.replace(_KILL_VOX, '    if ( !isdefined( attacker ) || !isplayer( attacker ) )\n        return;\n\n' + _KILL_VOX)
    text = text.replace(_DROP_CALL, '    level thread maps\\mp\\waw\\_waw2bo2_compat::waw_powerup_drop( origin );')
    return text + _WAW_DROP_CHECK


def stage_native_support(out_root: Path, bo2_root: Path, definition: dict) -> None:
    """Own the registered spawn callback so cached stock scripts cannot mask it."""
    if not (out_root / (RUNTIME.replace('\\', '/') + '.gsc')).is_file():
        return
    native, owned = definition['native_spawner'], definition['owned_spawner']
    source = (bo2_root / 'raw' / (native.replace('\\', '/') + '.gsc')).read_text(encoding='utf-8')
    script = gsc.parse(source, native)
    has_policy = (out_root / 'maps/mp/waw/_waw2bo2_source_chase_policy.gsc').is_file()
    skip = script.functions.get('should_skip_teardown')
    if has_policy and skip is None:
        raise ValueError(f'{native}: missing chase/entrance routing predicate')
    if has_policy:
        if len(skip.params) != 1:
            raise ValueError(f'{native}: expected one chase/entrance event argument')
        # The source alone decides chase vs. window. T6's own zombie_chaser
        # and find_flesh checks would override WaW's script_forcegoal, which
        # WaW tests first (Shi No Numa's hut chasers tear their windows).
        script.tokens[skip.body_open].text += ('\n    return self ' + RUNTIME +
            '::source_chases( ' + skip.params[0] + ' );\n')
    # A source chaser's optional target is an intermediate path node, not a
    # forced barricade. Let the native chase branch consume that node first.
    # A zombie_chaser's spawn point hands T6 the chase value; when the source
    # sends that actor to a window instead, drop the value so the native
    # entrance branches (target, then nearest windows) run.
    think = script.functions.get('zombie_think')
    if has_policy and think:
        result = _rise_result(script, think, definition['source_chase']['rise_event'])
        if result is None:
            raise ValueError(f'{native}: missing spawn-completion event result')
        for i in range(think.body_open + 1, think.body_close - 2):
            if script.tokens[i].low == 'if' and script.tokens[i + 1].text == '(':
                end = gsc._match(script.tokens, i + 1, '(', ')')
                cond = ''.join(t.low for t in script.tokens[i + 2:end])
                if cond == f'!isdefined({result.lower()})&&isdefined(self.target)&&self.target!=""':
                    script.tokens[end].text = ' && !self ' + RUNTIME + '::source_chases( ' + result + ' ) )'
                    chaser = definition['source_membership']['native_chaser_value']
                    script.tokens[i].text = (
                        f'if ( isdefined( {result} ) && {result} == "{chaser}" && '
                        f'!self {RUNTIME}::source_chases( {result} ) )\n        {result} = undefined;\n\n    if')
                    break
    core = out_root / 'maps/mp/waw/_waw2bo2_core.gsc'
    if core.is_file() and 'waw_tesla_init()' in core.read_text(encoding='utf-8'):
        death = script.functions.get('zombie_death_points')
        if death is None or 'zombie' not in death.params:
            raise ValueError(f'{native}: Tesla needs the native zombie death-points contract')
        # WaW skips its entire ordinary death-points/drop path for a Tesla
        # death; tesla_do_damage already awards the source chain's points.
        script.tokens[death.body_open].text += (
            '\n    if ( isdefined( zombie.tesla_death ) && zombie.tesla_death )\n'
            '        return;\n')
    fn = script.functions.get(definition['native_spawn_init'])
    location = script.functions.get(definition['native_location_function'])
    if fn is None or location is None:
        raise ValueError(f'{native}: required spawn callback/location function missing')
    refs = [r for r in gsc.references(script.tokens, fn.body_open, fn.body_close)
            if r.name.lower() == definition['native_speed_init'] and not r.pointer]
    if len(refs) != 1:
        raise ValueError(f'{native}: expected one speed initialization before zombie_think')
    ref = refs[0]
    if any(r.name.lower() == 'zombie_think' and r.index < ref.index
           for r in gsc.references(script.tokens, fn.body_open, fn.body_close)):
        raise ValueError(f'{native}: speed initialization must precede zombie_think')
    if ref.qualifier is not None or script.tokens[ref.index - 1].low != 'self':
        raise ValueError(f'{native}: expected synchronous zombie method speed initialization')
    movement = definition['native_movement']
    movement_source = bo2_root / 'raw' / (movement['script'].replace('\\', '/') + '.gsc')
    moving = gsc.parse(movement_source.read_text(encoding='utf-8'), movement['script'])
    move_fn = moving.functions.get(movement['function'])
    callback = ['self', '[[', 'self', '.', movement['callback'], ']]', '(']
    words = [t.low for t in moving.tokens[move_fn.body_open:move_fn.body_close]] if move_fn else []
    if not any(words[i:i + len(callback)] == callback for i in range(len(words))):
        raise ValueError(f'{movement["script"]}: native movement does not invoke the source speed callback')
    script.tokens[ref.index].text = RUNTIME + '::select_speed'
    script.tokens[fn.body_open].text += '\n    self ' + RUNTIME + '::bind_spawn_location();'
    selections = [r for r in gsc.references(script.tokens, location.body_open, location.body_close)
                  if r.name.lower() == 'random' and not r.pointer and
                  [t.low for t in script.tokens[r.index - 2:r.index + 5]] ==
                  ['spot', '=', 'random', '(', 'spots', ')', ';']]
    if len(selections) != 1:
        raise ValueError(f'{native}: expected one ordinary spawn location selection')
    # Select before target/zone metadata is copied, not only in the animation loop.
    script.tokens[selections[0].index - 2].text = (
        'if ( isdefined( self.spawn_point_override ) )\n'
        '        spot = self.spawn_point_override;\n    else\n        spot')
    # WaW zombie_think measures its nearest windows from the actor's own
    # spawner target (get_desired_origin). A converted location carries exactly
    # that target, or none. T6 copies it only when defined, so an actor created
    # from another spawner kept that spawner's target: Der Riese start-room
    # zombies measured from outside_up_spawners_goal_0 across the map, picked
    # unreachable windows and idled on bad_path. The location's target wins,
    # including its absence.
    words = [t.low for t in script.tokens]
    copy = ['if', '(', 'isdefined', '(', 'spot', '.', 'target', ')', ')',
            'self', '.', 'target', '=', 'spot', '.', 'target', ';']
    copies = [i for i in range(location.body_open, location.body_close)
              if words[i:i + len(copy)] == copy]
    if len(copies) != 1:
        raise ValueError(f'{native}: expected one spawn location target copy')
    for token in script.tokens[copies[0]:copies[0] + 9]:
        token.text = ''
    # Local calls/pointers still resolve to the copied callback implementation.
    for ref in gsc.references(script.tokens):
        if ref.qualifier == native:
            script.tokens[ref.qual_index].text = owned
    for token in script.tokens:
        match = gsc.INCLUDE_RE.match(token.text) if token.kind == gsc.DIRECTIVE else None
        if match and match[1] == native:
            token.text = f'#include {owned};'
    text = waw_drop_rules(gsc.emit(script.tokens), native)
    if core.is_file():
        text = waw_quote_rules(text, native, core.read_text(encoding='utf-8'))
    # kill and hit points come from the map's own scorer (compat adapter)
    text = text.replace('maps\\mp\\zombies\\_zm_score::player_add_points(',
                        'maps\\mp\\waw\\_waw2bo2_compat::waw_player_add_points(')
    dest = out_root / (owned.replace('\\', '/') + '.gsc')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text('// waw2bo2: native AI with source spawn/speed adapters.\n' + text, encoding='utf-8')
    bootstrap = out_root / 'maps/mp/waw/_waw2bo2_zm.gsc'
    if bootstrap.is_file():
        zm = gsc.parse(bootstrap.read_text(encoding='utf-8'), 'owned zombie bootstrap')
        for ref in gsc.references(zm.tokens):
            if ref.qualifier == native:
                zm.tokens[ref.qual_index].text = owned
            if ref.name.lower() == 'set_zombie_run_cycle':
                if ref.qualifier is None or ref.qualifier == r'maps\mp\zombies\_zm_utility':
                    zm.tokens[ref.index].text = RUNTIME + '::refresh_speed'
                    if ref.qual_index is not None:
                        zm.tokens[ref.qual_index].text = ''
                        zm.tokens[ref.index - 1].text = ''
        fn = zm.functions.get('round_spawning')
        if fn:
            for i in range(fn.body_open + 1, fn.body_close - 6):
                words = [t.low for t in zm.tokens[i:i + 7]]
                if words == ['level', '.', 'zombie_spawn_locations', '.', 'size', '<=', '0']:
                    zm.tokens[i].text = '!' + RUNTIME + '::spawn_locations_available()'
                    for token in zm.tokens[i + 1:i + 7]:
                        token.text = ''
            history = [i for i in range(fn.body_open + 1, fn.body_close - 3)
                       if [t.low for t in zm.tokens[i:i + 4]] == ['old_spawn', '=', 'spawn_point', ';']]
            if history:
                zm.tokens[history[-1] + 3].text += (
                    '\n        if ( isdefined( level.waw_source_spawn_membership ) && level.waw_source_spawn_membership )\n'
                    '            spawn_point = ' + RUNTIME + '::choose_spawn_location();\n'
                    '        if ( !isdefined( spawn_point ) )\n'
                    '        {\n            wait 0.1;\n            continue;\n        }\n')
            for ref in gsc.references(zm.tokens, fn.body_open, fn.body_close):
                if ref.name.lower() == 'spawn_zombie' and not ref.pointer and ref.qualifier in (None, r'maps\mp\zombies\_zm_utility'):
                    zm.tokens[ref.index].text = RUNTIME + '::spawn_zombie'
                    if ref.qual_index is not None:
                        zm.tokens[ref.qual_index].text = ''
                        zm.tokens[ref.index - 1].text = ''
                if ref.name.lower() == 'random' and not ref.pointer and [t.low for t in zm.tokens[ref.index + 2:ref.index + 5]] in (
                    ['level', '.', 'zombie_spawners'], ['level', '.', 'zombie_spawn']):
                    zm.tokens[ref.index].text = RUNTIME + '::choose_spawner'
                    close = gsc._match(zm.tokens, ref.index + 1, '(', ')')
                    zm.tokens[close].text = ', spawn_point )'
            bootstrap.write_text(gsc.emit(zm.tokens), encoding='utf-8')
