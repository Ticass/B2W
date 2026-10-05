# Source contact kills and T6 melee

The WaW baby-gun script starts a proximity monitor after changing the zombie
model and health. Every 0.1 seconds it checks for a player within 70 units,
starts and launches the ragdoll, and kills the zombie with damage credited
to the shooter. Its normal and upgraded monitor durations follow the
source's five- and fifteen-second shrinking timers.

T6's `_zm::player_damage_override` supplies a zombie's `meleedamage` (60 on
the stock spawner), independently of its changed model or low health. The
source has no explicit attack suppression. The reported symptom is a full
zombie hit when walking into a baby; a competing ASD swipe before the
polling kick is a plausible timing difference, not a reproduced trace.

The translator recognizes proximity monitors using `getplayers`, a
distance check from `self.origin` to a player, `IsAlive(self)`, ragdoll
launch, lethal `self.health + ...` damage, and a wait. It does not key on
map, weapon, model, or function names. While that monitor runs, the T6
attacker's `custom_damage_func` returns zero. The source still performs
the kick and owns its radius, timer, audio, effects, kill and attribution.
Every normal return and fallthrough releases suppression. Overlapping
monitors share a counter and restore the previous damage function only
after the last exits; a callback installed by other code is not overwritten
on cleanup. Dead entities retain no active attack behavior.

This is a compatibility adaptation for source contact-kill behavior,
rather than an assertion that WaW and T6 execute melee identically.
Regression coverage checks preserved source actions, cleanup on braced
and unbraced exits, and exclusion of ordinary attacks and exploding enemies.
Generated references and native script compilation are also checked.
In-game kick, recovery, and multiplayer behavior still require testing.

The current map package is rebuilt and installed. Eighteen relevant tests
pass, generated script references report zero problems, both changed native
scripts extract byte-for-byte equal to their compiled inputs, and the map's
66,545 material arguments pass the audit. All nine installed package files
match their build hashes. Evidence: `work/contact_kill_fix/verification.json`.
