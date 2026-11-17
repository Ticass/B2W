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

Tools: `native/tools/shadow_*.py` run a real stage-bridge with the Python
step and the native step on the same inputs and compare every output file.
Found while porting (fixed): ACM fails concurrent stream opens now and then;
`json::empty()` is false for every string; Windows keeps a folder's first
spelling, so folders are created in the Python order.
