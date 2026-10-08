# Changelog

## v0.2.13

- Preserve each WaW zombie spawner's original actor, character selection, body/head models and dependencies instead of using Nuketown's characters. Animated models use the T6 dynamic renderer flags and retain every source LOD and distance.
- Decode WaW's HUD font-scale network encoding. Values such as `8` become the effective WaW display scale `1.6`, fixing oversized health and scoreboard text.
- Enable solo Quick Revive through BO2's native life/revive controller in source-perk mode, preserving each map's machines and purchase rules.
- Spawn queued zombies on converted WaW spawners through BO2's force-spawn branch.
- Read ScoreSolo's WaW session statistics from BO2's live counters, including player names, headshots and gibs.
- Build Bank Job and Empty Walls with their edited stock damage conditions and zero-damage solo-revive cancellation preludes. Unrecognized damage application remains an explicit error.
- Resolve script-only models, wrapper model arguments, stock raw actor scripts and techniques outside the BO2 donor zone. Map assets retain priority over stock definitions.
- Correct melee wall-buy charges, item-weapon melee registration, source zone-volume recognition, single-leaf collision trees, localized weapon hints and startup weapon-table guards.
- Restore default vectors for WaW script structs, including unrotated game-over camera endpoints. Wall buys without display models receive an unmatched target rather than an undefined native lookup.
- Include exception types in CLI failure messages so exceptions with empty text remain diagnosable.
- Carry weapon/script rumble profiles and both graph dependencies, preventing a missing `rumble/flamethrower` runtime drop. Translate DLC powerup registration callbacks with their original parameters and initialize the Plutonium flag used by WaW scorebar libraries.

## v0.2.12

- Linux: the release no longer bundles Python, Tk or system libraries from the build machine (which tied it to Ubuntu 24.04 / glibc 2.39). It runs on the distribution's own Python; the README and START HERE list the packages for Ubuntu/Debian-based distributions, Fedora and Arch, and the app names anything missing.
- Maps without a `mod.ff` (everything in the map zone) are no longer blocked by the setup check, and a map shipped only as `mod.ff` can be chosen: the build finds its world inside it.
- Script-model entities naming a model that exists in no WaW zone or source (e.g. weapon paths typed as models) keep their entity without a model, as WaW loads its default, instead of failing the build.

## v0.2.11

