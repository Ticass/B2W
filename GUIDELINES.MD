# WaW → BO2 Converter Development Guidelines

## Core Goal

This project is a **general-purpose World at War → Black Ops II map converter**.

It is not a Nuketown porting script, a one-map compatibility patcher, or a collection of map-specific hacks.

The converter must act as a **translation layer between WaW and BO2** and should be designed so that, in principle, any valid WaW custom map can be supplied as input and converted into a BO2-compatible map with as much content preserved as technically possible.

Every architectural decision, bug fix, parser change, asset conversion rule, script transformation, or compatibility layer must be evaluated against this goal.

The primary question should always be:

> "Will this work for arbitrary WaW maps?"

Not:

> "Will this make the current test map work?"

---

# 1. Never Substitute Content Just to Make the Current Map Run

Do not silently replace unsupported WaW content with generic BO2 content simply because an equivalent already exists in BO2.

The converter's job is to **convert or translate the original content**, not approximate it.

Examples of behavior that should generally be avoided:

- Replacing an unknown WaW FX with a generic BO2 FX.
- Replacing a custom WaW weapon with a stock BO2 weapon.
- Replacing custom sounds with BO2 sounds.
- Replacing unsupported models with placeholder models.
- Dropping animations because BO2 does not already contain them.
- Replacing WaW entities with vaguely similar BO2 entities without attempting translation.
- Removing script behavior merely because BO2 implements the same concept differently.

A successful conversion should preserve the source map's identity and behavior.

If a WaW map uses a custom explosion, weapon, sound, model, animation, script entity, or gameplay system, the converter should attempt to bring that content into BO2.

---

# 2. Missing BO2 Assets Are a Conversion Problem, Not a Substitution Problem

If an asset referenced by WaW does not exist in BO2, the default assumption should be:

> "We need to convert/import this asset."

Not:

> "We need to find something similar in BO2."

For example, if a WaW script contains:

```c
loadfx("maps/mp_maps/fx_mp_some_custom_effect");
```

and BO2 does not already contain that effect, do not automatically remap it to something such as:

```c
"maps/zombie/fx_zmb_generic_explosion"
```

Instead, determine how WaW FX assets are represented, extract the relevant source asset and dependencies, translate or rebuild it into a BO2-compatible representation, include it in the generated zone/asset output, and update the converted script reference if required.

A fallback replacement may exist only as an explicitly defined last-resort compatibility mode.

It must never become the normal conversion strategy.

## 2a. BO2 Equivalent Instead of Nothing (user rule, 2026-09-29)

WaW always has priority: the map's zones, then stock WaW zones, then WaW Mod Tools sources (Section 27).
Only when that whole lookup ends with **nothing** for an asset (the asset is absent from WaW, or it exists but
cannot be converted at all) may the converter use a BO2 equivalent instead of leaving a hole. Rules:

- Never pick a BO2 asset while a WaW source exists, or merely because WaW conversion is not implemented yet
  but possible; that is converter work, not a fallback case.
