# Perk ownership

Converted maps use the source WaW perk framework, including the map's override
of `maps/_zombiemode_perks.gsc` and any perk extensions initialized by its
`maps/_zombiemode::main`. When the map does not override the standard script,
the converter ports the stock WaW version. The `preserved_scripts` table in
`compat/gsc_api.json` takes precedence over framework-name patterns. References
to WaW perk functions resolve to the translated WaW script, including purchases,
bottle handling, HUD creation, and perk loss.

`stage_bo2_perk_support` stages a project-local version of BO2's `_zm_perks`
that retains client-field registration and framework support exports. Its
`init` does not start BO2 machine spawning, vending triggers, power listeners,
Pack-a-Punch controllers, or perk host migration. The BO2 pause/unpause entry
points cannot change the availability of WaW machines. Other BO2 framework
exports remain available to engine support code; source WaW scripts do not
delegate their perk gameplay to them.

The client `_zm_perks.csc` override retains the original client-field
registrations, including names, versions, bit widths, order and FX callbacks.
It disables `perk_init_code_callbacks` and native custom-perk threads.
`setupclientfieldcodecallbacks` binds LUI events for native perk icons; these
bindings made a source purchase display both the WaW script HUD and BO2 HUD,
even after disabling BO2 machine controllers. Native `setperk` still applies
the source perk's engine effects, while WaW alone creates/removes perk icons.
Do not delete client fields or remove native perk effects to hide the BO2 HUD.

Do not rely only on a same-name CSC override: stock dependencies may already
be linked from a base zone before the map's assets arrive. The converter also
stages unique `_waw2bo2_perks.csc` and `_waw2bo2_zm.csc` client scripts. The
latter preserves the stock client bootstrap except for routing its perk
initializer to the unique compatibility script. `hook_bo2_perk_client`
rewrites the map's explicit `_zm::init` call to this bootstrap. Thus the
reachable startup chain calls the compatibility initializer directly, rather
than depending on stock script replacement. Both unique scripts are compiled
and included through `map_scripts` for every converted map.

The compatibility power bridge emits the standard WaW `sleight_on`,
`revive_on`, `doubletap_on`, `juggernog_on` and `Pack_A_Punch_on` events after
`electricity_on`, using WaW's network-frame ordering. BO2's `power_on` alone
does not wake the source model-swap threads. These events are emitted even
when BO2's power flag was already set; WaW keeps the model/FX behavior.

Source `setClientDvar("player_sprintTime", value)` uses T6's per-player
`setsprintduration` for player receivers. This preserves source durations
such as 15 seconds, which exceed the global dvar's 12.8-second maximum,
without changing every player's sprint or printing repeated dvar errors.

Source player `being_revived` reads are translated to `waw_being_revived`.
T6 exposes this state through `player.revivetrigger.beingrevived`; source
buildable collection/construction checks otherwise read an undefined field.
The helper also honors a true source-owned field for custom revive handlers.
Source assignments and explicit `isdefined` probes remain intact. This rule
applies to all ported source scripts, including equipment and perk interactions,
without identifying a specific buildable or map. Regression tests cover array
receivers, nested paths, parenthesized checks, source writes and idempotence in
`tests/test_player_state_compat.py`.

Verified against T6 `t6zm.exe`: client registration is `sub_7CD3D0`;
`sub_7CD8A0` (`setupclientfieldcodecallbacks`) attaches `sub_6FD5D0` for integer
fields. That callback sends the field-name LUI event with `oldValue` and
`newValue`, matching the stock `hudperkszombie.lua` event handlers.

The generated support scripts are included in script compilation and the map
zone by `bo2_scripts`/`map_scripts`. The installed BO2 raw files are never
modified. The rule applies to every converted map and does not inspect map
names or machine coordinates.

Regression checks are in `tests/test_perk_ownership.py`: stock WaW preservation,
initialization with custom extensions, function routing, suppression of BO2
controllers, unchanged client-field setup/FX, absence of LUI callback binding,
idempotent staging, and inclusion of both overrides in the compiled script list.
Runtime checks should cover power, one purchase per use, source perk icons,
perk loss on downing/death, and Pack-a-Punch.
