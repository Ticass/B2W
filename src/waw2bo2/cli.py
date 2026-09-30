from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import os
import sys
from pathlib import Path

from . import iwi, t6bridge, wawsource, weapons
from .fbx import write_collision_fbx, write_world_fbx
from .materials import translate_materials
from .world import inspect_collision, read_collision, read_gfx_world


def _inspect(args: argparse.Namespace) -> int:
    world = read_gfx_world(args.gfx)
    clip = inspect_collision(args.clip)
    report = {
        "world": world.name,
        "vertices": len(world.vertices),
        "indices": len(world.indices),
        "triangles": len(world.indices) // 3,
        "surfaces": len(world.surfaces),
        "materials": len({s.material for s in world.surfaces}),
        "static_models": len(world.static_models),
        "collision_vertices": clip.vertex_count,
        "collision_triangles": clip.triangle_count,
        "collision_brushes": clip.brush_count,
        "collision_materials": clip.material_count,
    }
    print(json.dumps(report, indent=2))
    return 0


def _fbx(args: argparse.Namespace) -> int:
    world = read_gfx_world(args.gfx)
    if args.surface_limit is not None:
        if args.surface_limit < 1:
            raise ValueError("--surface-limit must be positive")
        world.surfaces = world.surfaces[: args.surface_limit]
    write_world_fbx(world, args.output)
    print(f"wrote {args.output} ({len(world.surfaces)} surfaces, {len(world.vertices)} vertices)")
    return 0


def _collision_fbx(args: argparse.Namespace) -> int:
    collision = read_collision(args.clip)
    if args.triangle_limit is not None:
        if args.triangle_limit < 1:
            raise ValueError("--triangle-limit must be positive")
        collision.indices = collision.indices[: args.triangle_limit * 3]
        collision.summary.triangle_count = len(collision.indices) // 3
    write_collision_fbx(collision, args.output)
    print(f"wrote {args.output} ({collision.summary.triangle_count} collision triangles)")
    return 0


def _extract(args: argparse.Namespace) -> int:
    args.output.mkdir(parents=True, exist_ok=True)
    command = [str(args.unlinker), "--no-color", "--include-assets", "gfxworld,clipmap,mapents", "--output-folder", str(args.output)]
    if args.search_path:
        command += ["--search-path", ";".join(str(p) for p in args.search_path)]
    command.append(str(args.fastfile))
    return subprocess.run(command, check=False).returncode


def _official_build(args: argparse.Namespace) -> int:
    """Run the installed BO2 PC compiler/linker against a staged project.

    This deliberately does not fall back to the interchange linker: a non-zero
    result is surfaced to the caller and no zone is labelled as converted.
    """
    bo2 = args.bo2.resolve()
    stage = args.stage.resolve()
    project = args.project
    map_file = stage / "map_source" / f"{project}.map"
    if map_file.exists():
        cod9map = bo2 / "bin" / "cod9map64.exe"
        result = subprocess.run([str(cod9map), "-platform", "pc", str(map_file)], cwd=bo2)
        if result.returncode:
            return result.returncode
    linker = bo2 / "bin" / "Linker.exe"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    command = [str(linker), "--no-color", "--base-folder", str(stage),
               "--output-folder", str(output), project]
    result = subprocess.run(command, cwd=bo2)
    return result.returncode


