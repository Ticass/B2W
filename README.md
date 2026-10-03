# waw2bo2

Fresh, fail-closed conversion pipeline for compiled World at War custom maps.
It extracts typed assets directly from the original T4 fastfile and stages them
for the Black Ops II T6 linker. It does not use live game-memory captures or any
previously converted map.

Current verified stages:

1. Offline T4 fastfile loading.
2. Lossless render-world extraction (positions, colors, UV0, lightmap UV,
   normals, tangents, material assignments, surface metadata and static-model
   placements).
3. Collision extraction (terrain triangles, collision materials and brushes).
4. Strict schema/range validation.
5. UV/material-preserving FBX generation for OAT's T6 BSP linker.

## Quick start

Set `PYTHONPATH` to `src` and run the fresh pipeline against the downloaded
WaW fastfile:

```powershell
$env:PYTHONPATH = 'C:\WawConverter\src'
python -m waw2bo2.cli convert `
  'C:\Users\thrif\AppData\Local\Activision\CoDWaW\mods\Nuketown Remastered 1.2\nuketown.ff' `
  'C:\WawConverter\work\nuketown_fresh' `
  --unlinker 'C:\WawConverter\vendor\OpenAssetTools\build\bin\Release_x86\Unlinker.exe' `
  --search-path 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty World at War\main'
```

The WaW `main` search path is important: it supplies stock images referenced by
the downloaded custom map. Without it, hundreds of false “missing texture”
reports are produced.

`convert` writes typed world/collision dumps, WaW material/image assets, GLTF
static models, and `zone_raw\zm_test\BSP\map_gfx.fbx` plus `map_col.fbx`.
`official-build` is the packaging gate and invokes the
installed `bin\cod9map64.exe -platform pc` when a `.map` source is present,
then the installed `bin\Linker.exe`; a non-zero tool result is returned and no
successful zone is claimed.

Render interchange v4 also preserves the surface ranges belonging to moving
brush models. FBX merging separates owners, and the T6 linker keeps each brush
range outside static-world visibility and camera-region ranges. Legacy render
dumps must be refreshed when collision submodels exist. Verify a linked zone
with `tools/audit_brush_render.py <map_gfx.fbx> <map.gfxworld.txt>` after dumping
its mapents with the T6 Unlinker.

Material translation is explicit and fail-closed:

```powershell
python -m waw2bo2.cli materials <gfx.bin> <t4-material-dir> <t6-template.json> <output-dir> --images <dds-dir>
```

If even one material or referenced image is absent, the command stops and
reports it. This is deliberate: silently replacing missing WaW textures is the
failure mode that caused the earlier bad conversions.

Static model coverage can be checked with:

```powershell
python -m waw2bo2.cli static-models <gfx.bin> <model_export-dir> <report.json>
```

Material/image/model translators remain fail-closed until their output passes
an actual BO2 Linker build and zone-load test. A partial or placeholder zone is
never reported as successful.

## Repository layout

- `src/waw2bo2/`: the converter (Python); `tests/`: unit tests (`python -m unittest discover -s tests`).
- `tools/`: pipeline driver (`run_bridge.ps1`) and native investigation helpers.
- `vendor/OpenAssetToolsT6/`: T6 fork of [OpenAssetTools](https://github.com/Laupetin/OpenAssetTools)
  (GPL-3.0) with the bridge linker (BSP/clipmap/gfxworld linkers, loaders, diagnostic dumpers).
  Source only: fetch its `thirdparty/` submodules from upstream before building.
- `vendor/OpenAssetTools.patch`: T4 dumper changes (material/techset/shader, world, FX, sound
  dumpers) against upstream commit `7d027e8`; apply with `git apply` on a clone at that commit.
- `AGENT_HANDOFF.md`: session-by-session engineering log; `GUIDELINES.MD`: conversion rules.

Not in the repository: extracted game data (`work/`), game installs, Activision's WaW Mod Tools,
and all built binaries.
