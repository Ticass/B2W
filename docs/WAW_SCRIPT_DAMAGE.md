# WaW script damage and HUD geometry

Der Riese's nuke calls the source `zombie_head_gib`, then
`DoDamage(health + 666, origin)`. Its headless bleed thread calls
`DoDamage(health * 0.2, origin, attacker)` every second. These source rules
remain intact; neither needs a replacement nuke or an extra death timer.

Two builtin differences prevented the port from applying that damage:

- Passing an undefined attacker to T6 `DoDamage` together with explicit later
  optional arguments applied zero damage. The wrapper must supply worldspawn
  when WaW omitted the attacker, and use it as the default inflictor too.
- T4 truncates floating-point damage and clamps it to at least one point.
  T6 discarded a sub-one-point bleed, leaving very low-health headless actors
  alive indefinitely. The wrapper applies `max(1, int(amount))` before calling
  T6. It does not round larger fractional values up.

Native evidence: T4 `DoDamage` at `0x51C8D0` reads a float and passes the
integer conversion to `G_Damage` at `0x4F5D70`. The latter resolves null
damage entities to worldspawn and clamps damage to one before applying it.
The working IDB is `work/der_riese_acc/native_waw_fresh.i64`.

The baseline game probe measured zero damage from the old wrapper without an
attacker. A three-zombie nuke removed their heads but left all three at 150
health for eight seconds. A 0.4-point source bleed also left a two-health
zombie alive. Evidence is in `work/der_riese_acc/probe_nuke_20261031/`.

The missing white flash was separate: isolation renamed the engine-preloaded
`white` material to `waw_world/white`, but the new name was never registered
with `PrecacheShader`. Embedding a material asset in the FF does not register
its HUD shader index. Compatibility initialization now precaches every value
in the generated isolated HUD-material table, including materials the source
could rely on the engine having preloaded.

An A/B test showed native white while the isolated material was invisible.
Registering the isolated name restored the real full-screen source nuke fade
using the original translated material. Shader experiments were reverted.
Neither the nuke function nor its material/program is replaced.

Final game checks: script damage without a player attacker applied ten points;
the source head-gib thread killed a two-health zombie with its 0.4-point
bleed; all three nuke targets died under the source stagger. The white flash
and its fade were visually confirmed with Computer Use.