def _convert(args: argparse.Namespace) -> int:
    """Fresh extraction/staging entry point.

    It intentionally stops before packaging unless the caller supplies a
    source project and asks for the official build. This prevents a visually
    plausible but broken placeholder zone from being emitted.
    """
    args.output.mkdir(parents=True, exist_ok=True)
    extract = [str(args.unlinker), "--no-color", "--image-format", "DDS",
               "--model-format", "GLTF", "--include-assets",
               "gfxworld,clipmap,mapents,xmodel,material,image",
               "--output-folder", str(args.output)]
    if args.search_path:
        extract += ["--search-path", ";".join(str(p) for p in args.search_path)]
    extract.append(str(args.fastfile))
    rc = subprocess.run(extract).returncode
    if rc:
        return rc
    maps = args.output / "waw2bo2" / "maps"
    gfx = next(maps.glob("*.d3dbsp.gfx.bin"), None)
    clip = next(maps.glob("*.d3dbsp.clip.bin"), None)
    if gfx is None or clip is None:
        raise ValueError("extraction did not produce both gfxworld and clipmap dumps")
    bsp = args.output / "zone_raw" / args.project / "BSP"
    bsp.mkdir(parents=True, exist_ok=True)
    write_world_fbx(read_gfx_world(gfx), bsp / "map_gfx.fbx")
    write_collision_fbx(read_collision(clip), bsp / "map_col.fbx")
    # Preserve the extracted dependency tree in the project search path. The
    # official linker can then resolve images/materials/models without looking
    # back into the WaW installation.
    for asset_dir in ("materials", "images", "model_export"):
        source_dir = args.output / asset_dir
        if source_dir.exists():
            shutil.copytree(source_dir, args.output / "zone_raw" / args.project / asset_dir,
                            dirs_exist_ok=True)
    ents = maps / f"{gfx.name.removesuffix('.gfx.bin')}.ents"
    if ents.exists():
        ent_dst = args.output / "zone_raw" / args.project / "maps" / "mp" / f"{args.project}.d3dbsp.ents"
        ent_dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ents, ent_dst)
    if args.bo2:
        project_root = args.output / "zone_raw" / args.project
        boilerplate = args.bo2 / "mods" / "zm_test"
        for folder in ("maps", "clientscripts", "vision", "soundbank"):
            source_dir = boilerplate / folder
            if source_dir.exists():
                shutil.copytree(source_dir, project_root / folder, dirs_exist_ok=True)
        script_dir = project_root / "maps" / "mp"
        for suffix in ("", "_classic", "_fx", "_gamemodes"):
            src = script_dir / f"zm_test{suffix}.gsc"
            if src.exists():
                (script_dir / f"{args.project}{suffix}.gsc").write_text(
                    src.read_text(encoding="utf-8", errors="replace").replace("zm_test", args.project),
                    encoding="utf-8"
                )
        zone_source = args.output / "zone_source"
        zone_source.mkdir(parents=True, exist_ok=True)
        (zone_source / f"{args.project}.zone").write_text(
            "\n".join([
                ">game,T6",
                "clipmap_pvs,maps/mp/%s.d3dbsp" % args.project,
                "comworld,maps/mp/%s.d3dbsp" % args.project,
                "gameworldsp,maps/mp/%s.d3dbsp" % args.project,
                "mapents,maps/mp/%s.d3dbsp" % args.project,
                "gfxworld,maps/mp/%s.d3dbsp" % args.project,
                "script,maps/mp/%s.gsc" % args.project,
                "script,maps/mp/%s_classic.gsc" % args.project,
                "script,maps/mp/%s_fx.gsc" % args.project,
                "script,maps/mp/%s_gamemodes.gsc" % args.project,
                "",
            ]), encoding="utf-8"
        )
    print(f"staged fresh BSP sources in {bsp}")
    if args.bo2:
        build = argparse.Namespace(stage=args.output, project=args.project,
                                   output=args.output / "zone_out", bo2=args.bo2)
        return _official_build(build)
    return 0


def _materials(args: argparse.Namespace) -> int:
    world = read_gfx_world(args.gfx)
    names = {s.material for s in world.surfaces}
    count = translate_materials(args.source, names, args.template, args.output, args.images,
                                args.allow_generated_aliases, args.allow_missing_images)
    print(f"wrote {count} strict T6 material definitions")
    return 0


