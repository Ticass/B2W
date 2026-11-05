# Source Wunderwaffe runtime

The source Tesla gameplay library remains responsible for selecting targets,
damage, scoring, and death effects. The compatibility callback reserves the
shooter before starting its thread and runs the source entry synchronously within
that shooter-owned thread. This prevents competing projectile damage callbacks
from admitting multiple chains and avoids giving the initially hit actor ownership
of the traversal. The requested chain limit is ten targets.

Indefinitely looping source effects attached to script entities play through
native PlayFXOnTag one frame later, so a carrier spawned in the same frame has
its tag. They follow the source tag and end with the source parent, and cost no
entity, as in WaW.

The converter stages the source Tesla client library, translating its effects,
weapon names, and real-time waits, and bridges its single-argument level events.
This restores the source viewmodel rail and ammunition-dependent bulb effects.
First-person models use material variants without CASTS_SHADOW; world models
and their materials retain their original shadow behavior.

Validation on Shi No Numa: the build linked with no missing assets; 560 unit
tests passed; all 99 compiled scripts round-tripped through the final fastfile.
A controlled runtime test with twelve nearby source-spawned zombies per shot
produced ten kills on each of eight consecutive shots, with both chain state
fields cleared afterward. A separate looping carrier check followed a moved
parent with zero positional error and disappeared after parent deletion.
These checks do not establish visual fidelity or cover every target arrangement;
normal gameplay retesting remains necessary.

The first gameplay retest exposed a missed source API mismatch: WaW uses
`set_zombie_var(name, default, divisor, is_float)`, while BO2 uses
`set_zombie_var(name, default, is_float, column, is_team_based)`. Routing WaW's
float flag into BO2's column argument parsed the travelling arc's table value
as an integer, producing zero. MoveTo then raised `total time must be positive`
inside the source arc routine before carrier deletion and target damage.
The shared WaW helper now keeps the source signature and source table overrides,
or the authored default when no source table is supplied. BO2's table is not an
override for WaW gameplay. The earlier nearby-target test bypassed travelling
arcs because the source deliberately omits them below 128 units.

After the setter correction, a fresh runtime reported travel time 0.5. A direct
source arc between zombies 160 units apart returned normally with no residual
script-model carrier. Eight shots with two groups spaced 160 units apart killed
5, 10, 10, 10, 10, 10, 10, and 10 targets; the source range/visibility filters
remain active. No script errors occurred, and chain/choke fields reset after
every shot. The rebuilt fastfiles linked with no missing assets and passed the
material audits; 562 tests passed. This is controlled runtime evidence, with
normal gameplay and visual confirmation still outstanding.

Weapon reload sounds are authored as animation notifications and a weapon
notetrackSoundMap. Active BO2 viewmodel animations use native `sndnt#alias`
events rather than WaW's plain sound-map keys. Raw v17 conversion now adds the
namespaced sound events at the original notification frames, preserving each
original event for source client scripts (including Tesla bulb switches).
Motion data and frame counts remain byte-identical. Animations with different
weapon sound mappings receive separate content-addressed variants, so a shared
animation cannot acquire another weapon's audio. Unmapped animations are kept
unchanged; already adapted events are not duplicated.

The linked sound build was audited by reading its weapon animation reference
and final XAnim notifications. All seven native reload audio events match their
original event times and source frames exactly. The build adapted 55 source
weapon animations, passed material audits, and all 565 tests passed. Audible
playback still requires the gameplay retest.
