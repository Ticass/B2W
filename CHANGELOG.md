# Changelog

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
