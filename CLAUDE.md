# WawConverter: rules every session follows

The converted map is the source of truth. For every map, **WaW's code drives the game and BO2
is only engine glue.** Der Riese is the model: its own round loop, spawning, zones, box, wall
weapons, score, perks and power-ups run from its own scripts; BO2 only creates actors and
players and answers engine callbacks.

- **Run the map's code, never BO2's version of it.** Rounds, spawning (the zombie count
  included), zones, box, wall weapons, doors and windows, perks, power-ups, score, last stand:
  the map's own WaW functions run, called where WaW calls them. Do not lift a source formula
  into a BO2 hook (`max_zombie_func`, BO2's spawn loop, BO2's round loop); do not keep a BO2
  system running beside the map's. If the map's code does not port, fix the port (a compat
  builtin, a translator rule) or report it; never fall back to BO2's game logic.
- **The map's code is whatever the map ships.** Its zones (patch first), its mod iwds, and the
  framework it was built on: stock `_zombiemode*`, or a mod framework such as ZCT
  (`zct_*.iwd`) or UGX (`maps/_ugx*`). A mod framework's scripts are the map's scripts: port
  them like any other, no special-case game logic.
- **Values come from the map's data, newest source first:** `<map>_patch.ff` over the map
  zone, then tables (`mp/zombiemode.csv` overrides `set_zombie_var` literals), weapon files,
  then script literals. Trace how WaW resolves a value at runtime before trusting a literal.
- **BO2 only where T6 itself requires it** (actor and player creation, engine callbacks,
  LUI). Each glue entry lives in `src/waw2bo2/compat/gsc_api.json` `source_driven` `glue`
  with its reason. Anything else falling back to BO2 is a bug, reported, never silent.
- **Never hand-write a game rule in compat.** Compat is for engine semantics only (WaW
  builtins, T4 notifies T6 lacks, builtin differences).
- **Check it on every map:** every build writes `framework_coverage.md` and
  `FRAMEWORK_BO2` warnings; treat every "bo2" row that is not engine glue as work to do.
- Generic converter rules only, never per-map patches. Details: `GUIDELINES.MD` section 0,
  `docs/CONVERT_THE_WHOLE_MAP.md`.

## Workspace

- One checkout: `C:\WawConverter`. No extra worktrees or local copies (GitHub is the backup).
  Never delete a folder that may contain junctions into this checkout.
- Missing native tools: `powershell -ExecutionPolicy Bypass -File tools\restore_native.ps1`.
- Delete benchmark/scratch builds when done; keep test series short.
