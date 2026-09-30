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

import json
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path

from . import assetresolve, audio, entities, fx, fxmap, fxmaterials, gscport, hulls, iwi, lighting, lightmaps, paths, shaderruntime, shaders, sounds, t6api, techsets, visions, wawassets, wawsource, wavelet, weapons, zones
from .fbx import write_collision_fbx, write_world_fbx
from .world import read_collision, read_gfx_world

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


def stage_materials(report: StageReport, names: set[str], roots: list[Path], stock_materials: Path,
                    project_root: Path, techset_root: Path | None = None,
                    rename: dict[str, str] | None = None) -> set[str]:
    """``rename`` maps a WaW material name to the name it is written under."""
    rename = rename or {}
    donors = techsets.index_donors(stock_materials)
    if techset_root is not None:
        # a donor is only usable if its technique set was dumped (not just referenced)
        donors = {ts: p for ts, p in donors.items() if (techset_root / "techniquesets" / f"{ts}.json").exists()}
    if not donors:
        raise StageError(f"no stock T6 material donors found under {stock_materials}")
    candidates = sorted(donors)
    used_techsets: set[str] = set()
    for name in sorted(names):
        rel = techsets.oat_material_path(name)
        src = _find(roots, rel)
        out_rel = techsets.oat_material_path(rename.get(name, name))
        entry = {"name": rename.get(name, name), "file": out_rel.as_posix()}
        if name in rename:
            entry["source"] = name
        notes: list[str] = []
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
        while True:
            try:
                m = techsets.match(t4_ts, remaining)
            except techsets.TechsetError as exc:
                report.errors.append(f"material {name} ({t4_ts}): {exc}" + (f"; rejected: {'; '.join(rejected)}" if rejected else ""))
                m = None
                break
            attempt = notes + m.notes
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
        runtime = shaderruntime.bind_material(t4, out, roots, project_root, techset_root) if techset_root else {'active': [], 'unsupported': ['native technique dump absent']}
        entry['shader_runtime'] = runtime
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


def stage_images(report: StageReport, image_roots: list[Path], project_root: Path,
                 iwd_dirs: list[Path] | None = None, extra_images: list[str] = (),
                 wavelets: wavelet.IwdRecovery | None = None) -> list[str]:
    wanted: dict[str, str] = {name: name.replace("*", "_") for name in extra_images}
    for mat in report.materials:
        data = _load_json(project_root / mat["file"])
        for tex in data.get("textures", []):
            img = tex.get("image", "")
            if img and not img.startswith(","):
                wanted.setdefault(img, img.replace("*", "_"))
    for asset, stem in REQUIRED_WORLD_IMAGES.items():
        wanted[asset] = stem
    written = []
    for asset, stem in sorted(wanted.items()):
        dst = project_root / techsets.oat_image_path(asset)
        if asset == "lightmap0_secondary":
            # T6 lightmaps are three stacked, encoded pages (page 2 holds the
            # light direction); a flat grey decodes as a strong red cast.
            # Neutral values: the mean of each page of stock zm_nuked's
            # lightmap0 (measured with the gfxworld diagnostic dump).
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(iwi.paged_rgba_iwi(4, 4, NEUTRAL_LIGHTMAP_PAGES))
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
        if src is None and asset.startswith(_PACKED_SPEC_PREFIX):
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
OAT_T6_RAW = Path(__file__).resolve().parents[2] / "vendor" / "OpenAssetToolsT6" / "raw" / "t6"

T6_XMODEL_DEFAULTS = {
    "flags": 0x200000,
    "lightingOriginOffset": {"x": 0.0, "y": 0.0, "z": 0.5},
    "lightingOriginRange": 0.5,
}


SKYBOX_TEMPLATE = "skybox_dlc0_zm_nuketown"
# T6 sky cubemaps (mc_skycubemaphdr) carry an HDR intensity in alpha (stock
# zm_nuked: DXT5, mean alpha ~0.9, scaled by the material's skyColorParms).
# WaW sky cubes are LDR with alpha 0, which the T6 sky shader turns black.
# Alpha is set so the LDR colour comes out at about its WaW brightness
# (assumes intensity = alpha * skyColorParms[1] = 6 for the stock material).
SKY_HDR_ALPHA = round(255 / 6)
SKY_SRC_DIR = "sky_src"
SKYBOX_MATERIAL = "mc/mtl_dlc0_zm_skybox_nuketown"


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


