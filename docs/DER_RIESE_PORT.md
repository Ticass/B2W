# Stock WaW Der Riese conversion

Branch: `codex/der-riese-port`. Input is the stock World at War
`zone/english/nazi_zombie_factory.ff`. Build script: `work/der_riese/build.py`;
evidence and probes: `work/der_riese/probe`.

Every fix below is a general translation rule; nothing keys on the map name.

## Zone graph read from tokens, with flag arrays

Der Riese builds a local flag array before two of its links:

```c
flag_array[0] = "enter_tp_east";
flag_array[1] = "enter_wnuen_building";
add_adjacent_zone( "tp_east_zone", "outside_east_zone", flag_array, true );
```

`zones.read_waw_zones` copied each call's text, so the BO2 map script named a
local that did not exist and script compilation failed. It also read a whole
`/* ... */` block of example calls (including a zone that does not exist).
The reader now works on tokens: comments and `/# #/` developer blocks are not
part of the graph, and a local array argument is resolved from its assignments
in source order (`[ "f0", "f1" ]`).

Both frameworks treat an array as "every flag set" and repeated single-flag
calls as "any flag set" (WaW `_zombiemode_zone_manager`, BO2
`make_zone_adjacent` are identical). BO2's `add_adjacent_zone` indexes
`level.flag` with its flag argument, so array links are emitted through
`zone_init` and `make_zone_adjacent` directly, with each flag initialized.
Measured in game: `tp_east_zone -> outside_east_zone` has 2 flags, AND check.

## Spawn location target

Round-1 zombies idled outside the start room, in zones not yet opened. Probe:
the stuck actors carried `target = outside_up_spawners_goal_0`, the goal of an
unrelated spawner. WaW `zombie_think` measures its three nearest windows from
`get_desired_origin()` (the actor's spawner target), so they picked windows
across the map, and T6 raised `bad_path` on every goal (16-18 per actor) while
the WaW fallback cycled through farther windows.

Cause: T6 `do_zombie_spawn` copies `spot.target` only when defined. A converted
spawn location carries its WaW spawner's target, or none, but the actor can
come from another spawner and keep that spawner's target. The owned spawner
copy now assigns the location's target unconditionally. Measured: 3 of 5
spawned zombies stuck before, 0 of 5 after; all walked to their own windows.

Ruled out on the way: path nodes (T6 drops them to the floor, `findpath` from
the spawn to the window succeeds) and spawner height (Shi No Numa's spawners
float higher and path fine).

## Client listeners on the server

WaW client scripts react to server `clientnotify` messages. The port does not
run the source client scripts; listeners whose calls all have a server form
run as server threads (`gscport.client_fx_listeners`). That set was a short
hard-coded list of effect calls. It is now `gsc_api.json`
`client_listener_calls`: per client builtin, the local-client argument to drop
and the server or compat function to call. Der Riese gains:

- power-on lamp swaps (`light_model_swap`: indlight, milit lamp, tinhat lamp);
- teleporter map lights, including the receiver's flash and stop;
- the pad-wire sparks (`perk_wire_fx`: a mover with an effect and a looping
  fake sound entity, which notifies the teleporter light when it arrives);
- the fly-trap easter egg's floating objects.

Rules that make these work generally:

- One client `level` namespace: the copies' `level waittill/notify/endon`
  all use the `"waw_client_"` prefix that compat `waw_clientnotify` raises,
  so a message one listener sends another (`t01`) still arrives.
- `level._effect` is one client namespace across all client scripts.
- CSC fake sound entities become server `script_origin`s
  (`waw_client_spawnfakeent` etc.); `playsound( client, alias, origin )`
  becomes `waw_playsoundatposition`.
- A helper shared by several listeners is emitted once.
- A listener making a function-pointer call (`[[ ]]`) stays client-side and
  is reported: the table cannot vouch for an unknown target (Der Riese's
  teleport aftereffects fill that table in an unported listener).
- `setvolfog` listeners belong to `client_fog_monitors` (dog-round fog was
  already ported); they are no longer also reported as unsupported.

The lamps, cage lights and fly-trap objects are client-side script_models
(spawnflag 2), which T6, like WaW, never creates on the server: measured, the
server found 1 of Der Riese's 11 lamps (the one authored with flag 0). The
port records the string literals of the listeners it runs on the server, and
client-side script_models with one of those targetnames become server
entities (`entities.network_client_models`, reported as
`CLIENT_MODELS_NETWORKED`). Only the listeners' names count: WaW server scripts
never saw these models, so a server script naming one keeps that behavior.
They get no collision, as in WaW. Der Riese networks 37 models.

Verified in game: after power, 11/11 lamps and 5/5 military lamps show their
"on" models; the teleporter map lights turn red, then green when the pad-wire
spark arrives; 8/8 fly-trap objects lift and circle. No script errors.

Models these listeners set are found by the existing scan of the emitted core
script and precached at startup; their sound aliases by the script literal
scan. Shi No Numa's listener output is unchanged apart from comments.

Still client-only and reported: the teleport aftereffects (per-player vision
and FOV flashes, `settransported`) and the black-box vision swap.

## Knowledge ledger

Each build writes one observation per map to `<build folder>/knowledge`:
untranslated script features by kind, stage error kinds, reported fallbacks
and the failed step. `waw2bo2n knowledge-report` ranks the gaps by how many
maps share them, also from failure bundles and stage reports. It is evidence
for choosing the next generic fix; no conversion decision reads it.
