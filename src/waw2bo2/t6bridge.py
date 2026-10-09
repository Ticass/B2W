"""Stage a project for the OAT T6 BSP bridge (OpenAssetTools ``bsp-compilation-2``).

What the bridge actually needs (verified against the vendored source):

* ``zone_raw/<project>/BSP/map_gfx.fbx`` + ``map_col.fbx``      (BSPCreator.cpp)
* the map name is the *zone name*; assets are ``maps/mp/<zone>.d3dbsp``
* scripts ``maps/mp/<zone>{,_amb,_fx}.gsc`` and
  ``clientscripts/mp/<zone>{,_amb,_fx}.csc``, rawfile
  ``animtrees/fxanim_props.atr``                                 (BSPLinker.cpp)
* materials at ``materials/<name>.json``; ``*`` names at
  ``materials/generated/_<name up to '('>.json``                 (MaterialCommon.cpp)
* technique sets at ``techniquesets/<name>.json`` with shaders in
  ``shader_bin/{ps,vs}_<name>.cso``                               (LoaderTechniqueSetT6.cpp)
* images ONLY as ``images/<name>.iwi`` (IWI v27). DDS is never read.
                                                                  (LoaderImageT6.cpp)
* hard-coded images ``lightmap0_secondary``, ``reflection_probe0``,
  ``$outdoor``                                                    (GfxWorldLinker.cpp)

The bridge does not write a ``.d3dbsp``: it links gfxworld/clipmap/comworld/
gameworldmp/mapents straight into ``zone_out/<project>/<project>.ff``.
"""
from __future__ import annotations

import copy
import os
import json
import re
import shutil
import struct
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from .resources import resource_root
from . import progress

from . import assetresolve, audio, entities, fx, fxmap, fxmaterials, gscport, hulls, iwi, lighting, lightmaps, localization, oneway, paths, projectilecollision, shaderruntime, shaders, sounds, t6api, techsets, visions, wawassets, wawsource, wavelet, weapons, zones
from .fbx import collision_material_slots, write_collision_fbx, write_world_fbx
from .world import layer_formats_from_strides, read_collision, read_gfx_world

REQUIRED_WORLD_IMAGES = {
    # T6 bridge asset name -> WaW unlinker file stem (T4 asset names are "*..." )
    "lightmap0_secondary": "_lightmap0_secondary",
    "reflection_probe0": "_reflection_probe0",
    "$outdoor": "$outdoor",
}

SCRIPT_FILES = [
    ("maps/mp", "{p}.gsc"), ("maps/mp", "{p}_amb.gsc"), ("maps/mp", "{p}_fx.gsc"),
    ("clientscripts/mp", "{p}.csc"), ("clientscripts/mp", "{p}_amb.csc"), ("clientscripts/mp", "{p}_fx.csc"),
]


class StageError(RuntimeError):
    pass


@dataclass
class StageReport:
    project: str
    materials: list[dict] = field(default_factory=list)
    images: list[dict] = field(default_factory=list)
    techsets: list[str] = field(default_factory=list)
    collision: dict = field(default_factory=dict)
    entities: dict = field(default_factory=dict)
    scripts: dict = field(default_factory=dict)
    content: dict = field(default_factory=dict)
    fx_table: dict = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def to_json(self) -> str:
        return json.dumps(self.__dict__, indent=2, sort_keys=False) + "\n"


def _load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _techset_shaders(techset_json: dict) -> set[str]:
    files = set()
    for tech in techset_json.get("techniques", []):
        if not tech:
            continue
        for p in tech.get("passArray", []):
            ps = p.get("pixelShader") or {}
            vs = p.get("vertexShader") or {}
            if ps.get("name"):
                files.add(f"shader_bin/ps_{ps['name']}.cso")
            if vs.get("name"):
                files.add(f"shader_bin/vs_{vs['name']}.cso")
    return files


def _find(roots: list[Path], rel: Path) -> Path | None:
    for root in roots:
        if (root / rel).exists():
            return root / rel
    return None


def _star_parts(name: str) -> list[str]:
    return name[1:].split("(")[0].split("_")


def recover_material_sources(report: StageReport, names: set[str], roots: list[Path],
                             stock_waw, source_waw, stock_materials: Path,
                             bo2_root: Path | None = None) -> tuple[list[Path], dict[str, Path]]:
    """One lookup for world, HUD and weapon materials, before placeholders.

    Compiled WaW definitions win, followed by raw WaW sources. BO2 equivalents
    must be explicit or exact-name matches, and keep their native image payloads.
    """
    resolver = assetresolve.Resolver(roots, stock_waw)
    closure = resolver.expand({('material', n) for n in names})
    roots = resolver.roots
    equivalents = _load_json(Path(__file__).parent / 'compat/bo2_equivalents.json').get('material', {})
    native_fallbacks = {}
    for name in sorted(names):
        if _find(roots, techsets.oat_material_path(name)) is not None:
            continue
        if source_waw is not None:
            recovered = source_waw.compile('material', name)
            if recovered is not None:
                roots.append(recovered)
                report.warnings.append(f'WAW_SOURCE_ASSET material {name}: compiled from WaW Mod Tools')
                node = resolver.nodes['material', name]
                node.update(status='resolved', provenance='WAW_SOURCE_ASSET',
                            source=str(recovered / techsets.oat_material_path(name)))
                continue
        relative = techsets.oat_material_path(equivalents.get(name, name))
        candidates = [stock_materials.parent]
        if bo2_root is not None:
            candidates.append(bo2_root / 'raw')
        candidate = _find(candidates, relative)
        if candidate is not None:
            native_fallbacks[name] = candidate
            resolver.nodes['material', name]['native_equivalent'] = str(candidate)
    closure['roots'] = [str(root) for root in roots]
    closure['missing'] = [node for node in closure['nodes'] if node['status'] != 'resolved']
    report.content['material_dependencies'] = closure
    return roots, native_fallbacks


def dangling_substitute(name: str, roots: list[Path]) -> tuple[str, str] | None:
    """Pick a defined material for a reference that no WaW zone defines.

    WaW cannot resolve these either (they are ``,name`` references in the map
    zone and absent from every zone loaded with it), so it draws its default
    material. The geometry is kept and a substitute is used instead:

    * ``*a_b_c`` generated blends: the defined blend sharing the longest
      leading run of component materials (same base layer first), else the
      one sharing the most components;
    * everything else: ``mc/mtl_default``.
    """
    if name.startswith("*"):
        want = _star_parts(name)
        best = None
        for root in roots:
            for path in (root / "materials" / "generated").glob("_*.json"):
                parts = path.stem[1:].split("_")
                common = 0
                for a, b in zip(want, parts):
                    if a != b:
                        break
                    common += 1
                shared = len(set(want) & set(parts))
                if shared == 0:
                    continue
                key = (common, shared, -abs(len(parts) - len(want)), path.stem)
                if best is None or key > best[0]:
                    best = (key, "*" + path.stem[1:])
        if best is not None:
            return best[1], f"dangling reference {name}: WaW draws its default material; using blend {best[1]}"
        return None
    fallback = "mc/mtl_default"
    if _find(roots, techsets.oat_material_path(fallback)) is not None:
        return fallback, f"dangling reference {name}: WaW draws its default material; using {fallback}"
    return None


@progress.phase('Convert materials')
def stage_materials(report: StageReport, names: set[str], roots: list[Path], stock_materials: Path,
                    project_root: Path, techset_root: Path | None = None,
                    rename: dict[str, str] | None = None, *, image_prefix: str = 'waw_world/',
                    native_fallbacks: dict[str, Path] | None = None,
                    falloff_placement: tuple[float, float, float, float] | None = None,
                    light_shadows: bool = True) -> set[str]:
    """``rename`` maps a WaW material name to the name it is written under;
    ``falloff_placement`` is the map's WaW lightFalloffPlacement and
    ``light_shadows`` False when its primary lights are staged unshadowed."""
    rename = rename or {}
    donors = techsets.index_donors(stock_materials)
    if techset_root is not None:
        # a donor is only usable if its technique set was dumped (not just referenced)
        donors = {ts: p for ts, p in donors.items() if (techset_root / "techniquesets" / f"{ts}.json").exists()}
    if not donors:
        raise StageError(f"no stock T6 material donors found under {stock_materials}")
    candidates = sorted(donors)
    used_techsets: set[str] = set()
    for name in progress.items('Materials', sorted(names)):
        rel = techsets.oat_material_path(name)
        src = _find(roots, rel)
        out_rel = techsets.oat_material_path(rename.get(name, name))
        entry = {"name": rename.get(name, name), "file": out_rel.as_posix()}
        if name in rename:
            entry["source"] = name
        notes: list[str] = []
        if src is None and name in (native_fallbacks or {}):
            native_path = native_fallbacks[name]
            out = _load_json(native_path)
            native_root = stock_materials.parent
            for texture in out.get('textures', []):
                image = texture.get('image', '').removeprefix(',')
                if not image or image.startswith('$'):
                    texture['image'] = techsets.as_reference(image)
                    continue
                output = (image_prefix or 'waw_image/') + 'bo2_fallback/' + image
                # A definition can be present only in BO2 raw while its packed
                # image is available in the stock-zone dump.
                material_folder = next(p for p in native_path.parents if p.name == 'materials')
                image_roots = [native_root, material_folder.parent]
                image_path = _find(image_roots, techsets.oat_image_path(image))
                if image_path is None and _find(image_roots, techsets.oat_image_path(image, '.dds')) is None:
                    from .all2raw import required_bo2_image
                    image_path = required_bo2_image(native_root, image)
                dst = project_root / techsets.oat_image_path(output)
                dst.parent.mkdir(parents=True, exist_ok=True)
                if image_path is not None:
                    if image_path.read_bytes()[:4] != b'IWi\x1b':
                        raise StageError(f'BO2 equivalent image {image}: not an IWI27 source')
                    shutil.copy2(image_path, dst)
                else:
                    image_path = _find(image_roots, techsets.oat_image_path(image, '.dds'))
                    if image_path is None:
                        raise StageError(f'BO2 equivalent material {name}: image {image} missing from {native_root}')
                    iwi.convert_file(image_path, dst)
                texture['image'] = output
                report.images.append({'name': output, 'source': str(image_path), 'source_kind': 't6_equivalent'})
            dst = project_root / out_rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(json.dumps(out, indent=2) + '\n', encoding='utf-8')
            entry.update(t4_techset=None, t6_techset=out['techniqueSet'],
                         native_equivalent=str(native_path), notes=['original material absent from WaW lookup'])
            report.materials.append(entry)
            report.warnings.append(f'BO2_FALLBACK material {name} -> {native_path}: absent from WaW lookup')
            used_techsets.add(out['techniqueSet'])
            continue
        if src is None:
            sub = dangling_substitute(name, roots)
            if sub is None:
                report.errors.append(f"material {name}: no WaW source {rel.as_posix()} in any zone dump")
                continue
            entry["substituted_from"] = sub[0]
            notes.append(sub[1])
            report.warnings.append(sub[1])
            src = _find(roots, techsets.oat_material_path(sub[0]))
        t4 = _load_json(src)
        t4_ts = t4.get("techniqueSet", "")
        entry["t4_techset"] = t4_ts
        # The best-named technique set can still be unusable when its donor has
        # extra slots the source cannot fill; fall back to the next best.
        remaining = list(candidates)
        rejected: list[str] = []
        source_ts = techsets.material_techset(t4)
        approximations = techsets.last_resort(source_ts)
        approximated, first_error = None, None
        while True:
            try:
                m = techsets.match(source_ts, remaining)
            except techsets.TechsetError as exc:
                first_error = first_error or exc
                if approximations:
                    # no rule for this WaW technique: keep the material's own
                    # textures on the nearest generic pass instead of failing
                    source_ts, remaining = approximations.pop(0), list(candidates)
                    approximated = source_ts
                    continue
                report.errors.append(f"material {name} ({t4_ts}): {first_error}" + (f"; rejected: {'; '.join(rejected)}" if rejected else ""))
                m = None
                break
            attempt = notes + m.notes + ([f"APPROXIMATED: no T6 rule for '{t4_ts}', drawn as '{approximated}'"]
                                         if approximated else [])
            try:
                out = techsets.build_material(t4, _load_json(donors[m.target]), attempt)
            except techsets.TechsetError as exc:
                rejected.append(f"{m.target}: {exc}")
                remaining.remove(m.target)
                continue
            notes = attempt + [f"{r}, not used" for r in rejected]
            break
        if m is None:
            continue
        if approximated:
            report.warnings.append(f"APPROXIMATED_MATERIAL {name}: no T6 rule for WaW technique '{t4_ts}'; "
                                   f"drawn with its own textures as '{approximated}' ({m.target})")
        runtime = shaderruntime.bind_material(t4, out, roots, project_root, techset_root, falloff_placement,
                                               light_shadows) if techset_root else {'active': [], 'unsupported': ['native technique dump absent']}
        if t4_ts == 'wc_unlit_distfalloff' and all(
                any(p.get('slot') == slot and p.get('paired') for p in runtime.get('active', []))
                for slot in (2, 3)):
            notes = [n for n in notes if n != 'distance falloff dropped']
        entry['shader_runtime'] = runtime
        state_slots = shaderruntime.apply_source_pass_states(t4, out, runtime)
        if state_slots:
            notes.append(f"WaW blend/alpha-test state on T6 slots {state_slots}")
        for texture in out.get('textures', []):
            image_name = texture.get('image', '')
            if image_prefix and image_name and not image_name.startswith((',', '$', image_prefix)):
                texture['image'] = image_prefix + image_name
        entry.update(t6_techset=out['techniqueSet'], donor=donors[m.target].relative_to(stock_materials).as_posix(),
                     cost=m.cost, notes=notes)
        dst = project_root / out_rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
        used_techsets.add(out['techniqueSet'])
        report.materials.append(entry)
    return used_techsets


# Images whose content is defined rather than extracted.
SYNTHESIZED_IMAGES = {
}
# mean RGBA of the three pages of stock zm_nuked *lightmap0_secondary
# Near black: WaW materials have no specular maps, so T6 applies a default
# gloss and adds the probe as reflection. Any bright probe gives a wet sheen that
# slides with the view (it looked like a scrolling texture).
NEUTRAL_PROBE_RGBA = (6, 6, 6, 255)
NEUTRAL_LIGHTMAP_PAGES = [(116, 164, 181, 154), (132, 172, 191, 111), (129, 129, 192, 147)]
# ``~$white-rgb&<x>-l`` is a linker-packed spec map: RGB = $white, alpha =
# luminance of <x>. When neither it nor <x> exists anywhere, alpha is mid gloss.
_PACKED_SPEC_PREFIX = "~$white-rgb&"


def _synthesize(report: StageReport, asset: str, dst: Path, size, rgba, why: str) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(iwi.solid_rgba_iwi(size[0], size[1], rgba))
    report.warnings.append(f"image {asset}: synthesized, {why}")
    report.images.append({"name": asset, "source": None, "synthesized": why})


_IWD_CACHE: dict[tuple, set[str]] = {}
WORLD_IMAGE_PREFIX = 'waw_world/'


IWI6_WAVELET_FORMATS = {6: "wavelet RGBA", 7: "wavelet RGB", 8: "wavelet luminance alpha", 9: "wavelet luminance",
                        10: "wavelet alpha"}


def iwd_image_format(stem: str, iwd_dirs: list[Path]) -> int | None:
    """IWI v6 pixel format of images/<stem>.iwi in the first IWD holding it."""
    import zipfile

    for d in iwd_dirs:
        for iwd in sorted(d.glob("*.iwd")):
            with zipfile.ZipFile(iwd) as z:
                for n in z.namelist():
                    if n.lower() == f"images/{stem.lower()}.iwi":
                        head = z.read(n)[:5]
                        if head[:3] == b"IWi" and head[3] == 6:
                            return head[4]
    return None


