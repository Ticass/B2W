# Plan: the converter in C++

Move the converter and CLI to C++ and preserve existing map behavior while
improving build speed. A native desktop UI is optional and can replace the
Python frontend once it covers the same workflows.

## How it is ported without regressions

1. **Strangler, one module at a time.** Each Python module gets a C++
   replacement in `native/` with the same inputs and outputs. The Python build
   calls the native code for that step; nothing else changes in that commit.
2. **Equal outputs before the switch.** Compare the module against the Python
   reference on Cheese Cube Unlimited. Use existing staged inputs and focused
   regressions for component checks. Python behaviour that depended on the hash seed is made
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
| Export collision FBX (`cli collision-fbx`) | yes | Four maps matched FBX and stdout bytes for full exports and bounded previews. The triangle limit is applied after resolving ownership and opposing faces. | 0.22 s / 0.84 s (standalone, including CLI startup) |
| Inspect world dumps (`cli inspect`) | yes | Five real maps and synthetic render dump versions 1–6 matched the Python report byte for byte. | — |
| Check extracted GLTF coverage (`cli static-models`) | yes | Four real maps matched the Python report and exit status. A fully covered Der Riese fixture also matched report and stdout bytes. | 0.06 s / 3.67 s (standalone, including CLI startup) |
| Group render surfaces and stage visibility and shadows | yes | Five maps matched FBX, visibility and shadow bytes and mesh IDs. Full Der Riese build passed. | 0.85 s / 3.23 s (render and manifests) |
| Stage brush and static-model collision records | yes | 23 maps matched all three manifests and diagnostics. Der Riese's combined geometry stage matched the Python stage byte for byte. | 1.2 s / 5.3 s (combined geometry stage) |
| Convert DDS to IWI in destination batches | yes | 3,025 byte-identical textures across Der Riese, Clinic of Evil, and Nuketown. The standalone folder command also matched errors and output order. | 0.21 s / 0.43 s (812 textures) |
| Stage lightmap pages, primary lights, and reflection probes | yes | Seven maps matched files, probe assignments, and diagnostics. The full Der Riese build passed with the same 20 BSP files. | 0.08 s / 1.69 s (two lightmap pages, one worker) |
| Bake static-model light grid | yes | Six maps matched the model placements and grid byte for byte. Anguish's over-budget test returned the same palette-limit failure. | 0.09 s / 1.51 s |
| Recover textures from WaW IWDs | yes | 41 real textures matched DDS bytes and source reports across seven IWI formats. Der Riese linked and matched all 20 BSP files. | 0.25 s / 39.56 s (41 IWD requests) |
| Index stock T6 material donors | yes | All 323 technique-to-material mappings matched in the stock asset cache used by Der Riese. | 0.05 s / 0.27 s |
| Select material donors and prepare material JSON | yes | 2,847 source materials from Der Riese, Cheese Cube, Nightclub, and Clinic of Evil matched Python with layered vertex data enabled and disabled. Der Riese's 1,143 staged materials, 319 technique sets, and 1,780 shader binaries also matched. | 0.19 s / 1.20 s (921 materials with layered vertex data) |
| Convert compiled WaW effects and stage model overlays | yes | 800 converted FX assets and their reports matched. Der Riese's 231 staged FX files matched. Tests cover overlay files, rejected spotlights, and transitive failures. | 0.81 s / 1.28 s (800 source effects) |
| Stage compiled weapon fields, flame tables, animations, and accuracy graphs | yes | Cheese Cube Unlimited matched 667 staged files, including two flame tables, and the report for 100 weapons. Copied animation and accuracy graph timestamps also matched. | 0.75 s / 0.83 s (Cheese Cube Unlimited with stock dependencies) |
| Stage weapon models, skeletal LODs, and physics presets | yes | All 474 staged files and the report for 181 source models matched on Cheese Cube Unlimited. The oversized world-model rewrite also passed its synthetic test. | 0.73 s / 0.47 s (stage only) |
| Stage first-person weapon shadow variants | yes | Cheese Cube Unlimited matched 87 model variants, 151 material variants, rewritten weapon infostrings, and both updated reports. | 1.12 s / 1.23 s (stage only) |
| Resolve compiled WaW dependencies | yes | Cheese Cube Unlimited matched 3,400 nodes, 9,993 edges, and 19 missing-asset diagnostics. The stock extraction service still supplies cache generations. | 1.49 s / 3.00 s (cached stock dumps) |
| Stage dynamic entities, physics constraints, and rope runtime | yes | Cheese Cube Unlimited's empty physics manifests and a fixture with a dynent, a rope, and an unsupported joint matched files and reports. | 0.09 s / 0.006 s (three stages through the Python driver) |
| Convert map entities, synthesize zones, and stage spawns | yes | Cheese Cube Unlimited matched entity and spawn manifests and the report for 1,145 entities. A fixture also matched synthesized submodels, door flags, and wall-buy pairing. | 0.037 s / 0.008 s (including native process startup and clip read) |
| Stage AI paths and traversal scripts | yes | Cheese Cube Unlimited matched 654 nodes and 8,862 links. Fixtures matched the source dog library and traversal scripts for all 13 stock walking/crawling state pairs. Equal measured distances select the lower height. | 0.097 s / 0.030 s (including native process startup) |
| Link window traversals and extend path visibility | yes | Cheese Cube Unlimited matched both staged files. A 3,073-node fixture matched all 19 new endpoints, the packed visibility data, and two unsupported-window diagnostics. | 0.061 s / 0.050 s (fixture, including native process startup) |
| Align barricades, remove replaced chunks, and retime board repairs | yes | Cheese Cube Unlimited matched its entity stage and six retimed BO2 repair files. A fixture on its collision data matched floor alignment and removal of two source brushes. | 0.024 s / 0.008 s (entities), 0.036 s / 0.004 s (timing plus repairs) |
| Stage map models, collision boxes, and model lighting origins | yes | Cheese Cube Unlimited matched 341 staged files, 161 placements, 39 generated collision boxes, 202 materials, and reports. Models that share an output file retain source order. | 0.186 s / 0.382 s (cached dumps) |
| Isolate map model names and finish translucent surfaces | yes | Cheese Cube Unlimited matched 485 aliases and all 2,934 replay files. Fixtures verify that model literals are rewritten without renaming bone tags and that vertex-alpha and sort-key fixes match Python. Writes detach staging from cache hard links. | 0.451 s / 0.715 s (identities), 0.639 s / 0.920 s (surface fixes) |
| Isolate converted material names and stage controller rumble | yes | Cheese Cube Unlimited matched 48 material aliases, 22 rumble files and all 1,402 replay files. A fixture verifies that renaming staged glTF materials leaves the cache copy intact. | 0.424 s / 0.848 s (materials), 0.162 s / 0.356 s (rumble) |
| Bind decoded PCM to aliases and match stock sound curves | yes | Cheese Cube Unlimited matched all 3,829 output files and reports, including 6,719 successful PCM bindings, 6,995 curve-complete variants and the existing 276 PCM diagnostics. | 1.66 s / 4.41 s (PCM), 1.19 s / 2.22 s (curves) |
| Stage effect models, model overlay scripts, and water bullet runtime | yes | Cheese Cube Unlimited matched 21 files, including 18 effect models and one overlay variant, plus report entries. A separate water fixture matched weapon spread formatting, the dry script, and missing-table diagnostics. | 0.124 s / 0.102 s (three stages through the Python driver), 0.063 s / 0.083 s (overlays alone) |
| Stage BO2 script templates, discover map scripts, and write the map zone | yes | Cheese Cube Unlimited's template and resume cases matched the ordered script inventory, file bytes, rawfile timestamp, diagnostics, and the existing zone manifest. | 0.085 s / 0.037 s (template case, four calls through the Python driver) |
| Read the WaW zone graph, remove the BO2 manager, and guard the template switch | yes | Cheese Cube Unlimited matched nine zones, eight links, entity/spawn files, and the patched main. Graph parsing runs with entity staging and the main edit with template staging. | — |
| Stage menu artwork, metadata, loading music and Lua wrappers | yes | Text metadata, PCM, resampled WAV and FLAC matched Python. Stock, aligned and mixed artwork matched all 62 staged files, including textures, metadata and Lua wrappers. The driver stages menus in process. | 0.13–0.27 s / 0.41–0.62 s across three artwork fixtures |
| Compile map scripts, link the map zone, package the mod (`compile-scripts`, `bridge-link`, `package`) | yes | Der Riese built with the native steps and all-Python: 54,227 staged files identical; map zone and mod.ff assets equal apart from pointer values in the unlinker's text dumps and mod.zone line order | — |
| Audit built zones (`audit-zone`: material arguments, bone hitboxes) | yes | Runs in the native driver on every build | — |
| Shared stock caches and map source dumps (`all2raw` prepare / source_dumps) | yes | The existing WaW and BO2 caches and Cheese Cube Unlimited's four source dumps were found complete by the native keys and inventories; source dumps returned the same folders as Python. Catch2 tests run a scripted unlinker. | 0.6 s / — (cached) |
| Link the gameplay and lobby zones (`build-mod`: modzone.py, impact and probe material binding, lobby tables) | yes | Cheese Cube Unlimited built natively and all-Python: 51,213 staged files identical; map zone, mod.ff and mod_load.ff assets equal apart from pointer values in the unlinker's text dumps and zone line order. Catch2 tests cover csv parity, map name keys and zone edits. | 97.5 s / 92.6 s (both bound by the mod tools linker: 51 s baseline link, 41 s shader relink) |
| Stage localization, visions with the film overlay, and missile-only prop collision (`localization.stage`, `visions.stage` + `glow.stage`, `projectilecollision.recover`; shared `wawassets.StockWawAssets`) | yes | Cheese Cube Unlimited built natively and all-Python: 51,213 staged files identical. The localization, vision and glow test suites run through the native steps. | — |
| Stage materials and effects, port the map scripts, hook the map main (`stage-materials`, `stage-effects` on the IWD service, `port-scripts`, `hook-map-scripts`) | yes | Cheese Cube Unlimited built natively and all-Python: every staged file identical. | — |
| Sound: the bank, the mix measurements, discovery and the ambient script (`write-sound-bank`, `sound-mix`, `discover-sounds`, `write-amb-csc`) | yes | Cheese Cube Unlimited: every staged file, the report content and warnings identical. Curve areas use Python's compensated float sum; script/entity references resolve their graph in process. | — |
| Pack streamed sound aliases as lossless FLAC | yes | Cached Cheese Cube Unlimited replay matched the alias CSV, report, and all 1,759 encoded files byte for byte. The encoder reduced those files from 714 MiB to 417 MiB. | About 15 s on both paths, including encoding |
| Stage owned actor animation states and trees | yes | BO2's stock dog graph retains its attack and window traversal states. A source traversal fixture matched staged animations, state graphs, rewritten actor scripts and missing-animation diagnostics byte for byte. | — |
| Framework coverage, WaW HUD, menu art and in-game menus (`framework-coverage`, `stage-waw-hud`, `stage-waw-menu-art` on the IWD service, `stage-waw-menus`) | yes | `textures/pillow_decode` matched Pillow byte for byte on 630 stock images of all seven DDS formats. The menu translator matched Python on WaW's raw/ui sources; Cheese Cube Unlimited identical. | — |
| Carried weapons (`stage-weapon-runtime`: weapon sound events, runtime selection, BO2 equivalents; `finish-weapon-runtime`: map string tables, assets script, wall buys) | yes | Cheese Cube Unlimited: every staged file, the report content and warnings identical. | — |

