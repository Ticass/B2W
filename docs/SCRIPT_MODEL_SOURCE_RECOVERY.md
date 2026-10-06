# Runtime WaW model dependencies

Model names used by `precacheModel`, `setModel`, `setViewModel`, or `attach`
must be resolved before the converter filters the startup precache list.
Models used only after a gameplay event can be absent from the map's entity
and static model tables.

`recover_script_models` searches the supplied compiled WaW roots first,
then the stock WaW zone index, then binary `raw/xmodel` definitions compiled
with the WaW linker. Recovered dump roots feed the existing model, material,
and image conversion stages. All resolved script models enter the startup
precache list, including those referenced only by a later model swap.

The report retains every requested name in `scripts.requested_models` and
records its result in `scripts.model_sources`. This lookup never substitutes
BO2 models. Missing original model definitions remain explicit diagnostics.

For a literal `setModel` or `precacheModel` whose source is unavailable and
whose name is not a loaded stock runtime asset, the converter emits a
`waw_missing_model` call. It reports the unsupported operation once, retains
the existing entity model, and lets subsequent source statements execute.
This avoids stopping power-on threads before their original dynamic-light FX
and purchase notifications. Available models, dynamic names, comments, and
user-defined model functions are untouched. Guarded calls are recorded in
`scripts.guarded_missing_model_calls`.

## Current source limitation

The October 5, 2026 audit of the supplied Nuketown Remastered 1.2 files found
no definitions for the powered-on Cherry, Mule Kick, or Stamin-Up models
named by its perk scripts. The audit checked all five map fastfiles, both
IWDs, the installed stock WaW zone index, and installed WaW Mod Tools raw
model definitions. The supplied current machine models are present.

Evidence: `work/perk_waw_source_audit_20261005.json`. The user confirmed these
machines light in the original WaW game. Their source power-on threads also
start `misc/fx_zombie_cola_*` effects containing looping dynamic lights; these
are converted. Missing model calls occur before those lights and can stop
the T6 thread. Runtime verification of the guarded behavior remains necessary.
No BO2 machine substitutes or invented lit model variants were added.