def _in_iwds(stem: str, iwd_dirs: list[Path]) -> bool:
    import zipfile

    key = tuple(str(d) for d in iwd_dirs)
    if key not in _IWD_CACHE:
        names: set[str] = set()
        for d in iwd_dirs:
            for iwd in d.glob("*.iwd"):
                with zipfile.ZipFile(iwd) as z:
                    names.update(Path(n).stem.lower() for n in z.namelist() if n.lower().endswith(".iwi"))
        _IWD_CACHE[key] = names
    return stem.lower() in _IWD_CACHE[key]


@progress.phase('Convert images')
def stage_images(report: StageReport, image_roots: list[Path], project_root: Path,
                 iwd_dirs: list[Path] | None = None, extra_images: list[str] = (),
                 wavelets: wavelet.IwdRecovery | None = None) -> list[str]:
    wanted: dict[str, str] = {name: name.replace("*", "_") for name in extra_images}
    for mat in report.materials:
        data = _load_json(project_root / mat["file"])
        for tex in data.get("textures", []):
            img = tex.get("image", "")
            if img and not img.startswith(","):
                wanted.setdefault(img, img.removeprefix(FX_IMAGE_PREFIX).removeprefix(WORLD_IMAGE_PREFIX).replace("*", "_"))
    for asset, stem in REQUIRED_WORLD_IMAGES.items():
        wanted[asset] = stem
    written = []
    native_images = {e['name'] for e in report.images if e.get('source_kind') == 't6_equivalent'}
    for asset, stem in progress.items('Images', sorted(wanted.items()), name=lambda pair: pair[0]):
        dst = project_root / techsets.oat_image_path(asset)
        if asset in native_images:
            if not dst.is_file():
                raise StageError(f'staged BO2 equivalent image {asset} disappeared')
            written.append(asset)
            continue
        if asset == "lightmap0_secondary":
            # T6 lightmaps are three stacked, encoded pages (page 2 holds the
            # light direction); a flat grey decodes as a strong red cast.
            # Neutral values: the mean of each page of stock zm_nuked's
            # lightmap0 (measured with the gfxworld diagnostic dump).
            dst.parent.mkdir(parents=True, exist_ok=True)
            pages = NEUTRAL_LIGHTMAP_PAGES
            if os.environ.get('WAW2BO2_DIAG_FALLBACK_MAGENTA'):
                pages = [(255, 0, 255, 255)] * 3  # diagnostic: is the fallback page bound?
            dst.write_bytes(iwi.paged_rgba_iwi(4, 4, pages))
            why = ("fallback flat lightmap, linked only when the map has no WaW lightmap pages "
                   "(BSP/lightmaps.json); pages = stock zm_nuked lightmap means")
            report.warnings.append(f"image {asset}: synthesized, {why}")
            report.images.append({"name": asset, "source": None, "synthesized": why})
            written.append(asset)
            continue
        if asset == "reflection_probe0":
            # WaW's dumped *reflection_probe0 is solid pure red (an unbaked
            # placeholder); T6 world shaders add the probe as specular
            # reflection, turning every glossy world surface red.
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(iwi.solid_cube_rgba_iwi(4, NEUTRAL_PROBE_RGBA))
            why = "near-black cube probe; the WaW probe is a solid red placeholder"
            report.warnings.append(f"image {asset}: synthesized, {why}")
            report.images.append({"name": asset, "source": None, "synthesized": why})
            written.append(asset)
            continue
        if asset in SYNTHESIZED_IMAGES:
            size, rgba, why = SYNTHESIZED_IMAGES[asset]
            _synthesize(report, asset, dst, size, rgba, why)
            written.append(asset)
            continue
        src = next((r / f"{stem}.dds" for r in image_roots if (r / f"{stem}.dds").exists()), None)
        if src is None and wavelets is not None:
            try:
                src = wavelets.recover(stem)
            except (iwi.IwiError, OSError) as exc:
                report.errors.append(f"image {asset}: {exc}")
                continue
        if src is None and stem.startswith(_PACKED_SPEC_PREFIX):
            _synthesize(report, asset, dst, (4, 4), (255, 255, 255, 128),
                        "packed spec map with no pixel data in any zone or IWD; white RGB, mid gloss")
            written.append(asset)
            continue
        if src is None and iwd_dirs is not None and not _in_iwds(stem, iwd_dirs):
            # proven absent from the source: no zone dump and no IWD has pixels for it
            _synthesize(report, asset, dst, (4, 4), (128, 128, 128, 255),
                        "no pixel data in any zone or IWD (WaW draws its default image); neutral grey")
            written.append(asset)
            continue
        if src is None:
            report.errors.append(f"image {asset}: source {stem}.dds missing from every image root")
            continue
        try:
            header = iwi.convert_file(src, dst)
        except iwi.IwiError as exc:
            report.errors.append(f"image {asset}: {exc}")
            continue
        report.images.append({"name": asset, "source": str(src), **header,
                              **({"wavelet": wavelets.recovered[stem]}
                                 if wavelets and stem in wavelets.recovered else {})})
        written.append(asset)
    return written


# T6-only xmodel fields. WaW flag bits do not carry over; these are the values
# of the dominant stock static props in zm_nuked (0x200000: 397/687 models,
# lighting origin: 686/687 models).
OAT_T6_RAW = resource_root() / "vendor" / "OpenAssetToolsT6" / "raw" / "t6"

T6_XMODEL_DEFAULTS = {
    "flags": 0x200000,
    "lightingOriginOffset": {"x": 0.0, "y": 0.0, "z": 0.5},
    "lightingOriginRange": 0.5,
}


def model_runtime_defaults(model: dict) -> dict:
    """Use T6's dynamic model path for animated WaW models.

    Stock Nuketown zombie bodies/heads use 0x80000. 0x200000 is the
    static-prop flag: T6's renderer at 0x724E1A selects its optimized
    instance path from that bit. It must not be applied to live actors.
    Keep the source LOD geometry and distances, rather than forcing LOD0.
    """
    return {**T6_XMODEL_DEFAULTS,
            'flags': 0x80000 if model.get('type') in ('animated', 'viewhands') else 0x200000}


SKYBOX_TEMPLATE = "skybox_dlc0_zm_nuketown"
# T6 sky cubemaps (mc_skycubemaphdr) carry an HDR intensity in alpha (stock
# zm_nuked: DXT5, mean alpha ~0.9, scaled by the material's skyColorParms).
# WaW sky cubes are LDR with alpha 0, which the T6 sky shader turns black.
# Alpha is set so the LDR colour comes out at about its WaW brightness
# (assumes intensity = alpha * skyColorParms[1] = 6 for the stock material).
SKY_HDR_ALPHA = round(255 / 6)
SKY_SRC_DIR = "sky_src"
SKYBOX_MATERIAL = "mc/mtl_dlc0_zm_skybox_nuketown"


NO_SKY_IMAGE = "waw2bo2_no_sky"


def black_cube_dds(size: int, alpha: int) -> bytes:
    """Uncompressed A8R8G8B8 cube map (6 faces, one mip), black, the same DDS
    layout _sky_with_hdr_alpha writes for WaW sky cubes."""
    import struct
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, 0x100F, size, size, size * 4, 0, 1)
    pixel_format = struct.pack("<IIIIIIII", 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = struct.pack("<IIII4x", 0x1008, 0xFE00, 0, 0)
    face = bytes([0, 0, 0, alpha]) * (size * size)
    return header + pixel_format + caps + face * 6


def _sky_with_hdr_alpha(report: StageReport, project_root: Path, sky_image: str) -> str:
    """Copy the WaW sky cube with the T6 HDR intensity in alpha (uncompressed
    32-bit DDS only). Returns the image name to use."""
    import struct

    src = next((r / f"{sky_image}.dds" for r in _SKY_IMAGE_ROOTS if (r / f"{sky_image}.dds").exists()), None)
    if src is None:
        report.errors.append(f"skybox: {sky_image}.dds not found in the image roots")
        return sky_image
    data = bytearray(src.read_bytes())
    fourcc, bits = data[84:88], struct.unpack_from("<I", data, 88)[0]
    if fourcc != b"\0\0\0\0" or bits != 32:
        report.warnings.append(f"skybox: {sky_image} is {fourcc!r}/{bits}bpp; its alpha is not rewritten, the "
                               f"T6 sky may render dark")
        return sky_image
    amask = struct.unpack_from("<I", data, 104)[0]
    shift = {0xFF000000: 3, 0x000000FF: 0}.get(amask, 3)
    if not amask:
        # X8R8G8B8 -> A8R8G8B8 (converted to an RGBA IWI, not 24-bit RGB)
        struct.pack_into("<I", data, 80, struct.unpack_from("<I", data, 80)[0] | 0x1)
        struct.pack_into("<I", data, 104, 0xFF000000)
    for i in range(128 + shift, len(data), 4):
        data[i] = SKY_HDR_ALPHA
    name = f"{sky_image}_t6sky"
    dst = project_root / SKY_SRC_DIR / f"{name}.dds"
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(bytes(data))
    report.warnings.append(f"skybox: {sky_image} alpha set to {SKY_HDR_ALPHA}/255 as the T6 HDR sky intensity "
                           f"(WaW sky cubes have none)")
    return name


_SKY_IMAGE_ROOTS: list[Path] = []


def stage_source_skybox(report: StageReport, project: str, project_root: Path, roots: list[Path],
                        source_name: str, native_root: Path, bo2_root: Path) -> str:
    """Preserve the source dome, layered materials and animated cloud UVs."""
    source_file = _find(roots, Path("xmodel") / f"{source_name}.json")
    if source_file is None:
        raise StageError(f"source sky model {source_name} missing")
    model = _load_json(source_file)
    donor = _load_json(bo2_root / "raw/materials" / f"{SKYBOX_MATERIAL}.json")
    material_names = set()
    source_root = next(root for root in roots if root / 'xmodel' / f'{source_name}.json' == source_file)
    for lod in model["lods"]:
        original = weapons.model_lod_source(roots, source_root, lod["file"])
        if original is None:
            raise StageError(f"source sky model {source_name}: geometry {lod['file']} missing from WaW assets")
        mesh = _load_json(original)
        material_names.update(m["name"] for m in mesh.get("materials", []))
        destination = project_root / lod["file"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, destination)
        for buffer in mesh.get("buffers", []):
            uri = buffer.get("uri", "")
            if uri and not uri.startswith("data:"):
                source_buffer, target_buffer = original.parent / uri, destination.parent / uri
                target_buffer.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source_buffer, target_buffer)
    for name in sorted(material_names):
        material_file = _find(roots, techsets.oat_material_path(name))
        if material_file is None:
            raise StageError(f"source sky material {name} missing")
        source = _load_json(material_file)
        target = copy.deepcopy(donor)
        target["textures"] = copy.deepcopy(source["textures"])
        target["constants"] = copy.deepcopy(source.get("constants", []))
        target["sortKey"] = source["sortKey"]
        state = fxmaterials._main_state(source)
        if state:
            for native_state in target["stateBits"]:
                if not native_state.get("polymodeLine"):
                    for key in (*fxmaterials.BLEND_FIELDS, "depthTest", "depthWrite", "polygonOffset"):
                        if key in state:
                            native_state[key] = state[key]
        runtime = shaderruntime.bind_material(source, target, roots, project_root, native_root)
        if not any(a.get("paired") and a["slot"] == 2 for a in runtime["active"]):
            raise StageError(f"source sky shader {name} unsupported: {runtime['unsupported']}")
        destination = project_root / techsets.oat_material_path(name)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(target, indent=2) + "\n", encoding="utf-8")
        report.materials.append({"name": name, "file": techsets.oat_material_path(name).as_posix(),
                                "t6_techset": target["techniqueSet"], "shader_runtime": runtime})
    model["_game"] = "t6"
    model.update(T6_XMODEL_DEFAULTS)
    model.pop("physPreset", None)
    destination = project_root / "xmodel" / f"skybox_{project}.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(model, indent=2) + "\n", encoding="utf-8")
    report.content["sky"] = {"source_model": source_name, "status": "source_geometry_and_paired_shaders",
                             "materials": sorted(material_names)}
    return next(m["t6_techset"] for m in report.materials if m["name"] in material_names)


def stage_skybox(report: StageReport, project: str, project_root: Path, bo2_root: Path | None,
                 sky_image: str | None) -> tuple[str | None, str | None]:
    """Stage skybox_<project>: BO2's sky dome + sky technique, WaW's sky cubemap.

    WaW draws its skybox model and sky surfaces with sky techniques that have
    no T6 counterpart for its materials; drawn with an ordinary model
    technique the dome is opaque geometry around the camera that hides every
    distant surface. T6 skies are small domes drawn with mc_skycubemaphdr
    (behind everything), so the stock Nuketown dome and its material are used
    with the WaW sky cubemap. Returns (material name, image name)."""
    if bo2_root is None:
        report.errors.append("skybox: needs --bo2")
        return None, None
    synthesized = sky_image is None
    if synthesized:
        # An enclosed map with no sky surface and no skybox model: WaW draws
        # nothing there (black). Keep T6's dome behind everything, black.
        sky_image = NO_SKY_IMAGE
        dst = project_root / SKY_SRC_DIR / f"{sky_image}.dds"
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(black_cube_dds(4, SKY_HDR_ALPHA))
        report.warnings.append("SKY_SYNTHESIZED the WaW map has no sky surface or skybox model; "
                               "the T6 sky dome is black, as WaW draws it")
    raw = bo2_root / "raw"
    src_json = raw / "xmodel" / f"{SKYBOX_TEMPLATE}.json"
    src_mat = raw / "materials" / f"{SKYBOX_MATERIAL}.json"
    if not src_json.exists() or not src_mat.exists():
        report.errors.append(f"skybox: {src_json} or {src_mat} missing (run the mod tools All2Raw)")
        return None, None
    xm = _load_json(src_json)
    for lod in xm.get("lods", []):
        src = raw / lod["file"]
        if not src.exists():
            report.errors.append(f"skybox: {src} missing")
            return None, None
        dst = project_root / lod["file"]
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    dst_json = project_root / "xmodel" / f"skybox_{project}.json"
    dst_json.parent.mkdir(parents=True, exist_ok=True)
    dst_json.write_text(json.dumps(xm, indent=4) + "\n", encoding="utf-8")
    if not synthesized:
        sky_image = _sky_with_hdr_alpha(report, project_root, sky_image)
    mat = _load_json(src_mat)
    for tex in mat.get("textures", []):
        if tex.get("name") == "colorMap":
            tex["image"] = sky_image
    dst_mat = project_root / techsets.oat_material_path(SKYBOX_MATERIAL)
    dst_mat.parent.mkdir(parents=True, exist_ok=True)
    dst_mat.write_text(json.dumps(mat, indent=2) + "\n", encoding="utf-8")
    report.warnings.append(f"skybox: BO2 {SKYBOX_TEMPLATE} dome with the WaW sky cubemap {sky_image}")
    return mat.get("techniqueSet"), sky_image


_NEUTRAL_TRANSFORM = {"translation": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]}


def _neutralize_nan_transforms(gltf: dict) -> int:
    """The WaW exporter writes a NaN bone offset as JSON null, which OAT
    rejects ("type must be number, but is null"; Alcatraz's cell_elec_door
    joints). Such a component becomes its neutral value. Returns the count."""
    fixed = 0
    for node in gltf.get("nodes", []):
        for key, neutral in _NEUTRAL_TRANSFORM.items():
            values = node.get(key)
            if values and any(not isinstance(v, (int, float)) for v in values):
                node[key] = [v if isinstance(v, (int, float)) else n for v, n in zip(values, neutral)]
                fixed += 1
    return fixed


