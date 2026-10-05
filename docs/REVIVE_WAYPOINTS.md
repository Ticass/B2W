# Co-op revive waypoints

The compatibility layer now explicitly draws the `waypoint_revive` material
for active teammates of a downed player. This provides a script HUD path when
BO2's native teammate indicator does not appear in a converted WaW map.

The target must be alive, in last stand, and have a revive trigger. The viewer
must be alive, in the playing session state, on the target's team, and a
different player. Each viewer/target pair owns one client HUD element, which
tracks the target's position with a 40-unit height offset. The level checks
markers every 0.1 seconds and destroys obsolete elements on revive, bleedout,
disconnect, or a change in viewer eligibility. The bridge also handles late
joiners and multiple downed teammates. Source HUDs for custom fake bodies
remain under their original scripts' control.

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