`waw2bo2n inspect <gfx> <clip>` and `waw2bo2n world-fbx <gfx> <output.fbx>`
run directly, as does `waw2bo2n dds2iwi <source> <output>` for files or directories.
The Python builder still resolves source assets and filters render surfaces
before calling the native world steps. It opens the native IWD recovery
service once per build so the ZIP index and wavelet tables stay in memory.
Material dependency discovery, donor index cache invalidation, and binding
WaW shader programs to T6 passes still run in Python.
The effect source resolver and its stock-zone extraction callbacks also still
run in Python.
Weapon dependency discovery, skeletal model staging, and gameplay registration
still run in Python.

## Desktop launcher

`native/build/B2W.exe` is the native launcher: Win32, Direct3D 11 and Dear
ImGui (`vendor/imgui`, v1.93.0, MIT), laid out like the T6 Mod Tools
Launcher. It has the tkinter launcher's workflows: map choice, Setup paths
and preflight, Extract All, Build Map with step tracking, Install, Launch,
reports, diagnostics bundles and the console. `native/src/launcher` holds
the services without UI. `native/tools/check_launcher.py` compares its
settings, discovery, naming, build paths, build command and preflight with
`launcher.py`. Five settings files matched, including invalid names,
missing artwork and an empty build folder.
Builds still run the conversion worker (`WAW2BO2_WORKER`, the portable
`WawConverter.CLI.exe`, else `python -m waw2bo2.cli`) until the
pipeline is native. The Artwork tab draws `menupreviews.py`'s globe, map selection, lobby and
loading screens on the GPU: the globe is a pixel shader, stock DXT textures
upload as BC1/BC3, and clicks place the map and the start location. The
Refresh, Resume, BO2 stock perks and Remaster options are not offered, and
saved values are cleared. The native executables are long-path aware.
Map names near 25 characters push staging paths past `MAX_PATH`, which the closed
BO2 mod tools linker cannot open. The release packaging still ships the
tkinter launcher.

## Disk usage

Dependency extraction shares large files across generations after comparing
their bytes. A Cheese Cube Unlimited dependency sample reused 173 files and
101 MiB without changing any asset bytes.

Set `$env:WAW2BO2_STREAM_FLAC = '1'` in PowerShell before starting a build
to encode streamed bank entries as lossless FLAC when the result is smaller.
The option uses the native converter and discovers FFmpeg the same way as
audio resampling. Loaded sounds and the staged PCM used by
audits keep their WAV input.
Cheese Cube Unlimited's linked streamed bank dropped from 714 MiB to 415 MiB.
All 1,794 streams and the loaded bank matched the original decoded PCM. In-game
playback validation remains pending.

Tools: `native/tools/shadow_*.py` run a real stage-bridge with the Python
step and the native step on the same inputs and compare every output file.
Cheese Cube Unlimited is the current full-build fixture. Reuse its existing
staging for isolated linker and size checks.
Found while porting (fixed): ACM fails concurrent stream opens now and then;
`json::empty()` is false for every string; Windows keeps a folder's first
spelling, so folders are created in the Python order.