def stage_skybox(report: StageReport, project: str, project_root: Path, bo2_root: Path | None,
                 sky_image: str | None) -> tuple[str | None, str | None]:
    """Stage skybox_<project>: BO2's sky dome + sky technique, WaW's sky cubemap.

    WaW draws its skybox model and sky surfaces with sky techniques that have
    no T6 counterpart for its materials; drawn with an ordinary model
    technique the dome is opaque geometry around the camera that hides every
    distant surface. T6 skies are small domes drawn with mc_skycubemaphdr
    (behind everything), so the stock Nuketown dome and its material are used
    with the WaW sky cubemap. Returns (material name, image name)."""
    if bo2_root is None or sky_image is None:
        report.errors.append("skybox: needs --bo2 and a WaW sky cubemap")
        return None, None
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


def stage_models(report: StageReport, world, project: str, stage: Path, project_root: Path,
                 extra_models: set[str] = frozenset(), roots: list[Path] | None = None) -> set[str]:
    """Stage every placed static model and the skybox as T6 xmodel sources.

    Writes ``xmodel/<name>.json`` (OAT xmodel v2 pointing at the GLTF LODs)
    and ``BSP/models.json`` with the placements read by the bridge's
    GfxWorldLinker. Returns the material names used by the models.
    """
    targets = {m.name: m.name for m in world.static_models}
    targets.update({name: name for name in extra_models})
    # the skybox is staged by stage_skybox (T6 needs a sky technique)
    materials: set[str] = set()
    for src_name, dst_name in sorted(targets.items()):
        src_root = next((r for r in (roots or [stage]) if (r / "xmodel" / f"{src_name}.json").exists()), None)
        if src_root is None:
            report.errors.append(f"xmodel {src_name}: xmodel/{src_name}.json missing from every WaW zone dump")
            continue
        xm = _load_json(src_root / "xmodel" / f"{src_name}.json")
        lods = []
        for lod in xm.get("lods", []):
            gltf = src_root / lod["file"]
            if not gltf.exists():
                report.errors.append(f"xmodel {src_name}: LOD file {lod['file']} missing")
                continue
            dst_file = project_root / lod["file"]
            dst_file.parent.mkdir(parents=True, exist_ok=True)
            data = _load_json(gltf)
            if _strip_bad_skins(data):
                report.warnings.append(f"xmodel {src_name}: skin without a single root joint removed "
                                       f"({lod['file']}); drawn rigid in its bind pose")
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
        xm.update(T6_XMODEL_DEFAULTS)
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
                   "scale": m.scale, "flags": m.flags, "cullDist": m.cull_dist} for m in world.static_models]
    bsp = project_root / "BSP"
    bsp.mkdir(parents=True, exist_ok=True)
    (bsp / "models.json").write_text(json.dumps({"models": placements}, indent=1) + "\n", encoding="utf-8")
    return materials


def verify_techsets(report: StageReport, used: set[str], techset_root: Path | None, project_root: Path | None = None) -> None:
    report.techsets = sorted(used)
    if techset_root is None:
        report.errors.append("no --techset-dump given; the bridge cannot load technique sets without "
                             "techniquesets/*.json + shader_bin/*.cso dumped from a stock T6 zone")
        return
    for name in sorted(used):
        path = _find([*([project_root] if project_root else []), techset_root], Path('techniquesets') / f'{name}.json')
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
        if dst.exists():
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(src.read_text(encoding="utf-8", errors="replace").replace(template_name, project),
                       encoding="utf-8")
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
    dst.write_text(AMB_CSC, encoding="utf-8")


def stage_scripts(report: StageReport, project: str, project_root: Path,
                  template_root: Path | None, template_name: str | None) -> None:
    for folder, pattern in SCRIPT_FILES:
        dst = project_root / folder / pattern.format(p=project)
        if dst.exists():
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
    found.extend(gscport.bo2_scripts(project_root))
    found.extend(p.relative_to(project_root).as_posix() for p in
                 (project_root / 'clientscripts/mp/waw').glob('*.csc'))
    return found


