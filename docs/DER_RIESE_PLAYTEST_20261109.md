# Der Riese playtest — 2026-11-09

Branch `fix/der-riese-playtest-20261109` (from `fix/zombie-stall-native-pain`, the installed build's branch).
The uncommitted portal-visibility work found in the checkout (Nov 6, never installed) was set aside in
`git stash` ("portal visibility WIP ...") for this build and restored afterwards (still uncommitted on this branch).

## Fixed (converter rules)

| Report | Cause | Fix |
|---|---|---|
| Shooting zombies resets their walk | allowpain 1 (the stall) sends killanimscript; T6's zm_move restarts and `moverun` calls `setanimstatefromasd` without a substate, so every hit picked a random walk clip | actor glue sets `preserve_asd_substates = 1`; T6 then keeps the substate when the state is unchanged (T4 kept the run anim through pain) |
| Rebuilding a window shows no points | windows are BO2 zbarriers; their reward calls BO2 `_zm_score::player_add_points` → BO2 `add_to_player_score`, which has no popup | `native_replacements` replacefuncs BO2 `add_to_player_score` / `minus_to_player_score` with the map's own (score_highlight popup). `native_replacements` now handles scripts ported whole (`preserved_scripts`) |
| Scoreboard kills stay 0 | T4's engine credited actor kills itself; T6 leaves it to BO2's spawner, which the map's replaces | `waw2bo2_actor_kill_credit` (zombies and dogs): `kills++`, `headshots++` on head/helmet non-melee deaths |
| Prone at perks gives no 25 points | `bind_entity_sounds` renamed the bump triggers' `script_sound` to `waw/perks_rattle`; `check_for_change` compares `"perks_rattle"` | `sounds.bind_script_sound_comparisons`: a literal compared with `script_sound` takes the bound name when every entity holding it was bound |
| Fall damage plays the wrong grunt | T6 plays `fly_land_damage_plr`, unmatched (silent) | engine family: `fly_land_damage_plr` → WaW `land_damage_plr` (T4 sub_661D70), npc → `land_damage` |
| Wunderwaffe missing FX (partly) | model isolation renamed the client `PlayViewmodelFx( lc, fx, "tag_flash" )` tag to `waw_world_model/tag_flash` | `playviewmodelfx` arg 2 is a bone (modelisolation TAG_ARGUMENTS) |
| Low ammo (red) / reload text | the WaW LUI HUD never drew T4's `CG_PLAYER_WEAPON_LOW_AMMO_WARNING` | `waw_hud.lua`: sub_42CDF0 states (Reload / LOW AMMO / No Ammo), dvar colours and pulse from sub_42B2E0 (freq 1.7, min 0, max 1.5); reserve red (1, .3, .3) at ≤ maxAmmo × 0.2 (sub_44ED70) |
| MG42 counter wrong | belt geometry guessed | sub_42BC40: 8×4 rounds, step 8, rows 2 apart, start (count/20)·h/4; rocket 64×16 step 72 (sub_42BB00) |
| 0 grenades: icon gone | T6 drops an empty offhand from `hud_update_offhand` | icon stays with a 0 count once a grenade was held |
| No bouncing betty HUD | the WaW HUD had no action slots | hud.menu PC DPad: slots 1/3/4 from `hud_update_actionslots` (icon, ammo, `[key]`) |
| Garand GL shows launcher ammo | m1garand_gl and m7_launcher share a display name; the HUD keys weapons by it | alt-mode rows leave the display-name column empty; reached through the rifle's altWeapon when T6 reports alt mode |

## Open (not fixed, needs in-game look)

- **Molotov fire**: weapons (`molotov`, `napalmblob`, `napalmbloblight`) and FX are in mod.ff; T6 keeps
  `WEAPPROJEXP_FIRE` / `NAPALMBLOB`. Whether T6's engine spawns the blobs needs t6zm IDA (both i64s failed to
  open this session).
- **Ray Gun ADS light**: the weapon file has no ADS effect; likely the sight's material/technique. Needs a look.
- **Audio balance / PA inaudible**: the PA aliases match WaW (100–1250, start delay 1200, bus_voice); the mix
  around them is the issue. No change made.
- **Zombies leaving the one-window room (hanging man area)**: not reproduced; console shows one STUCK zombie at
  (-780, 137, 6).
- **PaP missing animations**: script paths (worldgun moves, wait_flag, knuckle crack via
  `waw_zombie_knuckle_crack`) look intact; needs which animation is missing.
- **Zombies glitch on objects / push the player**: WaW actors push players too; left as WaW behaviour.

## Verified (installed build, 2026-11-09 probe `work/der_riese_acc/probe_playtest_20261109`)

