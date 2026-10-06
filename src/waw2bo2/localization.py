"""Import WaW localized UI text and isolate it from BO2's string keys."""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import zipfile
from pathlib import Path

from . import gsc, weapons

PREFIX = "WAW2BO2_"
KEY = re.compile(r"[A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]+\Z")
WORD = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[^\s]+', re.S)


def unquote(text: str) -> str:
    if not text.startswith('"'):
        return text
    return re.sub(r'\\([\\"nrt])', lambda m: {'n': '\n', 'r': '\r', 't': '\t'}.get(m[1], m[1]), text[1:-1])


def quote(text: str) -> str:
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t') + '"'


def parse(text: str, language: str = "english") -> dict[str, str]:
    tokens = [m[0] for m in WORD.finditer(text) if not m[0].startswith(('//', '/*'))]
    result, reference = {}, None
    for i, token in enumerate(tokens[:-1]):
        if token == "REFERENCE":
            reference = unquote(tokens[i + 1])
        elif token == "LANG_" + language.upper() and reference is not None:
            value = unquote(tokens[i + 1])
            if reference in result and result[reference] != value:
                raise ValueError(f"conflicting localization reference {reference}")
            result[reference] = value
    return result


def dump_zone(ff: Path, unlinker: Path, work: Path, search: list[Path]) -> Path:
    """Dedicated cache avoids redumping unrelated models/textures for strings."""
    out = work / (ff.stem + '_' + hashlib.sha256(str(ff.resolve()).encode()).hexdigest()[:12])
    marker = out / 'localization.cache.json'
    identity = {'ff_size': ff.stat().st_size, 'ff_mtime': ff.stat().st_mtime_ns,
                'tool_mtime': unlinker.stat().st_mtime_ns, 'version': 1}
    if marker.is_file() and json.loads(marker.read_text()) == identity:
        return out
    out.mkdir(parents=True, exist_ok=True)
    with (out / 'extract.log').open('w', encoding='utf-8') as log:
        run = subprocess.run([str(unlinker), '--no-color', '--include-assets', 'localize',
                              '--search-path', ';'.join(str(p) for p in search),
                              '--output-folder', str(out), str(ff)], stdout=log, stderr=subprocess.STDOUT)
    if run.returncode:
        raise RuntimeError(f"localization extraction failed: {ff}; see {out / 'extract.log'}")
    marker.write_text(json.dumps(identity))
    return out


def source_files(root: Path, language: str) -> list[Path]:
    return sorted({*root.glob(f'{language}/localizedstrings/**/*.str'),
                   *root.glob('localizedstrings/**/*.str')})