def write_zone(stage: Path, project: str, images: list[str], ipak: bool,
               xmodels: list[str] = (), scripts: list[str] = (), zbarriers: list[str] = (),
               effects: list[str] = ()) -> Path:
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
    lines += [f"script,{name}" for name in scripts]
    path = zone_dir / f"{project}.zone"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


SKY_TECHSETS = {"wc_sky"}
NON_RENDER_TECHSETS = {"wc_tools"}


def stage_geometry(report: StageReport, world, clip, roots: list[Path], project_root: Path) -> str | None:
    """World FBX (sky surfaces removed), terrain collision FBX, brushes.json."""
    bsp = project_root / "BSP"
    bsp.mkdir(parents=True, exist_ok=True)
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
    if not techsets.LAYERED_VERTEX_DATA and blend_data:
        report.warnings.append(f"{len(blend_data)} layered materials drawn as their base layer; their vertex colour "
                               f"(blend weights) is written as white")
    else:
        blend_data = set()
    kept = [s for s in world.surfaces if s.material not in sky | tool_surfaces]
    if sky:
        report.warnings.append(
            f"{sum(s.material in sky for s in world.surfaces)} sky surfaces ({', '.join(sorted(sky))}) removed from the render "
            f"world; T6 draws the sky with the skybox xmodel")
    if tool_surfaces:
        report.warnings.append(
            f"{sum(s.material in tool_surfaces for s in world.surfaces)} WaW editor-only surfaces "
            f"({', '.join(sorted(tool_surfaces))}) removed from the render world")
    world.surfaces = kept
    write_world_fbx(world, bsp / "map_gfx.fbx", frozenset(blend_data))
    slot_materials = write_collision_fbx(clip, bsp / "map_col.fbx")
    # BSP/clipmaterials.json: FBX collision material -> WaW clip flags. The
    # bridge linker gives every terrain partition its own clip material.
    clip_materials = [{"fbx": "waw_collision", "name": "waw_collision", "contentFlags": 1, "surfaceFlags": 0}
                      if m < 0 else
                      {"fbx": f"clip_{m}", "name": clip.materials[m].name,
                       "contentFlags": hulls._signed(clip.materials[m].content_flags),
                       "surfaceFlags": hulls._signed(clip.materials[m].surface_flags)} for m in slot_materials]
    (bsp / "clipmaterials.json").write_text(json.dumps({"materials": clip_materials}, indent=1) + "\n", encoding="utf-8")
    brushes, summary = hulls.collision_brushes(clip)
    (bsp / "brushes.json").write_text(json.dumps({"brushes": brushes}, separators=(",", ":")) + "\n",
                                      encoding="utf-8")
    subs = hulls.submodel_records(clip)
    (bsp / "submodels.json").write_text(json.dumps({"submodels": subs}, separators=(",", ":")) + "\n",
                                        encoding="utf-8")
    summary["collision_triangle_materials"] = len(clip_materials)
    summary["collision_triangles_noncolliding_dropped"] = sum(
        1 for m in clip.triangle_materials if m == 0xFFFF or not clip.materials[m].content_flags)
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
                 t6_sound_driver: Path | None = None, approximate_sound_curves: bool = False) -> StageReport:
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

    world = read_gfx_world(gfx_bin)
    clip = read_collision(clip_bin)
    sky_image = stage_geometry(report, world, clip, roots, project_root)
    ents_file = gfx_bin.parent / f"{gfx_bin.name.removesuffix('.gfx.bin')}.ents"
    if not ents_file.exists():
        ents_file = stage / "maps" / ents_file.name
    script_models: set[str] = set()
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
        start_zones = None
        if waw_map_script is not None and waw_map_script.exists():
            start_zones = zones.read_waw_zones(waw_map_script.read_text(encoding="utf-8", errors="replace"))[0]
        report.entities, script_models = entities.write_entities(ents_file, project, project_root / "BSP",
                                                                 clip, start_zones, animscripts)
        if (project_root / "BSP" / "paths.json").exists():
            report.warnings += entities.link_barrier_traversals(project_root / "BSP")
    else:
        report.errors.append(f"map entities {ents_file.name} missing")
    effects = EffectsResult()
    weapon_report = None
    if waw_map_script is not None and bo2_root is not None:
        script_models |= port_scripts(report, stage, project_root, waw_map_script, bo2_root, waw_script_roots or [],
                                      iwd_dirs or [], waw_stock_scripts, t6_unlinker, roots)
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
        source_waw = None
        if waw_mod_tools is not None and waw_root is not None and t4_unlinker is not None:
            source_waw = wawsource.WawSourceAssets(waw_mod_tools, waw_root, t4_unlinker,
                                                   waw_source_dumps or stage / "waw_source_dumps")
            problems = source_waw.check()
            if problems:
                report.errors.append(f"WaW Mod Tools sources unusable: {'; '.join(problems)}")
                source_waw = None
        effects = stage_effects(report, fx_names + weapon_fx, roots, stock_waw, project_root, stock_materials,
                                techset_dump,
                                bo2_root, [*(iwd_dirs or []), *([waw_root / "main"] if waw_root else [])],
                                source_waw, wavelets)
        compiled_sound_names |= effects.sounds
        roots = [*roots, *effects.extra_roots]
        script_models |= effects.models
        api = t6api.build(bo2_root, stage / "t6api_cache.json", t6_unlinker)
        stage_fx(report, fx_names, effects.table, waw_map_script.stem, bo2_root, api, project_root, fx_fallback)
    # clipmap static models reference their xmodel (collSurfs) in the map zone
    clip_models = {m.name for m in clip.static_models if m.contents and m.surfaces}
    model_materials = stage_models(report, world, project, stage, project_root, script_models | clip_models, roots)
    names = {s.material for s in world.surfaces} | model_materials
    used = stage_materials(report, names, roots, stock_materials, project_root, techset_dump)
    used |= effects.techsets
    # Lightmap pages in both encodings; world surfaces whose material runs
    # translated WaW lit programs get the WaW-encoded copy (by FBX material name).
    waw_lightmap_materials = {m.get('source', m['name']) for m in report.materials
                              if m.get('shader_runtime', {}).get('lightmap') == 'waw'}
    lightmap_report = lightmaps.stage(world, [r / 'images' for r in roots], project_root, waw_lightmap_materials)
    report.errors += lightmap_report['errors']
    report.warnings += lightmap_report['warnings']
    report.content['lightmaps'] = {'pages': lightmap_report['pages'],
                                   'waw_encoded_materials': len(waw_lightmap_materials)}
    _SKY_IMAGE_ROOTS[:] = [r / "images" for r in roots]
    sky_techset, sky_image = stage_skybox(report, project, project_root, bo2_root, sky_image)
    if sky_techset:
        used.add(sky_techset)
    images = stage_images(report, [project_root / SKY_SRC_DIR, *[r / "images" for r in roots]], project_root,
                          image_iwds,
                          [sky_image] if sky_image else [], wavelets)
    verify_techsets(report, used, techset_dump, project_root)
    if bo2_root is not None and (bo2_root / "mods" / "zm_test").exists():
        stage_template_scripts(report, project, project_root, bo2_root / "mods" / "zm_test", "zm_test")
    stage_scripts(report, project, project_root, template_root, template_name)
    write_amb_csc(report, project, project_root)
    for script_name in map_scripts(project_root, project):
        script_path = project_root / script_name
        source = script_path.read_text(encoding="utf-8", errors="replace")
        if "zm_test" in source:
            script_path.write_text(source.replace("zm_test", project), encoding="utf-8")
            report.warnings.append(f"template references renamed in {script_name}")
    main_gsc = project_root / "maps" / "mp" / f"{project}.gsc"
    if waw_map_script is not None and main_gsc.exists():
        try:
            initial, links = zones.apply(waw_map_script, main_gsc, project)
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
        except ValueError as exc:
            report.errors.append(f"scripts: {exc}")
    stage_rawfiles(report, project_root, bo2_root)
    if weapon_report is None:
        weapon_report, roots = _stage_weapons(report, roots, project_root, compiled_weapon_names, script_models,
                                              stock_waw, compiled_sound_names)
    if weapon_report is not None:
        _stage_weapon_runtime(report, project, project_root, roots, stock_materials, techset_dump, bo2_root,
                              t6_unlinker, stage, wavelets, set(effects.table),
                              waw_map_script.stem if waw_map_script is not None else None)
    shader_report = shaders.stage(roots, project_root / 'content_source/shaders')
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
    vision_report = visions.stage(project_root,
        [*([waw_map_script.parent.parent] if waw_map_script else []),
         *(waw_script_roots or []), *roots, *([waw_stock_scripts] if waw_stock_scripts else [])],
        sorted({f for d in (iwd_dirs or []) for f in d.glob('*.iwd')}), stock_waw,
        visions.lut_donor(techset_dump), waw_mod_tools/'raw' if waw_mod_tools else None)
    report.errors += vision_report['errors']
    report.warnings += [f'VISION_ABSENT {name}: absent from WaW lookup' for name in vision_report['missing']]
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
    client_main.write_text(client_text.replace('    start_zombie_stuff();', '    start_zombie_stuff();\n' + hook, 1), encoding='utf-8')
    sound_resolver = assetresolve.Resolver(roots, stock_waw)
    defined_sounds = compiled_sound_names | ({n for kinds in stock_waw.index.values()
        for n in kinds.get('sound', [])} if stock_waw else set())
    script_sounds = sounds.script_aliases(project_root, defined_sounds)
    compiled_sound_names.update(script_sounds)
    report.content['script_sound_dependencies'] = sorted(script_sounds)
    sound_graph = sound_resolver.expand({("sound", n) for n in compiled_sound_names})
    sound_names = {n["name"] for n in sound_graph["nodes"] if n["kind"] == "sound"}
    sound_report = sounds.stage(sound_resolver.roots, project_root / "content_source", image_iwds,
                                stock_waw, sound_names)
    audio_report = audio.stage(project_root / "content_source", project_root / "content_source/pcm", audio_decoder, xwma_decoder)
    binding_report = sounds.bind_pcm(project_root / "content_source", project_root / "content_source/pcm")
    if t6_sound_driver is None and techset_dump is not None:
        t6_sound_driver = techset_dump / "sounddriverglobals/singleton.w2bsdg"
    curve_report = sounds.bind_curves(project_root / "content_source", t6_sound_driver, approximate_sound_curves)
    bank = f"waw_{project}.all"
    budget = sounds.loaded_budget(bo2_root, bo2_root / "mods" / "zm_test" / "soundbank" / "zmb_test.all.aliases.csv") \
        if bo2_root is not None else None
    bank_report = sounds.write_t6_bank(project_root / "content_source", bank, project_root / "soundbank", budget)
    if budget is not None:
        report.warnings.append(
            f"SOUND_LOADED_TO_STREAMED {budget['streamed_files']} WaW loaded sounds streamed: loaded-bank budget "
            f"{budget['entries']} files / {budget['bytes'] // 2**20} MB (largest stock map bank "
            f"{budget['reference_bank']} minus the template bank); kept {budget['loaded_files']} shortest loaded "
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
        report.content["lighting"] = lighting.read_grid(grid_file).report()
        destination = project_root / "content_source/lighting"
        destination.mkdir(parents=True, exist_ok=True)
        for suffix in (".lightgrid.bin", ".lighting.json", ".primarylights.json"):
            original = Path(lighting_base + suffix)
            if original.exists():
                shutil.copy2(original, destination / original.name)
        pages = len(report.content.get("lightmaps", {}).get("pages", []))
        report.warnings.append(f"CONTENT_INCOMPLETE lighting: {pages} WaW lightmap pages ported (see lightmaps); the "
                               "WaW light grid is preserved but not converted, so model lighting uses T6's grid data")
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
    scripts = map_scripts(project_root, project)
    write_zone(stage, project, images, ipak, sorted(script_models), scripts, zbarriers, effects.zone_fx)

    (project_root / "bridge_stage.report.json").write_text(report.to_json(), encoding="utf-8")
    return report


MOD_EXTRA_ZONE = "mod_extra.zone"
FX_MATERIAL_PREFIX = "waw_fx/"


@dataclass
class EffectsResult:
    table: dict[str, str] = field(default_factory=dict)  # script-loaded WaW effect -> converted name
    zone_fx: list[str] = field(default_factory=list)  # fx lines for the map zone
    models: set[str] = field(default_factory=set)
    extra_roots: list[Path] = field(default_factory=list)  # stock WaW zone dumps used
    techsets: set[str] = field(default_factory=set)
    sounds: set[str] = field(default_factory=set)


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
        out_rel = techsets.oat_material_path(out_name)
        dst = project_root / out_rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
        res.techsets.add(donor.techset)
        material_names[mat] = out_name
        report.materials.append({"name": out_name, "source": mat, "file": out_rel.as_posix(), "t4_techset": ts,
                                 "t6_techset": donor.techset,
                                 "donor": donor.path.relative_to(stock_materials).as_posix(), "notes": notes})
    if world_materials:
        res.techsets |= stage_materials(report, set(world_materials), all_roots, stock_materials, project_root,
                                        techset_dump, rename=world_materials)

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
    return weapon_report, roots


WEAPON_IPAK_SUFFIX = "_mod"
TEMPLATE_ZONE = Path("mods") / "zm_test" / "zm_test.zone"


def _stage_weapon_runtime(report: StageReport, project: str, project_root: Path, roots: list[Path],
                          stock_materials: Path, techset_dump: Path | None, bo2_root: Path | None,
                          t6_unlinker: Path | None, stage: Path, wavelets, converted_fx: set[str],
                          waw_map: str | None) -> None:
    """Weapon visuals + the weapons mod.ff can carry (weapons.stage_runtime).
    Their zone lines go to mod_extra.zone (built by the BO2 mod tools linker,
    which reads the raw xanims); the scripts' weapon table is regenerated."""
    from . import bo2equiv

    content = project_root / "content_source"
    if techset_dump is None:
        report.errors.append("weapons: no --techset-dump; weapon materials cannot be translated")
        return
    visuals = weapons.stage_visuals(roots, content, stock_materials, techset_dump, wavelets)
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


SCRIPT_MODEL_RE = re.compile(r'\b(?:precachemodel|setmodel|setviewmodel|attach)\s*\(\s*"([^"]+)"', re.IGNORECASE)


def port_scripts(report: StageReport, stage: Path, project_root: Path, waw_map_script: Path, bo2_root: Path,
                 roots: list[Path], iwd_dirs: list[Path], stock: Path | None,
                 t6_unlinker: Path | None, model_roots: list[Path]) -> set[str]:
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
    sources = gscport.Sources(script_roots, iwds, stock)
    animtrees = {p.stem for d in (bo2_root / "raw" / "animtrees", project_root / "animtrees") if d.exists()
                 for p in d.glob("*.atr")}
    port = gscport.port_map(sources, api, map_name, project_root, animtrees=animtrees)
    report.scripts = port.to_json()
    report.errors += [f"scripts: {e}" for e in port.errors]
    # empty converted-asset table for the link check; stage_fx writes the real
    # one once the effects are converted (stage_bridge)
    (project_root / "maps" / "mp" / "waw" / "_waw2bo2_assets.gsc").write_text(gscport.assets_source({}, map_name),
                                                                            encoding="utf-8")
    report.errors += [f"scripts: link: {e}" for e in gscport.link_check(project_root, api)]
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
        wanted |= {m for m in SCRIPT_MODEL_RE.findall(sources.text.get(path + ".gsc", ""))}
    stock_models = api.stock_assets.get("xmodel", set())
    models = set()
    for name in sorted(wanted):
        if any((r / "xmodel" / f"{name}.json").exists() for r in model_roots):
            models.add(name)
        elif name.lower() not in stock_models:
            report.warnings.append(f"UNSUPPORTED_ASSET xmodel {name}: used by the map scripts, not in any WaW "
                                   f"zone dump nor a stock BO2 zone")
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
    scripts = map_scripts(project_root, project)
    work = stage / "script_build"
    if work.exists():
        shutil.rmtree(work)
    (work / "zone_source").mkdir(parents=True)
    zone = f"{project}_scripts"
    (work / "zone_source" / f"{zone}.zone").write_text(
        ">game,T6\n" + "".join(f"script,{s}\n" for s in scripts), encoding="utf-8")
    linker = bo2_root / "bin" / "Linker.exe"
    proc = subprocess.run([str(linker), "--no-color", "--source-search-path", str(work / "zone_source"),
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
    proc = subprocess.run([str(oat_unlinker), "--no-color", "--include-assets", "script", "--output-folder", str(out),
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
