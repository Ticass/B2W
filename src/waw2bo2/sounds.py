"""Compiled T4 sound semantics and original audio staging.

This IR is deliberately not called a T6 bank: driver curves, channel matrices,
ducking and several flags require further engine translation. All source fields
remain available so CSV generation cannot silently discard behavior.
"""
from __future__ import annotations

import re

import csv
import hashlib
import json
import math
import struct
import zipfile
from pathlib import Path

from . import gsc
from .weapons import output_name


class SoundError(ValueError):
    pass


def script_aliases(project_root: Path, defined: set[str]) -> set[str]:
    """Discover original aliases carried through script variables and arrays.

    Looking only at map-zone sound assets misses aliases used by imported
    stock routines, such as weather. Match against actual WaW definitions;
    arbitrary script strings never become invented sound dependencies.
    """
    names = {name.lower(): name for name in sorted(defined)}
    found = set()
    for path in (project_root/'maps/mp/waw').rglob('*.gsc'):
        for token in gsc.tokenize(path.read_text(encoding='utf-8')):
            if token.kind == gsc.STRING:
                name = names.get(token.text[1:-1].lower())
                if name is not None:
                    found.add(name)
    return found


def decode_flags(flags: int) -> dict:
    """Measured in official T4 linker_pc sub_404D50, not T6 flag layout."""
    if not isinstance(flags, int) or not 0 <= flags <= 0xffffffff:
        raise SoundError("sound flags must be an unsigned 32-bit value")
    bits = {"looping": 0, "master": 1, "slave": 2, "fullDryLevel": 3,
            "noWetLevel": 4, "randomLooping": 5, "spatialized": 6,
            "realDelay": 7, "distanceLpf": 8, "doppler": 11, "isBig": 12}
    out = {name: bool(flags & (1 << bit)) for name, bit in bits.items()}
    out.update(maturity=(flags >> 9) & 3, loadType=(flags >> 13) & 3,
               legacyPriority=(flags >> 15) & 127, busIndex=(flags >> 22) & 63,
               moveType=(flags >> 28) & 7, unknownBits=flags & 0x80000000)
    return out


def _indexed(globals_data: dict, table: str, index: int) -> dict:
    hits = [row for row in globals_data[table] if row["index"] == index]
    if len(hits) != 1 or not hits[0].get("name"):
        raise SoundError(f"undefined T4 {table} slot {index}")
    return hits[0]


def translate(document: dict, globals_data: dict) -> dict:
    if (document.get("_game"), document.get("_type"), document.get("_version")) != (
            "T4", "waw2bo2_sound", 2):
        raise SoundError("requires sound schema 2: redump with the current T4 Unlinker")
    if (globals_data.get("_game"), globals_data.get("_type"), globals_data.get("_version")) != (
            "T4", "waw2bo2_sound_globals", 1):
        raise SoundError("invalid T4 sound driver globals")
    output_name("sound", document["name"])
    rows = []
    for alias in document["aliases"]:
        flags = decode_flags(alias["flags"])
        curves = {key: _indexed(globals_data, "curves", alias[key]) for key in (
            "volumeFalloffCurve", "volumeMinFalloffCurve", "reverbFalloffCurve", "reverbMinFalloffCurve")}
        for curve in curves.values():
            count = curve["pointCount"]
            if not 2 <= count <= 8 or len(curve["points"]) != 8:
                raise SoundError(f"invalid source curve {curve['name']}")
        references = {}
        for key in ("secondaryAliasName", "chainAliasName"):
            name = alias.get(key, "")
            if name:
                output_name("sound", name)
                references[key] = "waw/" + name
        file = alias.get("soundFile")
        if file is not None and file["type"] != flags["loadType"]:
            raise SoundError("sound storage type disagrees with packed flags")
        rows.append({"source": alias, "flags": flags, "references": references,
                     "bus": _indexed(globals_data, "buses", flags["busIndex"]),
                     "curves": curves, "speakerMap": _indexed(globals_data, "speakerMaps", alias["speakerMap"])})
    return {"_type": "waw2bo2_sound_ir", "_version": 1, "name": "waw/" + document["name"],
            "status": "semantic_ir_not_T6_bank", "aliases": rows}


def audio_name(file: dict) -> str:
    name = "/".join(p for p in (file.get("dir", ""), file.get("name", "")) if p)
    # WaW's file system joins "sound/" + name and collapses repeated separators,
    # so a compiled "/sfx/x.wav" is sound/sfx/x.wav (the T4 dumper writes it there)
    name = re.sub("/+", "/", name.replace(chr(92), "/")).lstrip("/")
    if not name:
        raise SoundError("sound file has no name")
    output_name("audio", name)
    return name.removeprefix("sound/")