- The choice is data-driven (a global compat table keyed on the asset's role/kind), never map-name code.
- Every use is reported, e.g. `BO2_FALLBACK xmodel a1_fs_vm_usa_cia_camo_arms -> <bo2 model>: absent from WaW`,
  so it can be removed once the WaW asset becomes convertible.

**Opt-in exception: Remaster (user request, 2026-10-08).** With the "Remaster with BO2 materials" option
(`--remaster-bo2-materials`, off by default) a WaW *model* material that is a port of a BO2 material (same
name, optionally behind a `bo2_`/`bo1_`/`t6_`/`t5_` prefix, same `mc/` class) is drawn with the native BO2
material, technique set and images. This is for maps built from BO2 props ported back to WaW. World materials
keep the WaW lighting path. Every replacement is reported (`REMASTER material ...`), and a material that cannot
be extracted keeps its WaW translation. See `src/waw2bo2/remaster.py`.

---

# 3. Prefer Translation Over Emulation, and Preservation Over Approximation

Whenever possible:

WaW representation → BO2 representation.

The converter should understand differences between both engines/toolchains and convert one representation into the other.

Examples include:

- GSC syntax/API differences
- Entity/classname differences
- Dvar differences
- Material/shader differences
- XModel formats
- XAnim formats
- Weapon definitions
- Sound aliases
- FX definitions
- Rawfiles
- CSV/zone asset declarations
- Map entity definitions
- Radiant keys
- Spawn systems
- Zombie-specific scripting APIs
- Worldspawn settings
- Trigger behavior
- AI-related entities
- Script structs
- Path nodes
- Vision settings
- Lighting/environment settings

Avoid building a growing list of replacements that merely imitate WaW behavior using unrelated BO2 assets.

---

# 4. Every Fix Must Be Global by Default

When an issue is discovered while converting a particular map, treat that map as a **test case exposing a converter deficiency**.

Do not treat it as a special case unless there is absolutely no generalized interpretation.

For example:

Bad:

```python
if map_name == "nazi_zombie_nuketown":
    fix_nuketown_fx()
```

Better:

```python
if source_fx_uses_waw_format(asset):
    convert_waw_fx_to_bo2(asset)
```

Bad:

```python
if weapon_name == "nuketown_custom_raygun":
    ...
```

Better:

```python
if weapon_definition.requires_waw_weapon_translation():
    convert_weapon_definition(...)
```

Bad:

```python
if entity.targetname == "nuketown_power_switch":
    ...
```

Better:

```python
if entity.classname == "script_model" and entity_has_waw_power_interaction(entity):
    translate_interaction_entity(...)
```

A bug encountered on Nuketown should ideally result in support for an entire **class of WaW maps/assets**, not merely Nuketown.

---

# 5. Map-Specific Hacks Are Strongly Discouraged

Map-specific hacks fundamentally work against the purpose of this project.

They should only be introduced when all of the following are true:

1. The behavior genuinely cannot be inferred from standard WaW data.
2. The source map itself contains unusual/nonstandard logic.
3. A generalized converter rule would negatively affect other maps.
4. The exception is clearly documented.
5. The exception is isolated from the main conversion pipeline.

Any unavoidable compatibility rule should be implemented as a data-driven override or compatibility profile rather than hardcoded deeply into conversion logic.

Preferred structure:

```text
converter/
    scripts/
    assets/
    fx/
    models/
    sounds/
    entities/
    compat/
        overrides/
```

The global conversion logic must remain the default path.

---

# 6. Do Not Code Toward zm_nuked's Existing Asset Pool

`zm_nuked` is a useful BO2 reference map and test target.

It is not the asset universe of the converter.

Do not reason like this:

> "Which assets already exist in zm_nuked that we can substitute?"

Instead reason like this:

> "What assets does the WaW map require, and how do we make those assets available to BO2?"

The resulting converted map should not depend on `zm_nuked` accidentally containing compatible assets.

A different WaW map may require completely different:

- FX
- models
- weapons
- materials
- sounds
- animations
- scripts
- entities
- vision settings
- map-specific assets

The converter should build the required BO2 asset set from the WaW source map.

---

# 7. Asset Dependency Discovery Must Be Recursive

The converter should not only convert assets directly referenced by the map.

It should discover and convert dependencies recursively.

For example:

```text
Weapon
 ├── XModel
 │    ├── Material
 │    │    ├── Image
 │    │    └── Shader configuration
 │    └── Additional XModels
 ├── XAnim
 ├── Sound aliases
 │    └── Sound files
 ├── FX
 │    ├── Materials
 │    ├── Models
 │    └── Images
 └── Scripts
```

If a converted asset depends on another WaW asset, that dependency becomes part of the conversion graph.

The goal should be to construct an asset dependency graph and process every required asset.

---

# 8. FX Must Be Converted

WaW FX should be considered first-class convertible assets.

Do not assume FX must map to an existing BO2 FX path.

The converter should eventually support:

- Parsing WaW FX definitions.
- Resolving referenced materials/models/images.
- Translating compatible FX elements into BO2 equivalents.
- Rebuilding incompatible FX constructs where necessary.
- Generating BO2-compatible FX asset declarations.
- Adding required assets to the zone output.
- Rewriting script references only when the output asset path changes.

If exact FX conversion is not implemented yet, report the FX as unsupported/incomplete.

Do not hide the missing feature by silently replacing it with unrelated BO2 FX.

---

# 9. Custom Weapons Must Be Fully Ported

Custom guns are part of the map and should not be reduced to stock BO2 replacements.

The converter should aim to port:

- weapon definitions
- world models
- view models
- attachments where applicable
- materials
- textures
- firing sounds
- reload sounds
- mechanical sounds
- muzzle FX
- shell eject FX
- impact FX references
- idle animations
- fire animations
- reload animations
- sprint animations
- raise/lower animations
- ADS-related animation data
- weapon scripts
- ammo properties
- damage properties
- projectile behavior
- weapon-specific script callbacks

When WaW and BO2 weapon systems differ, implement a translation layer.

Do not solve this with:

```text
WaW weapon → closest stock BO2 weapon
```

unless the user explicitly enables a compatibility/fallback mode.

---

# 10. XModels Must Be Ported

WaW XModels referenced by a map should be detected and converted.

This includes:

- static props
- scripted models
- weapon models
- zombie-related models
- custom environment assets
- animated models
- destructibles
- map-specific props

The model conversion pipeline should also discover dependencies such as:

- materials
- textures/images
- skeleton/bone information
- collision where applicable
- LOD data where applicable
- associated animations

Do not replace unknown models with stock BO2 props.

If model conversion fails, report the failure explicitly.

---

# 11. XAnims / Animations Must Be Ported

Animations are not optional decoration.

They may be required for:

- weapons
- scripted models
- zombie interactions
- doors
- traps
- machines
- environmental sequences
- custom enemies
- scripted sequences

The converter should translate WaW XAnim assets into BO2-compatible animation assets wherever technically possible.

Bone/skeleton compatibility issues should be handled by the animation/model conversion layer.

Do not simply strip animation references because the converted map can technically load without them.

---

# 12. Sounds Must Be Ported

Sound preservation includes more than copying audio files.

The converter should account for:

- raw audio files
- sound aliases
- alias groups
- volume
- pitch
- distance attenuation
- channel/type
- looping behavior
- random variants
- script references
- weapon sound references
- environmental sound references

WaW sound aliases should be translated into BO2-compatible sound alias definitions.

If audio encoding must change, transcode it.

Do not replace custom audio with BO2 stock audio merely because it is easier.

---

# 13. Entities Must Be Translated Semantically

Entities should be converted according to what they **mean and do**, not merely by matching names.

For every WaW entity or classname, determine:

- What does this entity represent?
- What properties are important?
- What behavior does it cause?
- Is there a direct BO2 entity equivalent?
- If not, can the same behavior be reproduced using BO2 entity/script systems?

Examples include:

- script_model
- script_origin
- triggers
- spawn points
- zombie spawners
- path nodes
- doors
- doors with script behavior
- destructible objects
- vehicles
- blockers
- volumes
- zone entities
- lighting entities
- ambient sound entities

The conversion should preserve entity properties and relationships such as:

- target
- targetname
- script_noteworthy
- script_linkname
- spawnflags
- angles
- origin
- model
- script_string
- custom keys

Do not drop unknown keys without first determining whether they affect behavior.

---

# 14. GSC Conversion Should Use a Compatibility/Translation Layer

WaW and BO2 scripting APIs differ.

Do not solve every script error individually with ad-hoc patches.

Build a reusable translation system.

Examples:

```text
WaW function
    ↓
translation rule
    ↓
BO2 function / wrapper implementation
```

Where there is no direct BO2 equivalent, consider implementing a BO2 compatibility wrapper that reproduces the WaW behavior.

For example:

```text
WaW API
    ↓
converter compatibility function
    ↓
BO2 implementation
```

This allows many maps to reuse the same compatibility logic.

Prefer centralized compatibility modules over rewriting every map script independently.

---

# 15. Preserve Source Behavior Before Improving It

Do not redesign the map while converting it.

The converter should first attempt to preserve:

- gameplay
- timing
- interactions
- progression
- effects
- audio
- weapons
- entities
- scripts
- visual identity

Optimization and BO2-specific enhancements can happen later.

Conversion fidelity comes first.

---

# 16. Unsupported Features Must Be Explicit

If something cannot currently be converted, say so.

Examples:

```text
UNSUPPORTED_ASSET: fx/waw_custom_flame
UNSUPPORTED_ENTITY: custom_vehicle_controller
UNSUPPORTED_GSC_API: maps\_utility::legacy_function
UNSUPPORTED_XANIM_VERSION: ...
```

The converter should generate useful diagnostics including:

- source asset
- source file
- reference location
- reason conversion failed
- dependency chain if applicable
- suggested converter subsystem requiring implementation

Do not silently:

- remove it
- replace it
- ignore it
- fake it

Silent degradation makes debugging converted maps much harder.

---

# 17. Fallbacks Must Be Explicit and Opt-In

Fallback behavior may be useful during development, but it must be separated from real conversion.

Example modes:

```text
--strict
--compat
--allow-placeholders
```

Suggested behavior:

## Strict mode

The default development target.

- No arbitrary substitutions.
- Missing conversions are reported as errors.
- Asset fidelity is prioritized.

## Compatibility mode

Allows carefully defined behavioral compatibility conversions where exact representation is impossible.

## Placeholder mode

May substitute placeholder assets for debugging map loading.

This mode should clearly report every substitution.

Placeholder mode must never be confused with successful conversion.

---

# 18. Do Not Confuse "Map Loads" With "Map Converted Successfully"

A map that boots in BO2 but has:

- missing FX
- substituted weapons
- missing sounds
- broken animations
- removed entities
- altered gameplay

is not successfully converted.

Loading is one milestone.

Conversion fidelity is the actual goal.

---

# 19. Test Using Multiple Maps

Nuketown should be treated as one fixture in a larger test suite.

As development continues, test maps should intentionally exercise different systems.

Examples:

```text
Map A:
    stock WaW assets

Map B:
    custom weapons

Map C:
    custom XModels

Map D:
    custom FX

Map E:
    heavy scripting

Map F:
    unusual entities

Map G:
    custom sounds

Map H:
    custom animations
```

A fix should ideally improve multiple maps.

When adding a regression test for an issue discovered in one map, test the generalized feature rather than the map name.

---

# 20. Build Conversion Subsystems, Not One-Off Fixes

Prefer architecture such as:

```text
WaW Source
   ↓
Source Scanner
   ↓
Dependency Graph
   ↓
Intermediate Representation
   ↓
Conversion Subsystems
   ├── BSP / Map Geometry
   ├── Entities
   ├── GSC
   ├── XModels
   ├── XAnims
   ├── Materials
   ├── Images
   ├── FX
   ├── Weapons
   ├── Sounds
   ├── Rawfiles
   └── Zone Assets
   ↓
BO2 Asset Graph
   ↓
Generated BO2 Map
```

Whenever possible, parse WaW content into an intermediate representation rather than directly performing string replacements.

This makes differences between WaW and BO2 explicit and allows the converter to evolve cleanly.

---

# 21. Conversion Decisions Must Be Data-Driven

Avoid giant collections of hardcoded source asset names.

Prefer rules based on asset metadata, type, structure, version, properties, dependencies, and engine semantics.

Hardcoded lookup tables are acceptable when the relationship genuinely exists globally between WaW and BO2.

Example:

```text
WaW API name → BO2 API name
```

may be legitimate.

But:

```text
nuketown_fx_01 → bo2_fx_17
```

is likely a map-specific workaround and should be questioned.

---

# 22. Preserve Unknown Data Whenever Possible

When the converter encounters data it does not fully understand, do not immediately discard it.

Preserve it in:

- intermediate representation
- metadata
- conversion logs
- sidecar files

Future converter versions may learn how to translate it.

Data loss should be deliberate, documented, and rare.

---

# 23. Generated Output Should Be Self-Contained

The final BO2 map should contain or reference all assets it legitimately requires.

It should not accidentally work only because the developer already has:

- zm_nuked assets
- another custom map installed
- manually imported files
- development-only compatibility assets

Dependency generation and zone inclusion should be deterministic.

A clean BO2 modding environment should be able to build the converted output using the generated assets.

---

# 24. Before Implementing a Shortcut, Ask These Questions

Before adding any fallback, substitution, or special case, ask:

1. Is this actually converting the WaW content?
2. Would this work for another WaW map using the same feature?
3. Are we preserving the source map's behavior and identity?
4. Are we relying on an asset that happens to exist in our test map?
5. Could the original WaW asset itself be converted instead?
6. Is this revealing a missing converter subsystem?
7. Will this create technical debt that makes true conversion harder later?
8. Is this a temporary debugging workaround or a real conversion rule?

If the answer suggests the converter is merely hiding an unsupported feature, implement or extend the appropriate conversion subsystem instead.

---

# 25. Example: Correct Handling of Missing FX

Do NOT do this by default:

```text
WaW:
loadFx("custom/waw/flame_large")

BO2:
loadFx("maps/zombie/fx_generic_fire")
```

just because the BO2 effect exists.

Instead:

```text
1. Resolve custom/waw/flame_large.
2. Locate its WaW FX asset.
3. Parse the FX definition.
4. Find referenced materials, images, models, and other FX.
5. Convert those dependencies.
6. Translate the FX definition to BO2's format.
7. Generate/import the BO2 FX asset.
8. Add it to the output zone.
9. Rewrite the reference only if its generated asset path changes.
```

If step 6 is not implemented yet, return something like:

```text
FX conversion unsupported:
custom/waw/flame_large
```

That missing capability then becomes work for the converter.

It should not be hidden behind a generic fire effect.

---

# 26. Definition of Success

The project's ideal end state is:

```bash
waw2bo2 convert path/to/waw_map
```

and the converter automatically:

- scans the map
- discovers dependencies
- extracts required WaW assets
- translates map data
- translates entities
- translates scripts
- converts models
- converts animations
- converts materials
- converts textures
- converts FX
- converts sounds
- converts lights / lighting
- converts custom weapons
- generates BO2 zone declarations
- creates the BO2-compatible map/project
- reports only features that truly cannot yet be translated

The resulting BO2 map should resemble and behave like the original WaW map because the original content was actually converted.

That is the standard every implementation decision should work toward.

---

# 27. WaW Asset Sources and Lookup Order

A WaW map does not carry every asset it uses. A compiled zone can hold an asset only as a *reference* (the WaW linker found it in another zone), and scripts can name assets the mapper never compiled. The converter must know every place WaW content can come from, and search them in a fixed order:

1. **The map's own zones**: `<map>.ff`, `<map>_patch.ff`, `mod.ff`, their localized zones, and the mod's IWDs.
2. **The zones WaW loads with every map**: `common.ff`, `code_post_gfx.ff`, `patch.ff`, and the stock IWDs in `main`.
3. **Other stock zones of the WaW install**, for assets the map only references. These are real WaW content (other zombie maps, campaign levels).
4. **WaW Mod Tools raw sources** (`raw/fx/*.efx`, raw images, materials, weapon files, sound aliases), for assets that exist only as uncompiled sources.

The tool must take both locations as inputs:

```text
--waw-root        WaW install (zone/, main/)
--waw-mod-tools   WaW Mod Tools install (raw/, deffiles/, bin/)
```

and should eventually locate them automatically (Steam libraries, the registry, a `wawModTools` folder next to the project), with the explicit options always taking precedence. Paths are never hardcoded into conversion logic.

Every asset that comes from sources 3 or 4 is reported with where it came from:

```text
WAW_STOCK_ASSET  material gfx_fxt_light_flare2: ported from the stock WaW zone nazi_zombie_factory.ff
WAW_SOURCE_ASSET fx env/fire/fx_fire_campfire_small: compiled from WaW Mod Tools raw/fx
```

The same lookup is what makes **stock WaW maps** (the retail and DLC zombie maps) convertible: their assets are the install's zones plus the Mod Tools sources.

---

# 28. The WaW Build Is the Reference Behavior

When deciding what a converted map should do, the reference is what the *compiled WaW map* did in WaW, not what its scripts or comments suggest the mapper intended.

Every referenced asset falls into exactly one class:

| Class | Example | Converter action |
|---|---|---|
| In the map's zones | `generator_top` | Convert. |
| Only referenced; defined in a stock WaW zone | `gfx_fxt_light_flare2` | Convert from that zone. Report `WAW_STOCK_ASSET`. |
| Exists only as a Mod Tools source, never compiled for this map | UGX Easy FX presets such as `env/fire/fx_fire_campfire_small` | Opt-in only (it adds content the WaW build did not show). Report `WAW_SOURCE_ASSET`. |
| Absent from the whole install and the Mod Tools | the Nuketown "storm" rain/lightning, `steamred` | Skip and report. Never fake it with a BO2 asset. |

Know what WaW itself did with a missing asset before choosing the converted behavior. For example, `loadfx` of an absent effect returns WaW's default `misc/missing_fx` (a brief placeholder sprite), and playing an alias no script defines is a runtime script error that plays nothing. Neither is a reason to invent content.

---

# 29. Shared Script Libraries

Many WaW maps reuse community script packages (for example UGX Easy FX, UGX weather). Support them through the general machinery, not per-library or per-map code:

- the script ports like any other map script;
- the entities it relies on (script_structs, triggers, their KVPs) are preserved;
- the assets it names resolve through Section 27.

Only add library-aware logic when a library's *runtime pattern* needs a BO2 translation (for example, one spawned `script_model` per effect struct can exceed BO2's entity budget). Such logic must key on the pattern, not the library or map name, and must be reported.