def _strip_bad_skins(gltf: dict) -> bool:
    """OAT requires every skin to have exactly one common root joint. The
    WaW exporter can emit several; drop such skins (and their vertex joint
    attributes) so the mesh loads rigidly. Returns True if anything changed."""
    parent = {c: i for i, node in enumerate(gltf.get("nodes", [])) for c in node.get("children", [])}
    bad = False
    for skin in gltf.get("skins", []):
        joints = set(skin.get("joints", []))
        if len([j for j in joints if parent.get(j) not in joints]) != 1:
            bad = True
    if not bad:
        return False
    gltf.pop("skins", None)
    for node in gltf.get("nodes", []):
        node.pop("skin", None)
    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            attrs = prim.get("attributes", {})
            for key in [k for k in attrs if k.startswith(("JOINTS_", "WEIGHTS_"))]:
                del attrs[key]
    return True


def _gltf_game_bounds(path: Path):
    """Model-space bounds of a GLTF LOD in game axes.

    Measured against WaW collSurf bounds of three models: the T4 GLTF export
    stores game (x, y, z) as (x, z, -y)."""
    data = _load_json(path)
    mins, maxs = [float("inf")] * 3, [float("-inf")] * 3
    for mesh in data.get("meshes", []):
        for prim in mesh.get("primitives", []):
            accessor = data["accessors"][prim["attributes"]["POSITION"]]
            lo, hi = accessor["min"], accessor["max"]
            for axis, (a, b) in enumerate(((lo[0], hi[0]), (-hi[2], -lo[2]), (lo[1], hi[1]))):
                mins[axis] = min(mins[axis], a)
                maxs[axis] = max(maxs[axis], b)
    if mins[0] == float("inf"):
        return None
    return tuple(mins), tuple(maxs)


@progress.phase('Convert models and collision')
def stage_models(report: StageReport, world, project: str, stage: Path, project_root: Path,
                 extra_models: set[str] = frozenset(), roots: list[Path] | None = None,
                 entity_box_models: set[str] = frozenset()) -> set[str]:
    """Stage every placed static model and the skybox as T6 xmodel sources.

    Writes ``xmodel/<name>.json`` (OAT xmodel v2 pointing at the GLTF LODs)
    and ``BSP/models.json`` with the placements read by the bridge's
    GfxWorldLinker. Returns the material names used by the models.

    ``entity_box_models`` are placed as script_model entities. WaW traces such
    an entity without collSurfs as its model bounds with contents 0x2080; T6
    only traces collSurfs, so those models get that box as their collSurf.
    """
    targets = {m.name: m.name for m in world.static_models}
    targets.update({name: name for name in extra_models})
    # the skybox is staged by stage_skybox (T6 needs a sky technique)
    materials: set[str] = set()
    for src_name, dst_name in progress.items('Models', sorted(targets.items()), name=lambda pair: pair[0]):
        src_root = next((r for r in (roots or [stage]) if (r / "xmodel" / f"{src_name}.json").exists()), None)
        if src_root is None:
            report.errors.append(f"xmodel {src_name}: xmodel/{src_name}.json missing from every WaW zone dump")
            continue
        xm = _load_json(src_root / "xmodel" / f"{src_name}.json")
        lods = []
        for lod in xm.get("lods", []):
            gltf = weapons.model_lod_source(roots or [stage], src_root, lod["file"])
            if gltf is None:
                report.errors.append(f"xmodel {src_name}: LOD file {lod['file']} missing")
                continue
            dst_file = project_root / lod["file"]
            dst_file.parent.mkdir(parents=True, exist_ok=True)
            data = _load_json(gltf)
            changed = False
            if _strip_bad_skins(data):
                report.warnings.append(f"xmodel {src_name}: skin without a single root joint removed "
                                       f"({lod['file']}); drawn rigid in its bind pose")
                changed = True
            bad_nodes = _neutralize_nan_transforms(data)
            if bad_nodes:
                report.warnings.append(f"xmodel {src_name}: {bad_nodes} node transform(s) with NaN components "
                                       f"neutralized ({lod['file']})")
                changed = True
            if changed:
                dst_file.write_text(json.dumps(data), encoding="utf-8")
            else:
                shutil.copy2(gltf, dst_file)
            for mat in data.get("materials", []):
                materials.add(mat["name"])
            lods.append(lod)
        if not lods:
            continue
        xm["_game"] = "t6"
        xm["lods"] = lods
        if dst_name in entity_box_models and not xm.get("collSurfs"):
            bounds = _gltf_game_bounds(weapons.model_lod_source(roots or [stage], src_root, lods[0]["file"]))
            if bounds is None:
                report.errors.append(f"xmodel {src_name}: no LOD0 positions for its script_model collision box")
            else:
                xm["collSurfs"] = [hulls.bounds_box_collsurf(*bounds, xm.get("rootBoneName", "tag_origin"))]
                xm["contents"] = xm.get("contents", 0) | hulls.SCRIPT_MODEL_CONTENTS
                # T6 XModelTraceLine (0x40DFD0) rejects a negative collLod
                # before reading collSurfs. Enable only the synthesized entity
                # box; authored movement/world clip masks stay untouched.
                xm["collLod"] = 0
                report.content.setdefault("script_model_collision_boxes", []).append(dst_name)
        xm.update(model_runtime_defaults(xm))
        if xm.pop("physPreset", None) is not None:
            report.warnings.append(f"xmodel {src_name}: physPreset dropped (static models do not simulate)")
        dst = project_root / "xmodel" / f"{dst_name}.json"
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(json.dumps(xm, indent=4) + "\n", encoding="utf-8")
    # OAT's xmodel loader reads this from the search-path root
    part_csv = OAT_T6_RAW / "partclassification.csv"
    if part_csv.exists():
        shutil.copy2(part_csv, project_root / part_csv.name)
    else:
        report.errors.append(f"{part_csv} missing")
    placements = [{"name": m.name, "origin": list(m.origin), "axis": list(m.axis),
                   "scale": m.scale, "flags": m.flags, "cullDist": m.cull_dist,
                   "primaryLightIndex": m.primary_light_index} for m in world.static_models]
    bsp = project_root / "BSP"
    bsp.mkdir(parents=True, exist_ok=True)
    (bsp / "models.json").write_text(json.dumps({"models": placements}, indent=1) + "\n", encoding="utf-8")
    return materials


def stage_fx_models(report: StageReport, project_root: Path, names: set[str]) -> set[str]:
    """Isolate converted FX models from BO2's same-named gameplay models.

    Geometry and materials are the already staged WaW sources. The independent
    asset name prevents an FX dependency from resolving to a preloaded BO2 model.
    """
    staged = set()
    entries = []
    for name in sorted(names):
        source = project_root / "xmodel" / (name + ".json")
        if not source.is_file():
            report.errors.append(f"FX xmodel {name}: converted WaW model missing")
            continue
        model = _load_json(source)
        if model.get("_game", "").lower() != "t6" or not model.get("lods"):
            report.errors.append(f"FX xmodel {name}: converted WaW model is invalid")
            continue
        output = fx.model_name(name)
        target = project_root / "xmodel" / (output + ".json")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        staged.add(output)
        entries.append({"source_model": name, "name": output,
                        "file": target.relative_to(project_root).as_posix(),
                        "lods": [lod["file"] for lod in model["lods"]]})
    report.content["fx_model_assets"] = entries
    return staged



def stage_model_overlay_fx(report: StageReport, project_root: Path) -> list[str]:
    entries = []
    for source, name in sorted(report.fx_table.items()):
        path = fx.fx_file(project_root, name)
        if not path.exists(): continue
        for model, variant in fx.model_overlay_variants(_load_json(path)):
            dst = fx.fx_file(project_root, variant['name'])
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(json.dumps(variant, indent=1) + '\n', encoding='utf-8')
            entries.append({'source': source, 'model': model, 'variant': variant['name']})
    assets = project_root / 'maps/mp/waw/_waw2bo2_assets.gsc'
    if assets.exists():
        text = assets.read_text(encoding='utf-8')
        text = re.sub(r'\n    // BEGIN MODEL OVERLAY FX.*?    // END MODEL OVERLAY FX\n', '', text, flags=re.S)
        lines = ['    // BEGIN MODEL OVERLAY FX', '    level.waw2bo2_model_overlay_fx = [];']
        for source in sorted({e['source'] for e in entries}):
            lines.append('    level.waw2bo2_model_overlay_fx[' + json.dumps(source) + '] = [];')
        for entry in entries:
            lines.append('    level.waw2bo2_model_overlay_fx[' + json.dumps(entry['source']) + ']['
                         + json.dumps(entry['model']) + '] = ' + json.dumps(entry['variant']) + ';')
        lines.append('    // END MODEL OVERLAY FX')
        text = text.replace('init()\n{', 'init()\n{\n' + '\n'.join(lines) + '\n', 1)
        assets.write_text(text, encoding='utf-8')
    report.content['model_overlay_fx'] = entries
    return [e['variant'] for e in entries]

def verify_techsets(report: StageReport, used: set[str], techset_root: Path | None, project_root: Path | None = None) -> None:
    report.techsets = sorted(used)
    if techset_root is None:
        report.errors.append("no --techset-dump given; the bridge cannot load technique sets without "
                             "techniquesets/*.json + shader_bin/*.cso dumped from a stock T6 zone")
        return
    for name in sorted(used):
        path = _find([*([project_root] if project_root else []), techset_root], Path('techniquesets') / f'{name}.json')
        if path is None and project_root is not None:
            # outside the donor view: copy it and its shaders from its own zone
            from .all2raw import required_bo2_techset
            owner = required_bo2_techset(techset_root, name)
            if owner is not None:
                source = owner / "techniquesets" / f"{name}.json"
                for rel in [Path("techniquesets") / f"{name}.json",
                            *map(Path, _techset_shaders(_load_json(source)))]:
                    if (owner / rel).is_file():
                        (project_root / rel).parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(owner / rel, project_root / rel)
                path = project_root / "techniquesets" / f"{name}.json"
        if path is None:
            path = techset_root / "techniquesets" / f"{name}.json"
        if not path.exists():
            report.errors.append(f"technique set {name}: {path} missing from dump")
            continue
        for rel in sorted(_techset_shaders(_load_json(path))):
            if _find([*([project_root] if project_root else []), techset_root], Path(rel)) is None:
                report.errors.append(f"technique set {name}: shader {rel} missing from dump")


def stage_template_scripts(report: StageReport, project: str, project_root: Path,
                           template_root: Path, template_name: str) -> None:
    """Instantiate every map script of a template map (e.g. the mod tools'
    zm_test boilerplate: main, _classic, _gamemodes, _fx, createfx, createart
    and the client scripts) under the project name."""
    for src in sorted(template_root.rglob(f"{template_name}*.[gc]sc")):
        rel = src.relative_to(template_root)
        if rel.parts[0] not in ("maps", "clientscripts"):
            continue
        dst = project_root / rel.parent / src.name.replace(template_name, project, 1)
        # Template createart owns a nonexistent map vision and grid tweaks.
        # The imported WaW createart owns these settings instead. Also migrate
        # an unchanged template from an earlier conversion, preserving edits.
        template = src.read_text(encoding="utf-8", errors="replace").replace(template_name, project)
        source_art = rel.parent == Path("maps/mp/createart") and src.suffix == ".gsc"
        if dst.exists() and (not source_art or dst.read_text(encoding="utf-8", errors="replace") != template):
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        text = ("// WaW createart is initialized by the imported map script.\n"
                "main()\n{\n    level.tweakfile = 1;\n}\n") if source_art else template
        dst.write_text(text, encoding="utf-8")
        report.warnings.append(f"script {dst.relative_to(project_root).as_posix()} generated from template {rel.as_posix()}")


AMB_CSC = """#include clientscripts\\mp\\_utility;
#include clientscripts\\mp\\_ambientpackage;
#include clientscripts\\mp\\_music;
#include clientscripts\\mp\\_audio;

// generated by waw2bo2: the client music states the zombies server scripts
// request (as in stock zm_nuked). The WaW map music is not in any BO2 sound
// bank, so every state plays the null alias. Without these declarations
// clientscripts/mp/_music::updatemusic spins on an unknown state.
main()
{
    declaremusicstate( "WAVE" );
    musicalias( "null", 1 );
    declaremusicstate( "EGG" );
    musicalias( "null", 1 );
    declaremusicstate( "SILENCE" );
    musicalias( "null", 1 );
}
"""


def write_amb_csc(report: StageReport, project: str, project_root: Path) -> None:
    dst = project_root / "clientscripts" / "mp" / f"{project}_amb.csc"
    dst.parent.mkdir(parents=True, exist_ok=True)
    mix = sounds.default_reverb_mix(project_root / "content_source")
    text = AMB_CSC
    if mix["status"] == "silent_source_default":
        text = text.replace('    declaremusicstate( "WAVE" );',
            '    // WaW DEFAULT is silent; the BO2 DEFAULT radverb has reflections.\n'
            '    declareambientroom( "waw_default", 1 );\n'
            '    setambientroomreverb( "waw_default", "default", 1, 0, 0 );\n'
            '    setreverb( "snd_enveffectsprio_level", "default", 1, 0, 0 );\n'
            '    declaremusicstate( "WAVE" );', 1)
    else:
        report.warnings.append(f"SOUND_DEFAULT_REVERB {mix['status']}: source baseline not translated")
    report.content["default_reverb"] = mix
    dst.write_text(text, encoding="utf-8")


def source_entities(gfx_bin: Path, stage: Path, roots: list[Path]) -> Path:
    """The world exporter and ordinary mapents dumper use different roots."""
    name = gfx_bin.name.removesuffix('.gfx.bin') + '.ents'
    candidates = [gfx_bin.parent / name, stage / 'maps' / name]
    for root in roots:
        candidates.extend((root / 'maps' / name, root / 'maps/mp' / name))
    return next((p for p in candidates if p.is_file()), candidates[0])


def stage_scripts(report: StageReport, project: str, project_root: Path,
                  template_root: Path | None, template_name: str | None) -> None:
    for folder, pattern in SCRIPT_FILES:
        dst = project_root / folder / pattern.format(p=project)
        if dst.exists():
            continue
        # The shipped BO2 zm_test template has client ambience but no server
        # _amb script. WaW ambience remains in the imported source scripts.
        if folder == 'maps/mp' and pattern == '{p}_amb.gsc' and (
                template_root is None or template_name is None or
                not (template_root / folder / pattern.format(p=template_name)).is_file()):
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text('// Server ambience runs in the imported WaW scripts.\nmain()\n{\n}\n', encoding='utf-8')
            report.warnings.append(f'script {dst.name} generated: WaW owns server ambience')
            continue
        if template_root is None or template_name is None:
            report.errors.append(f"script {dst.relative_to(project_root).as_posix()} missing")
            continue
        src = template_root / folder / pattern.format(p=template_name)
        if not src.exists():
            report.errors.append(f"script template {src} missing")
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        text = src.read_text(encoding="utf-8", errors="replace").replace(template_name, project)
        dst.write_text(text, encoding="utf-8")
        report.warnings.append(f"script {dst.name} generated from template {src.name}")


