# Convert the Whole Map: Source Code Decides, Never Cherry-Pick

User rule, 2026-10-25: "we must not cherry pick stuff... we must convert the whole map."
Every agent working on this converter follows it. It sharpens GUIDELINES.MD sections 3, 14, 15 and 28.

## The rule

A converted map runs **the WaW code its source runs, called where the source calls it**. When the
converter replaces a WaW system with a BO2 one (round loop, perks, spawner, dogs...), the WaW
functions inside it do not disappear. Each one is either:

1. **ported from the source** (extracted by gscport) and called from the BO2 side at the point
   where the WaW caller called it, or
2. **reported as unported** (a build report line naming the source function and why).

Never quietly drop it. Never rewrite it by hand from what you remember or think it does.

Hand-written compat code (`src/waw2bo2/compat/*.gsc`) is for **engine** semantics only: WaW
builtins, notifies the T4 engine sends and T6 does not, differences in what a builtin does. It is
not for game rules that already exist as WaW script. Those rules are in the source; port them.

## Why: the zombie-health failure (2026-10-25)

Players saw insta-kill rounds at round 163 on Shi No Numa, and dog rounds raised zombie health on
Shi No Numa and Der Riese. Neither happens in WaW.

- In WaW, `_zombiemode::round_spawning()` calls `ai_calculate_health()`, which adds one step to
  `level.zombie_health`. `_zombiemode_dogs::dog_round_tracker` swaps `level.round_spawn_func` to
  `dog_round_spawning`, so **a dog round never calls it**. The source code already says "dog rounds
  don't raise health"; nobody had to know it.
- The port runs BO2's `round_spawning`, which calls BO2's `ai_calculate_health(round_number)`.
  That one rebuilds health from the round number, counting every round.
- The earlier insta-kill fix was a hand-written loop over rounds 2..N (`waw_round_health`). It
  copied WaW's *formula* and lost the *call site*, so the dog-round behaviour vanished.
- The first fix for that tried a dog-round list: a second hand rule patching the first. Wrong
  direction. The real fix extracts the source `ai_calculate_health` and calls it from BO2's
  `round_spawning`, the same place the source calls it. Dog-round skipping, the 32-bit wrap and
  any custom map's own health formula then all come from the source.

The same pass found `set_zombie_var( "zombie_health_increase_percent", 10, 100 )` ported as `10`:
the defaults reader dropped WaW's divisor argument. Reading only part of a source statement is
cherry-picking too.

## How to apply it

Before writing or changing any gameplay behaviour:

1. **Find the source.** Read the map's own scripts and its framework (`_zombiemode*.gsc` in the
   asset cache, `custom_maps/.../maps/`). Note who calls the function, when, and under which
   conditions (flags, swapped function pointers like `round_spawn_func`, `level.*_func` hooks).
2. **Check what BO2 does in its place.** If the owned BO2 framework (`_waw2bo2_zm.gsc`,
   `_waw2bo2_ai_dogs.gsc`, `_waw2bo2_perks.gsc`...) runs its own version, that is a hole in the
   conversion: the source version is not running.
3. **Port, then hook.** Extract the source function with the Translator (`extract_entry`,
   `extract_round_total` is the model: lift the source statements, register a `level.*_func`)
   and make the owned BO2 caller use it at the matching point, with an exact-count text patch
   that raises if BO2's text changes (see `WAW_ROUND_COUNTER`).
4. **Read whole statements.** Every argument of a source call matters (`set_zombie_var`'s
   divisor, `is_float`). If a statement shape cannot be parsed, report it; don't guess.
5. **Keep policies thin.** A user-requested policy (stock maps: BO-style solo QR, the Der Riese
   insta-kill exclusion) wraps the ported source behaviour; it never replaces it with a hand
   formula.
6. **Measure in game.** Probe the real build (round health per round, with a forced dog round)
   and compare it with the source sequence. Tests that only check generated text are not proof.

## Red flags in review

- A compat function that recomputes a game value (health, counts, costs, timers) from first
  principles instead of calling the source's own function.
- A comment that says "WaW does X" without naming the source function and its call site.
- A fix that adds a second hand rule to correct the first.
- A map name or level list deciding behaviour that the source code could decide.
- A `stock_map_policies` entry doing more than the user asked for.

## Known hand-written game rules to revisit

Replace these with ported source over time. Note each one here when it is done.

- `waw_round_health` / `waw_stock_health_overflow` (compat): being replaced by the source
  `ai_calculate_health` hooked into BO2's `round_spawning` (branch `waw-health-qr-dogs`).
- `framework_zombie_vars`: reads only the literal value and drops the divisor (being fixed on
  the same branch).
- BO2's round loop (`round_think`, `round_wait`, intermission) still runs in place of the
  source's. Only the count formula, the round counter and selected pieces are ported. Audit the
  rest function by function against `_zombiemode::round_think`.
