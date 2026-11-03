# Der Riese casual fixes (retired)

The optional `der_riese_waw_casual_fixes.gsc` script is gone. Its fixes are now
built into every converted map as console dvars, off by default
(`src/waw2bo2/compat/waw_fixes.json`):

| console | fix |
|---|---|
| `g_fix_tesla_bug 1` | A health write keeps the player's maximum health, as Plutonium T4's dvar does: a Wunderwaffe self-hit no longer leaves you at 1/1 with no regeneration. |
| `g_fix_last_stand 1` | Solo with Quick Revive, a lethal hit enters T6's native last stand and self-revive (at most three times; the third purchase hides the machine) instead of ending the game. |

Type them in the console before or during a game; they stay set through
level restarts until the game is closed. Delete the old script from
`%LOCALAPPDATA%\Plutonium\storage\t6\scripts\zm\`. Current maps no longer
publish the hooks it patched, so a leftover copy prints "incompatible map
release; fixes disabled" and does nothing.

`der_riese_waw_health_audit.gsc` is a passive diagnostic and is unchanged.
