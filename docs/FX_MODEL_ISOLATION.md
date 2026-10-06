# Model references in converted effects

Converted FX assets already use `waw/` names, but their model visuals previously
retained the original model names. BO2 gameplay fastfiles can contain different
models under those same names, so a converted effect could resolve to a BO2 model
even though the WaW geometry was staged in the map fastfile.

Every nonempty model visual now references `waw_fx_model/<source name>`. After
staging the original WaW models, the bridge emits independent model assets under
these names and includes them in the world zone. They reuse the exact converted
source LODs, materials and model definitions. No particle elements are removed;
spawn ranges, transforms, counts, lifetimes and visual graphs remain intact.
Missing converted source models produce errors. The staged asset mapping appears
under `content.fx_model_assets` in the bridge report.

For Nuketown, native extraction verifies five owned models with matching source
geometry and seventeen effects changed only in model references and the resulting
string-storage sizes. Pack-a-Punch's temporary model remains the original WaW
model with its original ten-second lifetime. The user subsequently confirmed
that asset isolation did not resolve the overlapping machine, and that the
battery appears only during upgrading. The extracted effect model geometry
matches WaW; a foreign BO2 model was not established as the cause.

## Server initialization and effect ownership

The later conditional model-reuse workaround also failed the user's gameplay
check and has been removed. No source FX elements are suppressed.

The server had relied on replacing `maps/mp/zombies/_zm_perks.gsc` after stock
scripts were already available. The client had a unique bootstrap to avoid
cached stock initialization, but the server did not. The stock server initializer
starts its own machine swaps and purchase controllers and assigns its stock
Pack-a-Punch effect to `level._effect["packapunch_fx"]`. This is a competing path;
the actual earlier runtime execution was not captured in this session.

Both server and client now have unique `_waw2bo2_zm` and `_waw2bo2_perks` script
names. Map initialization explicitly calls the owned server bootstrap, whose
perk initializer registers support fields and arrays without starting stock
machine controllers. Power availability calls also use the owned perk script.
Other BO2 server framework initialization remains intact.

The translator separately renames source `level._effect` accesses to
`level.waw_effect`, initialized by compatibility startup. This prevents BO2 FX
initializers from replacing source effects even if initialization order changes.
The full eight-element WaW Pack-a-Punch effect, its models, and its timing remain
unchanged. No model, map, or perk name exception is introduced.

All 237 tests pass. Native extraction verifies 75 compiled scripts, the explicit
owned server call chain, the separated source effect table, the unchanged full
source effect, and byte-identical grenade collision data. All nine installed
files match build hashes. Evidence: `work/pap_server_ownership_20261005`.
The user's screenshots confirm an extra shell/sign/rollers appears during use,
with a battery already visible on the idle machine. The correction is installed;
in-game behavior remains unverified. Do not claim duplication is resolved until
a fresh-session runtime test confirms it.

## Conditional model reuse with owned effect IDs

The user confirmed the server/effect ownership correction selects the correct
WaW model, while the full-model overlap remains. The earlier model-reuse attempt
preceded effect-table isolation and did not resolve the user's gameplay test.
The native engine analysis did not establish a new flag or shader defect, and
Pack-a-Punch geometry/materials/full effect already existed in the pre-lighting
backup. Do not claim the missing-model lighting guard introduced those assets.

The current conditional reuse mapping is registered by `waw_loadfx` against
native IDs stored in the source-owned `level.waw_effect` table. `waw_playfx`
selects a variant only when a script_model of the same source model is within
four units of the effect origin. Eligible elements are stationary, single-spawn,
unit-scale models without physics, motion, random rotation, collision, or child
FX. Source FX remain intact at unmatched locations; animated debris is excluded.
This map has one qualifying effect: Pack-a-Punch. Its variant contains the exact
other seven elements and timing; the powered WaW entity renders the machine.
No existing entity is hidden, swapped or deleted and no perk/map name exception
is introduced. A console-only `WAW2BO2 FX MODEL REUSE` diagnostic records the
first matching selection; `WAW2BO2 FX MODEL UNMATCHED` records a failed match.

236 tests pass. Native extraction verifies 75 scripts, full original effect,
seven exact retained elements in the conditional variant, and byte-identical
collision to the grenade rollback. Nine installed output hashes match. Evidence:
`work/pap_overlay_owned_ids_20261005`. The current revision still needs a fresh
in-game upgrade test; user confirmation so far covers model selection only.