def _models(args: argparse.Namespace) -> int:
    world = read_gfx_world(args.gfx)
    available = {p.stem.lower().removesuffix("_lod0") for p in args.model_dir.rglob("*.gltf")}
    missing = sorted({m.name for m in world.static_models if m.name.lower() not in available})
    report = {
        "placements": len(world.static_models),
        "unique_models": len({m.name for m in world.static_models}),
        "available_unique_models": len({m.name.lower() for m in world.static_models if m.name.lower() in available}),
        "missing_models": missing,
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if missing:
        raise ValueError(f"{len(missing)} static models have no extracted GLTF; see {args.output}")
    print(json.dumps(report, indent=2))
    return 0


def _dds2iwi(args: argparse.Namespace) -> int:
    sources = sorted(args.source.glob("*.dds")) if args.source.is_dir() else [args.source]
    failures = 0
    for src in sources:
        dst = args.output / (src.stem + ".iwi") if args.source.is_dir() else args.output
        try:
            header = iwi.convert_file(src, dst)
            print(f"{src.name} -> {dst.name} fmt={header['format']:#x} flags={header['flags']:#x} "
                  f"{header['width']}x{header['height']}")
        except iwi.IwiError as exc:
            failures += 1
            print(f"{src.name}: {exc}", file=sys.stderr)
    return 1 if failures else 0


def _stage_weapons(args: argparse.Namespace) -> int:
    stock = None
    if args.waw_root or args.t4_unlinker:
        if not args.waw_root or not args.t4_unlinker:
            raise ValueError("stock WaW dependency resolution needs both --waw-root and --t4-unlinker")
        from .wawassets import StockWawAssets
        stock = StockWawAssets(args.waw_root.resolve(), args.t4_unlinker.resolve(),
                              (args.waw_stock_dumps or args.output / "stock_dumps").resolve())
        stock.load()
    report = weapons.stage([p.resolve() for p in args.root], args.output.resolve(),
                           set(args.weapon) if args.weapon else None, stock,
                           {("xmodel", n) for n in args.viewmodel or []})
    print(json.dumps({"status": report["status"], "weapons": len(report["weapons"]),
                      "animations": len(report["animations"]),
                      "missing_animations": report["missing_animations"],
                      "report": str(args.output.resolve() / "weapons.stage.json")}, indent=2))
    if args.stock_materials or args.techset_dump:
        if not args.stock_materials or not args.techset_dump:
            raise ValueError("weapon visuals require both --stock-materials and --techset-dump")
        resolved_roots = [Path(p) for p in report.get("resolved_roots", [])] or [p.resolve() for p in args.root]
        visuals = weapons.stage_visuals(resolved_roots, args.output.resolve(),
                                         args.stock_materials.resolve(), args.techset_dump.resolve())
        print(f"Weapon visuals: {len(visuals['materials'])} materials, {len(visuals['images'])} images; "
              f"{len(visuals['errors'])} unresolved dependencies; see weapons.visuals.json")
        if visuals["errors"]:
            return 1
    return 0


def _stage_bridge(args: argparse.Namespace) -> int:
    waw_root = args.waw_root.resolve() if args.waw_root else None
    mod_tools = wawsource.locate_mod_tools(args.waw_mod_tools.resolve() if args.waw_mod_tools else None,
                                          waw_root, Path(__file__).resolve().parents[2])
    if args.waw_source_fx:
        if mod_tools is None:
            print("error: --waw-source-fx needs the WaW Mod Tools (--waw-mod-tools; none found automatically)",
                  file=sys.stderr)
            return 2
    report = t6bridge.stage_bridge(
        stage=args.stage.resolve(), project=args.project, gfx_bin=args.gfx.resolve(),
        clip_bin=args.clip.resolve(),
        stock_materials=args.stock_materials.resolve(),
        techset_dump=args.techset_dump.resolve() if args.techset_dump else None,
        bo2_root=args.bo2.resolve() if args.bo2 else None,
        template_root=args.script_template.resolve() if args.script_template else None,
        template_name=args.script_template_name,
        extra_roots=[p.resolve() for p in args.extra_root or []],
        iwd_dirs=[p.resolve() for p in args.iwd_dir] if args.iwd_dir else None,
        waw_map_script=args.waw_map_script.resolve() if args.waw_map_script else None,
        waw_script_roots=[p.resolve() for p in args.waw_script_root or []],
        waw_stock_scripts=args.waw_stock_scripts.resolve() if args.waw_stock_scripts else None,
        t6_unlinker=args.t6_unlinker.resolve() if args.t6_unlinker else None,
        fx_fallback=args.fx_fallback,
        waw_root=args.waw_root.resolve() if args.waw_root else None,
        t4_unlinker=args.t4_unlinker.resolve() if args.t4_unlinker else None,
        waw_stock_dumps=args.waw_stock_dumps.resolve() if args.waw_stock_dumps else None,
        waw_mod_tools=mod_tools if args.waw_source_fx else None,
        wavelet_binary=mod_tools / "bin" / "AssetViewer.exe" if mod_tools else None,
        audio_decoder=args.audio_decoder.resolve() if args.audio_decoder else None,
        xwma_decoder=args.xwma_decoder.resolve() if args.xwma_decoder else None,
        t6_sound_driver=args.t6_sound_driver.resolve() if args.t6_sound_driver else None,
        approximate_sound_curves=args.approximate_sound_curves,
        waw_source_dumps=args.waw_source_dumps.resolve() if args.waw_source_dumps else None,
        ipak=not args.no_ipak,
    )
    print(f"collision: {json.dumps(report.collision)}")
    degraded = [m for m in report.materials if m.get("notes")]
    print(f"materials: {len(report.materials)} written, {len(degraded)} with degradations "
          f"(see zone_raw/{args.project}/bridge_stage.report.json)")
    print(f"images: {len(report.images)} IWI written; technique sets used: {len(report.techsets)}")
    for w in report.warnings:
        print(f"warning: {w}")
    for e in report.errors[:40]:
        print(f"error: {e}", file=sys.stderr)
    if len(report.errors) > 40:
        print(f"... {len(report.errors) - 40} more errors in the report", file=sys.stderr)
    return 1 if report.errors else 0


def _build_mod(args: argparse.Namespace) -> int:
    from . import modzone

    extra = args.stage.resolve() / "zone_raw" / args.project / t6bridge.MOD_EXTRA_ZONE
    extra_lines = extra.read_text(encoding="utf-8").splitlines() if extra.exists() else []
    # converted sound banks name their PCM relative to the decoded audio root
    project_root = extra.parent
    if args.linker and args.techset_dump:
        with modzone.baseline_shader_materials(project_root, args.techset_dump) as active:
            ff, unavailable = modzone.link_mod(args.bo2.resolve(), args.work, args.unlinker.resolve(), extra_lines,
                project_root, [project_root/'content_source/pcm', project_root/'content_source'])
        if active:
            modzone.activate_mod_shaders(ff, project_root, args.work, args.linker, args.unlinker, args.techset_dump)
    else:
        ff, unavailable = modzone.link_mod(args.bo2.resolve(), args.work, args.unlinker.resolve(), extra_lines,
            project_root, [project_root/'content_source/pcm', project_root/'content_source'])
    print(f"mod.ff: {ff}")
    weapons = modzone.unavailable_weapons(unavailable)
    project_root = args.stage.resolve() / "zone_raw" / args.project
    for entry in unavailable:
        print(f"template entry unavailable: {entry}")
    for script in t6bridge.map_scripts(project_root, args.project):
        for line in modzone.prune_weapon_references(project_root / script, weapons):
            print(f"{script}: disabled {line}")
    return 0


def _package(args: argparse.Namespace) -> int:
    from . import package

    dest = package.package(args.project, args.stage.resolve() / "zone_out" / args.project, args.work.resolve() / "out",
                           args.dest)
    print(f"packaged {dest}")
    print("launch: " + " ".join(f'"{c}"' if " " in c else c for c in package.launch_command(args.project, args.bo2)))
    return 0


def _compile_scripts(args: argparse.Namespace) -> int:
    compiled, _ = t6bridge.compile_scripts(args.stage.resolve(), args.project, args.bo2.resolve(),
                                           args.unlinker.resolve())
    for name in compiled:
        print(f"compiled {name}")
    return 0


def _bridge_link(args: argparse.Namespace) -> int:
    stage = args.stage.resolve()
    command = t6bridge.linker_command(args.linker.resolve(), stage, args.project,
                                      args.techset_dump.resolve() if args.techset_dump else None)
    log_path = args.log or (stage / f"{args.project}_bridge.log")
    print(" ".join(f'"{c}"' if " " in c else c for c in command))
    # KNOWN ISSUE: the bridge linker crashes nondeterministically (access
    # violation, ~1 in 3 runs on this map, same inputs link fine on retry).
    # Retry a bounded number of times and report every crash; not yet root-caused.
    access_violation = 0xC0000005
    for attempt in range(1, 4):
        proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              cwd=str(stage), text=True, errors="replace")
        if proc.returncode != access_violation:
            break
        print(f"bridge linker crashed (access violation) on attempt {attempt}; retrying", file=sys.stderr)
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        log.write(proc.stdout)
    lines = proc.stdout.splitlines()
    missing = sorted({l for l in lines if l.startswith("Missing asset")})
    failed = [l for l in lines if "has failed" in l or l.startswith("ERROR") or "Failed to" in l]
    for l in missing[:30]:
        print(l)
    for l in failed[:30]:
        print(l)
    ff = stage / "zone_out" / args.project / f"{args.project}.ff"
    print(f"linker exit code {proc.returncode}; {len(missing)} distinct missing assets; log: {log_path}")
    if proc.returncode or missing or failed or not ff.exists():
        print("bridge link NOT successful", file=sys.stderr)
        return proc.returncode or 1
    print(f"bridge fastfile: {ff}")
    return 0


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="waw2bo2")
    sub = p.add_subparsers(dest="command", required=True)
    inspect_p = sub.add_parser("inspect", help="validate and report fresh offline world dumps")
    inspect_p.add_argument("gfx", type=Path)
    inspect_p.add_argument("clip", type=Path)
    inspect_p.set_defaults(func=_inspect)
    fbx_p = sub.add_parser("world-fbx", help="write a UV/material-preserving T6 BSP source FBX")
    fbx_p.add_argument("gfx", type=Path)
    fbx_p.add_argument("output", type=Path)
    fbx_p.add_argument("--surface-limit", type=int)
    fbx_p.set_defaults(func=_fbx)
    col_p = sub.add_parser("collision-fbx", help="write the original collision mesh as a T6 BSP source FBX")
    col_p.add_argument("clip", type=Path)
    col_p.add_argument("output", type=Path)
    col_p.add_argument("--triangle-limit", type=int)
    col_p.set_defaults(func=_collision_fbx)
    extract_p = sub.add_parser("extract", help="extract typed world assets from a WaW fastfile")
    extract_p.add_argument("fastfile", type=Path)
    extract_p.add_argument("output", type=Path)
    extract_p.add_argument("--unlinker", type=Path, required=True)
    extract_p.add_argument("--search-path", type=Path, action="append")
    extract_p.set_defaults(func=_extract)
    build_p = sub.add_parser("official-build", help="run the installed BO2 PC compiler/linker")
    build_p.add_argument("stage", type=Path)
    build_p.add_argument("project")
    build_p.add_argument("output", type=Path)
    build_p.add_argument("--bo2", type=Path, required=True)
    build_p.set_defaults(func=_official_build)
    conv_p = sub.add_parser("convert", help="fresh extract and stage a map for BO2")
    conv_p.add_argument("fastfile", type=Path)
    conv_p.add_argument("output", type=Path)
    conv_p.add_argument("--unlinker", type=Path, required=True)
    conv_p.add_argument("--project", default="zm_test")
    conv_p.add_argument("--bo2", type=Path)
    conv_p.add_argument("--search-path", type=Path, action="append")
    conv_p.set_defaults(func=_convert)
    mat_p = sub.add_parser("materials", help="translate used T4 materials to strict T6 JSON")
    mat_p.add_argument("gfx", type=Path)
    mat_p.add_argument("source", type=Path)
    mat_p.add_argument("template", type=Path)
    mat_p.add_argument("output", type=Path)
    mat_p.add_argument("--images", type=Path)
    mat_p.add_argument("--allow-generated-aliases", action="store_true")
    mat_p.add_argument("--allow-missing-images", action="store_true")
    mat_p.set_defaults(allow_generated_aliases=False)
    mat_p.set_defaults(allow_missing_images=False)
    mat_p.set_defaults(func=_materials)
    model_p = sub.add_parser("static-models", help="verify extracted GLTF coverage for world placements")
    model_p.add_argument("gfx", type=Path)
    model_p.add_argument("model_dir", type=Path)
    model_p.add_argument("output", type=Path)
    model_p.set_defaults(func=_models)
    iwi_p = sub.add_parser("dds2iwi", help="convert DDS (file or folder) to BO2 IWI v27")
    iwi_p.add_argument("source", type=Path)
    iwi_p.add_argument("output", type=Path)
    iwi_p.set_defaults(func=_dds2iwi)
    weapons_p = sub.add_parser("stage-weapons", help="stage original compiled weapons/animations; report remaining dependencies")
    weapons_p.add_argument("output", type=Path)
    weapons_p.add_argument("--root", type=Path, action="append", required=True,
                           help="compiled WaW dump roots in source priority order (repeatable)")
    weapons_p.add_argument("--weapon", action="append", help="select weapon plus alternate closure; default: all")
    weapons_p.add_argument("--stock-materials", type=Path, help="T6 material donors for weapon visuals")
    weapons_p.add_argument("--techset-dump", type=Path, help="T6 techniquesets and shader_bin dump")
    weapons_p.add_argument("--waw-root", type=Path, help="stock WaW install for missing original dependencies")
    weapons_p.add_argument("--t4-unlinker", type=Path, help="native T4 extractor for stock dependency definitions")
    weapons_p.add_argument("--waw-stock-dumps", type=Path, help="stock WaW index/dump cache")
    weapons_p.add_argument("--viewmodel", action="append", help="additional map viewmodel to resolve (repeatable)")
    weapons_p.set_defaults(func=_stage_weapons)
    sb_p = sub.add_parser("stage-bridge", help="stage materials/images/scripts/zone for the OAT T6 BSP bridge")
    sb_p.add_argument("stage", type=Path, help="unlinker output folder (e.g. work\\final_stage)")
    sb_p.add_argument("project", help="zone/map name, e.g. zm_nuketown_waw")
    sb_p.add_argument("--gfx", type=Path, required=True, help="waw2bo2\\maps\\<map>.d3dbsp.gfx.bin")
    sb_p.add_argument("--clip", type=Path, required=True, help="waw2bo2\\maps\\<map>.d3dbsp.clip.bin (v3)")
    sb_p.add_argument("--waw-map-script", type=Path,
                      help="the WaW map GSC (rawfile dump) whose zone graph is carried into the BO2 map script")
    sb_p.add_argument("--iwd-dir", type=Path, action="append",
                      help="folder of WaW .iwd archives; an image absent from every zone dump AND every IWD is "
                           "proven missing from the source and gets a reported neutral placeholder")
    sb_p.add_argument("--extra-root", type=Path, action="append",
                      help="unlinker dump of a zone WaW loads with the map (mod.ff, common.ff); repeatable, in order")
    sb_p.add_argument("--stock-materials", type=Path, required=True,
                      help="T6 material JSON dumped from a stock zone (donors), e.g. work\\stock_t6_assets\\materials")
    sb_p.add_argument("--techset-dump", type=Path,
                      help="folder containing techniquesets\\ and shader_bin\\ dumped from the same stock zone")
    sb_p.add_argument("--bo2", type=Path, help="BO2 install (for raw\\animtrees\\fxanim_props.atr)")
    sb_p.add_argument("--script-template", type=Path, help="zone_raw folder holding template scripts")
    sb_p.add_argument("--script-template-name", help="name used in the template script file names")
    sb_p.add_argument("--no-ipak", action="store_true", help="do not emit the >ipak section")
    sb_p.add_argument("--waw-script-root", type=Path, action="append",
                      help="rawfile dump of a zone WaW loads with the map (mod.ff), searched after the map zone "
                           "and before the IWDs")
    sb_p.add_argument("--fx-fallback", action="store_true",
                      help="OPT-IN: play WaW effects that are not converted as stock BO2 effects "
                           "(compat/fx_fallback.json + compat/overrides/<map>.json); each is reported")
    sb_p.add_argument("--t6-unlinker", type=Path, help="OAT T6 Unlinker.exe (lists the stock BO2 zones' assets)")
    sb_p.add_argument("--waw-root", type=Path, help="WaW install (stock zones hold assets the map only references)")
    sb_p.add_argument("--t4-unlinker", type=Path, help="OAT T4 Unlinker.exe (lists/dumps stock WaW zones)")
    sb_p.add_argument("--waw-stock-dumps", type=Path, help="folder for stock WaW zone dumps and their index")
    sb_p.add_argument("--audio-decoder", type=Path,
                      help="optional external audio decoder; supported RIFF codecs use verified native decoding")
    sb_p.add_argument("--xwma-decoder", type=Path,
                      help="native XWMA bridge executable (default: tools/bin/xaudio_wma_decoder.exe)")
    sb_p.add_argument("--waw-mod-tools", type=Path,
                      help="WaW Mod Tools (bin/linker_pc.exe; raw/ there or in --waw-root). Default: found in "
                           "--waw-root or a wawModTools folder next to the project")
    sb_p.add_argument("--waw-source-fx", action="store_true",
                      help="OPT-IN: effects in no compiled WaW zone are compiled from the Mod Tools raw/fx with "
                           "WaW's own linker (content the WaW build did not show); each is reported "
                           "WAW_SOURCE_ASSET")
    sb_p.add_argument("--waw-source-dumps", type=Path, help="folder for effects compiled from Mod Tools sources")
    sb_p.add_argument("--waw-stock-scripts", type=Path,
                      help="rawfile dump of the stock WaW zones (common.ff, nazi_zombie_*.ff): the WaW framework")
    sb_p.add_argument("--t6-sound-driver", type=Path,
                      help="stock BO2 sounddriverglobals/singleton.w2bsdg (default: under --techset-dump)")
    sb_p.add_argument("--approximate-sound-curves", action="store_true",
                      help="compat: bind WaW falloff curves no stock BO2 curve matches to the nearest shape "
                           "(each one reported as APPROXIMATED_SOUND_CURVE)")
    sb_p.set_defaults(func=_stage_bridge)
    bm_p = sub.add_parser("build-mod", help="link the zombies gameplay mod.ff with the BO2 mod tools linker")
    bm_p.add_argument("stage", type=Path)
    bm_p.add_argument("project")
    bm_p.add_argument("--bo2", type=Path, required=True)
    bm_p.add_argument("--work", type=Path, required=True, help="build folder for mod.ff")
    bm_p.add_argument("--unlinker", type=Path, required=True, help="OAT T6 Unlinker.exe (lists stock zone scripts)")
    bm_p.add_argument("--linker", type=Path, help="T6 linker supporting translated technique sets")
    bm_p.add_argument("--techset-dump", type=Path, help="stock T6 shader dependencies for translated passes")
    bm_p.set_defaults(func=_build_mod)
    pk_p = sub.add_parser("package", help="assemble the Plutonium mod folder")
    pk_p.add_argument("stage", type=Path)
    pk_p.add_argument("project")
    pk_p.add_argument("--bo2", type=Path, required=True)
    pk_p.add_argument("--work", type=Path, required=True, help="mod.ff build folder")
    pk_p.add_argument("--dest", type=Path, help="default: %%LOCALAPPDATA%%\\Plutonium\\storage\\t6\\mods\\<project>")
    pk_p.set_defaults(func=_package)
    cs_p = sub.add_parser("compile-scripts", help="compile map GSC/CSC with the BO2 mod tools linker")
    cs_p.add_argument("stage", type=Path)
    cs_p.add_argument("project")
    cs_p.add_argument("--bo2", type=Path, required=True)
    cs_p.add_argument("--unlinker", type=Path, required=True, help="OAT T6 Unlinker.exe")
    cs_p.set_defaults(func=_compile_scripts)
    bl_p = sub.add_parser("bridge-link", help="run the OAT T6 bridge linker with a controlled search path")
    bl_p.add_argument("stage", type=Path)
    bl_p.add_argument("project")
    bl_p.add_argument("--linker", type=Path, required=True)
    bl_p.add_argument("--techset-dump", type=Path)
    bl_p.add_argument("--log", type=Path)
    bl_p.set_defaults(func=_bridge_link)
    shader_p = sub.add_parser("translate-shader", help="translate native WaW SM3 bytecode to SM5 HLSL/DXBC")
    shader_p.add_argument("source", type=Path)
    shader_p.add_argument("output", type=Path, help="output filename stem (.hlsl/.cso/.json)")
    shader_p.add_argument("--bindings", type=Path, help="explicit T6 pass input/constant/texture contract JSON")
    shader_p.add_argument("--no-compile", action="store_true", help="emit HLSL only")
    shader_p.set_defaults(func=_translate_shader)
    return p


def _translate_shader(args: argparse.Namespace) -> int:
    from . import shaders
    bindings = json.loads(args.bindings.read_text()) if args.bindings else None
    report = shaders.translate_file(args.source, args.output, bindings, compile=not args.no_compile)
    print(json.dumps(report, indent=2))
    return 0


def main(argv: list[str] | None = None) -> int:
    try:
        args = parser().parse_args(argv)
        return args.func(args)
    except Exception as exc:
        if os.environ.get("WAW2BO2_TRACEBACK"):
            raise
        print(f"waw2bo2: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

