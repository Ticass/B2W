# Source hint strings and weapon labels

The converter stages localization used by converted server/client scripts and
weapon display fields. Source map fastfiles and IWD/StringEd files take priority,
followed by stock WaW and raw source strings. Exact BO2 strings and explicitly
listed compatibility labels are used only when source text is absent, with their
provenance recorded. Missing technical keys are conversion errors.

Each imported key receives a `WAW2BO2_` prefix. Script references and weapon
fields use these owned keys, preventing a BO2 stock string from silently replacing
the original map's wording. Literal custom weapon names also become localization
entries. The original text, color codes, button tokens and substitution markers
are preserved. Repeated staging is idempotent.

WaW's `init_strings` registry is extracted and initialized before map scripts run.
Adapted `add_zombie_hint`, `get_zombie_hint` and `set_hint_string` calls, along with
direct source `level.zombie_hints` accesses, use a separate source registry. This
preserves cost-bearing IDs such as `default_buy_door_750`, which BO2's native
utility normally expects as a base ID plus a separate cost argument.

The catalogue is included in the world and gameplay fastfiles. The native shader
relink overlay also includes it. `content_source/localization.stage.json` records
every value, original key, output key, source and unresolved reference. Coverage
is limited to references available in supplied scripts and weapon data; new
in-game reports should be checked against that catalogue and their call sites.