def stage_rawfiles(report: StageReport, project_root: Path, bo2_root: Path | None) -> None:
    rel = Path("animtrees") / "fxanim_props.atr"
    dst = project_root / rel
    if dst.exists():
        return
    if bo2_root is None:
        report.errors.append("animtrees/fxanim_props.atr missing and --bo2 not given")
        return
    src = bo2_root / "raw" / rel
    if not src.exists():
        report.errors.append(f"{src} missing")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def map_scripts(project_root: Path, project: str) -> list[str]:
    """Every map-specific script staged for the project (asset names)."""
    found = []
    for folder, ext in (("maps/mp", ".gsc"), ("clientscripts/mp", ".csc")):
        for sub in ("", "createfx/", "createart/"):
            for path in sorted((project_root / folder / sub).glob(f"{project}*{ext}")):
                found.append(f"{folder}/{sub}{path.name}")
    found.extend(paths.traverse_scripts(project_root))
    found.extend(p.relative_to(project_root).as_posix() for p in
                 (project_root / 'aitype').glob('waw_*.gsc'))
    found.extend(p.relative_to(project_root).as_posix() for p in
                 (project_root / 'aitype/clientscripts').glob('waw_*.csc'))
    found.extend(gscport.bo2_scripts(project_root))
    found.extend(p.relative_to(project_root).as_posix() for p in
                 (project_root / 'clientscripts/mp/waw').glob('*.csc'))
    return found


def write_zone(stage: Path, project: str, images: list[str], ipak: bool,
               xmodels: list[str] = (), scripts: list[str] = (), zbarriers: list[str] = (),
               effects: list[str] = (), materials: list[str] = (), localizations: list[str] = (),
               rawfiles: list[str] = ()) -> Path:
    zone_dir = stage / "zone_source"
    zone_dir.mkdir(parents=True, exist_ok=True)
    lines = [
        ">game,T6",
        ">map,zm",
        ">level.@00012d3c,0",
    ]
    if ipak:
        lines += [f">level.ipak_read,{project}", f">ipak,{project}"]
    lines += [f"image,{name}" for name in images]
    # script_model entities reference these by name; nothing else pulls them in
    lines += [f"xmodel,{name}" for name in xmodels]
    lines += [f"zbarrier,{name}" for name in zbarriers]
    # converted WaW effects the scripts load; the bridge's FX loader pulls in
    # the effects, materials and models they reference
    lines += [f"fx,{name}" for name in effects]
    # HUD materials the scripts draw (setShader); nothing else references them
    lines += [f"material,{name}" for name in materials]
    lines += [f"script,{name}" for name in scripts]
    lines += [f"localize,{name}" for name in localizations]
    lines += [f"rawfile,{name}" for name in rawfiles]
    path = zone_dir / f"{project}.zone"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


SKY_TECHSETS = {"wc_sky"}
NON_RENDER_TECHSETS = {"wc_tools"}


def write_collision_edges(clip, path: Path) -> dict:
    """BSP/collisionedges.bin: WaW's compiled walkable bit per triangle edge.

    T6 counts a capsule contact on a walkable edge as ground whatever its
    slope (sub_881940 reads triEdgeIsWalkable; sub_6D8770 only falls back to
    normal.z >= 0.7 for unflagged contacts). The bridge reorders triangles, so
    each record carries its corners: uint32 count, then per triangle 9 float32
    corners in game coordinates and one byte holding the bits of edges 0..2.
    """
    if not clip.edge_walkable:
        return {"collision_edges_source": "none (pre-v6 dump; bridge keeps every edge walkable)"}
    per_triangle, _ = collision_material_slots(clip)
    records = bytearray()
    count = walkable = 0
    for t, slot in enumerate(per_triangle):
        if slot is None:
            continue
        bits = 0
        for e in range(3):
            bit = 3 * t + e
            bits |= ((clip.edge_walkable[bit >> 3] >> (bit & 7)) & 1) << e
        walkable += bin(bits).count("1")
        corners = [clip.vertices[i] for i in clip.indices[t * 3:t * 3 + 3]]
        records += struct.pack("<9fB", *corners[0], *corners[1], *corners[2], bits)
        count += 1
    path.write_bytes(struct.pack("<I", count) + bytes(records))
    return {"collision_edges_source": "waw", "collision_edges_walkable": walkable,
            "collision_edges_total": 3 * count}


def _layer_gap_stride(world, surface) -> int | None:
    """Bytes per vertex of ``surface``'s records in the WaW layer buffer."""
    later = [s.layer_data_offset for s in world.surfaces
             if s.world_vert_format != 0 and s.layer_data_offset > surface.layer_data_offset]
    gap = (min(later) if later else len(world.layer_data)) - surface.layer_data_offset
    return gap // surface.vertex_count if surface.vertex_count and gap % surface.vertex_count == 0 else None


def write_shadow_geometry(report: StageReport, world, mesh_of_surface: dict[int, int], path: Path) -> None:
    """BSP/shadowgeom.json: per WaW primary light, the static geometry its
    shadow map draws (GfxShadowGeometry: world meshes by FBX mesh index, static
    models by index - placements keep WaW order) and its light region hulls.
    The bridge linker fills T6 shadowGeom/lightRegion (same structures)."""
    if path.exists():
        path.unlink()
    if not world.shadow_lights:
        return
    lights = []
    dropped = 0
    for light in world.shadow_lights:
        meshes = sorted({mesh_of_surface[i] for i in light.surfaces if i in mesh_of_surface})
        dropped += sum(1 for i in light.surfaces if i not in mesh_of_surface)
        lights.append({"meshes": meshes, "smodels": sorted(set(light.smodels)),
                       "hulls": [{"kdopMidPoint": list(h.kdop_mid_point), "kdopHalfSize": list(h.kdop_half_size),
                                  "axes": [{"dir": list(d), "midPoint": m, "halfSize": s} for d, m, s in h.axes]}
                                 for h in light.hulls]})
    path.write_text(json.dumps({"lights": lights}, separators=(",", ":")) + "\n", encoding="utf-8")
    report.content["shadow_geometry"] = {"lights": len(lights), "casting_meshes": sum(len(l["meshes"]) for l in lights),
                                         "casting_smodels": sum(len(l["smodels"]) for l in lights),
                                         "removed_source_surfaces": dropped}


@progress.phase('Convert world geometry')
def stage_geometry(report: StageReport, world, clip, roots: list[Path], project_root: Path,
                   stock_waw=None) -> str | None:
    """World FBX (sky surfaces removed), terrain collision FBX, brushes.json."""
    bsp = project_root / "BSP"
    bsp.mkdir(parents=True, exist_ok=True)
    # A surface whose technique set was not loaded with the map zone (its
    # material lives in a companion or stock zone) has no recorded world vertex
    # format; its layer offset is valid, the format comes from the technique name.
    unknown = {s.material for s in world.surfaces if s.world_vert_format == 0xFF}
    if unknown and world.layer_data:
        resolver = assetresolve.Resolver(roots, stock_waw)
        resolver.expand({("material", name) for name in unknown})
        resolved = {}
        for name in unknown:
            src = _find(resolver.roots, techsets.oat_material_path(name))
            if src is not None:
                resolved[name] = techsets.world_vert_format(_load_json(src).get("techniqueSet") or "")
        # generated blends substituted later have no source technique here: the
        # layer buffer's record spacing gives their stride (world.layer_formats_from_strides)
        by_stride = layer_formats_from_strides(world)
        for surface in world.surfaces:
            if surface.material in resolved:
                surface.world_vert_format = resolved[surface.material]
            elif surface.index in by_stride:
                surface.world_vert_format = by_stride[surface.index]
        # A 24-byte stride is TEX_3_NRM_3 or TEX_4_NRM_1; a generated blend's
        # name lists its layer materials ("*59_60_61_7": four layers).
        for surface in world.surfaces:
            if surface.world_vert_format == 0xFF and surface.material.startswith("*"):
                layers = len(surface.material[1:].split("_"))
                stride_formats = {3: 5, 4: 6}
                if layers in stride_formats and _layer_gap_stride(world, surface) == 24:
                    surface.world_vert_format = stride_formats[layers]
        still = {s.material for s in world.surfaces if s.world_vert_format == 0xFF}
        if still:
            report.warnings.append(f"world vertex format unknown for {sorted(still)}; layers dropped")
    sky = set()
    tool_surfaces = set()
    blend_data = set()
    for name in {s.material for s in world.surfaces}:
        src = _find(roots, techsets.oat_material_path(name))
        if src is not None:
            technique = _load_json(src).get("techniqueSet")
            if technique in SKY_TECHSETS:
                sky.add(name)
            elif technique in NON_RENDER_TECHSETS:
                tool_surfaces.add(name)
            elif len(techsets.parse(technique or "").layers) > 1:
                blend_data.add(name)
        elif name.startswith("*"):
            # dangling generated blend (substituted later): also a layered blend
            blend_data.add(name)
    # A v5 render dump carries the WaW vertex layer buffer: layered materials
    # then keep every layer (T6 layered techniques read it from vd1).
    techsets.LAYERED_VERTEX_DATA = bool(world.layer_data)
    if not techsets.LAYERED_VERTEX_DATA and blend_data:
        report.warnings.append(f"{len(blend_data)} layered materials drawn as their base layer; their vertex colour "
                               f"(blend weights) is written as white (re-extract gfxworld with the v5 T4 dumper)")
    else:
        blend_data = set()
    kept = [s for s in world.surfaces if s.material not in sky | tool_surfaces]
    if world.brush_model_count is None and len(clip.submodels) > 1:
        raise StageError('render dump lacks brush ownership; re-extract gfxworld with the v4 T4 dumper')
    if world.brush_model_count is not None and world.brush_model_count != len(clip.submodels):
        raise StageError('render and collision brush model counts differ')
    if any(s.brush_model >= len(clip.submodels) for s in kept if s.brush_model):
        raise StageError('render brush ownership exceeds collision submodel count')
    report.content['brush_render'] = {
        'source_surfaces': sum(bool(s.brush_model) for s in world.surfaces),
        'rendered_surfaces': sum(bool(s.brush_model) for s in kept),
        'models': len({s.brush_model for s in kept if s.brush_model}),
    }
    if sky:
        report.warnings.append(
            f"{sum(s.material in sky for s in world.surfaces)} sky surfaces ({', '.join(sorted(sky))}) removed from the render "
            f"world; T6 draws the sky with the skybox xmodel")
    if tool_surfaces:
        report.warnings.append(
            f"{sum(s.material in tool_surfaces for s in world.surfaces)} WaW editor-only surfaces "
            f"({', '.join(sorted(tool_surfaces))}) removed from the render world")
    world.surfaces = kept
    mesh_of_surface = write_world_fbx(world, bsp / "map_gfx.fbx", frozenset(blend_data))
    write_shadow_geometry(report, world, mesh_of_surface, bsp / "shadowgeom.json")
    slot_materials = write_collision_fbx(clip, bsp / "map_col.fbx")
    # BSP/clipmaterials.json: FBX collision material -> WaW clip flags. The
    # bridge linker gives every terrain partition its own clip material.
    clip_materials = [{"fbx": "waw_collision", "name": "waw_collision", "contentFlags": 1, "surfaceFlags": 0}
                      if m < 0 else
                      {"fbx": f"clip_{m}", "name": clip.materials[m].name,
                       "contentFlags": hulls._signed(clip.materials[m].content_flags),
                       "surfaceFlags": hulls._signed(clip.materials[m].surface_flags)} for m in slot_materials]
    (bsp / "clipmaterials.json").write_text(json.dumps({"materials": clip_materials}, indent=1) + "\n", encoding="utf-8")
    edge_summary = write_collision_edges(clip, bsp / "collisionedges.bin")
    brushes, summary = hulls.collision_brushes(clip)
    (bsp / "brushes.json").write_text(json.dumps({"brushes": brushes}, separators=(",", ":")) + "\n",
                                      encoding="utf-8")
    subs = hulls.submodel_records(clip)
    (bsp / "submodels.json").write_text(json.dumps({"submodels": subs}, separators=(",", ":")) + "\n",
                                        encoding="utf-8")
    summary["collision_triangle_materials"] = len(clip_materials)
    summary["collision_triangles_noncolliding_dropped"] = sum(
        1 for m in clip.triangle_materials if m == 0xFFFF or not clip.materials[m].content_flags)
    sheets = oneway.one_way_sheets(clip)
    summary["collision_triangles_one_sided_removed"] = len(sheets)
    if sheets:
        kinds = Counter(s["material"] for s in sheets)
        report.warnings.append(f"ONE_WAY_COLLISION_REMOVED {len(sheets)} one-sided traversal triangles: "
                               f"collision omitted to allow passage in both directions "
                               f"({', '.join(f'{k} {v}' for k, v in kinds.most_common())})")
    summary.update(edge_summary)
    summary["submodels"] = len(subs)
    summary["empty_submodels"] = sum(1 for s in subs if not s["brushes"])
    static_models, static_summary = hulls.static_model_records(clip)
    (bsp / "staticmodels.json").write_text(json.dumps({"staticModels": static_models}, indent=1) + "\n",
                                           encoding="utf-8")
    summary.update(static_summary)
    if static_summary["static_model_triangles_outside_waw_bounds"]:
        report.errors.append(f"{static_summary['static_model_triangles_outside_waw_bounds']} static model collision "
                             f"triangles fall outside their WaW bounds: transform mismatch")
    report.collision = summary
    # the WaW sky cubemap, reused on the T6 skybox dome
    for name in sorted(sky):
        src = _find(roots, techsets.oat_material_path(name))
        for tex in _load_json(src).get("textures", []):
            if tex.get("name") == "colorMap" and tex.get("image"):
                return tex["image"]
    return None


