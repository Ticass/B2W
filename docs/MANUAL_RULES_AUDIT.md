# Audit: what the converter does by hand instead of converting the map

User request, 2026-10-25: find everything that is "manually done", make it come from the
map, and explain how it got into the tool. Rule: docs/CONVERT_THE_WHOLE_MAP.md.
Measured with `tools/framework_coverage.py` on real builds (numbers below).

## How the manual rules got in

One architectural decision made early (Sessions 1-10, Nuketown): **WaW's zombie framework
is replaced by BO2's.** `gsc_api.json` `core_scripts` maps each WaW framework script to a BO2
one, and `no_extract` keeps `maps\_zombiemode` itself from being ported:

| WaW framework script | runs instead in the port |
|---|---|
| `maps\_zombiemode` (round loop, spawning, health, intermission, game over) | `maps\mp\zombies\_zm` (owned copy `_waw2bo2_zm`) |
| `maps\_zombiemode_spawner` (zombie init, speed, pathing, damage, gibs) | `_zm_spawner` |
| `maps\_zombiemode_powerups` (drop rate, types, effects) | `_zm_powerups` |
| `maps\_zombiemode_score` (points) | `_zm_score` |
| `maps\_zombiemode_blockers(_new)` (doors, debris, windows) | `_zm_blockers` |
| `maps\_zombiemode_dogs` (dog round schedule, counts) | owned copy of `_zm_ai_dogs` |
| `maps\_laststand` (downs, revive) | `_zm_laststand` |
| `maps\_zombiemode_utility`, `_weapons`, `_zone_manager`, `_net`... | the BO2 script of the same role |

It made the first maps load quickly: BO2's engine expects `_zm`'s callbacks, HUD, last stand
and gametype hooks. But it means the **map's own framework code never runs**. A source
call to a WaW framework function is redirected to BO2's function of the same name
(different body), and the WaW systems that nothing calls directly (the round loop) are simply
BO2's. Every time a playtest showed a difference (round counts, health, dog-round health,
spawn delay, Juggernog regen, damage scaling, failsafe requeue...), a fix patched BO2's
copy toward what WaW does, written from reading WaW, so each one is a hand rule. The
"converter, not map fixes" rule was kept (the patches are generic), but the content of each
patch was still recreated by hand instead of converted.

## Measured coverage

`python tools/framework_coverage.py <stage>/zone_raw/<project> <map script dump>`
(reads the build's own report). Shi No Numa, build of 2026-10-18:

| | functions |
|---|---|
| ported from the source (runs) | 86 |
| redirected to BO2's same-named function | 31 |
| never run: BO2's framework does that job | 336 |

Per script: `_zombiemode` 79 not run / 10 ported / 1 BO2; `_zombiemode_spawner` 66 / 5 / 6;
`_zombiemode_utility` 49 / 1 / 14; `_zombiemode_powerups` 28 / 0 / 2; `_zombiemode_blockers`
18 / 0 / 0; `_zombiemode_score` 8 / 0 / 3; `_zombiemode_weapons` 16 / 20 / 5;
`_zombiemode_perks` 0 / 31 / 0 (fully ported); `_zombiemode_tesla` 3 / 19 / 0.
Full tables: work/framework_coverage_sumpf.md (regenerate per build).

## Hand rules found (and status)

Gameplay values or behaviour written by hand where the source has the code:

| rule | where | status |
|---|---|---|
| zombie health per round (closed formula, counted dog rounds) | compat `waw_round_health` | **converted** 2026-10-25: source `ai_calculate_health` at its call site |
| `set_zombie_var` divisor dropped (health % 10 instead of 0.1) | `framework_zombie_vars` | **fixed** 2026-10-25 |
| traversal scripts (zombie-only; dog branch dropped) | `paths.py` | dog branch **converted** 2026-10-25; zombie branch still mapped by height onto BO2 substates |
| dog round schedule, counts, spawn pacing | owned `_zm_ai_dogs` (`waw_dog_rounds`) | open: source `_zombiemode_dogs` `dog_round_tracker`/`dog_round_spawning` |
| round loop: wait, intermission, spawn delay, max AI, game over | owned `_zm` + `WAW_*` text patches (`WAW_FAILSAFE_NO_REQUEUE`, round counter, start location) | open: source `round_think`/`round_wait`/`round_spawning` |
| zombie spawn init, speed, pathing, damage callbacks, gibs | `_zm_spawner` + zombiebehavior.py source policies | partly from source (speed selector, chase policy); rest open |
| player damage scale, explosive self-damage cap removal | `WAW_PLAYER_DAMAGE_SCALE` (`_gameskill` value), `WAW_SELF_DAMAGE_CAP` | value read from source; the callback itself is BO2's: open |
| Juggernog regen loop and health | juggernog hooks | ported from source `check_for_jugg_perk` (keep); verify call site |
| power-up drops (rate, cycle, playable area) | `_zm_powerups` + compat `waw_powerup_drop` | open: source `_zombiemode_powerups` |
| points (kills, headshots, repairs, doors) | `_zm_score` | open: source `_zombiemode_score` |
| doors / debris / windows | `_zm_blockers`, barriers.py | open |
| last stand / revive | `_zm_laststand` | open (engine-facing UI may stay BO2) |
| solo Quick Revive 500 / 3 buys | stock_map_policies | **user-requested** BO-style feature, not WaW; stays opt-in policy |
| BO2 stock perks mode | stockperks.py | **user-requested** opt-in, not a conversion |

Legitimate runtime patches (engine differences, keep in compat): builtin wrappers
(`waw_playsound`, `waw_setvisibletoplayer`, weapon queries, HUD font scales), T6 limits
(`waw_startragdoll`, `waw_linkto` contacts, `waw_window_paths` clip cut, water bullets,
`waw_precachemodel`), framework signals T6 never sends (`all_players_connected`,
`first_player_ready`, `weapon_pvp_attack`).

## Plan: run the map's framework, keep BO2 only as engine glue

1. Invert `core_scripts` one system at a time: port the WaW script (Translator), and keep
   BO2's only where the T6 engine needs its hook (gametype callbacks, LUI HUD, last-stand
   UI). Each step lists the BO2 functions it stops calling.
2. Order by gameplay weight: round loop (`round_think`, `round_wait`, `round_spawning`,
   intermission) -> dogs (`_zombiemode_dogs`) -> spawner (zombie init/damage) -> power-ups ->
   score -> blockers -> last stand.
3. After each step, re-run `tools/framework_coverage.py`; "not run" and "bo2" must shrink
   and every remaining entry needs a reason (engine glue) in this file.
4. Retire the `WAW_*` text patches and compat game-rule functions each step makes
   unnecessary.
