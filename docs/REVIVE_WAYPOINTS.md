# Co-op revive waypoints

The compatibility layer now explicitly draws the `waypoint_revive` material
for active teammates of a downed player. This provides a script HUD path when
BO2's native teammate indicator does not appear in a converted WaW map.

The target must be alive and have a revive trigger (WaW's own last-stand
predicate). Do not additionally require BO2's `self.laststand` field: WaW's
co-op `playerlaststand` never writes it. That extra condition suppressed the
indicator on source-driven co-op downs. The viewer
must be alive, in the playing session state, on the target's team, and a
different player. Each viewer/target pair owns one client HUD element, which
tracks the target's position with a 40-unit height offset. The level checks
markers every 0.1 seconds and destroys obsolete elements on revive, bleedout,
disconnect, or a change in viewer eligibility. The bridge also handles late
joiners and multiple downed teammates. Source HUDs for custom fake bodies
remain under their original scripts' control.

## Size

T6 ignores the hud element's width as a pixel size for waypoints: it draws
type-15 (setwaypoint) elements at `waypointIconWidth * width / 8`, where 8 is
BO2's own objpoint shader size. An in-game probe at 1080p measured
`setshader` 8, 16, 32 and 64 as roughly 75, 150, 300 and 600 pixels, with 8
matching a 36x36 2D element. WaW draws its native revive icon at the plain
36-unit `waypointIconWidth`, so the marker uses an 8x8 shader. The former
32x32 shader made it four times too large.

The material is precached during compatibility initialization. HUD dependency
discovery includes compatibility script references, so maps do not have to
name the material in their own scripts to stage it.

Verification: BO2's script compiler successfully compiled the updated
compatibility script; 17 related player-state, framework, appearance, and
perk tests passed. New script calls pass the cached T6 builtin checks.
The initial isolated compatibility link check found five pre-existing
`wait_network_frame` references in the power bridge. A compatibility helper
now resolves them, and the full generated script set passes the T6 reference
check. Evidence is under `work/revive_icon_fix` and
`work/teleporter_deathbarrier_fix`.

The changes were rebuilt and installed after the game closed on 2026-10-04,
together with the teleporter boundary and sound fixes.
Live two-player verification should cover movement, revive completion,
bleedout, disconnect, and repeated downs. Native revive rendering is not
disabled, so builds where that indicator also works need a visual check for
overlapping markers.

2026-10-31: rebuilt Der Riese with the source-down predicate correction.
A host plus test-client run reached a source co-op down with a revive trigger,
no BO2 `laststand` field, and marker eligibility true. Reviving removed the
trigger and eligibility became false. The bot can move into spectator state
after spawning; a follow-up diagnostic confirmed spectators remain correctly
excluded. These checks verify the server HUD path, not the icon's visual
appearance on a remote human client. Twelve related tests passed. Evidence:
`work/der_riese_acc/probe_revive_indicators_20261031/` (the first successful
playing-viewer run is in its archived console logs).