def stage_bridge(stage: Path, project: str, gfx_bin: Path, clip_bin: Path, stock_materials: Path,
                 techset_dump: Path | None, bo2_root: Path | None,
                 template_root: Path | None = None, template_name: str | None = None,
                 extra_roots: list[Path] | None = None, ipak: bool = True,
                 iwd_dirs: list[Path] | None = None, waw_map_script: Path | None = None,
                 waw_script_roots: list[Path] | None = None, waw_stock_scripts: Path | None = None,
                 t6_unlinker: Path | None = None, fx_fallback: bool = False,
                 waw_root: Path | None = None, t4_unlinker: Path | None = None,
                 waw_stock_dumps: Path | None = None, waw_mod_tools: Path | None = None,
                 waw_source_dumps: Path | None = None, wavelet_binary: Path | None = None,
                 audio_decoder: Path | None = None, xwma_decoder: Path | None = None,
                 t6_sound_driver: Path | None = None, approximate_sound_curves: bool = False,
                 bo2_stock_perks: bool = False) -> StageReport:
    """Stage everything the bridge links. ``extra_roots`` are unlinker dumps of
    the zones WaW loads with the map (mod.ff, common.ff): references the map
    zone does not define resolve there, in that order."""
    report = StageReport(project)
    project_root = stage / "zone_raw" / project
    project_root.mkdir(parents=True, exist_ok=True)
    roots = [stage, *(extra_roots or [])]
    compiled_weapon_names = set(weapons.discover(roots))
    compiled_sound_names = {p.relative_to(r / "soundaliases").as_posix().removesuffix(".w2bsnd.json")
                            for r in roots for p in (r / "soundaliases").rglob("*.w2bsnd.json")}
    stock_waw = None
    if waw_root is not None and t4_unlinker is not None:
        stock_waw = wawassets.StockWawAssets(waw_root, t4_unlinker, waw_stock_dumps or stage / "waw_stock_dumps")
        stock_waw.load()
    image_iwds = [*(iwd_dirs or []), *([waw_root / "main"] if waw_root else [])]
    wavelets = wavelet.IwdRecovery(image_iwds, wavelet_binary, project_root / "wavelet_src")
    # Only files recovered this run are visible; old DDS files are not a cache.
    # Do not add the recovery root to general lookup paths until dependencies
    # have been decoded; stage_images calls recover independently below.

    world = progress.timed('Read map world', read_gfx_world, gfx_bin)
    clip = progress.timed('Read map collision', read_collision, clip_bin)
    sky_image = stage_geometry(report, world, clip, roots, project_root, stock_waw)
    ents_file = source_entities(gfx_bin, stage, roots)
    script_models: set[str] = set()
    # map-placed script_model entities: WaW collides each one (see hulls.SCRIPT_MODEL_CONTENTS)
    entity_box_models: set[str] = set()
    animscripts: dict[str, str] = {}
    paths_file = gfx_bin.parent / f"{gfx_bin.name.removesuffix('.gfx.bin')}.paths.json"
    if paths_file.exists():
        try:
            # the zombie actor the converted spawners use (entities.ZOMBIE_ACTOR_CLASS)
            asd = bo2_root / "raw" / "animstatedefs" / "zm_nuked_basic.asd" if bo2_root is not None else None
            path_summary, animscripts = paths.stage_paths(paths_file, project_root / "BSP", project_root,
                                                          asd if asd is not None and asd.exists() else None)
            report.warnings.append(f"path nodes: {path_summary.nodes} nodes, {path_summary.links} links, "
                                   f"vis {path_summary.vis_bytes} bytes; traversals {path_summary.traversals}")
        except paths.PathError as exc:
            report.errors.append(f"path nodes: {exc}")
    else:
        report.errors.append(f"path nodes {paths_file.name} missing (dump gameworldsp): zombies cannot path")
    if ents_file.exists():
        start_zones, zone_names = None, set()
        if waw_map_script is not None and waw_map_script.exists():
            start_zones, adjacency = zones.read_waw_zones(waw_map_script.read_text(encoding="utf-8", errors="replace"))
            zone_names = zones.zone_names(start_zones, adjacency)
        try:
            report.entities, script_models = entities.write_entities(ents_file, project, project_root / "BSP",
                                                                     clip, start_zones, animscripts, zone_names)
        except ValueError as exc:
            raise StageError(f"map entities: {exc}") from exc
        synthesized = report.entities.get("synthesized_zones")
        if synthesized:
            report.warnings.append(
                f"ZONES_SYNTHESIZED the WaW map has no add_adjacent_zone graph: {len(synthesized['groups'])} spawner "
                f"group(s) became map-wide zones, opened by the doors that add them in WaW "
                f"(initial {synthesized['initial']}, {len(synthesized['adjacency'])} door-unlocked)")
            if synthesized["never_unlocked"]:
                report.warnings.append(f"ZONE_SPAWNERS_UNREACHED {synthesized['never_unlocked']}: no door or debris "
                                       f"adds these spawners; they stay inactive unless the map script enables them")
        entity_box_models = set(script_models)
        if (project_root / "BSP" / "paths.json").exists():
            report.warnings += entities.link_barrier_traversals(project_root / "BSP")
    else:
        report.errors.append(f"map entities {ents_file.name} missing")
    effects = EffectsResult()
    source_waw = None
    weapon_report = None
    if waw_map_script is not None and bo2_root is not None:
        if waw_mod_tools is not None and waw_root is not None and t4_unlinker is not None:
            source_waw = wawsource.WawSourceAssets(waw_mod_tools, waw_root, t4_unlinker,
                                                   waw_source_dumps or stage / "waw_source_dumps")
            problems = source_waw.check()
            if problems:
                report.errors.append(f"WaW Mod Tools sources unusable: {'; '.join(problems)}")
                source_waw = None
        script_models |= port_scripts(report, stage, project_root, waw_map_script, bo2_root, waw_script_roots or [],
                                      iwd_dirs or [], waw_stock_scripts, t6_unlinker, roots, clip,
                                      stock_waw, source_waw, bo2_stock_perks=bo2_stock_perks,
                                      stock_variant_roots=[r / "raw" for r in (waw_mod_tools, waw_root) if r is not None])
        fx_names = list(report.scripts.get("fx", [])) if report.scripts else []
        # Weapons first: their resolution widens the roots (stock WaW dumps for
        # script models too) and their effects join the map zone's conversion.
        weapon_report, roots = _stage_weapons(report, roots, project_root, compiled_weapon_names, script_models,
                                              stock_waw, compiled_sound_names)
        weapon_fx = sorted(set(weapon_report["dependencies"].get("fx", [])) - set(fx_names)) \
            if weapon_report else []
        if stock_waw is None:
            report.warnings.append("no --waw-root/--t4-unlinker: assets the map only references cannot be "
                                   "looked up in the stock WaW zones")
        effects = stage_effects(report, fx_names + weapon_fx, roots, stock_waw, project_root, stock_materials,
                                techset_dump,
                                bo2_root, [*(iwd_dirs or []), *([waw_root / "main"] if waw_root else [])],
                                source_waw, wavelets)
        compiled_sound_names |= effects.sounds
        roots = [*roots, *effects.extra_roots]
        script_models |= effects.models
        api = t6api.build(bo2_root, stage / "t6api_cache.json", t6_unlinker)
        stage_fx(report, fx_names, effects.table, waw_map_script.stem, bo2_root, api, project_root, fx_fallback)
        # script animations (stage_core_animtrees) load from mod.ff, before the map's
        # scripts; the game resolves #using_animtree through the animtree rawfile
        # (without it: ERR_DROP "unknown anim tree" while the map loads)
        xanims = report.scripts.get("staged_xanims", []) if report.scripts else []
        trees = report.scripts.get("staged_animtrees", []) if report.scripts else []
        if xanims:
            with (project_root / MOD_EXTRA_ZONE).open("a", encoding="utf-8") as zone:
                zone.write("".join(f"rawfile,animtrees/{t}.atr\n" for t in trees))
                zone.write("".join(f"xanim,{n}\n" for n in xanims))
    # clipmap static models reference their xmodel (collSurfs) in the map zone
    clip_models = {m.name for m in clip.static_models if m.contents and m.surfaces}
    # Entity placements need the same complete lookup as script-only models.
    resolved = recover_script_models(report, script_models | clip_models | {m.name for m in world.static_models},
                                     roots, stock_waw, source_waw)
    entities_json = project_root / "BSP" / "entities.json"
    if entities_json.exists():
        # An entity naming a model no WaW zone or source has (b01's script
        # models "weapons/sp/bar": weapon paths typed as models): WaW loads
        # its default model. Keep the entity for scripts, without a model.
        absent = entities.placed_models(entities_json) - resolved - clip_models - \
            {m.name for m in world.static_models}
        if absent:
            count = entities.strip_models(entities_json, absent)
            script_models -= absent
            entity_box_models -= absent
            report.warnings.append(f"ENTITY_MODEL_MISSING {sorted(absent)}: in no WaW zone or source; "
                                   f"{count} entities keep no model (WaW draws its default model)")
    model_materials = stage_models(report, world, project, stage, project_root, script_models | clip_models, roots,
                                   entity_box_models)
    projectile_models = projectilecollision.recover(report, world, clip, project_root, roots)
    fx_models = stage_fx_models(report, project_root, effects.models)
    hud_materials = sorted(report.scripts.get("hud_materials", [])) if report.scripts else []
    names = {s.material for s in world.surfaces} | model_materials | set(hud_materials)
    weapon_materials = set(weapon_report['models']['materials']) if weapon_report else set()
    if weapon_report:
        weapon_materials.update(weapon_report['dependencies'].get('material', []))
    roots, native_fallbacks = recover_material_sources(report, names | weapon_materials, roots,
                                                      stock_waw, source_waw, stock_materials, bo2_root)
    # Primary lights keep their shadow maps only with the WaW shadow geometry
    # (lighting.stage_primary_lights); otherwise the shadowed techniques are unreachable.
    primary_lights = Path(str(gfx_bin).removesuffix(".gfx.bin") + ".primarylights.json")
    falloff_placement = shaderruntime.light_falloff_placement(primary_lights, roots)
    report.content['light_falloff_placement'] = falloff_placement
    used = stage_materials(report, names, roots, stock_materials, project_root, techset_dump,
                           native_fallbacks=native_fallbacks, falloff_placement=falloff_placement,
                           light_shadows=not primary_lights.exists() or bool(world.shadow_lights))
    used |= effects.techsets
    hud_images = {t["image"] for m in report.materials if m["name"] in set(hud_materials)
                  for t in _load_json(project_root / m["file"]).get("textures", []) if t.get("image")}
    if hud_images:
        write_image_streaming(project_root, hud_images, merge=True)
    # Lightmap pages in both encodings; world surfaces whose material runs
    # translated WaW lit programs get the WaW-encoded copy (by FBX material name).
    waw_lightmap_materials = {m.get('source', m['name']) for m in report.materials
                              if m.get('shader_runtime', {}).get('lightmap') == 'waw'}
    lightmap_report = progress.timed('Convert lightmaps', lightmaps.stage, world, [r / 'images' for r in roots], project_root, waw_lightmap_materials)
    report.errors += lightmap_report['errors']
    report.warnings += lightmap_report['warnings']
    report.content['lightmaps'] = {'pages': lightmap_report['pages'],
                                   'waw_encoded_materials': len(waw_lightmap_materials)}
    _SKY_IMAGE_ROOTS[:] = [r / "images" for r in roots]
    source_sky_model = next((e.get("skyboxmodel") for e in entities.parse_entities(ents_file.read_text())
                             if e.get("classname") == "worldspawn"), None) if ents_file.exists() else None
    if source_sky_model and bo2_root and techset_dump:
        sky_techset = stage_source_skybox(report, project, project_root, roots, source_sky_model, techset_dump, bo2_root)
        sky_image = None
    else:
        sky_techset, sky_image = stage_skybox(report, project, project_root, bo2_root, sky_image)
    if sky_techset:
        used.add(sky_techset)
        used.update(m["t6_techset"] for m in report.materials
                    if m["name"] in report.content.get("sky", {}).get("materials", []))
    images = stage_images(report, [project_root / SKY_SRC_DIR, *[r / "images" for r in roots]], project_root,
                          image_iwds,
                          [sky_image] if sky_image else [], wavelets)
    verify_techsets(report, used, techset_dump, project_root)
    if bo2_root is not None and (bo2_root / "mods" / "zm_test").exists():
        stage_template_scripts(report, project, project_root, bo2_root / "mods" / "zm_test", "zm_test")
    stage_scripts(report, project, project_root, template_root, template_name)
    for script_name in map_scripts(project_root, project):
        script_path = project_root / script_name
        source = script_path.read_text(encoding="utf-8", errors="replace")
        if "zm_test" in source:
            script_path.write_text(source.replace("zm_test", project), encoding="utf-8")
            report.warnings.append(f"template references renamed in {script_name}")
    main_gsc = project_root / "maps" / "mp" / f"{project}.gsc"
    if waw_map_script is not None and main_gsc.exists():
        try:
            initial, links = zones.apply(waw_map_script, main_gsc, project,
                                         (report.entities or {}).get("synthesized_zones"))
            report.warnings.append(f"zone graph from {waw_map_script.name}: start zones {initial}, {links} links")
        except zones.ZoneError as exc:
            report.errors.append(f"zones: {exc}")
    else:
        report.errors.append("no WaW map script given; the BO2 map script would keep the template's single zone")
    if main_gsc.exists():
        source = main_gsc.read_text(encoding="utf-8", errors="replace")
        marker = "    master_switch notsolid();"
        guard = "    if ( !isdefined( trig ) || !isdefined( master_switch ) )\n        return;\n"
        if marker in source and guard not in source:
            main_gsc.write_text(source.replace(marker, guard + marker, 1), encoding="utf-8")
            report.warnings.append("zm_test electric_switch guarded: WaW map has no template switch entities")
    if report.scripts and main_gsc.exists():
        try:
            gscport.hook_bo2_main(main_gsc, waw_map_script.stem)
            if report.scripts.get("characters"):
                gscport.hook_bo2_characters(main_gsc)
            gscport.hook_bo2_weapons(main_gsc)
            gscport.hook_bo2_box(main_gsc)
            if bo2_root is None:
                raise ValueError("BO2 runtime sources required to stage WaW perk ownership")
            gscport.stage_bo2_perk_support(project_root, bo2_root, bo2_stock_perks=bo2_stock_perks)
            if bo2_stock_perks:
                from . import stockperks
                stockperks.translate_entities(project_root / 'BSP/entities.json')
                report.warnings.append('BO2_STOCK_PERKS: opted in to native BO2 perk purchases, HUD and last stand')
            gscport.hook_bo2_perk_server(main_gsc)
            gscport.hook_bo2_perk_client(project_root / "clientscripts" / "mp" / f"{project}.csc")
            gscport.hook_bo2_animtrees(main_gsc, project_root / "clientscripts" / "mp" / f"{project}.csc", project_root,
                                       report.scripts.get("staged_animtrees", []))
        except ValueError as exc:
            report.errors.append(f"scripts: {exc}")
    # A cancelled Wine/Linker run can leave the generated client entry point
    # absent while the compile list still expects it. Recreate it now.
    if bo2_root is not None:
        try:
            gscport.ensure_owned_client_bootstrap(project_root, bo2_root)
        except ValueError as exc:
            report.errors.append(f"scripts: {exc}")
    stage_rawfiles(report, project_root, bo2_root)
    if weapon_report is None:
        weapon_report, roots = _stage_weapons(report, roots, project_root, compiled_weapon_names, script_models,
                                              stock_waw, compiled_sound_names)
    if weapon_report is not None:
        _stage_weapon_runtime(report, project, project_root, roots, stock_materials, techset_dump, bo2_root,
                              t6_unlinker, stage, wavelets, set(effects.table),
                              waw_map_script.stem if waw_map_script is not None else None, native_fallbacks)
    shader_report = progress.timed('Translate shaders', shaders.stage, roots, project_root / 'content_source/shaders')
    report.content['shaders'] = {k: v for k, v in shader_report.items() if k != 'shaders'}
    report.content['shaders']['report'] = 'content_source/shaders/stage.json'
    runtime_materials = list(report.materials)
    weapon_visuals = project_root/'content_source/weapons.visuals.json'
    if weapon_visuals.is_file():
        runtime_materials += json.loads(weapon_visuals.read_text())['materials']
    active = [p for m in runtime_materials for p in m.get('shader_runtime', {}).get('active', [])]
    report.content['shaders']['runtime'] = {
        'active_materials': sum(bool(m.get('shader_runtime', {}).get('active')) for m in runtime_materials),
        'pixel_passes': len(active),
        'vertex_passes': sum(bool(p.get('vertex_shader')) for p in active),
        'unique_shaders': len({s for p in active for s in (p['shader'], p.get('vertex_shader')) if s}),
    }
    if shader_report['shaders']:
        report.warnings.append(f"CONTENT_INCOMPLETE shaders: {shader_report['translated']} translated to SM5; "
                               f"{shader_report['unsupported']} translation failures; {len(active)} runtime pixel passes bound; "
                               "remaining programs require supported T6 pass adapters")
    # No loose raw/ visions: WaW (fastfile mode) reads visions only from loaded
    # zones and replaces a missing one with vision/default (CoDWaW sub_4629F0).
    vision_report = progress.timed('Convert vision and glow', visions.stage, project_root,
        [*([waw_map_script.parent.parent] if waw_map_script else []),
         *(waw_script_roots or []), *roots, *([waw_stock_scripts] if waw_stock_scripts else [])],
        sorted({f for d in (iwd_dirs or []) for f in d.glob('*.iwd')}), stock_waw,
        visions.lut_donor(techset_dump), None, techset_dump)
    report.errors += vision_report['errors']
    overlay = vision_report.get('overlay') or {}
    for name in overlay.get('materials', []):
        report.materials.append({'name': name, 'file': techsets.oat_material_path(name).as_posix(),
                                 't6_techset': 'waw film/glow overlay', 'notes': ['WaW film + glow pass (glow.py)']})
    report.warnings += [f'GLOW {vision}: {note}' for vision, notes in overlay.get('notes', {}).items() for note in notes]
    hud_materials = [*hud_materials, *overlay.get('materials', [])]
    report.warnings += [f'VISION_ABSENT {name}: absent from WaW lookup' for name in vision_report['missing']]
    report.warnings += [f'VISION_DEFAULT {name}: in no WaW zone; WaW loads vision/default instead, so does the port'
                        for name in vision_report.get('default_fallback', [])]
    if vision_report['visions']:
        images.append('waw_vision_lut')
    client_dir = project_root / 'clientscripts/mp/waw'
    client_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(gscport.COMPAT_DIR / '_waw2bo2_environment.csc', client_dir / '_waw2bo2_environment.csc')
    client_main = project_root / 'clientscripts/mp' / f'{project}.csc'
    client_text = client_main.read_text(encoding='utf-8')
    # The donor's fog-volume monitor selects its own vision every frame.
    # Converted WaW scripts own both fog and vision changes instead.
    client_text = client_text.replace('    level thread init_fog_vol_to_visionset();\n', '')
    hook = '    clientscripts\\mp\\waw\\_waw2bo2_environment::init();\n'
    if hook.strip() not in client_text:
        client_text = client_text.replace('    start_zombie_stuff();', '    start_zombie_stuff();\n' + hook, 1)
    client_main.write_text(client_text, encoding='utf-8')
    sound_resolver = assetresolve.Resolver(roots, stock_waw)
    defined_sounds = compiled_sound_names | ({n for kinds in stock_waw.index.values()
        for n in kinds.get('sound', [])} if stock_waw else set())
    script_sounds = sounds.script_aliases(project_root, defined_sounds)
    compiled_sound_names.update(script_sounds)
    report.content['script_sound_dependencies'] = sorted(script_sounds)
    sound_graph = progress.timed('Resolve sound dependencies', sound_resolver.expand, {("sound", n) for n in compiled_sound_names})
    sound_names = {n["name"] for n in sound_graph["nodes"] if n["kind"] == "sound"}
    sound_report = progress.timed('Stage sound sources', sounds.stage, sound_resolver.roots, project_root / "content_source", image_iwds,
                                stock_waw, sound_names)
    audio_report = progress.timed('Decode audio', audio.stage, project_root / "content_source", project_root / "content_source/pcm", audio_decoder, xwma_decoder)
    binding_report = sounds.bind_pcm(project_root / "content_source", project_root / "content_source/pcm")
    if t6_sound_driver is None and techset_dump is not None:
        t6_sound_driver = techset_dump / "sounddriverglobals/singleton.w2bsdg"
    curve_report = sounds.bind_curves(project_root / "content_source", t6_sound_driver, approximate_sound_curves)
    bank = f"waw_{project}.all"
    budget = sounds.loaded_budget(bo2_root, bo2_root / "mods" / "zm_test" / "soundbank" / "zmb_test.all.aliases.csv") \
        if bo2_root is not None else None
    bank_report = progress.timed('Build sound bank', sounds.write_t6_bank, project_root / "content_source", bank, project_root / "soundbank", budget)
    write_amb_csc(report, project, project_root)
    if budget is not None:
        report.warnings.append(
            f"SOUND_LOADED_TO_STREAMED {budget['streamed_files']} WaW loaded sounds streamed: loaded-bank budget "
            f"{budget['entries']} files / {budget['bytes'] // 2**20} MB (largest stock map bank "
            f"{budget['reference_bank']} minus the template bank); kept {budget['loaded_files']} loaded files, prioritizing weapon dependencies "
            f"({budget['loaded_bytes'] // 2**20} MB). List: content_source/sounds.bank.json")
    if bank_report["variants"]:
        with (project_root / MOD_EXTRA_ZONE).open("a", encoding="utf-8") as zone:
            zone.write(f"soundbank,{bank}\n")
    report.content["sound_bank"] = {"status": bank_report["status"], "bank": bank,
                                    "aliases": bank_report["aliases"], "variants": bank_report["variants"],
                                    "not_translated": bank_report["not_translated"],
                                    "clamped_to_t6_range": len(bank_report["clamped"]),
                                    "errors": len(bank_report["errors"]), "report": "content_source/sounds.bank.json"}
    (project_root / "content_source/sounds.dependencies.json").write_text(
        json.dumps(sound_graph, indent=2) + "\n", encoding="utf-8")
    report.content["sound_aliases"] = {"status": sound_report["status"],
                                       "aliases": len(sound_report["aliases"]), "audio": len(sound_report["audio"]),
                                       "errors": sound_report["errors"], "report": "content_source/sounds.stage.json"}
    report.content["audio"] = {"status": audio_report["status"],
                                "pcm_files": sum("output" in a for a in audio_report["audio"]),
                                "errors": audio_report["errors"], "report": "content_source/pcm/audio.stage.json"}
    report.content["visions"] = vision_report
    report.content["sound_bindings"] = {"status": binding_report["status"],
                                        "bound_variants": binding_report["bound_variants"],
                                        "errors": binding_report["errors"],
                                        "report": "content_source/sounds.bindings.json"}
    report.content["sound_curves"] = {"status": curve_report["status"],
                                      "complete_variants": curve_report["complete_variants"],
                                      "incomplete_variants": curve_report["incomplete_variants"],
                                      "errors": curve_report["errors"], "report": "content_source/sounds.curves.json"}
    for entry in curve_report["curves"].values():
        if entry["status"] != "exact_shape":
            report.warnings.append(f"{entry['status']} {entry['source']}: {entry['uses']} alias curve slot(s); "
                                   f"nearest stock T6 curve {entry['nearest']['t6']} "
                                   f"(max error {entry['nearest']['maxError']:.4f})")
    lighting_base = str(gfx_bin).removesuffix(".gfx.bin")
    grid_file = Path(lighting_base + ".lightgrid.bin")
    if grid_file.exists():
        report.content["lighting"] = lighting.stage_grid(grid_file, project_root / "BSP/lightgrid.bin")
        sun_file = Path(lighting_base + ".lighting.json")
        if sun_file.exists():
            shutil.copy2(sun_file, project_root / "BSP/lighting.json")
            # WaW loads the sun into r_lightTweak* dvars (sub_6FCDE0) and rebuilds
            # it whenever a script changes one (sub_6F6680): the values a map
            # script sets are the sun WaW draws.
            sun_dvars = {"r_lighttweaksunlight": "sunLight", "r_lighttweakambient": "ambientScale",
                         "r_lighttweakdiffusefraction": "diffuseFraction"}
            script_values = {sun_dvars[k]: float(v) for k, v in vision_report.get("script_render_dvars", {}).items()
                             if k in sun_dvars}
            if script_values:
                staged = json.loads((project_root / "BSP/lighting.json").read_text(encoding="utf-8"))
                staged["sun"].update(script_values)
                (project_root / "BSP/lighting.json").write_text(json.dumps(staged, indent=2) + "\n", encoding="utf-8")
                report.content["lighting_script_sun"] = script_values
        primary_file = Path(lighting_base + ".primarylights.json")
        if not primary_file.exists():
            report.errors.append("source LightGrid requires its matching primary-light table")
        else:
            unshadowed = []
            definitions = lighting.stage_primary_lights(primary_file, project_root / "BSP/primarylights.json",
                                                        unshadowed, keep_shadows=bool(world.shadow_lights))
            if unshadowed:
                report.warnings.append(f"{len(unshadowed)} primary spot/omni lights drawn without shadow maps "
                                       "(render dump has no shadow geometry: re-extract gfxworld with the v6 T4 dumper)")
            for name in sorted(definitions):
                source_def = _find(roots, Path(f"lightdef/{name}.json"))
                if source_def is None:
                    report.errors.append(f"source light definition {name} is missing; re-extract lightdef assets")
                    continue
                definition = _load_json(source_def)
                image_name = definition["attenuation"].removeprefix(",")
                if image_name:
                    source_image = _find(roots, Path(f"images/{image_name}.dds"))
                    if source_image is None:
                        report.errors.append(f"light attenuation image {image_name} is missing from source dumps")
                        continue
                    output_image = "waw_light/" + image_name
                    image_path = project_root / f"images/{output_image}.iwi"
                    image_path.parent.mkdir(parents=True, exist_ok=True)
                    image_path.write_bytes(iwi.dds_to_iwi(source_image.read_bytes()))
                    definition["attenuation"] = output_image
                    images.append(output_image)
                definition["name"] = "waw_light/" + name
                output_def = project_root / f"lightdef/waw_light/{name}.json"
                output_def.parent.mkdir(parents=True, exist_ok=True)
                output_def.write_text(json.dumps(definition, indent=1) + "\n", encoding="utf-8")
        destination = project_root / "content_source/lighting"
        destination.mkdir(parents=True, exist_ok=True)
        for suffix in (".lightgrid.bin", ".lighting.json", ".primarylights.json"):
            original = Path(lighting_base + suffix)
            if original.exists():
                shutil.copy2(original, destination / original.name)
        pages = len(report.content.get("lightmaps", {}).get("pages", []))
        report.warnings.append(f"WaW LightGrid staged with {pages} lightmap pages; T4 corner obstruction traces "
                               "require a separate runtime adapter because T6 stores scalar visibility")
    zbarriers = []
    stock_barrier = techset_dump / "zbarrier" / entities.ZBARRIER_ASSET if techset_dump else None
    if stock_barrier is not None and stock_barrier.exists():
        target = project_root / "zbarrier" / entities.ZBARRIER_ASSET
        target.parent.mkdir(parents=True, exist_ok=True)
        barrier_text = stock_barrier.read_text(encoding="utf-8", errors="replace")
        # The bridge has no T6 FX loader. The stock barrier's repair effects
        # would make the otherwise valid barrier fail to link.
        barrier_text = barrier_text.replace(r"\impacts/fx_large_woodhit", "\\")
        target.write_text(barrier_text, encoding="utf-8")
        report.warnings.append("stock T6 barrier repair FX omitted (bridge has no T6 FX loader)")
        # zm_test's mod.ff already owns this asset. Including it again in the
        # map zone produces a runtime "Attempting to override asset" error.
        report.warnings.append("T6 barrier asset supplied by zm_test mod.ff")
    else:
        report.errors.append(f"stock T6 zbarrier {entities.ZBARRIER_ASSET} missing from techset dump")
    model_overlay_fx = stage_model_overlay_fx(report, project_root)
    scripts = map_scripts(project_root, project)
    if report.scripts and bo2_root is not None:
        # Validate the completed graph, including owned perk/bootstrap modules.
        # port_scripts runs before these dependencies are generated.
        api = t6api.build(bo2_root, stage / 't6api_cache.json', t6_unlinker)
        report.errors += [f'scripts: link: {e}' for e in gscport.link_check(project_root, api)]
    localization_report = progress.timed('Stage localization', localization.stage, project_root, roots, iwd_dirs or [], stock_waw,
        t4_unlinker, stage / 'localization_dumps',
        [root / 'raw' for root in (waw_mod_tools, waw_root) if root is not None], bo2_root,
        t6_unlinker=t6_unlinker, t6_stock_dump=techset_dump)
    report.content['localization'] = localization_report
    report.errors += localization_report['errors']
    report.warnings += localization_report['warnings']
    with (project_root / MOD_EXTRA_ZONE).open('a', encoding='utf-8') as zone:
        zone.write(f"localize,{localization_report['asset']}\n")
        if bo2_stock_perks:
            zone.write('// Opt-in stock BO2 perk dependencies\n')
            zone.write(''.join(line + '\n' for line in stockperks.asset_lines(project_root)))
    staged = {m["name"] for m in report.materials}
    from . import rumbles
    rumble_report = rumbles.stage(project_root,
        [*roots, *[r / 'raw' for r in (waw_mod_tools, waw_root) if r is not None]],
        bo2_root / 'raw' if bo2_root else None)
    report.content['rumbles'] = rumble_report
    report.errors += [f'RUMBLE_DEPENDENCY_MISSING {name}' for name in rumble_report['missing']]
    write_zone(stage, project, images, ipak, sorted(script_models | fx_models | projectile_models), scripts, zbarriers, effects.zone_fx + model_overlay_fx,
               [m for m in hud_materials if m in staged], [localization_report['asset']], rumble_report['files'])

    (project_root / "bridge_stage.report.json").write_text(report.to_json(), encoding="utf-8")
    return report


