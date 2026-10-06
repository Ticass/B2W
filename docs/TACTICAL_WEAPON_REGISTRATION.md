# Tactical weapon startup registration

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