- Convert WaW maps without an `add_adjacent_zone` graph (prototype/asylum/sumpf-style scripts): spawner groups become map-wide zones, opened by the doors and debris that add them in WaW.
- Read zone links written as qualified calls (`maps\_zombiemode_zone_manager::add_adjacent_zone`).
- Resolve localized string references case-insensitively, as WaW does; a reference with no source text is shown as its key and reported as a warning instead of failing the build.
- Approximate materials whose WaW technique set has no T6 rule (foliage sway, water, additive falloff) with the nearest plain lit or unlit pass using their own textures, reported as `APPROXIMATED_MATERIAL`.
- Rebuilding a map no longer fails the shader relink on materials left in the stage by an earlier build (`Cant find pixel shader ...`); only materials staged by the current build are bound.
- The world link now always prefers the map's own assets over same-named BO2 stock assets (the linker orders search paths by name, so the stock cache could shadow them; e.g. `mtl_fx_bullet_chain`).
- The gameplay-mod shader relink no longer rebinds a BO2 stock material that shares its name with a converted WaW one (`mtl_prop_bear` failed the material audit).
- Flat script models (zero thickness on one axis) get a valid collision box instead of failing staging with `division by zero`.
- WaW weapon, script and vision files written in the Windows code page are read correctly (`'utf-8' codec can't decode byte 0xd7`).
- Custom-map extraction no longer silently loses files past Windows' 260-character path limit (long shader, sound and model names; e.g. 534 shaders of Empty Walls, Alcatraz's tarp models): exports are staged near the cache top, an unwritten model or image fails the extraction instead of publishing an incomplete cache, and previously extracted custom maps are extracted again once.
- Extracted WaW framework functions that reference `#animtree` without playing animations (e.g. `_spawner`'s drones) compile instead of failing with `trying to use animtree without specified using animtree`.
- Enclosed maps with no sky surface or skybox model get a black sky, as WaW draws them, instead of failing (`skybox: needs --bo2 and a WaW sky cubemap`).
- Plain DXT images that a map ships only in its IWD, and that the zone dump did not write, are read straight from the IWD (`source coffee_machine_col.dds missing from every image root`).
- An unlinker crash during extraction (access violation or fail-fast) is retried up to twice.
- Map functions named like T6 built-ins (back-ported BO2 helpers such as `getFirstArrayKey`) are renamed with their calls instead of failing script compilation.
- Weapons with an unset enum field (e.g. a custom crossbow's empty `playerAnimType`) link with the BO2 default instead of failing (`Not a valid value for field "playerAnimType": ""`).
- A parenthesised method caller (`(self) IsTouching( trig )`, accepted by WaW) is unwrapped for the T6 compiler.
- A `/*` comment left open at the end of a script hides the rest of the file, as in WaW, instead of being ported as code and failing compilation.
- Model bones with NaN offsets (written as JSON `null`) are neutralized instead of failing the world link.
- Long WaW source-asset names no longer exceed the WaW linker's 63-character zone-name limit (`Could not open '../zone_source/...csv'`).
- Converted materials no longer inherit their BO2 donor's thermal-vision material, which could fail the world link on a missing stock image (`thermal_gradient2`).

## v0.2.9

- Parse malformed standalone block-comment endings found in stock WaW ambient scripts, and normalize them for the BO2 compiler without enabling commented-out code.
- Include the script name and correct line number in tokenizer errors.

## v0.2.8

- Extract and locate custom map entities, refreshing incomplete caches automatically.
- Preserve partial final path visibility bytes in conversion and native linking.
- Check script links after generated perk modules exist and supply the missing server ambient entry.
- Resolve additional standard door prices and the community Mule Kick localization key.
- Recover meshes split across WaW zones and save complete desktop failure diagnostics.
- Add opt-in BO2 stock perks and restore box use triggers after relocation.

## v0.2.7

- Limit BO2 preparation to Nuketown, prison (basic wooden barrier), and shared Zombies runtime zones. Other BO2 maps are neither exported nor required.
- Trim prison to the wooden barrier dependency closure; fetch additional BO2 textures only when needed.
- Includes the compact-cache migration and cleanup from v0.2.6.

## v0.2.6

- Replace full-game extraction with a compact conversion cache: stock metadata, scripts, and shared render/runtime donors.
- Extract other WaW stock payloads only when referenced; cache those dependencies once.
- Migrate existing full exports without another dump where possible, and remove obsolete stock generations after validation.
- Remove failed partial native exports while preserving diagnostic logs.

## v0.2.5

- Extract installed zones and custom-map companion fastfiles in parallel using every logical CPU.
- Preserve deterministic duplicate-asset priority, resumable caches, and per-zone logs across concurrent workers.
- Report worker and completion counts; WAW2BO2_EXTRACT_WORKERS can limit concurrency.
- Avoid extracting mod.ff twice when it is selected as the source.

## v0.2.4 - Extract All search-path crash fix (2026-10-07)

- Pass only existing search directories to the native extractors.
- Fix Extract All aborting on normal WaW installations without zone/all or sound directories.
- Resume previously completed zone extractions when retrying after the crash.

## v0.2.3 - Shared extraction cache and build progress (2026-10-07)

- Add Extract All and standalone All2Raw executables for Windows and Linux.
- Cache all installed WaW/BO2 zones, asset indexes, and stock sound-driver data across builds.
- Resume interrupted extraction and detect changed inputs or missing cache files.
- Add Verbose Console, asset counts, phase timings, and 15-second quiet-process status messages.
- Accept numeric prefixes in vision scalar fields like WaW, including shipped 0.O458 brightness values, and report them.
- Retain the v0.2.2 rectangular lightmap fix.

## v0.2.2 � Rectangular lightmap hotfix (2026-10-07)

- Accept rectangular WaW lightmap layers, including 512x2048 secondary textures
  that previously stopped conversion with an expected-size error.
- Preserve layer dimensions, lighting data, and sun visibility in both encodings.
- 36 related lighting and shader tests pass, including the reported dimensions.
  The affected user map has not yet been converted or playtested.

## v0.2.1 — Windows and Linux executables (2026-10-07)

- Native Linux x86-64 GUI/CLI, with a bundled Windows conversion worker using
  the selected Wine/Faugus prefix. Linux paths translate to the same build
  workspace, and Install uses that prefix's Plutonium storage.
- Python build pipeline replaces the desktop PowerShell driver on both
  platforms, fixing Wine's silent no-op PowerShell build failure.
- Required native outputs are checked before packaging; failed cache refreshes
  invalidate completion markers. Linux builds lock their workspace and cancel
  their own process group.
- Updated portable Windows package, native Linux package, checksums, and
  automated Linux compilation/startup checks. Full Linux map conversion and
  gameplay remain unverified.

## v0.2.0 — Desktop mod tools preview (2026-10-07)

- Map Details & Artwork tab with title/description editing and Blit, Large,
  and Blur uploads, required-resolution guidance, alpha/size validation,
  and live map-selection, lobby-thumbnail, and loading-screen previews.
- Uploaded artwork exports to native T6 textures/materials; metadata appears
  in frontend/gameplay menus and the packaged mod. Large supplies loading
  art and the lobby thumbnail. Preview framing still needs an in-game check.
- Artwork update: 33 focused tests and native frontend link/roundtrip
  registration verification pass.
- Native Windows launcher with Mod Builder, Setup, Reports, a build checklist,
  live console, saved settings, Steam path discovery, and prerequisite checks.
- Build, Install to Plutonium, Launch Map, and cancellation controls.
- Automatic working folders and cache refresh when source/game archives change.
- Portable GUI/CLI executables bundling Python, native extractors/bridge,
  audio decoder, schemas, compatibility data, licenses, and native sources.
- Builds stay separate from installed maps until Install is clicked.
- Portable resource lookup and a relocatable PowerShell pipeline; bundled
  large-address-aware linkers do not need Visual Studio on the user's machine.
- 267 tests pass; packaged GUI/resource checks, real native map extraction,
  relocation to a folder with spaces, and a 67,022-argument material audit pass.
  A full conversion through the GUI has not yet been playtested.

## v0.1.0 — Development preview

First tagged source release of the World at War to Black Ops II converter.

### Included

- Offline T4 extraction and T6 bridge staging/linking for render worlds,
  collision, moving brush models, materials, images, and static models.
- Script/API translation, source asset dependency recovery, localization,
  weapons/animations, effects, sound conversion, and Plutonium packaging.
- Projectile collision recovery from qualified physical props and additional
  exported model collision metadata.
- Stationary FX carrier translation to reduce network entity load. Live test
  map captures fell from snapshots exceeding the client's 512-entity limit to
  383–390 entities; all 14 observed grenade throws reached the client. The user
  subsequently confirmed grenade visibility works.
- Native BO2 primary frags, offhand adapters, bounce sound dependencies, and
  tactical inventory slot translation.
- Optional authored titles, descriptions, icons, artwork, and versioned menu
  image packs in gameplay and frontend zones.
- Setup, native build, conversion, launch, and troubleshooting instructions.

### Validation and limitations

- 250 Python regression tests pass.
- Prior native build, asset roundtrip, installed hash, and runtime evidence is
  recorded in `AGENT_HANDOFF.md`. Release publication does not constitute a new
  complete map build or playtest.
- Minor physical collision gaps remain in the test map and were deferred.
- Final custom frontend artwork still needs a fresh-session alignment check.
  Authored pictures and extracted game data are not bundled.
- Arbitrary-map support and full lighting, sound, and gameplay parity remain
  under development. Review conversion warnings/errors and compare with WaW.
- Source distribution only: native binaries, external dependency checkouts,
  game content, and proprietary Mod Tools are not included.
