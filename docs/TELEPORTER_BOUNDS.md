# WaW teleports and BO2 playable-area deaths

WaW's spawn-zone volumes are not lethal player boundaries. BO2's
`_zm::player_out_of_playable_area_monitor` treats being outside enabled
player volumes as a reason to kill a player. Its kill sequence waits 0.5
seconds after making that decision, disables invulnerability, and applies
lethal damage without checking the player's new position.

This conflicts with scripted WaW teleports that place players above a
destination and let them fall. The current map's hell portals use the original
eight spawn structs plus a 300-unit vertical offset. Source-collision checks
show all eight destinations inside playable volumes at landing height but
outside every such volume at arrival height. This also explains why the
source script's temporary invulnerability cannot reliably prevent the death.

The compatibility layer installs the native monitor's callback to allow
deaths only when the player touches an explicit kill brush. If a prior
callback exists, it remains responsible for deciding whether that explicit
kill is allowed. Source `trigger_hurt` damage, scripted kills, teleport
coordinates, invulnerability timing, and collision geometry are preserved.
This rule applies to every converted WaW map, without portal-name checks.

Verification: eight source destinations audited, native BO2 script compilation
successful, and eight related player-state/framework tests passing. Evidence
is under `work/teleporter_deathbarrier_fix`. The complete generated script set
also passes its T6 reference check with zero unresolved calls. The map and
gameplay zones were rebuilt and installed after the game closed on 2026-10-04,
including the pending sound and revive-waypoint changes. Material audits
checked 133,635 arguments with zero violations. Verify both portal
destinations and repeated use with multiple players; live gameplay has not
been verified.
