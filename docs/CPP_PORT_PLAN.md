# Plan: the converter in C++

Decision (user, 2026-11-16): every part of the converter runs as C++; only the
desktop UI (`gui.py`, `gui_artwork.py`) stays Python. Priority: build speed,
with no regression in any built map.

## How it is ported without regressions

1. **Strangler, one module at a time.** Each Python module gets a C++
   replacement in `native/` with the same inputs and outputs. The Python build
   calls the native code for that step; nothing else changes in that commit.
2. **Equal outputs before the switch.** A module switches only after its C++
   output equals the Python output byte for byte on real maps (Der Riese, Shi
   No Numa, Cheese Cube, Clinic of Evil at least), plus the module's own C++
   tests. Python behaviour that depended on the hash seed is made
   deterministic in Python first, then matched.
3. **Python is the oracle until removal.** `WAW2BO2_NATIVE=0` keeps the Python
   path, and `native/tools/shadow_*.py` runs both and diffs them. The Python
   module is deleted only after its native step has shipped in playtested builds.
4. **Then the data path changes.** Once the steps that read OAT's JSON/raw dumps
   are native, they read zones in process through OAT's libraries (no
   Unlinker dump, no JSON round trip), and the final link calls OAT's linker
   library instead of a process. The closed BO2 mod tools linker stays a process.

## Layout

```
native/
  CMakeLists.txt          one static library per area, one executable
  src/core/               files, Python-compatible JSON/float text, thread pool, hashing, cache
  src/shaders/            SM3 -> HLSL translation, D3DCompile/D3DDisassemble     (shaders.py)
  src/... (one folder per area, added as they are ported)
  src/cli/                waw2bo2n.exe: one subcommand per ported step
  tests/                  Catch2 tests per area
```

No file over ~1,000 lines; areas talk through headers only.

## Order (by build time and independence)

| Step | Python today | Lines |
|---|---|---:|
| 1 | shaders.py (unbound stage, then bound) | 622 |
| 2 | audio.py, xwma.py, acm.py, audiocache.py | 507 |
| 3 | world.py, lightmaps.py, lighting.py, fbx.py, hulls.py, visibility.py | 1,800 |
| 4 | gsc.py, gscport.py, clientport.py, zones.py (script translator) | 6,200 |
| 5 | sounds.py, audioremaster.py | 2,200 |
| 6 | techsets.py, shaderruntime.py, materials, images (iwi, wavelet) | 3,500 |
| 7 | weapons.py, fx.py, entities.py, barriers.py, paths, traversals ... | 5,000 |
| 8 | t6bridge.py stage orchestration, modzone.py, builddriver.py, cli.py | 5,700 |
| 9 | in-process OAT zone reading and linking (no dumps, no JSON) | - |

The remaining small modules move with the area that uses them.

## Status

| Step | Native | Verified on | Native / Python time (Der Riese) |
|---|---|---|---|
| Translate shaders (`shaders.stage`) | yes | DR stage-bridge shadow: 27,652 files identical; full DR build equal to pure Python | 17.6 s cold, 7.4 s cached / 17.4 s |
| Decode audio (`audio.stage`, acm, xwma) | yes | DR: 3,478 files identical, cold and cached | 21 s cold, 4-6 s cached / 30 s |
| Stage sound sources (`sounds.stage`) | yes | DR shadow: 3,837 IR files, 3,477 payloads, report identical | 2.7 s / 5.5 s |
| Translate map scripts (`gscport.port_map`) | yes | 15 maps matched Python script outputs and reports, and the full Der Riese build passed | 1.7 s / 2.5 s |
| Decode render-world vertices (`world.read_gfx_world`) | yes | Der Riese, Shi No Numa, Cheese Cube, Clinic of Evil, Nightclub: Python and native render worlds equal | 1.30 s / 3.15 s (complete world read) |
| Write render-world FBX (`fbx.write_world_fbx`) | yes | Five maps matched FBX bytes and mesh mappings. Full Der Riese build passed with identical BSP files. | 0.78 s / 3.89 s |
| Stage terrain collision (`fbx.write_collision_fbx`, `t6bridge.write_collision_edges`) | yes | 23 maps matched FBX bytes, walkable-edge records, material slots, and collision diagnostics. Reader tests cover versions 2–9. | 0.05 s / 1.13 s (clip read and collision mesh staging) |
| Inspect world dumps (`cli inspect`) | yes | Five real maps and synthetic render dump versions 1–6 matched the Python report byte for byte. | — |
| Group render surfaces and stage visibility and shadows | yes | Five maps matched FBX, visibility and shadow bytes and mesh IDs. Full Der Riese build passed. | 0.85 s / 3.23 s (render and manifests) |
| Stage brush and static-model collision records | yes | 23 maps matched all three manifests and diagnostics. Der Riese's combined geometry stage matched the Python stage byte for byte. | 1.2 s / 5.3 s (combined geometry stage) |
| Convert DDS to IWI in destination batches | yes | 3,025 byte-identical textures across Der Riese, Clinic of Evil, and Nuketown. The standalone folder command also matched errors and output order. | 0.21 s / 0.43 s (812 textures) |
| Stage lightmap pages, primary lights, and reflection probes | yes | Seven maps matched files, probe assignments, and diagnostics. The full Der Riese build passed with the same 20 BSP files. | 0.08 s / 1.69 s (two lightmap pages, one worker) |
| Bake static-model light grid | yes | Six maps matched the model placements and grid byte for byte. Anguish's over-budget test returned the same palette-limit failure. | 0.09 s / 1.51 s |
| Recover textures from WaW IWDs | yes | 41 real textures matched DDS bytes and source reports across seven IWI formats. Der Riese linked and matched all 20 BSP files. | 0.25 s / 39.56 s (41 IWD requests) |
| Select material donors and prepare material JSON | yes | 2,847 source materials from Der Riese, Cheese Cube, Nightclub, and Clinic of Evil matched Python with layered vertex data enabled and disabled. Der Riese's 1,143 staged materials, 319 technique sets, and 1,780 shader binaries also matched. | 0.19 s / 1.20 s (921 materials with layered vertex data) |

`waw2bo2n inspect <gfx> <clip>` and `waw2bo2n world-fbx <gfx> <output.fbx>`
run directly, as does `waw2bo2n dds2iwi <source> <output>` for files or directories.
The Python builder still resolves source assets and filters render surfaces
before calling the native world steps. It opens the native IWD recovery
service once per build so the ZIP index and wavelet tables stay in memory.
Material dependency discovery, stock donor indexing, and binding WaW shader
programs to T6 passes still run in Python.

Tools: `native/tools/shadow_*.py` run a real stage-bridge with the Python
step and the native step on the same inputs and compare every output file.
Found while porting (fixed): ACM fails concurrent stream opens now and then;
`json::empty()` is false for every string; Windows keeps a folder's first
spelling, so folders are created in the Python order.