---

# 30. Measure Layouts; Do Not Assume Them

Binary layouts, enums and flag bits must be verified against ground truth before a conversion rule relies on them:

- pair stock compiled BO2 assets with their BO2 raw sources to learn T6 layouts;
- pair WaW assets with same-named BO2 assets to confirm the mapping;
- check statistics over whole datasets (e.g. "this flag is set exactly when gravity is non-zero").

Vendored headers can be wrong. The T4 `FxElemType` enum names in OpenAssetTools do not match WaW's data (WaW 3 = line, 4 = trail, 5 = cloud); only the data told us.

Every new binary path needs a round trip: dump the built BO2 zone again and compare it with what the converter staged.

---

# 31. Converted Assets Must Not Collide With Stock BO2 Assets

WaW and BO2 share many asset names (`misc/fx_zombie_eye_single`, `gfx_fxt_smk_gen`, ...) with different content. A map-zone asset with the same name as one in an always-loaded BO2 zone is an override error, or silently shows the stock asset instead.

- Converted assets live in a converter namespace (e.g. `waw/<fx>`, `waw_fx/<material>`).
- Scripts reach them through generated tables (`level.waw2bo2_fx`), never through hand-edited names.
- Anything left unconverted is renamed too when it would otherwise resolve to a stock BO2 asset of the same name (e.g. FX sound elements use `waw/<alias>` until WaW sounds are converted), so BO2 content never plays in place of WaW content by accident.

---

# Final Rule

**Do not solve converter deficiencies by hiding them.**

If WaW contains something BO2 does not already have, that is usually a signal that the converter needs another translation capability.

The objective is not:

> "Make this WaW map run somehow in BO2."

The objective is:

> **"Teach the converter how to translate World at War maps into Black Ops II maps."**