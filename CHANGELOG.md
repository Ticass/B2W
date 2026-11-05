# Changelog

## Unreleased

- Fix Yote's Lair hanging at "Waiting for other players". It starts a zipline script with no zipline in the map: WaW ends that thread on the first undefined value, while T6 carries on and `i < undefined` never ends the loop. Loops over a `.size` now stop when the array is undefined.
- Fix Project X hanging at "Waiting for other players" with `potential infinite loop in script`, and failing with `G_Spawn: no free entities`. A missing sound's notify now waits a frame, so a map replaying its ambient sound on that notify cannot spin; looping effects play on their tag and no longer cost an entity each; zone volumes without a `script_noteworthy` no longer get a second entity.
- Fix zombies sinking into the floor at Project X windows while tearing boards and after climbing in. BO2 plays its tear and window-entry animations from the barrier's origin, with the zombie's feet a constant 30.9 units below it (stock animations); Project X's WaW window goals sit only 8-16 above the floor. Converted barriers now stand that height above the WaW collision floor under them. Windows whose boards are unnumbered or freely named (Project X: `window_south_1`) are now also aligned with their boards; before, all 23 stood ~17 units outside them.

## v0.2.15

- Add **Resume: keep my edits to the staged files** to Build Map (off by default). Every build re-stages the map, which translates the WaW scripts again into `stage/zone_raw/<map>/`, so a hand fix to a staged file that failed script compilation or linking was overwritten on the next build. **Refresh source files** only re-extracts the WaW fastfiles. With Resume on, a build whose map sources, options and converter version match the last finished staging skips staging, lists the edited files, and reruns linking, script compilation and the audits. See [Desktop app](docs/DESKTOP.md).
- Give menu artwork its own launcher section, **Map Details & Artwork**, with in-game previews drawn in BO2's menu space: a rotatable globe (click to place the map), map selection and focused selection (click to place the start location), the lobby preview frame and the loading screen. Stock textures are read from the user's BO2 install; nothing from the game ships.
- Fill the whole Custom Games lobby preview frame for converted maps. Stock ZM leaves a strip under the picture to the frame's dark gradient, so a converted map's picture looked cut off by a black bar. Stock maps keep the stock layout.
- Carry WaW's Juggernog health and regeneration into the port: 160 health, regaining 40 every 0.5 s (3 during dog rounds). The port never started WaW's loop, so three zombie hits downed a Juggernog player.

- Fix Hotel of Hell and other maps failing to build with `raw animation has no valid notification suffix` when a weapon sound-map key occurs inside an existing `sndnt#` event. Sound translation now matches complete event names after validating the notification section, preserving existing native events.
- Fit converted sound voice counts to the BO2 linker's maximum of 128, reporting every changed count in `sounds.bank.json`. WaW aliases with counts of 256 previously stopped Hotel of Hell at soundbank linking.

- Fix Project X (and other large maps) failing to boot with `G_Spawn: no free entities`. T6 zombies leaves only 912 of its 1022 entities to the map, about 100 fewer than WaW, and each window kept its ~7 WaW plank and clip brushes beside the BO2 zbarrier that replaces them until BO2 deleted them after startup. Those brushes are now left out of the map (Project X: 156 brushes, 821 -> 666 map entities). Project X now boots into the map.

