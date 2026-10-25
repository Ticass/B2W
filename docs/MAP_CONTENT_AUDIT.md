# Map content audit (2026-10-25)

Rule: the map is king. The converter brings in everything the map ships; it trims or
substitutes only for a measured engine limit, and reports it.

## Fixed on `audio-remaster` / `map-content-audit`
| Finding | Effect | Fix |
|---|---|---|
| Client music table replaced by BO2 placeholder states | no round start/end, Easter egg, dog round, game over music on any map | `sounds.port_music` ports the map's table |
| Sounds bundled only when a script names them literally | 1,399 Shi No Numa aliases lost (announcer, quotes, music) | every alias the map zones ship |
| WaW power-up script replaced by BO2's | BO2 power-up models shown (skull, nuke, x2, ammo) | `Translator.powerup_models_hook` puts the map's models on BO2's registry |
| WaW power-up announcer replaced by BO2's | silent announcer | `announcer_hook` + bank copies |
| `iswaitingonsound` stubbed | sound entities deleted mid-sound on owner death | compat `waw_iswaitingonsound` |
| `level.zombie_sounds` filled by BO2's `init_sounds` | WaW code played BO2 aliases the bank lacks: no mystery box music | `sound_table_hook` binds the map's `add_sound` table (waw/ aliases) |
| WaW quote triggers live in replaced framework scripts | few player quotes: kills, close calls, kill streaks, power-ups, ammo silent | `quote_hooks`: Plutonium `replacefunc` puts the map's `play_death_vo` / `powerup_vo` in place of BO2's (same call, same arguments); the owned spawner threads the map's `zombie_death_event` and drops BO2's copies of its work (`zombiebehavior.waw_quote_rules`); `track_players_ammo_count` starts with the map; `player_killstreak_timer` is source-driven (`only`) |
| A missing quote alias never sent its `sound_done` | `level.player_is_speaking` held for good, every later quote silent | compat `waw_playsound` ends a missing sound next frame |

## Open
- **Framework script edits not ported** (`scripts:` warning): maps override `_zombiemode_blockers`,
  `_zombiemode_spawner`, `_zombiemode_weapons`, animscripts…; BO2's versions run. Largest remaining
  gap; continue the WaW-drives `source_driven` work per script.
- **Assets absent at the source** (Nuketown: 23 models, 4 materials named by its perk-pack scripts,
  in no zone it ships nor its `missingasset.csv`): reported and guarded; verify the code paths are
  unreachable in WaW.
- **Wavelet FX images** need `wawModTools/bin/AssetViewer.exe`; a worktree without the untracked
  `wawModTools` drops light glows/rays (build from the main checkout or link it).
