# WawConverter: rules every session follows

The converted map is the source of truth. For every map, WaW's code drives the game
and BO2 is only engine glue.

- **Port the map's own logic.** Rounds, spawning, zones, box, wall weapons, doors and windows,
  perks, power-ups, score, last stand: run the map's own WaW functions (its zones, its patch
  zone first), hooked where WaW calls them. Never substitute BO2's version of a system the
  map defines, and never hand-write a game rule in compat.
- **Values come from the map's data, newest source first:** `<map>_patch.ff` over the map
  zone, then tables (`mp/zombiemode.csv` overrides `set_zombie_var` literals), weapon files,
  then script literals. Trace how WaW resolves a value at runtime before trusting a literal.
- **BO2 only where T6 itself requires it** (actor and player creation, engine callbacks,
  LUI). Each such glue entry lives in `src/waw2bo2/compat/gsc_api.json` `source_driven`
  `glue` with its reason. Anything else falling back to BO2 is a bug, reported, never silent.
- **Check it on every map:** after a build, run
  `python tools/framework_coverage.py <stage>/zone_raw/<project> <map patch root> <map root> --md out.md`
  and treat every "bo2" row that is not engine glue as work to do.
- Generic converter rules only, never per-map patches. Details: `GUIDELINES.MD`,
  `docs/CONVERT_THE_WHOLE_MAP.md`.

## Workspace

- One checkout: `C:\WawConverter`. No extra worktrees or local copies (GitHub is the backup).
  Never delete a folder that may contain junctions into this checkout.
- Missing native tools: `powershell -ExecutionPolicy Bypass -File tools\restore_native.ps1`.
- Delete benchmark/scratch builds when done; keep test series short.
