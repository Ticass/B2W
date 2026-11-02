# Der Riese playtest regressions — 2026-11-01

User reports: red cubes persist after zombie kills and flash near gunfire; zombie legs disappear when shot; dropping from the stairs into the power area causes damage and explosion stun; missing/repetitive/poorly balanced audio, including round music, Max Ammo, and boards.

Confirmed causes and converter changes:

- `waw_attach(name)` forwarded an explicit undefined tag into T6's Attach. A native runtime fixture rendered an intact model, upper body with the wrapper, and upper body with native Attach side by side. The wrapper yielded zero attached models and invisible legs; native Attach yielded one attachment and visible legs. The wrapper now omits an absent argument.
- Model isolation renamed bone-tag literals that shared a model identity (notably tag_origin). Tag arguments now retain their source identity. Internal missing-tag effect carriers resolve the converted anchor model and hide their render mesh.
- Source animation notes still used bare sndnt# aliases while the bank contains waw/ aliases. Audit found 345 affected notes in 76 staged animations. Notes now bind to converted sound identities without changing motion, count, order or frame timing.
- Main-state extraction discarded guarded defaults if another event also assigned the field, and only accepted bare assignments. The source music suppression flag was undefined in game and natural round music never started. Guarded source defaults are now retained.
- WaW HUD font creation was incorrectly routed to BO2's player-only createfontstring. The source power-up code calls it on level before playing the announcer. Source createfontstring/createserverfontstring now run with their global/explicit-player semantics.
- T6 fall defaults measured at 128/300. WaW PM_RegisterDvars at 0x40E298/0x40E2CE loads float defaults 200/350 from 0x8E4554/0x82B684. Compatibility startup sets these engine defaults before source/map overrides. WaW's own damage and death behavior remains source-driven.

The native load also exposed the source helper's one-argument NewHudElem call against a zero-argument compatibility declaration. The wrapper now accepts the optional owner while retaining server-HUD creation.

Validation completed: 857 tests ran, one skipped. Both final fastfiles linked with zero missing map assets; 93,076 map and 47,371 mod material arguments passed, as did 4,212 authored bone hitboxes across 108 models. The nine installed package files match the release manifest.

Fresh runtime checks: wrapper and native leg attachments both count one; the lower body was visually present in the comparison fixture. A 160-unit drop landed at 100 health with native defaults 200/350. The source music flag initialized to zero and natural music reached round_begin. The full Max Ammo path entered and reset its announcer state; a separated native sound playback completed after 3,000 ms. No script errors occurred, the source head-gib test completed without an observed red marker, and the temporary observer was removed. The initial 100 ms timing result came from an overlapping duplicate playback; the alias has a one-second delay and a one-voice limit.

Evidence and receipts are under work/der_riese_acc/issues_20261101. The exact power-area stair jump and the overall audio balance still require normal playtest confirmation; do not claim a full audio-fidelity audit from these targeted checks.