MOD_EXTRA_ZONE = "mod_extra.zone"
FX_MATERIAL_PREFIX = "waw_fx/"
# Effect images are renamed too: 38 of Nuketown's 68 effect images share a name
# with a stock BO2 image in common_zm, which loads first, so the game drew BO2's
# texture (another size and atlas layout) under the WaW UVs.
FX_IMAGE_PREFIX = "waw_fx/"
# Measured on every stock effect image of zm_nuked/common_zm (125/125) and on
# the HUD (trivial 2D) images (174/181, the rest tiny inline): they use
# streaming mode 2. Mode 1 (world/model images) is only made resident by the
# world streamer, which never requests effect or HUD images (effects drew a
# fallback, HUD icons nothing).
EFFECT_IMAGE_STREAMING = 2


def write_image_streaming(project_root: Path, images: set[str], merge: bool = False) -> Path:
    """images/streaming.json for the bridge linker's image loader. ``merge``
    adds to the file this run already wrote (effects first, HUD materials later)."""
    path = project_root / "images" / "streaming.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    modes = json.loads(path.read_text(encoding="utf-8"))["streamingMode"] if merge and path.exists() else {}
    modes.update({name: EFFECT_IMAGE_STREAMING for name in images})
    path.write_text(json.dumps({"streamingMode": dict(sorted(modes.items()))}, indent=2) + "\n", encoding="utf-8")
    return path


@dataclass
class EffectsResult:
    table: dict[str, str] = field(default_factory=dict)  # script-loaded WaW effect -> converted name
    zone_fx: list[str] = field(default_factory=list)  # fx lines for the map zone
    models: set[str] = field(default_factory=set)
    extra_roots: list[Path] = field(default_factory=list)  # stock WaW zone dumps used
    techsets: set[str] = field(default_factory=set)
    sounds: set[str] = field(default_factory=set)


