# Convert the Whole Map: WaW's Code Drives, BO2 Is Glue

User rules, 2026-10-25 and 2026-10-27: "we must not cherry pick stuff... we must convert the whole
map"; "We're going away from bo2 spawn loop... we now use the waw's code"; "always use waw's code
like we did with Der Riese". Every agent working on this converter follows it. It is section 0 of
GUIDELINES.MD.

## The rule

A converted map runs **the WaW code its source runs, called where the source calls it**. The
map's own systems run, not BO2's:

- round loop (`_zombiemode::round_think` and what it calls), spawning (`round_spawning`, which
  sets `level.zombie_total` itself), zombie health (`ai_calculate_health` from its call site),
  dog rounds
- zones (`_zombiemode_zone_manager`), box and wall weapons (`_zombiemode_weapons`), score
  (`_zombiemode_score`), perks, power-ups, the map's own helpers (`_zombiemode_utility`)
- whatever framework the map was built on. Stock `_zombiemode*`, a ZCT map's `zct_*.iwd`
  scripts, a UGX map's `_ugx*` scripts: all of it is the map's code.

BO2 keeps only what T6 itself requires: actor and player creation, the native AI, engine
callbacks, LUI. Each such piece is listed in `gsc_api.json` `source_driven` `glue` with the reason
T6 needs it. Everything else that falls back to BO2 is a bug, reported in the build
(`FRAMEWORK_BO2`, `framework_coverage.md`).

Hand-written compat code (`src/waw2bo2/compat/*.gsc`) is for **engine** semantics only: WaW
builtins, notifies the T4 engine sends and T6 does not, differences in what a builtin does. It is
not for game rules that already exist as WaW script. Those rules are in the source; run them.

## What not to do (each of these happened)

- **Running BO2's system and feeding it a lifted formula.** The round count was once lifted out
  of `round_spawning` into BO2's `level.max_zombie_func` so BO2's spawn loop could run. ZCT maps
  compute the count in shapes no lifter parses and their builds failed. The map's own
  `round_spawning` sets the count; run it. (Removed 2026-10-27.)
- **Hand-writing a rule from memory.** A hand-written zombie-health loop over rounds 2..N copied
  WaW's formula and lost its call site: dog rounds raised health, which WaW never does because
  `_zombiemode_dogs` swaps `round_spawn_func` and `ai_calculate_health` is never called.
- **A second hand rule to fix the first** (a dog-round list patching the health loop).
- **Reading part of a statement:** `set_zombie_var( "zombie_health_increase_percent", 10, 100 )`
  ported as `10`, dropping the divisor.
- **Trusting a literal the map overrides:** Der Riese's Juggernog is 250 from
  `mp/zombiemode.csv` in its patch zone, not the `set_zombie_var` literal.
- **Keeping BO2's helper because it has the same name:** `_zombiemode_utility::is_player_valid`
  reads WaW state BO2's version does not.

## How to apply it

Before writing or changing any gameplay behaviour:

1. **Find the source.** Read the map's scripts (patch zone first, mod iwds, then the map zone and
   stock WaW). Note who calls the function, when, and under which conditions (flags, swapped
   function pointers like `round_spawn_func`, `level.*_func` hooks).
2. **Make it source-driven.** Port the whole script (`preserved_scripts`) or mark its functions
   `source_driven` in `gsc_api.json`, so the map's calls run the map's function. Give WaW-owned
   state its own `level.waw_*` field (`WAW_LEVEL_FIELDS`) when BO2 uses the same name.
3. **Glue only for the engine.** If T6 cannot run a piece (actor creation, zbarriers, LUI), keep
   BO2's for that piece only, in `glue` with the reason, and connect it to the map's code at the
   call site.
4. **Fix ports, not behaviour.** A WaW builtin T6 lacks gets a compat wrapper with WaW's
   semantics (`IsNoTarget`, `SetOrigin`'s teleport); a statement shape the translator cannot read
   gets a translator rule. Never a substitute rule.
5. **Read whole statements.** Every argument of a source call matters. If a shape cannot be
   parsed, report it; don't guess.
6. **Keep policies thin.** A user-requested policy wraps the ported source behaviour; it never
   replaces it with a hand formula.
7. **Measure in game.** Probe the real build (Der Riese's probes in `work/der_riese_acc/`:
   spawning, score, weapons, box) and compare with the source. Tests that only check generated
   text are not proof.

## Red flags in review

- BO2's version of a game system still running for a map that defines its own.
- A source formula moved into a BO2 hook instead of the source function running.
- A compat function that recomputes a game value (health, counts, costs, timers) instead of the
  source's own function deciding it.
- A comment that says "WaW does X" without naming the source function and its call site.
- A map name or a framework name (ZCT, UGX) deciding behaviour that the source code decides.
- A `stock_map_policies` entry doing more than the user asked for.

## Still BO2 (work to do)

`framework_coverage.md` lists the current state per map. Known areas: last stand
(`_laststand`), blockers (doors and windows still BO2 zbarriers), zombie AI init
(`_zombiemode_spawner::zombie_spawn_init`, `zombie_damage`), zombie traversals (mapped onto
BO2's `zm_traverse` substates; dog traversals already convert from the source).
