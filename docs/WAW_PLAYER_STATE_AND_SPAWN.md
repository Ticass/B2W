# WaW player state and zombie spawners

These conversions depend on engine behavior, not a particular map name.

## Zombie creation

WaW `maps/_zombiemode_utility::spawn_zombie` chooses `StalingradSpawn`
when `script_forcespawn` is true and `DoSpawn` otherwise. BO2's
`maps/mp/zombies/_zm_utility::spawn_zombie` calls `spawnactor` only in
the true branch. There is no alternate creation branch.

Converted WaW zombie actor spawners therefore require `script_forcespawn=1`.
This preserves the BO2 spawning pipeline, including its actor budget, zone
selection, spawn functions, risers and barrier entry. It does not move the
authored spawner or modify unrelated actor classes.

Before this conversion, Bank Job had an active start zone, 19 spawners,
six queued zombies, zero AI and 32 free actor slots. The queue stayed stuck
because the native helper never created an actor, rather than because the
actor budget was exhausted.

## Player counters and names

BO2 initializes zombie session counters in
`maps/mp/zombies/_zm_stats::player_stats_init` under `player.pers`.
The translator routes known WaW `player.stats["key"]` reads through
`waw_player_stat`, which uses the live BO2 counters:

| WaW key | BO2 source |
| --- | --- |
| kills, downs, revives, headshots | same key in `player.pers` |
| perks | `player.pers["perks_drank"]` |
| zombie_gibs | `player.pers["gibs"]` |
| score | `player.score_total` |

Absent native counters fall back to an existing WaW counter, then zero.
Compatibility initialization supplies all seven stock WaW keys. Custom
keys, counter writes and `isdefined` probes retain their source behavior.
The WaW player name property `playername` becomes BO2's `name` property;
string literals retain their contents. These changes cover ScoreSolo's
repeated undefined-counter and undefined-name errors.

## Script HUD font scale

Native executable inspection found the same base font-scale calculation in
WaW and BO2. WaW's mapped executable HUD setup is at `0x44C480`; BO2
`t6zm.exe` HUD setup is at `0x7A2ED0`. Their font multipliers are:

| Script font | Multiplier | Height in virtual units before rounding |
| --- | --- | --- |
| bigfixed | 0.5 | `24 * fontscale` |
| smallfixed | 1/3 | `16 * fontscale` |
| default, objective, big, small | 0.25 | `12 * fontscale` |

The engines normalize glyph scale by `48 / font.pixelHeight`. Multiplying
by the font height for HUD bounds cancels that denominator. Screen placement
then scales virtual units to screen pixels. BO2 additionally applies its
split-screen and dynamic-render factors; they are one in ordinary solo play.

Matching draw formulas does not imply matching script values. WaW's HUD
netfield `fontScale` uses encoding -86 (six bits): the writer at 0x67A2AE
rounds `(scale - 1) * 10`, sends its low six bits, and the reader at 0x676DFB
decodes `1 + bits * 0.1`. A script value of 8 therefore displays as 1.6 in
WaW. BO2's setter instead clamps values above 4.2; copying 8 produces huge
text. The converter decodes WaW's effective scale for literal assignments
and routes calculated assignments through `waw_fontscale`.

Ordinary representable values such as 1.6 stay unchanged. BO2's objective
font face still differs, and effective WaW scales above BO2's 4.2 cap remain
a fidelity limitation. Animated endpoints which assign fontscale use the
same translation; engine interpolation and glyph widths require playtests.

## Zombie appearance and solo Quick Revive

Source actor class names survive entity translation as staging metadata.
Each gets an owned T6 actor script whose animation/state setup stays native,
while its appearance calls the translated WaW actor's character selection.
The character and xmodelalias scripts join normal recursive dependency
discovery, including body/head model arrays. Missing source actor scripts
fail explicitly instead of selecting Nuketown's hazmat characters.

Animated models use T6's dynamic flags (`0x80000`, measured on stock zombie
bodies and heads). The old static-prop default (`0x200000`) selects T6's
optimized instance-rendering branch at 0x724E1A and is inappropriate for live
actors. Every original mesh and LOD distance is retained. Native roundtrips
verify these flags and distances; a live/dead visual comparison after this
flag correction remains a separate visual validation step.

In source-perk mode `waw_setperk("specialty_quickrevive")` registers one
BO2 life for a single connected player. BO2's existing last-stand controller
performs the revive and consumes the life. Source purchase controllers,
machine availability, limits and HUD remain owned by the map. Multiplayer
retains the native faster-teammate-revive effect. Native BO2 perk mode already
registers lives and bypasses the compatibility registration.

## Verification boundary

Damage-prelude extraction also recognizes edited stock control expressions
as the boundary between leading map rules and the stock framework body.
A leading `self finishPlayerDamageWrapper(..., 0, ...); return;` with the
stock wrapper signature becomes callback damage cancellation. Nonzero or
unrecognized explicit damage application remains an error.

WaW script structs with omitted origin/angles receive their native zero
vectors. This prevents undefined-vector errors on game-over camera endpoints.
Weapon and literal script rumble references carry their source profiles and
both graph files; a missing dependency is a staging error. DLC powerup
callbacks retain their source parameters and register through T6's native
powerup API. The compatibility initializer supplies the Plutonium flag used
by WaW libraries to defer TAB's scoreboard to the native client.

Bank Job's runtime checks showed original WaW zombie bodies, working hands
and guns, normal HUD scale, and solo Quick Revive returning health to 100
while consuming its one life. A subsequent game-over check had no script
errors after the struct-vector correction. Full Bank Job and Empty Walls
builds, native script compilation on five regression maps, unit tests, and native
asset roundtrips are recorded under `work/bankjob_20261008`. These checks do
not establish complete visual/gameplay fidelity for every converted map.

Empty Walls also completed startup, spawned its Russian zombie body, consumed
one solo-revive life, and reached game-over with zero script runtime errors
and no fatal asset drop after the rumble/callback corrections. Temporary
runtime probes were removed and the agent-started game processes were stopped.