@progress.phase('Convert effects')
def stage_effects(report: StageReport, names: list[str], roots: list[Path], stock_waw, project_root: Path,
                  stock_materials: Path, techset_dump: Path | None, bo2_root: Path,
                  iwd_dirs: list[Path] = (), source_waw=None,
                  wavelets: wavelet.IwdRecovery | None = None) -> EffectsResult:
    """Convert the WaW effects the scripts load (and everything they reference)
    to T6 (fx.py), with their materials (fxmaterials.py / world techsets),
    models and images. Output names carry fx.OUTPUT_PREFIX / FX_MATERIAL_PREFIX
    so no converted asset overrides a stock BO2 one of the same name."""
    res = EffectsResult()
    stock_used: dict[tuple[str, str], str] = {}
    # converted output of earlier runs (effects that no longer convert must not linger)
    for old in (project_root / "fx", project_root / "materials" / FX_MATERIAL_PREFIX.rstrip("/")):
        if old.exists():
            shutil.rmtree(old)

    def from_stock(kind: str, rel: Path) -> Path | None:
        if stock_waw is None:
            return None
        name = rel.as_posix()
        if kind == "fx":
            name = name.removeprefix("fx/").removesuffix(".w2bfx.json")
        elif kind == "material":
            name = name.removeprefix("materials/").removesuffix(".json")
        elif kind == "xmodel":
            name = name.removeprefix("xmodel/").removesuffix(".json")
        hit = stock_waw.root_for(kind, name)
        if hit is None or not (hit[1] / rel).exists():
            return None
        zone, root = hit
        stock_used[(kind, name)] = zone
        if root not in res.extra_roots:
            res.extra_roots.append(root)
        return root / rel

    source_used: dict[str, list[str]] = {}  # effect compiled from Mod Tools source -> linker warnings

    def from_source(name: str) -> Path | None:
        # GUIDELINES 27 source 4: only asked for effects no WaW zone defines
        if source_waw is None:
            return None
        rel = Path("fx") / f"{name}.w2bfx.json"
        try:
            root = source_waw.compile("fx", name)
        except wawsource.WawSourceError as exc:
            report.errors.append(f"fx {name}: {exc}")
            return None
        if root is None or not (root / rel).exists():
            return None
        source_used[name] = source_waw.link_warnings("fx", name)
        if root not in res.extra_roots:
            res.extra_roots.append(root)
        return root / rel

    def resolve_fx(name: str) -> Path | None:
        return from_stock("fx", Path("fx") / f"{name}.w2bfx.json") or from_source(name)

    cl = fx.closure(names, roots, resolve_fx)
    where = "no zone of the WaW install" + (" and no WaW Mod Tools source" if source_waw is not None else "")
    for name, parent in sorted(cl.absent.items()):
        report.warnings.append(f"WAW_FX_ABSENT {name} (loaded by {parent}): {where} defines it, so WaW plays "
                               f"nothing either; skipped")
    for name, missing in sorted(source_used.items()):
        report.warnings.append(f"WAW_SOURCE_ASSET fx {name}: in no compiled WaW zone; compiled from WaW Mod Tools "
                               f"raw/fx with the WaW linker")
        report.warnings += [f"fx {name}: WaW linker: {m}" for m in missing]

    # materials the effects draw with
    all_roots = [*roots, *res.extra_roots]
    material_names: dict[str, str] = {}
    world_materials: dict[str, str] = {}
    donors = fxmaterials.index_effect_donors(stock_materials, techset_dump, bo2_root / "raw")
    failed_materials: set[str] = set()
    effect_images: set[str] = set()
    for mat in sorted(cl.deps.materials):
        rel = techsets.oat_material_path(mat)
        src = _find(all_roots, rel) or from_stock("material", rel)
        all_roots = [*roots, *res.extra_roots]
        out_name = FX_MATERIAL_PREFIX + mat
        if src is None:
            # e.g. a zone that only REFERENCES the material (",name") and no
            # zone of the install defines it: absent from WaW, not a stage failure;
            # the effects drawing with it are reported UNSUPPORTED_FX below
            report.warnings.append(f"WAW_ASSET_ABSENT fx material {mat}: in no WaW zone (map, companions or "
                                   f"stock install); effects using it are not converted")
            failed_materials.add(mat)
            continue
        t4 = _load_json(src)
        ts = t4.get("techniqueSet", "")
        undecodable = []
        for tex in t4.get("textures", []):
            img = tex.get("image", "")
            if not img or img.startswith("$"):
                continue
            stem = img.replace("*", "_")
            if any((r / "images" / f"{stem}.dds").exists() for r in all_roots):
                continue
            fmt = iwd_image_format(stem, iwd_dirs)
            if fmt in IWI6_WAVELET_FORMATS:
                try:
                    recovered = wavelets.recover(stem) if wavelets else None
                except (iwi.IwiError, OSError) as exc:
                    undecodable.append(f"{img}: {exc}")
                else:
                    if recovered is None:
                        undecodable.append(f"{img} ({IWI6_WAVELET_FORMATS[fmt]} IWI): decoder unavailable")
        if undecodable:
            report.warnings.append(f"UNSUPPORTED_IMAGE fx material {mat}: {', '.join(undecodable)}")
            failed_materials.add(mat)
            continue
        if not fxmaterials.is_effect_techset(ts):
            world_materials[mat] = out_name  # decals etc.: world/model technique sets
            material_names[mat] = out_name
            continue
        try:
            donor, notes = fxmaterials.choose_donor(t4, donors)
            out = fxmaterials.build_effect_material(t4, donor, notes)
        except techsets.TechsetError as exc:
            report.errors.append(f"fx material {mat} ({ts}): {exc}")
            failed_materials.add(mat)
            continue
        runtime = shaderruntime.bind_material(t4, out, all_roots, project_root, techset_dump) if techset_dump else {'active': [], 'unsupported': ['native technique dump absent']}
        for tex in out.get("textures", []):
            if tex.get("image") and not tex["image"].startswith((",", "$")):
                tex["image"] = FX_IMAGE_PREFIX + tex["image"]
        out_rel = techsets.oat_material_path(out_name)
        dst = project_root / out_rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
        res.techsets.add(out['techniqueSet'])
        effect_images |= {t["image"] for t in out.get("textures", []) if t.get("image")}
        material_names[mat] = out_name
        report.materials.append({"name": out_name, "source": mat, "file": out_rel.as_posix(), "t4_techset": ts,
                                 "t6_techset": out['techniqueSet'], "shader_runtime": runtime,
                                 "donor": donor.path.relative_to(stock_materials).as_posix(), "notes": notes})
    if world_materials:
        res.techsets |= stage_materials(report, set(world_materials), all_roots, stock_materials, project_root,
                                        techset_dump, rename=world_materials)
    write_image_streaming(project_root, effect_images)

    # effects; one that cannot be converted takes every effect using it along
    converted: dict[str, dict] = {}
    failed: dict[str, str] = {}
    for name, src in sorted(cl.effects.items()):
        t4 = _load_json(src)
        d = fx.dependencies(t4)
        res.sounds |= d.sounds
        if d.materials & failed_materials:
            failed[name] = f"materials {sorted(d.materials & failed_materials)} not converted"
            continue
        try:
            t6, notes = fx.convert_effect(t4, material_names)
        except fx.FxConvertError as exc:
            failed[name] = str(exc)
            continue
        report.warnings += [f"fx {name}: {n}" for n in notes]
        converted[name] = t6
        res.models |= d.models
    changed = True
    while changed:
        changed = False
        for name in list(converted):
            d = fx.dependencies(_load_json(cl.effects[name]))
            bad = sorted(e for e in d.effects if e in failed or e in cl.absent)
            if bad:
                failed[name] = f"references effects that are not converted: {bad}"
                del converted[name]
                changed = True
    for name, why in sorted(failed.items()):
        report.warnings.append(f"UNSUPPORTED_FX {name}: not converted: {why}")
    for name, t6 in sorted(converted.items()):
        dst = fx.fx_file(project_root, t6["name"])
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(json.dumps(t6, indent=1) + "\n", encoding="utf-8")
    for name in sorted(set(names)):
        if name in converted:
            res.table[name] = fx.output_name(name)
            res.zone_fx.append(fx.output_name(name))
    for (kind, name), zone in sorted(stock_used.items()):
        report.warnings.append(f"WAW_STOCK_ASSET {kind} {name}: the map only references it; ported from the "
                               f"stock WaW zone {zone}.ff")
    report.warnings.append(f"fx: {len(converted)} WaW effects converted ({len(res.table)} loaded by scripts), "
                           f"{len(failed)} failed, {len(cl.absent)} absent from WaW")
    return res


@progress.phase('Resolve weapons and dependencies')
def _stage_weapons(report: StageReport, roots: list[Path], project_root: Path, names: set[str],
                   script_models: set[str], stock_waw, sound_names: set[str]):
    """Resolve and stage the map's compiled weapons (weapons.stage). The map
    scripts' models only widen the resolved roots (stock WaW dumps); they are
    staged into the map zone under their WaW names by stage_models."""
    if not names and not script_models:
        return None, roots
    weapon_report = weapons.stage(roots, project_root / "content_source", names, stock=stock_waw,
                                  extra_requests={("xmodel", n) for n in script_models})
    roots = [Path(r) for r in weapon_report.get("resolved_roots", roots)]
    sound_names.update(weapon_report["dependencies"].get("sound", []))
    report.content["weapons"] = {"status": weapon_report["status"], "weapons": len(weapon_report["weapons"]),
                                 "animations": len(weapon_report["animations"]),
                                 "missing_animations": weapon_report["missing_animations"],
                                 "report": "content_source/weapons.stage.json"}
    for name, bones in sorted(weapon_report["models"].get("reparented_roots", {}).items()):
        report.warnings.append(f"SKELETON_REPARENTED xmodel {name}: root bones {bones} share one bind transform; "
                               f"secondary roots became identity children of the primary (globals unchanged)")
    for name, why in sorted(weapon_report["models"]["unsupported"].items()):
        report.warnings.append(f"UNSUPPORTED_XMODEL {name} (weapon model): {why}")
    graph_file = project_root / 'content_source/weapons.dependencies.json'
    if graph_file.is_file():
        graph = json.loads(graph_file.read_text(encoding='utf-8'))
        for node in graph['nodes']:
            for source in node.get('recovered_geometry', []):
                report.warnings.append(f"WAW_MODEL_GEOMETRY_RECOVERED {node['name']}: {source}")
            if node['status'] == 'missing_model_geometry':
                report.errors.append(f"MODEL_GEOMETRY_MISSING {node['name']}: the map and WaW stock assets "
                                     f"do not contain {', '.join(node['missing_files'])}; "
                                     'verify the source map and WaW installation files')
    return weapon_report, roots


WEAPON_IPAK_SUFFIX = "_mod"
TEMPLATE_ZONE = Path("mods") / "zm_test" / "zm_test.zone"


@progress.phase('Convert weapon runtime assets')
def _stage_weapon_runtime(report: StageReport, project: str, project_root: Path, roots: list[Path],
                          stock_materials: Path, techset_dump: Path | None, bo2_root: Path | None,
                          t6_unlinker: Path | None, stage: Path, wavelets, converted_fx: set[str],
                          waw_map: str | None, native_fallbacks: dict[str, Path] | None = None) -> None:
    """Weapon visuals + the weapons mod.ff can carry (weapons.stage_runtime).
    Their zone lines go to mod_extra.zone (built by the BO2 mod tools linker,
    which reads the raw xanims); the scripts' weapon table is regenerated."""
    from . import bo2equiv

    content = project_root / "content_source"
    if techset_dump is None:
        report.errors.append("weapons: no --techset-dump; weapon materials cannot be translated")
        return
    visuals = weapons.stage_visuals(roots, content, stock_materials, techset_dump, wavelets,
                                    native_fallbacks=native_fallbacks)
    report.warnings += visuals['warnings']
    loaded: dict[str, set[str]] = {}
    reserved: set[str] = set()
    if bo2_root is not None:
        api = t6api.build(bo2_root, stage / "t6api_cache.json", t6_unlinker)
        loaded = {k: set(v) for k, v in api.stock_assets.items()}
        template = bo2_root / TEMPLATE_ZONE
        if template.exists():
            reserved = {l.split(",")[-1].strip() for l in template.read_text(encoding="utf-8").splitlines()
                        if l.startswith("weapon,")}
        raw_weapons = bo2_root / "raw" / "weapons"
        if raw_weapons.is_dir():
            reserved |= {p.name for p in raw_weapons.rglob("*") if p.is_file()}
    runtime = weapons.stage_runtime(content, f"{project}{WEAPON_IPAK_SUFFIX}", bo2equiv.Bo2Equivalents(bo2_root, loaded),
                                    loaded, converted_fx, reserved)
    with (project_root / MOD_EXTRA_ZONE).open("a", encoding="utf-8") as zone:
        zone.write("".join(f"{line}\n" for line in runtime["zone_lines"]))
    report.warnings += runtime["notes"]
    for name, reasons in sorted(runtime["excluded"].items()):
        report.warnings.append(f"UNSUPPORTED_WEAPON {name}: not carried in mod.ff: {'; '.join(reasons)}")
    (project_root / "maps" / "mp" / "waw").mkdir(parents=True, exist_ok=True)
    (project_root / "maps" / "mp" / "waw" / "_waw2bo2_assets.gsc").write_text(
        gscport.assets_source(report.fx_table, waw_map, runtime["table"]), encoding="utf-8")
    entities_json = project_root / "BSP" / "entities.json"
    if entities_json.exists():
        report.warnings += entities.map_wall_buys(entities_json, runtime["table"])
    report.content["weapons"].update(runtime=len(runtime["weapons"]), excluded=len(runtime["excluded"]),
                                     visual_errors=len(visuals["errors"]), runtime_report="content_source/weapons.runtime.json")


def stage_fx(report: StageReport, names: list[str], converted: dict[str, str], map_name: str, bo2_root: Path,
             api, project_root: Path, fallback: bool) -> None:
    """Script table of the WaW effects the scripts load: converted effects
    first; the rest are UNSUPPORTED_FX, unless --fx-fallback plays them as a
    stock BO2 effect (reported FX_FALLBACK). BO2 effects not in an
    always-loaded zone go to mod.ff (mod_extra.zone)."""
    table: dict[str, str] = dict(converted)
    mod_fx: list[str] = []
    rest = [n for n in names if n not in table]
    if fallback and rest:
        fb, mod_fx, lines, errors = fxmap.fallback_table(rest, map_name, bo2_root, api.stock_assets.get("fx", set()))
        table.update(fb)
        report.warnings += lines
        report.errors += errors
    for name in rest:
        if name not in table:
            report.warnings.append(f"UNSUPPORTED_FX {name}: not converted (skipped at runtime)")
    report.fx_table = table
    (project_root / "maps" / "mp" / "waw" / "_waw2bo2_assets.gsc").write_text(gscport.assets_source(table, map_name),
                                                                            encoding="utf-8")
    (project_root / MOD_EXTRA_ZONE).write_text("".join(f"fx,{n}\n" for n in mod_fx), encoding="utf-8")


SCRIPT_PRECACHE_MODEL_RE = re.compile(r'\bprecachemodel\s*\(\s*"([^"]+)"', re.IGNORECASE)
SCRIPT_SHADER_RE = re.compile(r'\b(?:precacheshader|setshader)\s*\(\s*"([^"]+)"', re.IGNORECASE)
# Builtins whose screen overlay material the engine draws by a hard-coded name
# (CoDWaW.exe and t6zm.exe both hold the string). BO2 loads it only in the
# zones of maps that use it (zm_transit), so a converted map stages WaW's.
ENGINE_BUILTIN_MATERIALS = {"setelectrified": "zombie_electric_shock_overlay"}
CORE_ANIMTREE_HEADER = "// waw2bo2: WaW animtree"
ANIM_REF_RE = re.compile(r'(?:[(,=\[]|\breturn|\[\[)\s*%\s*([A-Za-z_]\w*)')


def stage_core_animtrees(report: StageReport, sources, roots: list[Path], project_root: Path,
                         bo2_trees: set[str]) -> tuple[dict[str, set[str]], list[str]]:
    """The WaW animtrees of framework overrides whose entry points the
    converter extracts (gscport.WEAPON_REGISTRATION, e.g. the mystery box), as
    BO2 animtrees: animtrees/<tree>.atr lists the tree's animations that have a
    WaW raw xanim, which is staged as xanim/<name> for mod.ff (BO2's linker
    reads WaW's raw xanims). Returns ({tree: animations}, staged xanim names)."""
    trees: dict[str, set[str]] = {}
    staged: list[str] = []
    # only the box's script: other registration scripts (dlc3_code) name the
    # stock AI tree, whose animations BO2's own zombies replace
    cores = {core for core, _ in gscport.WEAPON_REGISTRATION["box"]}
    for core in sorted(cores):
        script = sources.get(core)
        if script is None or not script.animtree or script.animtree.lower() in {t.lower() for t in bo2_trees}:
            continue
        wanted = sorted(set(ANIM_REF_RE.findall(sources.text.get(core + ".gsc", ""))))
        found, missing = [], []
        for name in wanted:
            src = next((r / "xanim" / name for r in roots if (r / "xanim" / name).is_file()), None)
            if src is None:
                missing.append(name)
                continue
            dst = project_root / "xanim" / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            found.append(name)
        for name in missing:
            report.warnings.append(f"UNSUPPORTED_XANIM {name} (animtree {script.animtree}, {core}): no WaW xanim "
                                   f"in any zone dump; references become undefined")
        if not found:
            continue
        atr = project_root / "animtrees" / f"{script.animtree}.atr"
        atr.parent.mkdir(parents=True, exist_ok=True)
        atr.write_text(f"{CORE_ANIMTREE_HEADER} {script.animtree} ({core}), animations with WaW xanims\n" +
                       "".join(f"{n}\n" for n in found), encoding="utf-8")
        trees[script.animtree] = set(found)
        staged += found
    return trees, staged


