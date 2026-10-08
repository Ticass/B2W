# Changelog

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
