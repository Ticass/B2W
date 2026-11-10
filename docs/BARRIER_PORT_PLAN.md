# Window barriers: BO2 zbarriers (approved exception)

Decision 2026-10-27 (user): windows stay BO2 `zbarrier`s. This is the only BO2 game system
the user allows in place of the map's code. Reason: T6 leaves 912 map entities, and WaW's
board chunks (about 7 per window) exhaust that budget (Project X: "G_Spawn: no free entities").

What stays BO2's, by this decision:
- `entities.py` emits `zbarrier_zmcore_BasicWoodBarrier` per WaW window (`exterior_goal`);
  `barriers.align_native_boards` places it on the WaW planks; `barriers.drop_replaced_chunks`
  removes the WaW board brushes.
- BO2 `_zm_blockers` window repair, pieces and poses (torn boards lie under the window, BO2's
  "open" pose), and BO2's zombie board tearing and barricade climb.
- gsc_api glue: `_zombiemode_spawner` `is_spawner_targeted_by_blocker`; `_zombiemode_utility`
  `all_chunks_intact`, `all_chunks_destroyed`, `get_random_destroyed_chunk`; the carpenter's
  `native_argument_adapters`.

The board rebuild motion is the map's (2026-11-09, user: "port WaW rebuild anim"). WaW has no
rebuild animation: the map's `replace_chunk` moves the board in script (shows it where it landed,
lifts it to its height while turning it upright, waits, pushes it in). `barriers.replace_chunk_timing`
reads those literals from the map's own function (Der Riese: lift 0.15 s, turn 0.3 s, hold 0.2 s,
push 0.1 s) and `barriers.stage_waw_repair_animations` writes that motion over each native board's
repair animation (`o_zombie_board_N_repair`, same name), from BO2's start pose (where the pull left
the board, outside on the floor) to its rest pose. `modzone.link_mod` links them from the mod's first
asset layer (`mod_override`), so the stock barrier asset plays them; no entity is added. BO2 still
scales the animation for Speed Cola (0.31) where WaW's perk branch skips the lift.

Everything else about windows that the map's code decides (which window a zombie goes to,
zones activating windows, the carpenter power-up itself) still runs from the map's scripts.
Do not extend this exception to doors, debris or any other system.

The full WaW port was surveyed and planned before the decision (git history of this file):
WaW board chunks, the map's `_zombiemode_blockers_new` window functions, the map's
`tear_into_building`, and converted board-tear/window-attack/taunt animations.