SCRIPT_MODEL_RE = re.compile(r'\b(?:precachemodel|setmodel|setviewmodel|attach)\s*\(\s*"([^"]+)"', re.IGNORECASE)
SCRIPT_FUNC_RE = re.compile(r'^([A-Za-z_]\w*)\s*\(([^)]*)\)\s*\{', re.MULTILINE)


def wrapped_model_literals(texts: list[str]) -> set[str]:
    """Models named through a script helper that hands its parameter to a
    model call, e.g. WaW _loadout's set_player_viewmodel( "viewmodel_usa_marine_arms" )
    (the player's arms: without them T6 draws no viewmodel at all)."""
    wrappers: dict[str, int] = {}
    for text in texts:
        for m in SCRIPT_FUNC_RE.finditer(text):
            depth, end = 0, len(text)
            for i in range(m.end() - 1, len(text)):
                depth += {"{": 1, "}": -1}.get(text[i], 0)
                if depth == 0:
                    end = i
                    break
            body = text[m.end():end]
            for index, param in enumerate(p.strip() for p in m.group(2).split(",")):
                if param and re.search(rf'\b(?:precachemodel|setmodel|setviewmodel|attach)\s*\(\s*{re.escape(param)}\s*[,)]',
                                       body, re.IGNORECASE):
                    wrappers[m.group(1).lower()] = index
    found = set()
    for name, index in wrappers.items():
        call = re.compile(rf'\b{re.escape(name)}\s*\(\s*' + r'(?:"[^"]*"\s*,\s*)' * index + r'"([^"]+)"',
                          re.IGNORECASE)
        for text in texts:
            found.update(call.findall(text))
    return found


def stock_wrapped_models(sources, ported: list[str], map_roots: list[Path]) -> set[str]:
    """Wrapped model literals in stock WaW framework scripts the map does not
    override. Most maps ship no _loadout, so the stock one sets the player's
    arms; it names every campaign's arms, but the map's own zones carry the
    ones it uses (Rancid: viewmodel_usa_marine_arms)."""
    texts = [sources.text.get(path + ".gsc", sources.stock.get(path + ".gsc", "")) for path in ported]
    local = assetresolve.Resolver(map_roots)
    return {name for name in wrapped_model_literals(texts) if local.find("xmodel", name)}


def recover_script_models(report: StageReport, wanted: set[str], roots: list[Path],
                          stock_waw=None, source_waw=None) -> set[str]:
    """Resolve runtime model dependencies from WaW before filtering precaches.

    A setModel-only asset need not occur in the map's entity/model table. Keep
    those requests through the complete WaW lookup, without BO2 substitution.
    The caller's roots are widened so geometry and material staging see them.
    """
    resolver = assetresolve.Resolver(roots, stock_waw)
    models = set()
    dependencies = []
    raw_roots = []
    for name in sorted(wanted):
        path = resolver.find('xmodel', name)
        provenance = 'compiled_waw'
        if path is None and source_waw is not None:
            recovered = source_waw.compile('xmodel', name)
            if recovered is not None:
                raw_roots.append(recovered)
                path = recovered / assetresolve.asset_path('xmodel', name)
                provenance = 'WAW_SOURCE_ASSET'
                report.warnings.append(f'WAW_SOURCE_ASSET xmodel {name}: compiled from WaW Mod Tools')
        node = {'name': name, 'status': 'resolved' if path else 'missing_waw_source'}
        if path is not None:
            models.add(name)
            node.update(source=str(path), provenance=provenance)
        dependencies.append(node)
    roots[:] = list(dict.fromkeys([*resolver.roots, *raw_roots]))
    report.scripts['requested_models'] = sorted(wanted)
    report.scripts['model_sources'] = dependencies
    return models


@progress.phase('Port map scripts and animation dependencies')
def port_scripts(report: StageReport, stage: Path, project_root: Path, waw_map_script: Path, bo2_root: Path,
                 roots: list[Path], iwd_dirs: list[Path], stock: Path | None,
                 t6_unlinker: Path | None, model_roots: list[Path], clip=None,
                 stock_waw=None, source_waw=None, *, bo2_stock_perks: bool = False,
                 stock_variant_roots: list[Path] | None = None) -> set[str]:
    """Translate the WaW map's gameplay scripts (gscport). Returns the WaW
    models the scripts use that the map zone must carry."""
    map_name = waw_map_script.stem
    # the map zone's own rawfiles first (the folder holding maps/<map>.gsc)
    script_roots = [waw_map_script.parent.parent, *roots]
    iwds = sorted({f for d in iwd_dirs for f in d.glob("*.iwd")})
    if stock is None or not stock.exists():
        report.errors.append("no stock WaW scripts (--waw-stock-scripts): cannot tell the map's scripts from "
                             "the WaW framework")
        return set()
    api = t6api.build(bo2_root, stage / "t6api_cache.json", t6_unlinker)
    sources = gscport.Sources(script_roots, iwds, stock, stock_variant_roots)
    actor_types = gscport.zombieappearance.source_actor_types(project_root)
    actor_template = None
    if actor_types:
        actor_template = (bo2_root / 'raw/aitype/zm_nuked_basic_01.gsc').read_text(encoding='utf-8')
    for old in (project_root / "animtrees").glob("*.atr") if (project_root / "animtrees").exists() else ():
        if old.read_text(encoding="utf-8", errors="replace").startswith(CORE_ANIMTREE_HEADER):
            old.unlink()    # staged by an earlier run (stage_core_animtrees)
    animtrees = {p.stem for d in (bo2_root / "raw" / "animtrees", project_root / "animtrees") if d.exists()
                 for p in d.glob("*.atr")}
    core_trees, xanims = stage_core_animtrees(report, sources, model_roots, project_root, animtrees)
    port = gscport.port_map(sources, api, map_name, project_root, animtrees=animtrees, core_animtrees=core_trees,
                            bo2_stock_perks=bo2_stock_perks, actor_types=actor_types,
                            actor_template=actor_template)
    if actor_types:
        gscport.zombieappearance.bind_actor_types(project_root, actor_types)
    report.scripts = port.to_json()
    report.scripts["staged_xanims"] = xanims
    report.scripts["staged_animtrees"] = sorted(core_trees)
    report.errors += [f"scripts: {e}" for e in port.errors]
    # empty converted-asset table for the link check; stage_fx writes the real
    # one once the effects are converted (stage_bridge)
    (project_root / "maps" / "mp" / "waw" / "_waw2bo2_assets.gsc").write_text(gscport.assets_source({}, map_name),
                                                                            encoding="utf-8")
    # Collision export removes one-sided passage sheets; keep a no-op entry
    # point so older generated map-main hooks cannot run movement emulation.
    (project_root / "maps" / "mp" / "waw" / "_waw2bo2_oneway.gsc").write_text(oneway.oneway_source([]),
                                                                             encoding="utf-8")
    for api_name, where in sorted(port.unsupported.items()):
        report.warnings.append(f"UNSUPPORTED_GSC_API {api_name.removeprefix('GSC ')} ({len(where)} uses, first "
                               f"{where[0]}): stubbed, reports at runtime")
    for tree, users in sorted(port.animtrees.items()):
        if tree.lower() not in {a.lower() for a in animtrees}:
            report.warnings.append(f"UNSUPPORTED_XANIM animtree {tree} used by {', '.join(sorted(set(users)))}: "
                                   f"WaW animations are not converted yet")
    report.warnings.append(f"scripts: {len(port.ported)} WaW scripts ported, {len(port.extracted)} framework "
                           f"functions extracted, {len(port.fx)} FX names referenced")
    if port.core_overrides:
        report.warnings.append(f"scripts: the map overrides WaW framework scripts BO2 replaces with its own "
                               f"(behaviour changes there are not ported): {', '.join(port.core_overrides)}")
    # models the scripts precache / set / attach
    wanted = set()
    for path in port.ported:
        text = sources.text.get(path + '.gsc', sources.stock.get(path + '.gsc', ''))
        wanted |= set(SCRIPT_MODEL_RE.findall(text))
        if path.startswith('xmodelalias\\'):
            wanted |= {t.text[1:-1] for t in gscport.gsc.tokenize(text) if t.kind == gscport.gsc.STRING}
    wrapped = wrapped_model_literals([sources.text.get(path + ".gsc", "") for path in port.ported])
    wrapped |= stock_wrapped_models(sources, port.ported, model_roots)
    wanted |= wrapped
    stock_models = api.stock_assets.get("xmodel", set())
    models = recover_script_models(report, wanted, model_roots, stock_waw, source_waw)
    missing = {name for name in wanted if name not in models and name.lower() not in stock_models}
    guarded = []
    for path in (project_root / 'maps/mp/waw').rglob('*.gsc'):
        source, names = gscport.guard_missing_model_calls(path.read_text(encoding='utf-8'), missing)
        if names:
            path.write_text(source, encoding='utf-8')
            guarded.extend({'script': path.relative_to(project_root).as_posix(), 'model': name} for name in names)
    report.scripts['guarded_missing_model_calls'] = guarded
    for name in sorted(wanted):
        if name not in models and name.lower() not in stock_models:
            report.warnings.append(f"UNSUPPORTED_ASSET xmodel {name}: used by the map scripts, absent from "
                                   f"the supplied WaW zones and searched WaW sources")
    # the named models the scripts precache, in the first frame (WaW scripts
    # also precache late, which T6 rejects); only the ones the zones carry
    precached = {m for path in port.ported
                 for m in SCRIPT_PRECACHE_MODEL_RE.findall(sources.text.get(path + ".gsc", ""))}
    # T6 also requires models referenced only by setModel/attach to be cached
    # during startup; waiting until a power-on thread runs is too late.
    precached |= models | wrapped
    precached = sorted(m for m in precached if m in models or m.lower() in stock_models)
    (project_root / "maps" / "mp" / "waw" / "_waw2bo2_precache.gsc").write_text(
        gscport.precache_source(precached), encoding="utf-8")
    report.scripts["precached_models"] = precached
    # HUD materials the scripts precache / draw (setShader): nothing else
    # references them, so they would be missing (checkerboard icons)
    # Runtime compatibility HUDs also need dependencies even when no source
    # map script names them (for example the teammate revive waypoint).
    shaders = set(SCRIPT_SHADER_RE.findall(
        (gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8")))
    for path in port.ported:
        text = sources.text.get(path + ".gsc", "")
        shaders |= set(SCRIPT_SHADER_RE.findall(text))
        shaders |= {m for b, m in ENGINE_BUILTIN_MATERIALS.items() if re.search(rf"\b{b}\s*\(", text, re.I)}
    stock_materials = api.stock_assets.get("material", set())
    hud = []
    for name in sorted(shaders):
        if name.lower() in stock_materials:
            continue
        if any((r / techsets.oat_material_path(name)).exists() for r in model_roots):
            hud.append(name)
        else:
            report.warnings.append(f"UNSUPPORTED_ASSET material {name}: drawn by the map scripts, not in any WaW "
                                   f"zone dump nor a stock BO2 zone")
    report.scripts["hud_materials"] = hud
    return models


def compiled_scripts_root(stage: Path, project: str) -> Path:
    return stage / "script_build" / "compiled"


def compile_scripts(stage: Path, project: str, bo2_root: Path, oat_unlinker: Path) -> tuple[list[str], str]:
    """Compile the map GSC/CSC with the BO2 mod tools linker, then extract the
    bytecode with the OAT unlinker so the bridge embeds compiled scripts.

    The bridge (like the game) stores ScriptParseTree bytes verbatim, so the
    source files must never reach the map zone. Returns (compiled script names,
    linker log)."""
    import subprocess

    project_root = stage / "zone_raw" / project
    gscport.ensure_owned_client_bootstrap(project_root, bo2_root)
    scripts = map_scripts(project_root, project)
    work = stage / "script_build"
    if work.exists():
        shutil.rmtree(work)
    (work / "zone_source").mkdir(parents=True)
    zone = f"{project}_scripts"
    (work / "zone_source" / f"{zone}.zone").write_text(
        ">game,T6\n" + "".join(f"script,{s}\n" for s in scripts), encoding="utf-8")
    linker = bo2_root / "bin" / "Linker.exe"
    proc = progress.captured([str(linker), "--no-color", "--source-search-path", str(work / "zone_source"),
                           "--add-asset-search-path", str(project_root), "--output-folder", str(work / "out"), zone],
                          cwd=str(linker.parent), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                          errors="replace")
    log = proc.stdout
    (work / "linker.log").write_text(log, encoding="utf-8")
    compiled = sorted(set(re.findall(r'Compiled GSC script "([^"]+)"', log)))
    # a scripts-only zone has no BSP; that one error is expected
    errors = [l for l in log.splitlines() if "ERROR" in l and "Could not open BSP" not in l]
    missing = sorted(set(scripts) - set(compiled))
    ff = work / "out" / f"{zone}.ff"
    if errors or missing or not ff.exists():
        raise StageError(f"script compilation failed: {errors[:5]} missing={missing[:5]}; see {work / 'linker.log'}")
    out = compiled_scripts_root(stage, project)
    proc = progress.captured([str(oat_unlinker), "--no-color", "--include-assets", "script", "--output-folder", str(out),
                           str(ff)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    for name in compiled:
        blob = out / name
        if proc.returncode or not blob.exists():
            raise StageError(f"compiled script {name} was not extracted: {proc.stdout[-400:]}")
        if blob.read_bytes()[:2] == b"//" or b"#include" in blob.read_bytes()[:200]:
            raise StageError(f"extracted {name} is still GSC source, not bytecode")
    return compiled, log


def linker_command(linker: Path, stage: Path, project: str, techset_dump: Path | None) -> list[str]:
    roots = []
    if compiled_scripts_root(stage, project).exists():
        # compiled bytecode must shadow the GSC sources in the project root
        roots.append(compiled_scripts_root(stage, project))
    roots.append(stage / "zone_raw" / project)
    # OAT keeps search paths in a std::set and opens the first match, so the
    # order is that of the path STRINGS, not of this list: the stock cache
    # (".../asset_cache/...") would shadow a same-named map asset under the
    # build folder. "?base?" (the stage) sorts before any drive letter.
    roots = ["?base?\\" + str(r.relative_to(stage)) if r.is_relative_to(stage) else r for r in roots]
    if techset_dump is not None:
        roots.append(techset_dump)
    return [
        str(linker), "--no-color",
        "--base-folder", str(stage),
        # Replaces OAT's default search roots entirely, so nothing from
        # ?bin?/raw or BO2\raw can shadow the staged assets.
        "--asset-search-path", ";".join(str(r) for r in roots),
        "--source-search-path", str(stage / "zone_source"),
        "--output-folder", str(stage / "zone_out" / project),
        project,
    ]