- Fix converted map names in the BO2 lobby and loading screen. The map table carried the title itself, and BO2 appends `_CAPS` and `_LOC` to that column to build its own keys, so maps showed as "Title_CAPS" / "Title_LOC" (unbranded ones as "zm_map_waw_CAPS"). The column is now a key and every map ships the strings BO2 asks for. The line under the map name read "TRANZIT" (BO2's fallback when a map has no mode name); Map Details now has **Mode name** (default "Zombies") and **Location** (default "World at War").

- Reject a WaW extractor that is not the converter's patched build. A reported 9/11 Airport build had "WaW extractor folder" set to the app folder, where a different `Unlinker.exe` extracted the map "successfully" without the world dump, so staging stopped on a missing `<map>.d3dbsp.gfx.bin`. Setup now marks such an extractor as not ready, Extract All and builds stop with instructions to restore the bundled one, and a map dump missing a world its zone lists is an extraction error.

- Fix the Setup tab hiding "BO2 bridge tools folder" and "Audio decoder" behind Shared Game Assets when Advanced Paths is open. The page now scrolls when the window is too small.

- Restore the crosshair on converted maps. The full-screen WaW vision overlay redrew the frame resolved before the 2D pass, so it painted over the crosshair, which the engine draws just before the first HUD element. The overlay now blends with the frame, keeping the WaW grade unchanged on scene pixels while earlier 2D shows through. Images that template weapons pull into `mod.ff` are now also written to the mod ipak rather than referencing data no ipak holds.

- Keep material textures that translated programs add to a pass inside the pass's leading material-texture argument group. A texture placed after the code textures was never bound by T6, so the slot kept the previous draw's texture. This caused the Wunderwaffe's orange/black first-person bands through its detail normal map; they were not shadows.

- Restore the source Tesla chain through BO2's zombie damage callback, cap standard Wunderwaffe chains at ten targets, and suppress the unintended shell impact FX while retaining the source electric projectile trail.

- Extend stock WaW DLC2 conversion with source active-spawner membership, conditional movement tables and nearby-team respawns. Preserve map physics constraints and ropes, place native barricades at window-entry anchors, bind source water depth/refraction inputs, and correct translated shadow depth ordering. See [Shi No Numa validation and remaining limits](docs/SHI_NO_NUMA_DLC2_PORT.md).

- Compare map framework scripts with every stock-zone version retained in the asset cache, rather than only the merged stock definition. This fixes Baseball Field's newer stock damage body being rejected as a custom damage prelude and avoids duplicate stock damage processing.

- Normalize BO2 Mod Tools skybox bone-bound vector objects to the bridge's array schema, preserving their values and metadata. This fixes Collingwood's `boneInfo/0/mins` parsing failure during map linking.

- Recover only the requested WaW stock dependency asset types, retaining and expanding previously extracted types. Animation lookups no longer trigger unrelated campaign model/image/audio exports, and a material-only cache no longer claims missing asset kinds. See [Collingwood validation and limits](docs/COLLINGWOOD_DEPENDENCY_EXTRACTION.md).

- Bind zombies to the round controller's selected active location before deferred native spawn callbacks, and redirect its cached stock round provider to the owned controller. Preserve custom providers and authored spawn delays.
- Retain stock-style texture format/mip headers for streaming world and character images, and precache mapped effects before source scripts yield.
- Translate additive world light-ray techniques in both unlit and emissive passes, including their normal stream and original view-angle falloff. This addresses Empty Walls' one-sided opaque black rays. See [validation and limits](docs/EMPTY_WALLS_RUNTIME_AND_RAYS.md).

- Make the WaW source zombie speed authoritative over BO2 movement requests. Keep source actor/round speed fields separate, translate the selector's initialization and round inputs, and restore source speed through the native movement callback. Discover renamed movement-table initializers and reference authored clips in both actor scripts so T6 retains their animation-tree nodes.
- Restrict BO2 template weapon assets, automatic stats-table registrations and client weapon lists to the map's dependencies and native runtime equipment. Keep converted WaW weapons and stats row indices intact. This prevents Dead Sand Elite's load-time weapon registry overflow.
- Isolate converted WaW shared ammo cap groups from BO2 and distinguish groups with different authored limits. Weapons with matching source groups and limits still share their cap. This resolves Dead Sand Elite's bouncing-betty launch conflict and the equivalent mortar conflict without changing weapon capacities.
- Remove stale server and client animtree initializer hooks when rebuilding a map that no longer needs script-model tree registration. Validate generated map initializer targets before compilation, catching missing exports that otherwise surface only at launch. This fixes Dead Sand Elite's unresolved `init` external.
- Bind the source zombie movement animation table to owned server/client animation states and trees, using converted WaW clips for their authored movement pace. McDonald's sprint table uses `ai_zombie_sprint_v4`; preserving only its `sprint` label still selected slower BO2 clips. Translate the source round's spawner selection statements too, including its one-time repeat reroll and per-round history, rather than adding a distance bias or moving source spawners.
- Translate the source WaW zombie `set_run_speed()` function into a shared runtime adapter and invoke it before native zombie thinking starts. McDonald's authored round-one sprinters now keep that speed choice on BO2's Original runtime. Plain active spawners also retain their own spawn location and target metadata instead of being relocated independently to a random zone location; risers retain their zone placement. The contracts live in the shared API definitions, with no map-specific presets.
- Recognize renamed WaW zombie startup frameworks by their function and initialization contract, then apply the existing framework override translation. Preserve threaded startup hooks and terminate the generated startup comment for inline map scripts. This removes Dead Sand Elite's missing `_zombiemode::main()` build error without map-specific mappings.
- Resolve map-packaged IWD skins before recovered stock-zone textures for world, actor and weapon materials, while keeping the map's own compiled pixels authoritative. This restores Cheese Cube Unlimited's cheese Pack-a-Punch and zombie body skins without naming the map or its images in conversion logic. Report the source archive used.
- Preserve authored zombie bone hitboxes through T4 export and T6 import, matching bone names and translating hit-location names. This fixes the conversion loss behind McDonald's headless bodies having zero head/helmet bounds while their attached heads disable collision. Automatically audit animated-model hitboxes after native linking.
- Discover original script animation dependencies through the translator's recursive call graph, preserve each function's active animation tree, and stage namespaced WaW clips/trees. DLC3 animation initialization and factory animation functions now compile with their original references; missing clips remain reported.
- Translate the source DLC3 template recursively instead of limiting it to the power switch: electric traps, teleporter and Pack-a-Punch door logic, intro HUD, bridge logic, and createFX setup now follow the source map's implementation. Preserve DLC3 `spawnFX()` as the createFX bootstrap, fix method resolution after removing caller parentheses, discover extracted model/HUD dependencies, and keep BO2 callback/zone initialization owned by its framework. Missing source animation clips and the `settransported` client effect remain reported. See [DLC3 template compatibility](docs/DLC3_TEMPLATE_COMPAT.md) for validation and limits.
- Stop zombies standing still at spawn on maps without barricades or with chaser spawners. BO2's `zombie_think` takes the "hunt the player" choice from the spawn struct's `script_string` and otherwise walks to a barricade (`exterior_goal`); with none to match, it reads an undefined node and the zombie never moves (`OP_Plus ... zombie_goto_entrance -> start goto entrance` and `undefined`). WaW spawners with `script_string "zombie_chaser"` now give their spawn struct `find_flesh`, and a map with no `exterior_goal` at all (WaW's stock logic has no window either; such maps replace it) sends every spawn to `find_flesh`. From a community fix for another map.
- Stop gore and other glossy WaW model materials from looking like chrome. The placeholder reflection probe was near-black in BO2's linear units (6/255), but translated WaW programs read it back in gamma, where it is a 0.15 grey, and WaW's `envMapParms` fresnel multiplies the reflection by up to 12. Zombie organs (Cheese Cube's `mtl_char_jap_impinf_gore_*`) turned silver. The placeholder probe is now black.
- Keep zombies at their close-range models when several are on screen. The T6 linker sized the map's skinned-vertex cache (`skinnedverts` asset, `maxSkinnedVerts`) from the number of world vertices, so a simple map such as Cheese Cube Unlimited (28,841) or Bank Job (24,928) got a fraction of the stock 163,840. Once a few zombies filled 90% of it, BO2's adaptive skin LOD scaled every skinned model's LOD distance up, and zombies next to the player drew their lowest LOD. Converted maps now use the stock size. The unlinker also dumps `skinnedverts` now.
- Fix perk-machine and other effect lights glowing far too bright on every map. BO2 adds an effect omni light with linear colour sRGB->linear(rgb) x alpha x 32, a factor authored for stock exposure (HDR scale about 1/64). Converted maps run at HDR scale 1 so WaW lighting stays 1:1, which made every WaW effect light about 32 times too strong. The light's alpha (its intensity) now converts by 1/32.
- Translate WaW `setSavedDvar` to BO2's `setdvar` (through `waw_setsaveddvar` in the compat script). It was stubbed, so a map's dvar settings (player speed, gravity, melee range, last-stand bleed-out time...) never applied. A community member fixed another map by making this change by hand.
- Read the BO2 builtin tables stored name-first (`{name; id; min; max; fn; 0}`), such as the server mover methods (`moveto`, `rotateto`, `movez`...). Their argument counts were unknown, so the port could not see a wrong call and BO2 refused to load the map ("Unresolved external: moveto with 1 parameters").
- Preserve continuation after recoverable WaW builtin argument errors. Native VM inspection shows that these errors clean the argument stack, produce undefined, and resume the script; the previous halting shim incorrectly prevented Cheese Cube Unlimited's later timed water rise. Invalid calls remain reported, and the source movement and damage logic stays intact.
- Keep scripted animation items out of BO2's melee selection when their WaW melee animation is merely an idle placeholder. This avoids classifying perk bottles and the knuckle-crack assembly item as held melee weapons; actual held-item attacks and explicit melee settings remain preserved.
- Preserve WaW's exclusive `SetVisibleToPlayer` behavior for buildable placement triggers and other entities. BO2's method adds a visible player to the existing mask; the compatibility wrapper replaces that mask, as WaW does.
- Leave plain script strings alone unless they look like a localization key. Stock WaW `menu.str` defines a short reference `30`, so a dvar value `"30"` became `"WAW2BO2_30"`. `&"..."` strings are unchanged.
- Fix a crash while loading a map whose image pak (`.ipak`) has a data block ending exactly on a 32 KB chunk boundary. BO2 reads each image in fixed 256 KB windows counted from the image's first chunk. The T6 linker restarted its window at that boundary, so the next block ran past the game's window and the game read packed image bytes as a block header, writing far outside the image buffer (`0xC0000005` right after "Built adjacency info for IPaks"). Cheese Cube Unlimited's `jarvis_wm_screen7` hit it; any map could, about once per 256 blocks. `tools/ipak_check.py <file.ipak>` walks every entry the way the game does and reports overruns.
- Link WaW models whose LOD has more than 65,535 vertices across its surfaces. The T6 linker refused the whole LOD ("Lod exceeds limit of 65535 vertices"), so the map failed to link. Only each surface's own vertex count has that limit: every surface has its own vertex buffer, and WaW's own `baseVertIndex` wraps the same way. Cheese Cube Unlimited's `iron_man_world_prototype` (117,250 vertices in 32 surfaces) no longer stops the build.
- Fix zombie hitboxes, which made every gun feel weak and headshots hard to land. The T6 linker computed each bone's hit box in model space, while the game places it on the animated bone (stock BO2 `j_head`: about -1 to 8 units from the bone). On a converted zombie the head box sat far from the head, so shots meant for the head hit the body or missed. Bounds are now in bone space, from the vertices each bone mainly drives. This came with the switch to each map's own zombie models (v0.2.13). The ray gun fix did not cause it: that change only touched explosives and projectiles (ray gun, SMAW, C4, grenades, monkeys), never bullet guns.
- Keep WaW's zombie speed whatever the lobby difficulty. BO2 on Easy (`zmDifficulty` 0, also what a lobby without the setting reads) speeds zombies up by round × 2 and never lets them sprint, so round 15+ zombies only walked or jogged. WaW has no difficulty: round × 8, sprinting from about round 10.
- Make WaW Mule Kick work with wall buys. A WaW Mule Kick gives its own perk (`specialty_extraammo` in community packs) and raises `self.InventorySize`, which only the map's box read. BO2's wall buys and its too-many-weapons check count only BO2's Mule Kick, so a third wall buy replaced a gun. The weapon limit now honors `InventorySize` too.
- The opt-in "Use BO2 stock perks" now replaces the perk machines too (off by default; WaW perks stay the default): each machine uses BO2's model (Nuketown machines; Mob of the Dead for Deadshot, PhD Flopper and Electric Cherry), and the map no longer carries a converted WaW model of the same name. The map's WaW Pack-a-Punch stays as it was: BO2's own Pack-a-Punch controller no longer starts on the same machine. Perk packs keep their power relays (e.g. Stamin-Up and Electric Cherry turning on with the power), with the WaW notify translated to BO2's (`cherry_on` -> `electric_cherry_on`). The perk gameplay is BO2's code, not the ported WaW perks: a map's own damage rule gated on a perk (Nuketown's WaW PhD Flopper in `_callbackglobal`) is dropped so only BO2's PhD runs, while its other rules (the shield) stay. BO2's Mule Kick sets the WaW `InventorySize` the map's own box reads, so the box gives a third weapon with it. Scripts that briefly take an owned perk away and give it back (Nuketown's d2p and galvaknuckles toggle Speed Cola) stay an engine toggle and do not re-buy it, and WaW-only marker perks such as the bowie's are no longer given to the engine.
- Skip weapons that a map's scripts name but that neither the map nor BO2 provides, instead of crashing with `unknown weapon` (e.g. a mod script that registers `zombie_slipgun` without shipping it). Before, any name missing from the converted weapon table was treated as BO2's own and passed to the engine. `build-mod` now records every weapon the linked `mod.ff` loads, and the compat lookup passes through only those names. Any other name is reported once as `WAW2BO2 UNSUPPORTED WEAPON <name>` and the call is skipped. Weapon queries the port did not route through compat before (`WeaponMaxAmmo`, `WeaponAltWeaponName`, `GiveStartAmmo`, ...) now go through the same lookup.
- Port a map's own rules at the top of WaW's `_callbackglobal` `Callback_PlayerDamage` as a BO2 player-damage callback, as already done for `_zombiemode`'s `player_damage_override`. Nuketown's PhD Flopper cancels explosive, projectile, fire and fall damage there. The check never ran, so the perk did nothing.
- Extracted damage rules that leave a hit unchanged now return -1 (no opinion). BO2 stops at the first callback that returns anything else, so one map rule (e.g. Nuketown's shield) hid every later one.
- Keep the player's own pistol after a solo revive. BO2's solo last stand swaps in an upgraded pistol, judged against BO2's pistol list, and takes `level.laststandpistol` back on revive. WaW maps set that to their start pistol, so a revive took the player's 1911. WaW hands back exactly the weapons held when downed, so the swap is now off for maps with a WaW loadout.
- Show the authored map title on the loading screen. The loading screen reads the map name from `zm/mapstable.csv` before the mod's localized strings are loaded, so the localized key appeared instead of the name. That column now holds the title itself; the lobby keeps its localized strings.
- Convert the effects loaded by WaW framework and template functions the port extracts, not only those in whole ported scripts. The DLC3 template's `dlc3_fx` loads the mystery box's sky marker beam and its landing flare. Neither was converted, so the box had no light showing where it was.
- Give WaW projectile and grenade weapons full damage. WaW does not scale their damage by hit location, so its weapon files leave every location multiplier at 0. BO2 does scale it, so a direct hit and its explosion dealt nothing. Only BO2's small script bonus for projectile hits landed. When every multiplier is 0, they now convert to a neutral 1. This fixes Nuketown's ray gun doing little to no damage, and likely the SMAW, C4, claymores and monkey bombs too.
- Fling zombies that WaW scripts ragdoll while still alive. WaW scripts call `StartRagdoll`/`LaunchRagdoll` on a living zombie and then kill it, but BO2 only ragdolls dead actors. The launch was lost and the zombie played its normal death animation. The ragdoll and launch now wait until the end of the frame in which the zombie dies (a ragdoll started inside the death notify is ignored). The zombie also stops blocking players as soon as it is ragdolled. This fixes Nuketown's baby gun, where shrunken zombies did not go flying when a player ran into them.

- Read the script linker log correctly when its expected "Could not open BSP" error is printed in the middle of a "Compiled GSC script" line (Linux/Wine). The build no longer reports `clientscripts/mp/waw/_waw2bo2_zm.csc` as missing when it compiled.
- Start indented `#include` directives at the beginning of their line, as the BO2 compiler requires. Project X's IWD `_zombiemode_perks.gsc` no longer fails with "expected identifier, got '*INTERNAL*'". Scripts read from IWDs now use the same newlines as zone rawfiles, so compiler line numbers match the source.
- Convert only the sound aliases a map can play: weapon and FX dependencies, aliases named by server/client scripts, script-read string tables and map entities, and names scripts build at runtime, plus their chains. Previously every alias in the map's fastfiles was converted (3,773 on Bank Job, of which 1,720 remain), so dead sounds filled the loaded-bank budget and pushed used sounds to streaming. Omitted aliases are listed in `content_source/sounds.unreferenced.json`.
- Place the map on the Custom Games globe with the launcher's Longitude and Latitude fields, and upload a loading song (MP3, WAV, FLAC, OGG or M4A) that is packaged with the map and plays while it loads. Map branding is now written for every build, so the title and description appear even without uploaded artwork.

## v0.2.14

- Recreate the generated owned client bootstrap before script compilation, fixing Linux/Wine builds that retained a partial staging tree and then failed on `clientscripts/mp/waw/_waw2bo2_zm.csc`.

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