class AudioSources:
    """Compiled loaded payloads before map IWDs, then stock IWDs."""
    def __init__(self, roots: list[Path], iwd_dirs: list[Path]):
        self.roots = roots
        self.members = {}
        for directory in iwd_dirs:
            archives = [directory] if directory.is_file() else sorted(directory.glob("*.iwd"))
            for archive in archives:
                with zipfile.ZipFile(archive) as z:
                    for entry in z.infolist():
                        if not entry.is_dir():
                            self.members.setdefault(entry.filename.replace("\\", "/").lower(), (archive, entry.filename))

    def read(self, name: str, loaded: bool) -> tuple[bytes, str] | None:
        output_name("audio", name)
        if loaded:
            candidates = [Path(name), Path(name).with_suffix(".xwma")]
            for root in self.roots:
                for relative in candidates:
                    path = root / "sound" / relative
                    if path.is_file():
                        return path.read_bytes(), str(path)
        entry = self.members.get(("sound/" + name).lower())
        if entry:
            archive, member = entry
            with zipfile.ZipFile(archive) as z:
                return z.read(member), f"{archive}!{member}"
        return None


def stage(roots: list[Path], project: Path, iwd_dirs: list[Path], stock=None,
          names: set[str] | None = None) -> dict:
    aliases = {}
    for root in roots:
        for path in sorted((root / "soundaliases").rglob("*.w2bsnd.json")):
            name = path.relative_to(root / "soundaliases").as_posix().removesuffix(".w2bsnd.json")
            if names is None or name in names:
                aliases.setdefault(name, path)
    globals_file = next((p for r in roots for p in sorted((r / "soundglobals").glob("*.w2bsndglobals.json"))), None)
    if globals_file is None and stock is not None:
        result = stock.root_for("snddriverglobals", "singleton")
        if result:
            _, root = result
            globals_file = root / "soundglobals/singleton.w2bsndglobals.json"
    report = {"status": "partial_audio_and_semantic_stage", "aliases": [], "audio": [], "errors": [],
              "remaining": ["T6 driver curve/channel translation", "ducking and source-only flag translation",
                            "original alias T6 bank compilation", "runtime playback validation"]}
    for name in sorted((names or set()) - aliases.keys()):
        report["errors"].append({"alias": name, "reason": "missing original compiled alias"})
    globals_data = json.loads(globals_file.read_text()) if globals_file and globals_file.is_file() else None
    if globals_data is not None:
        destination = project / "sound_ir/driver_globals.json"
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(globals_data, indent=2) + "\n", encoding="utf-8")
        report["driver_globals_source"] = str(globals_file)
    audio_sources = AudioSources(roots, iwd_dirs)
    files = {}
    written = {}
    for name, path in sorted(aliases.items()):
        output_name("sound", name)
        document = json.loads(path.read_text())
        try:
            if globals_data is None:
                raise SoundError("missing original T4 sound driver globals")
            if document["name"] != name:
                raise SoundError("alias asset name disagrees with source filename")
            ir = translate(document, globals_data)
            destination = project / "sound_ir" / (name + ".json")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(json.dumps(ir, indent=2) + "\n", encoding="utf-8")
            report["aliases"].append({"name": ir["name"], "variants": len(ir["aliases"]), "source": str(path)})
        except SoundError as exc:
            report["errors"].append({"alias": name, "source": str(path), "reason": str(exc)})
        # Even an unsupported metadata schema must not discard recoverable audio.
        for alias in document.get("aliases", []):
            file = alias.get("soundFile")
            if not file:
                continue
            original = audio_name(file)
            key = (original, file["type"])
            if key in files:
                files[key]["aliases"].append(name)
                continue
            payload = audio_sources.read(original, file["type"] == 1)
            item = {"name": original, "loadType": file["type"], "aliases": [name]}
            files[key] = item
            if payload is None:
                item["status"] = "missing_original_audio"
                continue
            data, source = payload
            form = data[8:12] if data[:4] == b"RIFF" else b""
            suffix = ".xwma" if form == b"XWMA" else Path(original).suffix
            relative = Path("sound/waw") / Path(original).with_suffix(suffix)
            destination = project / relative
            digest = hashlib.sha256(data).hexdigest()
            if relative in written and written[relative] != digest:
                raise SoundError(f"conflicting original audio payloads: {relative}")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            written[relative] = digest
            item.update(status="preserved_requires_decode" if form == b"XWMA" else "preserved_not_bank_compiled",
                        source=source, output=relative.as_posix(), bytes=len(data), sha256=digest)
    report["audio"] = list(files.values())
    project.mkdir(parents=True, exist_ok=True)
    (project / "sounds.stage.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def bind_pcm(project: Path, pcm_root: Path) -> dict:
    """Bind each original variant to current PCM without claiming T6 semantics.

    Only current manifests are consumed: leftover files from a previous map or
    failed decode are never eligible. Keep driver data/source fields unchanged.
    """
    from .audio import chunks

    staged = json.loads((project / "sounds.stage.json").read_text())
    decoded = json.loads((pcm_root / "audio.stage.json").read_text())
    candidates = {}
    for entry in decoded["audio"]:
        if "output" in entry:
            candidates.setdefault(entry["name"], []).append(entry)
    report = {"status": "pcm_bound_semantic_ir_not_T6_bank", "aliases": [], "errors": [],
              "bound_variants": 0, "runtime_validated": False}
    verified = set()
    for item in staged["aliases"]:
        name = item["name"].removeprefix("waw/")
        output_name("sound", name)
        original = project / "sound_ir" / (name + ".json")
        document = json.loads(original.read_text())
        for index, row in enumerate(document["aliases"]):
            source = row["source"]
            try:
                file = source.get("soundFile")
                if not file:
                    raise SoundError("alias variant has no original audio file")
                hits = [e for e in candidates.get(audio_name(file), [])
                        if e.get("loadType", file["type"]) == file["type"]]
                if not hits:
                    raise SoundError("original audio has no successful PCM in the current manifest")
                identities = {(e["output"], e["pcm_sha256"], e["alias_pitch_scale"]) for e in hits}
                if len(identities) != 1:
                    raise SoundError("ambiguous PCM binding for original alias variant")
                entry = hits[0]
                output_name("audio", entry["output"])
                path = pcm_root / entry["output"]
                key = (path, entry["pcm_sha256"], entry["bank_sample_rate"], entry.get("channels"))
                if key not in verified:
                    form, parts = chunks(path.read_bytes())
                    if len(parts[b"fmt "]) != 16:
                        raise SoundError("PCM binding requires canonical 16-bit WAVE")
                    tag, channels, rate, byte_rate, align, bits = struct.unpack("<HHIIHH", parts[b"fmt "])
                    if (form != b"WAVE" or tag != 1 or bits != 16 or channels not in (1, 2)
                            or rate != entry["bank_sample_rate"] or channels != entry.get("channels", channels)
                            or align != channels * 2 or byte_rate != rate * align):
                        raise SoundError("PCM header disagrees with the current manifest")
                    if hashlib.sha256(parts[b"data"]).hexdigest() != entry["pcm_sha256"]:
                        raise SoundError("stale PCM payload disagrees with the current manifest")
                    verified.add(key)
                scale = entry["alias_pitch_scale"]
                pitches = [source[k] * scale for k in ("pitchMin", "pitchMax")]
                if not all(math.isfinite(p) and p > 0 for p in pitches):
                    raise SoundError("invalid original or rate-compensated pitch")
                cents = [1200 * math.log2(p) for p in pitches]
                if pitches[0] > pitches[1]:
                    raise SoundError("original alias pitch range is reversed")
                # Measured T6 CSV pitch range; do not clamp original behavior.
                if any(p < -2400 or p > 1200 for p in cents):
                    raise SoundError("rate-compensated pitch exceeds native T6 bank range")
                row["pcm_binding"] = {"output": entry["output"], "pcm_sha256": entry["pcm_sha256"],
                                      "bank_sample_rate": entry["bank_sample_rate"],
                                      "alias_pitch_scale": scale,
                                      "pitchMin": pitches[0], "pitchMax": pitches[1],
                                      "PitchMinCents": cents[0], "PitchMaxCents": cents[1]}
                report["bound_variants"] += 1
            except (SoundError, OSError, KeyError, TypeError) as exc:
                report["errors"].append({"alias": document["name"], "variant": index, "reason": str(exc)})
        document["status"] = report["status"]
        destination = project / "sound_bound_ir" / (name + ".json")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        report["aliases"].append({"name": document["name"], "output": destination.relative_to(project).as_posix()})
    (project / "sounds.bindings.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


# T6 SndDriverGlobals cannot come from a map or mod zone: t6zm DB_AddXAsset
# (0x7FBD30) sends a second snddriverglobals (type 0x20) to Com_Error(ERR_DROP,
# "Attempting to override asset ...") unless the dev dvar g_connectpaths >= 2,
# and type 0x20 has no override hook. Converted aliases can therefore only
# index curves of the stock BO2 driver, matched by curve data, never by name.
T6_DRIVER_MAGIC = b"W2BSDG1\0"
T6_DRIVER_STRIDES = (80, 100, 60, 36, 36, 240, 60, 100)  # groups, curves, pans, ...
CURVE_TOLERANCE = 1e-5
CURVE_KEYS = ("volumeFalloffCurve", "volumeMinFalloffCurve", "reverbFalloffCurve", "reverbMinFalloffCurve")
T6_CURVE_FIELDS = {"volumeFalloffCurve": "DryMaxCurve", "volumeMinFalloffCurve": "DryMinCurve",
                   "reverbFalloffCurve": "WetMaxCurve", "reverbMinFalloffCurve": "WetMinCurve"}


def read_t6_curves(path: Path) -> list[dict]:
    """Curves of a native T6 driver sidecar (.w2bsdg), in engine index order."""
    data = path.read_bytes()
    if data[:8] != T6_DRIVER_MAGIC:
        raise SoundError(f"{path}: not a version 1 T6 sound driver sidecar")
    offset, tables = 8, []
    for stride in T6_DRIVER_STRIDES:
        if offset + 8 > len(data):
            raise SoundError(f"{path}: truncated sound driver")
        count, actual = struct.unpack_from("<II", data, offset)
        offset += 8
        if actual != stride or offset + count * stride > len(data):
            raise SoundError(f"{path}: invalid sound driver table")
        tables.append(data[offset:offset + count * stride])
        offset += count * stride
    if offset != len(data):
        raise SoundError(f"{path}: trailing sound driver data")
    curves = []
    for index in range(len(tables[1]) // 100):
        raw = tables[1][index * 100:(index + 1) * 100]
        name = raw[:32].split(b"\0", 1)[0].decode("ascii")
        values = struct.unpack_from("<I16f", raw, 32)
        points = [list(values[1 + 2 * i:3 + 2 * i]) for i in range(8)]
        curves.append({"index": index, "name": name, "id": values[0], "points": points})
    if len(curves) > 64:
        raise SoundError("T6 alias curve indices have six bits")
    return curves


def _curve_value(points: list, x: float, right_closed: bool) -> float:
    """Piecewise-linear value; right_closed picks the left segment at a
    duplicated x (a vertical step), otherwise the right one."""
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        if x < x1 or (right_closed and x == x1):
            if x1 == x0:
                return y1
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return points[-1][1]


def _curve_points(curve: dict) -> list:
    points = curve["points"][:curve.get("pointCount", len(curve["points"]))]
    if len(points) < 2 or points[0][0] != 0 or points[-1][0] != 1 or any(
            not all(math.isfinite(v) for v in p) or p[0] < q[0] for q, p in zip(points, points[1:])):
        raise SoundError(f"invalid curve {curve.get('name')}")
    return points


def compare_curves(source: dict, target: dict) -> dict:
    """Max error over [0, 1) at every breakpoint and midpoint of both curves,
    and the x = 1 value, where a T6 vertical step makes the result depend on
    the unmeasured evaluator convention."""
    a, b = _curve_points(source), _curve_points(target)
    xs = sorted({p[0] for p in a + b})
    samples = [x for x in xs if x < 1] + [(p + q) / 2 for p, q in zip(xs, xs[1:])]
    # Both sides of internal vertical steps matter: sampling just the right
    # side and interval midpoints can miss the largest gain error.
    error = max(abs(_curve_value(a, x, closed) - _curve_value(b, x, closed))
                for x in samples for closed in (False, True))
    ends = {abs(_curve_value(a, 1, c) - _curve_value(b, 1, c)) <= CURVE_TOLERANCE for c in (False, True)}
    return {"maxError": error, "endpoint": "equal" if ends == {True} else
            "differs" if ends == {False} else "evaluator_dependent"}


def _curve_db_error(source: dict, target: dict) -> float:
    """RMS loudness error over distance, with a -60 dB audibility floor.

    Linear gain error undervalues the quiet tail: .1 versus .01 is a 20 dB
    difference. Midpoint integration avoids the ambiguous x=1 convention.
    """
    a, b = _curve_points(source), _curve_points(target)
    total = 0.0
    for i in range(256):
        x = (i + .5) / 256
        gain_a = max(_curve_value(a, x, False), .001)
        gain_b = max(_curve_value(b, x, False), .001)
        total += (20 * math.log10(gain_a / gain_b)) ** 2
    return math.sqrt(total / 256)


def _continuous_silent_end(curve: dict) -> bool:
    points = _curve_points(curve)
    return (points[-1][1] == 0 and
            _curve_value(points, 1, True) == 0)


def translate_curves(source_curves: list[dict], t6_curves: list[dict], approximate: bool = False) -> dict:
    """Bind each original curve to the stock T6 curve with the same evaluated
    shape. A curve without one is unsupported; the nearest shape is used only
    in the explicit approximation mode and is reported as such. Approximate
    shapes minimize loudness error, retaining a smooth silent tail when the
    source has one instead of substituting a hard cutoff."""
    if not t6_curves:
        raise SoundError("T6 sound driver contains no falloff curves")
    result = {}
    for curve in source_curves:
        scored = []
        for target in t6_curves:
            comparison = compare_curves(curve, target)
            comparison["rmsDbError"] = _curve_db_error(curve, target)
            scored.append((comparison, target))
        exact = [(c, t) for c, t in scored if c["maxError"] <= CURVE_TOLERANCE and c["endpoint"] != "differs"]
        # Same data under several names (T6 defaultmin/allon): prefer the
        # source name, then the lowest index, so the choice is deterministic.
        exact.sort(key=lambda ct: (ct[1]["name"] != curve["name"], ct[1]["index"]))
        candidates = scored
        if _continuous_silent_end(curve):
            smooth = [ct for ct in scored if _continuous_silent_end(ct[1])]
            if smooth:
                candidates = smooth
        nearest = min(candidates, key=lambda ct: (ct[0]["rmsDbError"], ct[0]["maxError"], ct[1]["index"]))
        entry = {"source": curve["name"], "sourceIndex": curve["index"],
                 "nearest": {"t6": nearest[1]["name"], "index": nearest[1]["index"], **nearest[0]}}
        if exact:
            comparison, target = exact[0]
            entry.update(status="exact_shape", t6=target["name"], index=target["index"], **comparison)
            if comparison["endpoint"] == "evaluator_dependent":
                entry["note"] = "value at x=1 depends on the T6 evaluator at a vertical step (unmeasured)"
        elif approximate:
            entry.update(status="APPROXIMATED_SOUND_CURVE", t6=nearest[1]["name"], index=nearest[1]["index"],
                         **nearest[0])
        else:
            entry["status"] = "UNSUPPORTED_SOUND_CURVE"
        result[curve["name"]] = entry
    return result


def bind_curves(project: Path, t6_driver: Path | None, approximate: bool = False) -> dict:
    """Attach T6 curve indices to the PCM-bound alias IR.

    Runs after bind_pcm; only aliases in its current report are considered.
    An alias variant is curve-complete only when all four curves bind.
    """
    bindings = json.loads((project / "sounds.bindings.json").read_text())
    report = {"status": "curves_bound_semantic_ir_not_T6_bank", "approximation": approximate,
              "approximation_metric": "RMS dB over normalized distance; -60 dB floor; preserve smooth silent tails",
              "driver_replacement": "impossible: BO2 rejects a second snddriverglobals asset (ERR_DROP)",
              "curves": {}, "complete_variants": 0, "incomplete_variants": 0, "errors": []}
    if t6_driver is None or not t6_driver.is_file():
        report.update(status="missing_t6_sound_driver",
                      errors=[{"reason": f"stock T6 sound driver sidecar not found: {t6_driver}"}])
        (project / "sounds.curves.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        return report
    t6_curves = read_t6_curves(t6_driver)
    report["t6_driver"] = str(t6_driver)
    documents = {}
    used = {}
    for item in bindings["aliases"]:
        path = project / item["output"]
        documents[path] = json.loads(path.read_text())
        for row in documents[path]["aliases"]:
            for key in CURVE_KEYS:
                used.setdefault(row["curves"][key]["name"], row["curves"][key])
    try:
        table = translate_curves([used[n] for n in sorted(used)], t6_curves, approximate)
    except SoundError as exc:
        report["errors"].append({"reason": str(exc)})
        table = {}
    for name, entry in table.items():
        entry["uses"] = 0
        report["curves"][name] = entry
    for path, document in documents.items():
        for index, row in enumerate(document["aliases"]):
            fields, missing = {}, []
            for key in CURVE_KEYS:
                entry = table.get(row["curves"][key]["name"])
                if entry is None:
                    missing.append(key)
                    continue
                entry["uses"] += 1
                if "index" in entry:
                    fields[T6_CURVE_FIELDS[key]] = {"t6": entry["t6"], "index": entry["index"],
                                                    "status": entry["status"], "maxError": entry["maxError"],
                                                    "rmsDbError": entry["rmsDbError"]}
                else:
                    missing.append(key)
            row["t6_curves"] = fields
            if missing:
                row["t6_curves_missing"] = missing
                report["incomplete_variants"] += 1
            else:
                row.pop("t6_curves_missing", None)
                report["complete_variants"] += 1
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    for name, entry in report["curves"].items():
        if entry["status"] == "UNSUPPORTED_SOUND_CURVE":
            report["errors"].append({"curve": name, "uses": entry["uses"],
                                     "reason": "no stock T6 curve has this shape and BO2 cannot load a replacement "
                                               "driver; nearest " + entry["nearest"]["t6"] +
                                               f" differs by {entry['nearest']['maxError']:.4f}"})
    (project / "sounds.curves.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


# Official BO2 alias CSV (60 columns, mod tools raw/soundbank/*.aliases.csv).
T6_ALIAS_COLUMNS = [
    "Name", "FileSource", "Secondary", "Storage", "Bus", "VolumeGroup", "DuckGroup", "Duck", "ReverbSend",
    "CenterSend", "VolMin", "VolMax", "DistMin", "DistMaxDry", "DistMaxWet", "DryMinCurve", "DryMaxCurve",
    "WetMinCurve", "WetMaxCurve", "LimitCount", "EntityLimitCount", "LimitType", "EntityLimitType", "PitchMin",
    "PitchMax", "PriorityMin", "PriorityMax", "PriorityThresholdMin", "PriorityThresholdMax", "PanType", "Pan",
    "Looping", "RandomizeType", "Probability", "StartDelay", "EnvelopMin", "EnvelopMax", "EnvelopPercent",
    "OcclusionLevel", "IsBig", "DistanceLpf", "FluxType", "FluxTime", "Subtitle", "Doppler", "ContextType",
    "ContextValue", "Timescale", "IsMusic", "IsCinematic", "FadeIn", "FadeOut", "Pauseable", "StopOnEntDeath",
    "StopOnPlay", "DopplerScale", "FutzPatch", "VoiceLimit", "IgnoreMaxDist", "NeverPlayTwice"]
T6_STORAGE = {1: "loaded", 2: "streamed", 3: "primed"}
# Fields WaW has and this playback mapping does not carry (yet). Reported, not silent.
PLAYBACK_NOT_TRANSLATED = ["driver routing/ducking (stock BO2 groups; original bus gain baked into alias)", "speaker map (stock pan)",
                           "softest voice limit (approximated with priority)", "chain alias",
                           "team/cylinder/move/slave/master fields",
                           "falloff curve shape where no stock BO2 curve matches (nearest loudness used)"]
T6_LIMIT_TYPES = {0: "none", 1: "oldest", 2: "reject", 3: "priority", 4: "priority"}


def _category(row: dict) -> tuple[str, str, str, str]:
    """Stock BO2 bus/group/duck group for the WaW bus by its role; BO2 cannot
    load WaW's own buses (they live in the driver asset)."""
    bus = row["bus"]["name"].lower()
    if "music" in bus or "mus" == bus[:3]:
        return "bus_music", "grp_music", "snp_music", "yes"
    if any(k in bus for k in ("voice", "vox", "dialog")):
        return "bus_voice", "grp_voice", "snp_voice", "no"
    if any(k in bus for k in ("ui", "menu", "hud")):
        return "bus_ui", "grp_menu", "snp_foley", "no"
    if any(k in bus for k in ("amb", "element")):
        return "bus_fx", "grp_ambience", "snp_ambience", "no"
    if any(k in bus for k in ("wpn", "weap", "rfl", "pis", "smg", "mg_", "shot", "explo")):
        return "bus_fx", "grp_weapon", "snp_wpn_1p" if "1st" in bus else "snp_wpn_3p", "no"
    return "bus_fx", "grp_foley", "snp_foley", "no"


def _num(value: float) -> str:
    return f"{value:.6g}"


def _dbspl(gain: float) -> str:
    """Inverse of T6 Common::DbsplToLinear, including its uint16 silence.

    BO2's CSV volume columns are dB SPL (100 = unity), not percentages.
    CSV 0 decodes below one uint16 step and therefore stores exact silence.
    """
    if not math.isfinite(gain) or not 0 <= gain <= 1:
        raise SoundError(f"invalid linear sound gain {gain}")
    return _num(max(0, 100 + 20 * math.log10(gain))) if gain else "0"


def t6_alias_rows(document: dict, weapon_aliases: set[str] | None = None) -> tuple[list[dict], list[dict]]:
    """Rows of the official BO2 alias CSV for one bound alias list."""
    rows, errors = [], []
    for index, row in enumerate(document["aliases"]):
        source, binding, curves = row["source"], row.get("pcm_binding"), row.get("t6_curves", {})
        if not binding:
            errors.append({"alias": document["name"], "variant": index, "reason": "no PCM binding"})
            continue
        if len(curves) != 4:
            errors.append({"alias": document["name"], "variant": index, "reason": "falloff curves not bound"})
            continue
        flags = row["flags"]
        limits = [source.get(k, 0) for k in ("limitType", "entityLimitType")]
        if any(limit not in T6_LIMIT_TYPES for limit in limits):
            errors.append({"alias": document["name"], "variant": index, "reason": "unknown T4 voice limit type"})
            continue
        bus, group, duck_group, music = _category(row)
        if document["name"].removeprefix("waw/") in (weapon_aliases or set()) and bus == "bus_fx":
            # Custom aliases often use full_vol, rather than a weapon bus.
            # Give weapon dependencies the same BO2 category gain, including
            # reload/notetrack audio, instead of mixing weapon and foley gains.
            group = "grp_weapon"
            duck_group = "snp_wpn_3p" if flags["spatialized"] else "snp_wpn_1p"
        # T4 applies this gain in its driver. Choosing a BO2 category alone
        # loses the mix between weapons, reloads, ambience and other effects.
        gain = row["bus"].get("volumeMod", 1.0)
        secondary = row["references"].get("secondaryAliasName", "")
        rows.append({
            "Name": document["name"], "FileSource": binding["output"], "Secondary": secondary,
            "Storage": T6_STORAGE.get(flags["loadType"], "loaded"), "Bus": bus, "VolumeGroup": group,
            "DuckGroup": duck_group, "Duck": "", "ReverbSend": _dbspl(source["reverbSend"]),
            "CenterSend": _dbspl(source["centerPercentage"]),
            "VolMin": _dbspl(source["volMin"] * gain), "VolMax": _dbspl(source["volMax"] * gain),
            "DistMin": _num(source["distMin"]), "DistMaxDry": _num(source["distMax"]),
            "DistMaxWet": _num(source["distReverbMax"]),
            **{k: v["t6"] for k, v in curves.items()},
            "LimitCount": str(source["limitCount"]), "EntityLimitCount": str(source["entityLimitCount"]),
            "LimitType": T6_LIMIT_TYPES[limits[0]], "EntityLimitType": T6_LIMIT_TYPES[limits[1]],
            "PitchMin": _num(binding["PitchMinCents"]), "PitchMax": _num(binding["PitchMaxCents"]),
            "PriorityMin": _num(source["minPriority"]), "PriorityMax": _num(source["maxPriority"]),
            "PriorityThresholdMin": _num(source["minPriorityThreshold"]),
            "PriorityThresholdMax": _num(source["maxPriorityThreshold"]),
            "PanType": "3d" if flags["spatialized"] else "2d", "Pan": "default" if flags["spatialized"] else "front",
            "Looping": "looping" if flags["looping"] else "nonlooping", "RandomizeType": "",
            "Probability": _num(source["probability"]), "StartDelay": str(source["startDelay"]),
            "EnvelopMin": _num(source["envelopMin"]), "EnvelopMax": _num(source["envelopMax"]),
            "EnvelopPercent": _dbspl(source["envelopPercentage"]),
            "OcclusionLevel": _num(source["occlusionLevel"]), "IsBig": "yes" if flags["isBig"] else "no",
            "DistanceLpf": "yes" if flags["distanceLpf"] else "no", "FluxType": "none", "FluxTime": "0",
            "Subtitle": source.get("subtitle", ""), "Doppler": "yes" if flags["doppler"] else "no",
            "ContextType": "", "ContextValue": "", "Timescale": "no", "IsMusic": music, "IsCinematic": "no",
            "FadeIn": "0", "FadeOut": "0", "Pauseable": "no", "StopOnEntDeath": "no", "StopOnPlay": "",
            "DopplerScale": "0", "FutzPatch": "", "VoiceLimit": "no", "IgnoreMaxDist": "no", "NeverPlayTwice": "no"})
    return rows, errors


# Column ranges the official BO2 linker enforces on alias CSVs (its error:
# "Invalid value for row N col 'DistMin' - 500000 [0, 65535]"): T6 stores
# alias distances as 16-bit. Beyond 65535 units nothing in a map is farther,
# so a larger WaW distance behaves the same at the T6 maximum.
T6_COLUMN_RANGES = {"DistMin": (0, 65535), "DistMaxDry": (0, 65535), "DistMaxWet": (0, 65535)}
# Integer columns in the native T6 loader. Volume columns accept fractional
# dB SPL; rounding them as if they were percentages changes the source gain.
T6_INT_COLUMNS = ("DistMin", "DistMaxDry", "DistMaxWet", "LimitCount",
                  "EntityLimitCount", "PitchMin", "PitchMax", "PriorityMin", "PriorityMax", "StartDelay",
                  "EnvelopMin", "EnvelopMax", "FluxTime", "FadeIn", "FadeOut", "DopplerScale")


def clamp_t6_ranges(rows: list[dict]) -> list[dict]:
    """Fit rows to the T6 column formats; every changed value is returned."""
    changed = []
    for row in rows:
        for column in T6_INT_COLUMNS:
            value = float(row[column])
            low, high = T6_COLUMN_RANGES.get(column, (-math.inf, math.inf))
            t6 = int(round(min(max(value, low), high)))
            row[column] = str(t6)
            if t6 != value:
                changed.append({"alias": row["Name"], "column": column, "source": value, "t6": t6,
                                "why": "range" if not low <= value <= high else "integer column"})
    return changed


# Banks every zombies level loads besides the map's own (not a map budget).
ALWAYS_LOADED_BANKS = ("cmn_", "zmb_common.", "zmb_patch", "zmb_code_post_gfx")


def _bank_entries(path: Path) -> int:
    """entryCount of a T6 SndAssetBankHeader (6th uint32)."""
    with path.open("rb") as f:
        return struct.unpack("<8I", f.read(32))[5]


def loaded_budget(bo2_root: Path, template_csv: Path | None) -> dict:
    """Loaded-bank headroom for the converted map, measured on stock BO2.

    A loaded (.sabl) bank lives whole in the 32-bit game's memory. WaW kept its
    loaded sounds ADPCM/XWMA-compressed; decoded to T6 PCM the same set can
    exceed what any stock level loads (measured freeze at the main menu with
    ~833 MB of loaded banks). Budget = the largest stock map bank (entries and
    bytes) minus the template map bank already in mod.ff; the template's bytes
    are estimated at the stock average bytes per entry."""
    banks = [p for p in sorted((bo2_root / "sound").glob("*.all.sabl")) if not p.name.startswith(ALWAYS_LOADED_BANKS)]
    if not banks:
        raise SoundError(f"no stock BO2 map sound banks under {bo2_root / 'sound'} to measure a budget")
    biggest = max(banks, key=lambda p: p.stat().st_size)
    entries, size = _bank_entries(biggest), biggest.stat().st_size
    template = 0
    if template_csv is not None and template_csv.exists():
        with template_csv.open(encoding="utf-8", errors="replace", newline="") as f:
            template = len({r["FileSource"] for r in csv.DictReader(f)
                            if r.get("Storage", "").lower() == "loaded" and r.get("FileSource")})
    return {"reference_bank": biggest.name, "reference_entries": entries, "reference_bytes": size,
            "template_entries": template, "entries": max(entries - template, 0),
            "bytes": max(size - int(template * size / entries), 0)}


def fit_loaded_budget(rows: list[dict], pcm_root: Path, budget: dict,
                      weapon_aliases: set[str] | None = None) -> list[dict]:
    """Keep weapon dependencies loaded before filling remaining headroom.

    Moving rapid weapon sounds to streaming competes for the engine's scarce
    streaming voices and can cause intermittent silence. Use the actual
    weapon dependency graph, rather than the WaW author's bus/name choices.
    """
    sizes = {}
    for row in rows:
        if row["Storage"] == "loaded" and row["FileSource"] not in sizes:
            sizes[row["FileSource"]] = (pcm_root / row["FileSource"]).stat().st_size
    keep, used = set(), 0
    weapon_files = {r["FileSource"] for r in rows
                    if r["Name"].removeprefix("waw/") in (weapon_aliases or set())}
    for source, size in sorted(sizes.items(), key=lambda kv: (kv[0] not in weapon_files, kv[1], kv[0])):
        if len(keep) >= budget["entries"] or used + size > budget["bytes"]:
            continue
        keep.add(source)
        used += size
    streamed = []
    for row in rows:
        if row["Storage"] == "loaded" and row["FileSource"] not in keep:
            row["Storage"] = "streamed"
            streamed.append({"alias": row["Name"], "file": row["FileSource"], "bytes": sizes[row["FileSource"]]})
    budget.update(loaded_files=len(keep), loaded_bytes=used, streamed_files=len(sizes) - len(keep),
                  weapon_files_loaded=len(weapon_files & keep),
                  weapon_files_streamed=len((weapon_files & sizes.keys()) - keep))
    return streamed


def write_t6_bank(project: Path, bank: str, destination: Path, budget: dict | None = None) -> dict:
    """Official BO2 alias CSV for every PCM- and curve-bound alias.

    Plays the original WaW audio through BO2's own sound engine. Categories,
    pans and unmatched curves come from the stock BO2 driver (a map cannot
    load its own, measured); every such mapping is listed in the report.
    """
    import csv

    bindings = json.loads((project / "sounds.bindings.json").read_text())
    weapon_report = project / "weapons.stage.json"
    weapon_aliases = {name for weapon in json.loads(weapon_report.read_text())["weapons"]
                      for name in weapon.get("dependencies", {}).get("sound", [])} if weapon_report.is_file() else set()
    report = {"status": "t6_playback_bank_not_full_semantics", "bank": bank, "aliases": 0, "variants": 0,
              "not_translated": PLAYBACK_NOT_TRANSLATED, "volume_encoding": "linear gain to T6 dB SPL",
              "source_bus_gain_applied": True,
              "voice_limit_approximations": [], "errors": []}
    rows = []
    for item in bindings["aliases"]:
        document = json.loads((project / item["output"]).read_text())
        new, errors = t6_alias_rows(document, weapon_aliases)
        for index, row in enumerate(document["aliases"]):
            for field in ("limitType", "entityLimitType"):
                if row["source"].get(field) == 4:
                    report["voice_limit_approximations"].append(
                        {"alias": document["name"], "variant": index, "field": field,
                         "source": "softest", "t6": "priority"})
        report["errors"] += errors
        if new:
            rows += new
            report["aliases"] += 1
    report["clamped"] = clamp_t6_ranges(rows)
    if budget is not None:
        report["loaded_to_streamed"] = fit_loaded_budget(rows, project / "pcm", budget, weapon_aliases)
        report["loaded_budget"] = budget
    known = {r["Name"] for r in rows}
    for row in rows:
        if row["Secondary"] and row["Secondary"] not in known:
            report["errors"].append({"alias": row["Name"], "reason": f"secondary alias {row['Secondary']} not converted"})
            row["Secondary"] = ""
    report["variants"] = len(rows)
    destination.mkdir(parents=True, exist_ok=True)
    with (destination / f"{bank}.aliases.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=T6_ALIAS_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
    (project / "sounds.bank.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report
