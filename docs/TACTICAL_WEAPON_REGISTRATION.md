# Tactical weapon startup registration

## Current source-driven monkey (October 30)

The map's WaW `_zombiemode_cymbal_monkey` now owns the monkey inventory,
throw handler, animation, attraction, explosion and cleanup. The native
server monkey initializer is not run beside it. The earlier BO2 inventory
alias approach below is historical.

An October 30 playtest investigation reproduced a shared solo/co-op
attraction failure in solo: the real `grenade_fire` event carried
`zombie_cymbal_monkey`, the source POI registered and generated 72 valid
positions, but only one zombie could claim a location. Other actors were
within the source radius and passed `can_attract`.

The source duplicate check compares `[position, owner]` array records with
ordinary equality. On T6, two records at different positions compared equal:
the generic duplicate check returned 0 while native `_zm_utility`'s
`poi_locations_equal` comparator returned 1 (available). After one claim,
every candidate therefore appeared occupied.

`gscport.bridge_poi_record_comparisons` adapts membership, invalidation and
removal for the typed `attractor_positions` and `claimed_attractor_positions`
arrays to compare their position element, matching T6's explicit native
record comparator. Ordinary entity-array membership remains unchanged.
The source chooses positions and retains its ranges, capacity, timing and
AI flow; the converter adds no monkey-specific gameplay rules.

Regression coverage is in `tests/test_gscport_poi_records.py`. Local runtime
evidence is under `work/der_riese_acc/probe_monkey_20261030` (ignored build
artifacts). The pre-fix equality probe is `equality_observations.log`.

After rebuilding and installing, a fresh solo throw attracted all three
nearby test actors through their normal AI loops (no direct allocator calls
in the verification probe). Three records remained claimed until detonation;
the POI disappeared at tick 18 and all three target overrides cleared by
tick 20, restoring player pursuit. The throw used the real source weapon
event without a synthetic notify. `observations.log` records this run.
Compilation and map linking completed with zero missing assets; the full
suite ran 844 tests successfully, with one skipped. A second co-op client
was not used for runtime verification.

## Historical native-helper startup (October 5)

The source mystery box registers `zombie_cymbal_monkey`, but the existing
BO2 monkey gameplay helper gives and checks `cymbal_monkey_zm`. Its server
initializer returns immediately unless the latter exists in
`level.zombie_weapons`. Previously that guard skipped model, effect, array,
and `ScriptModelsUseAnimTree` setup; throwing the monkey later crashed at
`UseAnimTree` with an unrecognized `zombie_cymbal_monkey` tree.

The generated asset table declares the helper's inventory alias only when
the source monkey weapon is supported. The compatibility include/add
functions register that alias before the native initializer runs. The alias
is excluded from the mystery box; the source definition and source box entry
remain. Reverse inventory lookup maps the carried alias back to the source
name. The existing `legacy_cymbal_monkey = 1` setting continues to select
the WaW `weapon_zombie_monkey_bomb` world model.

This repairs the initialization guard rather than registering the tree alone,
which would leave the other monkey state uninitialized. The existing client
template already includes `cymbal_monkey_zm` before its monkey initializer.

Evidence for the October 5 build is in `work/monkey_animtree_20261005`.