BO2 `_zm_score::add_to_player_score( 10 )` 500 → 510 through the map's function (old_score set: popup
path); 8 nonfatal hits on a walker kept `zm_move_walk:2` every time; a kill raised `kills` 0 → 1; no script
runtime errors, no LUI errors. The HUD changes (warning, D-pad, counters) were not seen on screen.
Backup of the previous install: `work/der_riese_acc/build/installed_backup_20261109_pretest`.

## Second playtest (evening, build with the stock class mix)

Fixed (converter rules):

| Report | Cause | Fix |
|---|---|---|
| Nothing heard unless next to it; board tearing not heard | the class mix moved reach by rank onto BO2's tables, whose classes are mostly short per-surface rows: scripted sounds lost two thirds of their reach (hdrfx 1500 -> 450, impacts 1200 -> 275, weapons 6000 -> 900; board break 1200 against stock BO2's 5000+) | classes keep WaW's authored distances and falloff (`audioremaster._mix_class` `reach`); only ambient emitters move by rank. `sounds.sound_table_twins`: the map's alias for a `level.zombie_sounds` ref takes the placement of BO2's alias for the same ref (`break_barrier_piece` -> `zmb_break_boards`) |
| Shooting cuts quotes (Ray Gun) | an alias starts its `Secondary` chain; the Ray Gun's flux layers (two per shot, two per impact) were left streamed, and T6 has 10 streamed voices | `fit_loaded_budget` keeps the whole Secondary chain of weapon and played aliases loaded |
| Melee sound wrong | a T6 player melees with BO2's `knife_zm` / `bowie_knife_zm`, whose sound fields name BO2 aliases with no WaW sound bound (only `wpn_melee_hit` -> the rifle butt) | engine families: `wpn_melee_knife_hit_body/other`, `wpn_melee_hit_other`, `wpn_melee_whoosh_*`, `zmb_bowie_*` -> WaW's knife and bowie aliases (`FRAMEWORK_WEAPON_SOUNDS`) |
| LOW AMMO white, reserve not red | `setRGB` on a label colours nothing: a label is one image per glyph | `label:setColor` colours the glyph ink |
| Grenade count not red at 0 | not drawn | T4 sub_46EBE0: (0.89, 0.18, 0.01) at 0 |
| Reload text stays while reloading | T4 sub_42CDF0 skips weaponstate 7..11; LUI has no weapon state | compat `waw_hud_reload_state` sends `waw_hud_reloading` (luinotifyevent) |

Measured, not the cause: delivered level (file RMS + alias dB) of each engine-named sound against the stock BO2
sound of the same name is within a few dB (median +6 dB, `work/der_riese_acc/loud/twins.py`); the two games'
files are equally loud per class. T4's clip bullets take the menu's forecolor only (sub_42B710): no red.

Open: EE jar look (`zombie_beaker_brain`: `mc_unlit_add` liquid, needs a screenshot), Ray Gun dark in ADS,
fall damage sound (`fly_land_damage_plr` plays WaW `land_damage_plr`; which sound is wrong is not known),
BO2 board rebuild animation (zbarrier exception), zombies leaving near the hanging man (clip shows the route,
no zombie).

## Client-script sounds (probe 2026-11-10, `work/der_riese_acc/probe_clientsound_20261110`)

The map's own `nazi_zombie_factory_amb.csc` runs (PA lines, clock ticks, pad loops played). Fixed-name
server messages never reached it: the client glue registered a system per name (`pl1`, `t2d`, `pap1`, ...)
and skipped those names on the `levelNotify` system, but the probe shows messages arrive only through
`levelNotify`. Power-on therefore played no light, generator, breaker or PA sounds and `t2d` no teleport
sound. Fix: `clientport` glue delivers every name from `levelNotify` (`waw_deliver`, one notify per message).
After it, power-on played light_start/light x16, switch_progress x23, gen_start/gen_loop x4, break x2,
pa_power_on x16, power_up_2d, and t2d the teleport 2D pair. Debug: dvar `waw_client_sound_debug 1`.

## Lights and fires gone (2026-11-10, regression in the posted build)

The map's client main now runs the map's _amb script and finishes after the local client has connected,
so BO2's connect path ran before the glue installed `on_connect`: the map's `_fx::fx_init` (its lights,
then its 249 createfx effects) never started. The Nov 9 16:19 build had the flames; the 21:39 one and later
did not (bisect, `work/der_riese_acc/probe_fx_20261110`). Fix: the glue's init starts `on_connect` for
local clients that already connected (once per client). After it: 158 createfx effects started at connect,
the barrel fire burns. Debug log lines (dvar `waw_client_sound_debug 1`): WAW CLIENT GLUE / CONNECT / FX.
