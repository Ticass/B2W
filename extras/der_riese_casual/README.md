# Der Riese casual fixes

Optional fixes for the **WaW Der Riese port running in Plutonium T6**.

Install `der_riese_waw_casual_fixes.gsc` into
`%LOCALAPPDATA%\Plutonium\storage\t6\scripts\zm\`. Every Der Riese release
since 2026-10-31 already ships the map side it needs: keep the release's own
`zm_der_riese_waw.ff` and never copy an older one over it (the map zone must
match that release's mod.ff and ipaks). Replace any older copy of the optional
script. Restart the map after installing.

- Solo QR enters engine last stand through `FinishPlayerDamage`. A replaced
  T6 callback routes that down to native `_zm_laststand::playerlaststand`.
  BO2 supplies its downed vision, pistol/inventory handling, revive HUD,
  ten-second solo revive, and `RevivePlayer` cleanup. Other downs retain the
  map's original callback.
- The third successful solo QR grant disables its purchase trigger and hides
  the machine immediately. The third purchased revive remains usable. The
  source perk-loss thread consumes QR and removes the perk HUD on each down.
  Co-op purchases do not count toward the solo limit.
- The replaced health setter preserves max health. This implements the engine
  semantics of T4 `g_fix_health_sets_max`, including preserving the actual
  Wunderwaffe self-damage and electrocution effects. It removes the old polling
  and highest-health restoration logic. Explicit max-health changes still run.

The runtime map check runs before installing hooks. The optional file has no
static references to `maps\mp\waw`, so putting it in the global scripts folder
does not introduce unresolved converter functions on other maps. It waits up
to ten seconds for the map API and disables itself with a console message on
older releases. **The new GSC alone cannot enable these fixes on an old map FF.**

The map update publishes a health-setter function pointer and a native perk
grant observer. These are generic, inactive interfaces; they change no gameplay
when this optional script is absent. Delete the optional GSC and restart the
map to restore the map's normal WaW behavior.

Plutonium documents the T4 dvar rename in its
[changelog](https://plutonium.pw/docs/changelog/). T4's dvar cannot repair the
T6 port's deliberately emulated WaW health setter by itself.
