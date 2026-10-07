# Changelog

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