def stage(project: Path, roots: list[Path], iwd_dirs: list[Path], stock=None,
          unlinker: Path | None = None, work: Path | None = None,
          raw_roots: list[Path] = (), bo2_root: Path | None = None,
          language: str = 'english', t6_unlinker: Path | None = None) -> dict:
    """Source zones/IWDs, stock WaW, then raw WaW; never fabricate key text."""
    work = work or project / 'content_source/localization_cache'
    previous = project / 'content_source/localization.stage.json'
    reverse = {e['output']: e['key'] for e in json.loads(previous.read_text()).get('entries', [])} if previous.is_file() else {}
    def original(text: str) -> str:
        return reverse.get(text, text.removeprefix(PREFIX))
    sources: dict[str, tuple[str, str]] = {}
    requested, strings, scripts, weapon_files = set(), set(), {}, {}
    for folder, extension in [('maps/mp/waw', '*.gsc'), ('clientscripts/mp/waw', '*.csc')]:
        for path in sorted((project / folder).rglob(extension)):
            if path.name == '_waw2bo2_compat.gsc':  # this adapter uses BO2's own keys
                continue
            tokens = gsc.tokenize(path.read_text(encoding='utf-8'))
            scripts[path] = tokens
            for token in tokens:
                if token.kind == gsc.STRING:
                    text = unquote(token.text.lstrip('&#'))
                    if token.text.startswith('&"'):
                        requested.add(original(text))
                    elif token.text.startswith('"'):
                        strings.add(original(text))
    for path in sorted((project / 'content_source/weapons').rglob('*')):
        if path.is_file():
            fields = weapons.read_info(path.read_text(encoding='utf-8'))
            weapon_files[path] = fields
            requested.update(original(v) for k, v in fields.items()
                             if k in ('displayName', 'AIOverlayDescription', 'modeName') and v)

    def add(text: str, origin: str, stem: str):
        for key, value in parse(text, language).items():
            sources.setdefault(key, (value, origin))
            # WaW raw StringEd files may use a file namespace and short refs.
            sources.setdefault(stem.upper() + '_' + key, (value, origin))

    def add_root(root: Path):
        for path in source_files(root, language):
            add(path.read_text(encoding='utf-8-sig', errors='replace'), str(path), path.stem)

    if unlinker is not None:
        zones = {ff for directory in iwd_dirs if directory.is_dir() for ff in directory.glob('*.ff')}
        # Patched definitions win; common localization is the map's baseline.
        for ff in sorted(zones, key=lambda p: ('patch' not in p.stem.lower(),
                                              'common' in p.stem.lower(), p.name)):
            add_root(dump_zone(ff, unlinker, work, [ff.parent, *iwd_dirs]))
    for root in roots:
        add_root(root)
    for directory in iwd_dirs:
        for archive in ([directory] if directory.is_file() else sorted(directory.glob('*.iwd'))):
            with zipfile.ZipFile(archive) as z:
                for name in sorted(z.namelist()):
                    low = name.lower().replace('\\', '/')
                    if low.endswith('.str') and (low.startswith('localizedstrings/') or
                                                 low.startswith(language + '/localizedstrings/')):
                        add(z.read(name).decode('utf-8-sig', errors='replace'), f'{archive}:{name}', Path(name).stem)
    # Include real keys passed through ordinary string variables as well.
    known = set(sources)
    if stock is not None:
        known.update(n for kinds in stock.index.values() for n in kinds.get('localize', []))
    requested.update(strings & known)
    if stock is not None:
        for key in sorted(requested - sources.keys()):
            zones = stock.zones_defining('localize', key)
            if zones:
                add_root(dump_zone(stock.zone_dir / (zones[0] + '.ff'), stock.unlinker, work, [stock.waw_root / 'main']))
    for root in raw_roots:
        add_root(root)
    known.update(sources)
    requested.update(strings & known)
    # Exact BO2 text is eligible only after the complete source search fails.
    fallback = {}
    if bo2_root is not None:
        if t6_unlinker is not None:
            for zone in ('patch_zm', 'common_zm', 'code_post_gfx_zm'):
                ff = bo2_root / 'zone' / language / (language[:2] + '_' + zone + '.ff')
                if ff.is_file():
                    root = dump_zone(ff, t6_unlinker, work, [bo2_root / 'sound', ff.parent])
                    for path in source_files(root, language):
                        for key, value in parse(path.read_text(encoding='utf-8-sig', errors='replace'), language).items():
                            fallback.setdefault(key, (value, str(path)))
        for path in source_files(bo2_root / 'raw', language):
            for key, value in parse(path.read_text(encoding='utf-8-sig', errors='replace'), language).items():
                fallback.setdefault(key, (value, str(path)))
    entries, mapping, resolved, missing = {}, {}, [], []
    compatibility = json.loads((Path(__file__).parent / 'compat/bo2_equivalents.json').read_text())
    equivalents = compatibility.get('localize', {})
    for key in sorted(requested):
        hit = sources.get(key)
        origin_kind = 'WaW'
        if hit is None and key in fallback:
            hit, origin_kind = fallback[key], 'BO2_EXACT_FALLBACK'
        if hit is None and equivalents.get(key) in fallback:
            hit, origin_kind = fallback[equivalents[key]], 'BO2_SHARED_FALLBACK'
        if hit is None and key in compatibility.get('localize_text', {}):
            hit, origin_kind = (compatibility['localize_text'][key], 'compat/bo2_equivalents.json'), 'COMPATIBILITY_TEXT_FALLBACK'
        if hit is None and not KEY.fullmatch(key):
            hit, origin_kind = (key, 'literal display text'), 'literal'
        if hit is None:
            missing.append(key)
            continue
        value, origin = hit
        name = PREFIX + (key if re.fullmatch(r'[A-Za-z0-9_]+', key) else
                         'TEXT_' + hashlib.sha256(key.encode()).hexdigest()[:20].upper())
        entries[name] = value
        mapping[key] = name
        resolved.append({'key': key, 'output': name, 'value': value, 'source': origin, 'kind': origin_kind})
    rewritten = []
    for path, tokens in scripts.items():
        before = gsc.emit(tokens)
        for token in tokens:
            if token.kind != gsc.STRING or token.text.startswith('#'):
                continue
            text = original(unquote(token.text.lstrip('&')))
            if text in mapping and (token.text.startswith('&') or text in known):
                token.text = ('&' if token.text.startswith('&') else '') + quote(mapping[text])
        after = gsc.emit(tokens)
        if after != before:
            path.write_text(after, encoding='utf-8')
            rewritten.append(path.relative_to(project).as_posix())
    for path, fields in weapon_files.items():
        for field in ('displayName', 'AIOverlayDescription', 'modeName'):
            key = original(fields.get(field, ''))
            if key in mapping:
                fields[field] = mapping[key]
        path.write_text(weapons.write_info(fields), encoding='utf-8')
    asset = 'waw_' + project.name
    destination = project / language / 'localizedstrings' / (asset + '.str')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text('VERSION "1"\nCONFIG ""\nFILENOTES "WaW source UI strings"\n' +
                           ''.join(f'\nREFERENCE {name}\nLANG_{language.upper()} {quote(value)}\n'
                                   for name, value in sorted(entries.items())) + '\nENDMARKER\n', encoding='utf-8')
    report = {'asset': asset, 'language': language, 'entries': resolved, 'rewritten_scripts': rewritten,
              'missing': missing, 'errors': [f'missing source localization: {key}' for key in missing]}
    output = project / 'content_source/localization.stage.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report
