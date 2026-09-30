# WaW → BO2 Custom Map Converter Handoff

## Session 21 (2026-09-30): static-model collision made native (512-entry gather cap)

- USER: grenades pass through houses, rocks, school bus, doors; only floors, the red/white truck and wall-buy signs collide; players sometimes clip into an object, get pushed out, then collide normally.
- ROOT CAUSE (IDA, mapped Plutonium image `work/T6_mapped_vision.exe`, headless session): T6 movement/missile/push-out collision gathers nearby brushes and terrain aabbs into lists capped at 512 (add routines sub_6A5CE0, sub_5C7760; gatherers sub_648AC0, swept sub_6F6C70 -> sub_4EF680 -> sub_4DADE0) and silently drops the rest. The swept trace against the gathered list is sub_885460 (called from sub_6D8770). The uncapped position test (sub_883CF0) still saw every brush, which explains the clip-in-then-push-out. Session 17-20 converted every static-model collision triangle to a prism brush: 20,439 prisms. In a 96-unit half-box the median was 594 brushes, the 90th percentile 6,651 prisms, and the max 8,400. Genuine WaW world brushes alone: 90th percentile 124, max 314 at the same size.
- FIX: static models now use T6's native `clipMap_t.staticModelList` (84-byte cStaticModel_s; linked into world sectors at load by sub_886940, line-traced against the xmodel's collSurfs via sub_886E50/886BF0 -> sub_6DB8A0/69ACC0 -> sub_40DFD0). `hulls.static_model_records` writes `BSP/staticmodels.json` (name, contents, origin, invScaledAxis verbatim, since both engines compute local = transpose(A)(p-o), plus absmin/absmax). ClipMapLinker::LoadXModelCollision loads it and links the XModel dependencies; the prism code is removed. Collision xmodels are staged into the map zone. As in WaW, static models stop bullets/grenades; players are blocked by the mapper's clip brushes. The prisms had made models solid for players, which WaW never did.
- Result: 16,218 world brushes (was 36,657), 52 collision static models (13 distinct: house/deck clumps, lockers, garage door, town sign, trucks, trees), 20,439 collision tris, 0 transform mismatches. Unlinker clipmap validator now prints static models: installed FF `static models 52 with collSurfs 52 solid 52`, `brush tree position contract violations 0 references 16218`. Installed map FF sha256 prefix c1540643b8a297ee2ebd matches the build. Log `work/run_bridge_staticmodel_collision.log`, roundtrip `work/staticmodel_roundtrip/`. 105 tests pass. T6 Linker + Unlinker Release Win32 rebuilt.
- NOT VERIFIED IN GAME. Residual: world brushes alone can exceed 512 only for gather boxes larger than about 128 units half-size (swept player queries are much smaller). Brush-model doors trace their own cmodel tree directly (sub_884710, no cap). If a buyable door still passes grenades after this fix, investigate that path.
- Shader state unchanged this session. Blockers from the last report: TEXCOORD1/4 pixel inputs on lit slots 4-6 (2,513 pass refs), technique metadata absent (638), native VS cannot supply TEXCOORD0/1 (309), TEXCOORD0/4 (266). Next shader step: bind the translated WaW VS+PS pair for lit passes so their varyings are WaW's own. That still needs normal/tangent unpacking of T6 vertex streams and lightmap/code-texture routing.

### Session 21 continued: paired lit shaders + WaW lightmaps (installed, NOT visually verified)

- USER: "Go for Shaders already", then "After Shaders: Lightmaps please".
- PAIRED TRANSLATION: `bind_material` first binds the original WaW VS+PS pair (varyings stay WaW's; PS input struct mirrors the VS output signature, `check_linkage` verifies compiled DXBC registers), else the old PS-only path. New measured adapters, all documented in `docs/SHADER_TRANSLATION.md` (table): `waw_ubyte4_vector` normal/tangent, `waw_uv_lightmap` (world float4 uv+lmap = T6 TEXCOORD0+1), `baseLightingCoords`->`gridLightingCoordsAndVis` (+ .w sun visibility carried on the same varying), modelLighting `sqrt(8 hdr)` + alpha = grid visibility, fogColor/sunDiffuse/sunSpecular(->sunDiffuse) gamma conversion, shadowmapSamplerSun texel `Load` (T6 binds a comparison sampler), reflection probe rgb/a -> rgb*a, destructibleParms neutral 0, donor-missing material constants embedded (WaW hash = djb2-xor nocase seed 0, verified). Code textures bind by reflected NAME (enums differ; lightmap/probe are bound outside pass args in both engines). Fog output found by component-exact taint from fogConsts (`tainted_outputs`; dp3 reads xyz only). Technique sets are content-named (per-material programs). Aliased samplers on one T6 slot share one declaration.
- Map zone materials were never dumped (step 0 excludes them): run_bridge now dumps `waw_zone_dumps/map_materials` (material only, v8 marker) as an extra root -> world `wc_*`/layered techsets + programs available.
- RESULT: 3,878 bound pixel passes (was 1,277 in the pre-session report), 3,166 vertex, 141 unique programs; lit slots 4/5/6 bound on 485/443/486 materials; 131 world materials fully on WaW lightmapped programs. Remaining: layered blend world materials (TEXCOORD3 absent from T6 world vertices, ~3.2k refs), spot/omni slots (7/8/13/14: lightFalloffPlacement/lightSpecular/attenuation adapters), depth prepass slot 1 (depthFromClip), unreachable for world anyway.
- LIGHTMAPS (`lightmaps.py`): WaW page = secondary W x 2W RGBA8 (colour+dir halves) + primary 2W x 2W L8 (sun visibility); T6 page = W x 3W RGBA8 three thirds. Each page written twice: WaW-encoded stack [top, bottom, primary box-filtered 2x2] read by translated WaW programs via exact V remap (`WAW_PAGE_UV`); T6-encoded (WaW flat-normal lighting squared into rgb/a ambient, no directional term, visibility in third alpha, hdr assumed 1: APPROXIMATION, reported) for donor surfaces. `BSP/lightmaps.json` -> GfxWorldLinker: surface lightmapIndex = page (WaW materials) or n + page. A material keeps WaW-lightmap passes only if all reachable lit slots (4,5,6; bridge emits only the sun primary light) are translated, else they revert. World FBX: meshes split by page, lightmap UV = UV set 2, page in `_lm<N>` name; BSPCreator/BSP.h read them; lmapCoord packed. WaW index 31 = no lightmap (95 surfaces).
- VERIFIED in installed FF (sha prefix 1246f1379bba59b51b52 == build): 10 lightmap pages inline 1024x3072 RGBA8 (126 MB total: watch 32-bit memory), surfaces per page WaW 0-4: 2,237 / T6 5-9: 1,460, 438,835/439,550 vertices with lightmap UV; wc/2228471478 slots 4/5/6 on waw/runtime programs binding t13 lightmap + t15 probe, VS reads TEXCOORD1; collision unchanged (0 violations, 52 static models). 113 tests (new tests/test_lit_adapters.py). Logs `work/run_bridge_lit_lightmaps.log`, `work/lit_roundtrip*`. Unlinker gfxworld dump now prints surfaces-per-lightmap and lightmap-UV counts.

- FIRST LAUNCH of the Session 20/21 builds: fatal "Could not load default asset 'default' for asset type 'impactfx'. Tried to load asset 'ImpactFx'". Cause: template mod.zone line `fximpacttable,,ImpactFx`; the mod-tools linker (singleton accessor) writes no entry for it (baseline mod.ff has none), but the Session 20 bridge relink (real name field) emitted a reference named "ImpactFx", while stock common_zm's only impact table has an EMPTY name. Fix: modzone.activate_mod_shaders strips `fximpacttable` lines from the relink zone. Relinked mod.ff verified: 0 fximpacttable entries. Repackaged via build-mod + package (no full run needed).

- PLAYTEST (user): collision "somewhat fixed" but grenades/bullets still pass the yellow bus, furniture, houses until a door is bought; visuals "extremely weird" (noisy "TV static" on house/models, magenta patches, black floors under grates, washed look), WaW blue tint visible, sky sunny, no WaW weather.
- COLLISION ROOT CAUSE #2: in WaW only 13 of 391 static model types carry collSurfs; the bus, house clumps, rocks, fences, furniture collide through triangles the WaW compiler baked into the clipmap (78,048 tris) -> T6 terrain partitions. ClipMapLinker made ONE triangle per partition and the terrain gather is capped at 512 partitions per query too (sub_6B9730 -> sub_5C7760): 96-unit query p90 1,157 / max 3,003. Stock measured (new dumper stats): zm_nuked/zm_transit partitions 1..16 tris (mean ~3.6). Box/point traces test each triangle of a partition (sub_4F6DB0 -> sub_881940/sub_5D1230; no partition-level box), so the old "multi-tri partitions = invisible walls" comment was wrong. FIX: triangles Morton-sorted per surface (32-unit cells), partitions up to 16 tris / 256 extent -> 7,868 partitions. Static models and entities share one sector tree (sub_886430 clear, sub_886940 link statics, sub_7040C0 link entities); the door-buy effect is consistent with gather overflow order changing. NOT yet retested in game.
- VISUAL DIAGNOSIS: lightmap data itself verified sane (work/lightmap_preview/*.png: real charts + clean sun shadows). Normal maps are passed unchanged (WaW .wy decode correct). WaW packed-UV adapter re-derived and matches. Cause of the noise still UNKNOWN -> A/B build: `WAW2BO2_RUNTIME_SHADERS=none` keeps every donor pass. Installed: mods/zm_nuketown_waw (WaW programs) and mods/zm_nuketown_waw_donor (donor programs, T6-encoded lightmaps), identical collision.
- Created private GitHub repo https://github.com/Ticass/B2W (converter, tests, tools, docs, T6 OAT fork source, T4 OAT patch vs 7d027e8; no game data/binaries). vendor/OpenAssetToolsT6/.git renamed .git.broken-worktree (pointed at deleted C:/NuketownPort).

- A/B RESULT (user): black patches/odd shadows in BOTH variants (so not the translated programs; lightmaps/geometry are shared); "nothing like WaW". Compact partitions stopped grenades at house walls but added random invisible walls, walk-over spots, floating zombies.
- COLLISION ROOT CAUSE #3: every WaW collision triangle was linked as SOLID (one clip material). WaW clip dump v4 (T4 WorldConverterDumperT4: per-triangle material from leaf aabb nodes) shows only 39,685/78,048 are solid (0x1); 37,340 are missile+shot clip 0x2080 (bullets/grenades only: the house/bus/furniture models), plus player/monster clip 0x30200/0x31640, glass 0x10 etc.; 13 have no material (dropped). FIX: collision FBX has one material per WaW clip material (`clip_<n>`), `BSP/clipmaterials.json` carries WaW content/surface flags; ClipMapLinker loads them (183 materials), gives each partition its surface's material, builds material-homogeneous aabb parents (engine checks parent material first, sub_6B9730) and sets leaf terrainContents = OR of its partitions. Verified built FF: 8,425 partitions, 0 brush violations, 52 static models. Waiting on game close to package (watcher).
- User note: shader struct pages (codresearch Pixel/Vertex Shader Asset) describe console containers (CgBinaryProgram = PS3, BO2 data1/data2 flags = console). PC: WaW D3D9 SM3, BO2 D3D11 DXBC (measured Session 19). BO1 PC is also SM3, so "BO1 shaders in BO2" is the same SM3->SM5 + engine-binding problem our translator handles.

### Next
0. A/B: compare the two installed mods at the same spots; then bisect model (mc_) vs world (wc_) programs if the noise follows the WaW programs.
1. User in-game test: collision (grenades/bullets vs houses, rocks, bus, lockers, garage door, buyable doors; clip-in/push-out gone) AND visuals: lit world/models, lightmap alignment (seams, wrong page, black/overbright surfaces), sun shadows, fog colour. Crash/memory check with 126 MB of lightmaps.
2. If lightmaps look offset: check WaW lightmap UV V orientation (FBX writes 1-v, BSPCreator flips back) and the page stacking order.
3. Remaining adapters: layered world blends (needs extra vertex stream), spot/omni, depth prepass; convert WaW light grid for model lighting.

## Session 20 (2026-09-30): supported shader passes activated and installed

- USER: "Make them active". Implemented `shaderruntime.py`: extracts original pass arguments from new T4 `waw_techniquesets` dumps, maps uniform names against native T6 reflection, material hashes to actual sampler slots, literal arguments to shader constants, and matching engine technique meanings. Supports common position/color/UV interfaces with packed VU half-float reconstruction, position padding and measured native T6 fog visibility. Clones technique sets and writes content-named SM5 programs to live shader_bin; materials reference these sets. Complex normal/tangent/code-texture/multi-pass/multi-target adapters remain unsupported and reported. This is PARTIAL renderer coverage, not activation of all 762 source programs or restored native lighting.
- Fixed `dcl_*_pp` semantic parsing and retained centroid declarations. Shader tests now cover native buffer byte offsets, texture hash/slot mapping, packing adapters, interpolation modifiers and restoration of baseline materials after link failures. 103 tests pass.
- Full stage reports 548 distinct bound materials, 978 pixel-pass references, 917 vertex-pass references, seven distinct compiled program variants. Weapon visuals have their own material report; include these when computing totals and preparing the baseline. Remaining 16 weapon visual errors are genuinely absent source pixels, not missing generated shader files. Weapon technique dependencies now resolve project-generated sets/programs before stock files.
- Gameplay mod requires TWO linkers: raw mod tools support .efx/gameplay formats; bridge supports JSON technique sets. `build-mod --linker ... --techset-dump ...` temporarily restores original stock technique references, builds a fresh baseline with mod-tools Linker, restores staged materials in finally, extracts baseline materials, then relinks its asset pools with a filtered shader overlay. Retains baseline material constants/textures (mod tools mangle image names) and baseline ipak; native relink ipak warnings are expected and that new ipak is NOT installed. Overlay includes project AND content_source shader dependencies. Loaded weapon asset names strip raw mp/sp directory prefixes. Roundtrip checks technique references, identical texture bindings and linked DXBC before replacing mod.ff.
- Corrected T6 PC ImpactFx name accessor and added named reference creator: the measured PC table has a name field, unlike its obsolete singleton accessor. T6 native Linker rebuilt successfully. run_bridge enables LAA before gameplay relink as well as world link. Companion cache now `.dump_v8_shader_bindings`.
- FINAL full run `work/run_bridge_active_shaders_final.log` succeeded with zero missing world assets and installed all eight package files. Their hashes match installation. `work/shader_activation_verification.json` contains hashes, extracted pass counts and actual linked bytecode hash checks. Mod FF: 160 bound materials / 282 pixel / 259 vertex pass references. World FF: 400 / 718 / 680; twelve material names overlap, combined totals match staging. Native world collision validator still has zero position-contract violations, 36,657 references/unique brushes.
- Installed map FF SHA256 `5903280219a3e0770c3daa052b8cb5b160ac0d8ef9e27e5dcd6eeaab69cb947a`; mod FF `b7977938cdb6730d3f48e0429540795aab6cbd0a6f6616b2e7eebde432b83c8f`. Source and deployment verified, but NO in-game visual verification or house/grenade/floor retest this session. Do not claim those user bugs solved. No UI automation used this session. Documentation updated in `docs/SHADER_TRANSLATION.md`.

### Next
1. Actual game test: translated shader appearance, vision/storm, house/projectile/generator collisions and rare floor holes. Previous empty-branch hang correction remains installed but gameplay misses unverified.
2. Extend measured renderer contracts for normal/tangent/model-lighting varyings, code textures, native lightmaps/grid and additional technique slots. Existing unsupported passes retain native donors; do not indiscriminately activate unbound artifacts.
3. Continue prior handoff items for prompts/HUD, muzzle FX ordering, knife/localization and full sound semantics.

## Session 19 (2026-09-30): collision hang correction and native shader translator

- USER: installed Session 18 collision still misses houses; throwing a grenade through a house hangs/crashes. Windows Application event 1001 at 13:04:22 records AppHangB1, not an access violation. No new Plutonium crash dump. Native sub_883660 point-contents traversal does NOT early-out on node.contents, unlike other trace routines. Session 18 created empty children with zero count and zero offsets: infinite self-loop. Expanded serialized validator finds 587 violations in Session 18 FF.
- FIX: BuildLeafBrushNode refuses a partition if either front OR back is empty (retains all brushes in terminal leaf), preserving negative-count crossing subtree only when both branches exist. No empty branches reachable. Validator now checks zero child offsets even when contents is zero. New FF roundtrip: zero violations, 36,657 world brush references/unique. T6 native builds pass. Installed corrected build. HOUSE COLLISION AND RARE FLOOR GAPS STILL REQUIRE ACTUAL RUNTIME VERIFICATION; do not call them fixed based on this validator.
- Temporary staged GSC bullet-trace probe was compiled, but launch attempt exited before a game process/map log appeared, so NO runtime evidence. Original staged compatibility script restored and recompiled before final packaging. No diagnostic probe in final map; no UI automation called after prior Escape. Test launch pid 37744 no longer exists.
- USER explicitly requested shader translator immediately. Existing techsets.py only selects/clones T6 donor materials; no prior instruction translator. New `src/waw2bo2/shaders.py` lowers native SM3 vertex/pixel assembly -> HLSL -> SM5 DXBC via Windows d3dcompiler_47. Preserves masked writes via snapshot, swizzles, constants, saturation, texture dimensions/sample modes, arithmetic, comparisons, derivatives, discard, nested ifs, literal bounded rep. Unsupported opcodes/relative addressing/flow fail explicitly. Partial precision promoted to float32 and reported.
- New CLI `translate-shader input.cso output_stem [--bindings JSON] [--no-compile]`. Explicit target contract maps source inputs/outputs, constants to T6 buffer/index, textures/samplers to T6 slots. Missing explicit bindings, invalid slots, truncated interface channels fail. Unbound translations use original register ABI and report requires_t6_bindings. Documentation: `docs/SHADER_TRANSLATION.md`.
- T4 material dumper now preserves BOTH vertex and pixel shader bytecode (previously pixel only); retampled ObjWriting/Material/MaterialJsonDumper.cpp.template and built T4 Unlinker Release Win32 successfully. run_bridge companion cache stamp upgraded to `.dump_v7_shaders` so extraction refreshes.
- Automatic bridge shader staging writes artifacts under `content_source/shaders`, outside live shader_bin, and explicitly reports CONTENT_INCOMPLETE shaders / requires_t6_pass_bindings. Existing donor shading continues until actual T6 pass vertex declarations, constant/resource routing and render-target contracts are reconstructed/verified. DO NOT install unbound output as live T6 shader or claim automatic live shader conversion complete.
- Native code_post_gfx corpus: 137 actual vertex/pixel shaders compile to DXBC, zero unsupported. Full current map/companions pipeline: 762 shaders translated/compiled, zero unsupported. 99 unit tests pass, including actual SM3 vertex bytecode -> SM5, resource mapping, interface/slot validation, register aliasing, flow typing, projective sampling/discard, fail-closed relative addressing, source precedence and failure reporting.
- Full build `work/run_bridge_shader_translator.log` succeeded, zero missing assets, packaged installed mod; all 8 installed files hashes match build. Final collision roundtrip `work/shader_build_collision_check/...clipmap.txt` zero violations. Shader report `work/final_stage/zone_raw/zm_nuketown_waw/content_source/shaders/stage.json`.
- User provided Vertex Shader Asset wiki screenshot referencing CgBinaryProgram/cachedPart/physicalPart. Treat as reference, not exact PC structs: our measured PC T4 loadDef uses uint32 program + uint16 word count; PC T6 uses char program + uint32 BYTE count, plus runtime vs pointer. T6 shader loader already fills this PC wrapper; don't copy screenshot layouts unverified. Next shader work is T6 technique-pass ABI/routing integration, not wrapping SM3 bytes unchanged.
- IDA mapped Plutonium session re-opened as `6ec7208e` (previous 2ef0a44f worker expired). Read-only native evidence sub_883660/883890/883980/884250/885710/883EA0 etc. No live memory writes.

### Next
1. Runtime house/projectile/floor collision verification; correct outstanding misses using native traces. In-game generator damage and storm visuals still unverified.
2. Rebuild T6 technique-pass contracts for translated shaders (VS declaration, semantics, CB layouts/material argument routing, texture/sampler slots, output targets); bind translated code only after validation. Native compiled lightmaps/grid lighting remain unfinished.
3. Retain earlier Session 17/18 Next items for prompts, muzzle FX ordering, original T4 brush recovery, knife/localization, sound semantics.

## Session 18 (2026-09-30): vision, storm and world brush collision

- Installed rebuilt mod after user authorized installation; package succeeded and all installed file hashes match outputs. Full build log: `work/run_bridge_session18_collision.log` (initial package failed because game held files; subsequent standalone package succeeded). 90 Python tests pass; T6 Linker and Unlinker Release Win32 builds pass; map links with zero missing assets.
- WORLD COLLISION FIX: ClipMapLinker previously partitioned brushes by centre and wrote positive overlap range. Native T6 position traversal ignores range; swept traversal subtracts it. Crossing brushes were incorrectly culled. BuildLeafBrushNode now partitions by whole bounds, retains crossing brushes in native negative-count immediately-following subtree, and writes range 0. Shared subtree visited before front/back branches. Native evidence in mapped Plutonium IDA session `2ef0a44f`: sub_4DADE0, sub_5BFCC0. Grenade mask sub_672CD0 is 0x280E893 (source solid/missile masks already compatible).
- Native roundtrip validator in MapEntsDumperT6 checks serialized brush bounds against position traversal partitions. `work/collision_roundtrip/maps/mp/zm_nuketown_waw.d3dbsp.clipmap.txt`: ZERO violations, 36,657 world brush references/unique brushes. User reports houses/fences/generators ignore bullets and grenades, truck/sign model collision works, occasional floor gaps. Fix installed; gameplay retest STILL REQUIRED, especially rare floor gaps and generator damage.
- Generic WaW film conversion in new visions.py: native 32-cube LUT atlas, original film shader equation, neutral T6 vision fields. GfxWorldLinker binds generated lutMaterial; roundtrip confirmed `waw/vision_lut`. Seven source visions + neutral reset installed. Global/player grade callbacks use native vc_LUT; film grade switches immediately, shared supported vision fields retain native fades. Missing cargoship vision reported; unsupported glow/revive fields reported.
- New shared environment CSC handles sunlight RGB/intensity changes and reset using native r_lightTweakSunColor/r_lightTweakSunLight. No nonexistent enable dvar. Removes donor fog/vision loop that overwrote imported vision. Server compatibility wrappers route original scripts through retained client systems.
- Generic sounds.script_aliases scans tokenized script literals against real WaW alias definitions, including aliases used in variables/arrays. Staged 91 script dependencies, including thunder_farL/thunder_closeL; imported stock weather thunder now packaged. Original absent rain/exploder assets remain absent and reported, not invented. Lighting remains flat; compiled lightmap/grid runtime integration unfinished.
- New tools/read_process_image.py and tools/disassemble_shader.py support native investigation. T4 material shader dumper retains pixel shader bytecode. Original shared T6 IDA session c445cce4 must not be terminated; own WaW session 14b51c6c and mapped Plutonium session 2ef0a44f used for evidence. No live game changes via memory writes.
- Computer Use was stopped by user Escape; no subsequent UI calls. Test game pid 46584 had exited by installation. Do not claim visual storm/vision verification; runtime scripts previously loaded without new script errors.

### Next
1. User gameplay retest installed collision: houses, fences, generators, rocks, floor gaps; storm flash/thunder and global/player visions.
2. Remaining Session 17 Next items: original T4 unlisted brush recovery root cause, prompts/HUD, muzzle FX ordering, knife/localization, full native lighting and sound semantics.

## Session 17 (2026-09-29): viewmodels, player models and custom weapons wired to runtime

- NEW USER RULE (GUIDELINES 2a, memory updated): WaW always first; ONLY when the whole WaW lookup yields
  nothing, use a BO2 equivalent instead of a hole, always reported `BO2_FALLBACK`. Never instead of a WaW asset
  that exists or is merely not-yet-convertible. `bo2equiv.py`: same-name asset in BO2 `raw/` (12,104 xmodels),
  then `compat/bo2_equivalents.json` (global role table, currently the lukkie perk-bottle pack); refuses
  candidates already in an always-loaded stock zone (override = ERR_DROP).
- Script source precedence FIXED (gscport.Sources): with fs_game, WaW reads a mod IWD script before the zone
  rawfile (IW3 Scr_ReadFile). Evidence: mod.ff compiled exactly gs_player_body/head + the rolledup_arms3_edit
  viewmodel used by the IWD `_loadout`/`character\char_zomb_player_*`, and none of the models the mod.ff rawfile
  copies name. Nuketown now ports 40 scripts (was 31), link-check clean. Many ported scripts changed source.
- `level.script` in ported WaW code -> `level.waw_script` (WaW map name, set in `_waw2bo2_assets::init`);
  BO2's own level.script is untouched (gscport.WAW_LEVEL_FIELDS).
- Player appearance: `maps\_loadout` is no longer core; gscport ports it whole (map override, else stock) when
  it has init_loadout+give_model, plus generated `maps/mp/waw/_waw2bo2_characters.gsc`:
  precache() = compat init, level.is_zombie_level=1, `_loadout::init_loadout()`; give() = detachall,
  entity_num (WaW connect handler), characterindex=entity_num, BO2 bookkeeping, `give_model(self.pers["class"])`.
  `hook_bo2_characters` repoints `level.precachecustomcharacters/givecustomcharacters` in the BO2 main.
  Character/viewmodel models reach the MAP zone under WaW names via SCRIPT_MODEL_RE (single-root skins: kept).
  NOT ported: BO2 vox/exert per character (template's), WaW `self.voice`.
- Weapons: WaW names kept (no always-loaded BO2 zone has a WaW-named weapon; template/stock collisions are
  excluded). Other weapon assets stay namespaced (waw_xmodel/, waw_xanim/, waw_material/, waw_image/,
  waw_physpreset/). New `weapons.stage_physpreset` (T4 PHYSIC keys == T6, alias prefix -> waw/).
  `stage_visuals` records failed_materials/material_images, recovers wavelet images.
  `weapons.stage_runtime` -> `content_source/weapons.runtime.json`: carried weapons, exclusions (unconverted
  model/material, multi-root skins, missing alternate), BO2_FALLBACK/cleared variant model slots
  (gunModel2..16/worldModel2..16), cleared unconverted UI materials/anims/FX; zone lines appended to
  mod_extra.zone: `>level.ipak_read,<project>_mod`, `>ipak,<project>_mod`, images, xanims, `fx,,waw/<fx>`
  (FX converted in the MAP zone: weapon FX now join stage_effects), `weapon,<name>`.
  build-mod adds content_source as asset root; package copies mod_out/*.ipak. Compat `waw_precacheitem`
  (level.waw2bo2_weapons table) skips+reports weapons mod.ff does not carry.
- stage_bridge order: port_scripts -> weapons.stage -> effects (script + weapon FX) -> ... -> runtime stage.
- Later in the session (all measured against the official BO2 mod-tools Linker, which is an OAT build):
  * BO2 name collisions: reserved = always-loaded stock weapons + template zone + every file in BO2
    `raw/weapons` (the mod linker finds those too, and our content_source shadowed them). Colliding WaW
    weapons are carried as `waw_<name>` (zombie_bowie_flourish, zombie_knuckle_crack); `none` maps to BO2's
    engine null weapon. Uncarried weapon files move to content_source/weapons_not_carried.
  * Weapon name layer in compat: level.waw2bo2_weapons (WaW -> BO2, "" = not carried) and
    level.waw2bo2_weapon_names (reverse). Wrappers: precacheitem, give/take/switchto/hasweapon, ammo
    get/set, givemaxammo, getcurrentweapon/getweaponslistprimaries (return WaW names). Other names pass through.
  * playerAnimType: T6 accepts only its list (T6_PLAYER_ANIM_TYPES, from the linker's own error). WaW
    autorifle/pistol/smg/mg/rocketlauncher/grenade -> default, measured on BO2 raw *_zm weapons of each class.
  * Sound bank: linker enforces uint + [0,65535] on distances and integer columns; T6_INT_COLUMNS measured
    on 458,371 stock alias rows; clamp/round reported (6,725 values, mostly WaW distMin 500000 = "everywhere").
  * WaW loaded-sound names with a leading "/" (WaW FS collapses separators) normalized, not rejected.
  * FX material absent from WaW (only a ", ref" in mod.ff): warning, effect skipped (was a stage error).
  * Absent image pixels (streamed image header without its .iwi anywhere; WaW drew its default): BO2 code
    image per slot (normal $identitynormalmap, spec $black, color $white) - material names it, zone lists it
    as reference `image,,$x` (this linker rejects `,$x` inside material JSON; ipak writer needs .iwi files).
  * BO2 fallback models must be buildable from raw: LOD .glb -> materials -> images; DDS-only images are
    converted to IWI into the project (ipak writer reads .iwi); images already loaded stock are refused.
  * Multi-root skins: rigid models whose root bones all share one bind transform under one parent
    (tag_flash, panzershreck rocket) -> secondary roots become identity children of tag_origin; globals
    unchanged; reported SKELETON_REPARENTED. Different transforms / animated models still refused.
  * Registration: WaW `include_weapon`/`add_zombie_weapon` -> compat shims (function_renames) calling BO2
    include_zombie_weapon / add_zombie_weapon(name, name_upgraded if carried, &"ZOMBIE_WEAPONCOSTONLY",
    cost, vo, "", ammo_cost); level.monolingustic_prompt_format=1 (prompt = display name + cost; WaW
    hint text not carried, reported). gscport extracts `dlc3_code::include_weapons` and
    `_zombiemode_weapons::init_weapons` (WEAPON_REGISTRATION) into `_waw2bo2_weapons.gsc`;
    `hook_bo2_weapons` replaces the template's include_weapons()/custom_add_weapons. BO2 framework weapons
    kept, never in box: knife_zm, frag_grenade_zm, m1911_zm(+upgraded) (BO2_FRAMEWORK_WEAPONS).
  * WaW start weapon (level.player_switchweapon) and last-stand pistol applied a frame after BO2's
    init_levelvars (which runs after the precache hook).
  * entities.WALL_WEAPON_MAP (stock substitutes) REMOVED; wall buys keep the WaW weapon, mapped/removed by
    `entities.map_wall_buys` from the runtime table (UNSUPPORTED_WALLBUY when not carried).
  * `tools/verify_mod_weapons.py`: dumps mod.ff weapons, compares every field with staged infostrings.
  * CLI: WAW2BO2_TRACEBACK=1 re-raises instead of the one-line error.
- Fixed a pre-existing syntax error in t6bridge (`zone.write(f"soundbank...` split by a raw newline).
  CAUTION: Git-bash heredocs here collapse `\\`; write Python/C++ with backslashes via Write/Edit tools.
- Tests: 75 pass (new test_weapon_runtime.py, test_gscport_appearance.py, test_bo2equiv.py).
- RESULT (full run_bridge, exit 0, packaged to Plutonium mods/zm_nuketown_waw, NOT tested in game):
  all 90 staged WaW weapons carried in mod.ff (0 excluded), 250 images in zm_nuketown_waw_mod.ipak, 532
  original xanims; tools/verify_mod_weapons.py: 90/90 weapons field-exact after dump, no zone line missing.
  Stage errors 0. 62 GSC compiled. Wall buys: fs_m14, fs_m16a2, fs_olympia, bf3_pdwr, bf3_pp2000,
  bf4_shorty, bfh_hcar (the real WaW guns). Sound bank 2,689 aliases in mod.ff. Log work/run_bridge_session17.log.

- IN-GAME RUNS (Session 17, user playing):
  * Launch: Start-Process with ONE argument string `t6zm "<BO2>" -lan +set fs_game mods/zm_nuketown_waw +devmap
    zm_nuketown_waw` (an argument array lost the path quoting; the bootstrapper exited silently).
  * Menu freeze with the first WaW bank: 833 MB of loaded (.sabl) banks in the 32-bit game (WaW kept loaded
    sounds ADPCM/XWMA-compressed; decoded PCM is 4-8x). `sounds.loaded_budget`: largest stock map bank
    (zmb_waw_sumpf: 1,656 files/246 MB) minus the template bank (1,264 loaded files from its CSV) -> 392 files /
    58 MB; shortest sounds stay loaded, 697 streamed (SOUND_LOADED_TO_STREAMED). Loaded WaW bank 338 -> 18 MB.
    Plutonium still prints "Too many sound entries, reduce them!" x3 (its own loader; not in t6zm.exe); no
    freeze since. Alias rows: stock map banks ~10.5k, template 9,912 + ours 3,896 - likely what it counts.
  * Crash 1 (0x7DF547, NULL char*): converted weapons lacked T6-only string/anim fields (fireIntroAnim =
    anim index 3). Stock raw weapons write all 1,027 fields; stage_runtime now writes every CSPFT_STRING /
    WFT_ANIM_NAME field ("" when unused).
  * Crash 2 (after zombie melee, read [NULL+0x1c]): XSurface rigid vert lists (vertListCount @+1, 12-byte
    XRigidVertList @+40, collisionTree @+8) had NULL XSurfaceCollisionTree. OAT's loader never built them
    (`// TODO`). Traversal measured in t6zm (sub_4C6D80/8E3420/8E3950/8E3880): query (p+trans)*scale in
    uint16 space, root node 0, node {u16 mins[3], maxs[3], childBegin, childCount}, childCount>=0x8000 =
    leaf range, leaf <0x8000 one tri else two tris from value-0x8000, indices into surface triIndices.
    LoaderXModel.cpp.template now builds median-split trees (<=4 tris/leaf) for both rigid paths (T6 only).
    Unlinker writes xmodel/<name>.collision.txt (every list validated). Result: map zone 1,996/1,996 lists
    valid (was all NULL); prebuilt mod-tools linker (mod.ff) already builds valid trees (2,721 ok).
    Rebuild note: MSBuild's template step fails (exit 3); run RawTemplater manually into a temp dir, copy
    ALL Game/*/ outputs and touch them, then build (see Session 4 note). Set MSYS_NO_PATHCONV=1 for
    ;-separated search paths in git-bash.

- Playtest round 3 fixes (user: gun OK; ADS over-zoomed, no sprint, stuck zombies, prop collision, door text):
  * ADS: WaW adsZoomFov -> T6 adsZoomFov1/2/3 (stock repeats it; a 0 level = 0-degree FOV).
  * Sprint: the character hook must call setsprintduration(4)/setsprintcooldown(0) (template did).
  * Barriers (13/13 windows): BO2 _zm_blockers wants exterior_goal -> {zbarrier, struct (trigger_location),
    Begin node (neg_start, disconnected while boarded)} (measured zm_prison). The goal now keeps WaW's
    target group (WaW board+clip brushes become chunks BO2 deletes; struct = trigger_location) and the zbarrier
    joins it; entities.link_barrier_traversals gives each window's inward wall_hop Begin node the group name
    (path data + node entity; float32 tolerance). Before, the WaW clip brush stayed solid forever.
  * Traversals: paths.traverse_substates parses zm_nuked_basic.asd (uncommented, with _crawl twin); same-name
    substate first (same WaW animation), else nearest by MEASURED height (BO2 dotraverse is noclip, no warp).
  * compat stuck_zombie_monitor prints "WAW2BO2 STUCK zombie at ..." once per zombie (15 s, 32 units).
  * Model collision: T4 dumper writes xmodel collSurfs (tris plane/svec/tvec, bounds, bone NAME, contents,
    surfFlags) + model contents into the JSON; T6 bridge loader rebuilds XModelCollSurf_s (identical layout),
    bones by name. Dump markers bumped (companions .dump_v6_collsurfs, stock v5, source v2); run_bridge now
    dumps the map zone's xmodels every run. Verified in the linked zone: 31 models with collSurfs (trucks 742 /
    25,924 / pickup 4,993 tris). The bunker generator model has NO collision in WaW either (no collSurfs,
    contents 0, type viewhands) - nothing to convert; its grenade-damage mechanic still needs an in-game check.
    The prebuilt mod-tools linker has its own incompatible "collSurfs" JSON schema: weapon models (mod.ff)
    drop the key (models report collision_not_carried).
  * tools/retemplate_oat.py regenerates OAT templates (and their .log stamps) so MSBuild skips its broken step.
- Playtest round 4 (user-verified: WaW colt + WaW arms visible, zombie melee no longer crashes, grenades
  destroy generators, doors buyable):
  * Spawners (root cause of "stuck zombies"): entities tagged EVERY WaW spawner find_flesh. WaW
    (_zombiemode_spawner / zone_manager): plain spawners walk to one of the 3 nearest windows; "riser" spawners
    rise at rise_locations = structs named volume.target + "_rise" (never at the actor, often parked outside).
    Now: plain spawner -> spawn_location at the actor, no script_string (BO2 default = nearest windows);
    riser actor -> nothing; each "<volume.target>_rise" struct -> riser_location named volume.target with
    script_string "find_flesh" only when WaW's script_noteworthy is find_flesh (BO2 reads any other string as a
    barricade id and fails; its no-string default equals WaW's nearest-windows). Nuketown: 8 spawn + 63 rise
    spots; 69/71 on the main path graph; all 8 plain spawners reach all 13 windows. Last session: 0 STUCK.
  * Collision: 2,792 of 16,650 WaW brushes (17%) were never exported - the T4 dumper's leaf-brush-node walk
    for cmodel 0 misses some (probably a node form it does not decode; not root-caused). hulls.collision_brushes
    now also takes every unlisted brush outside all brush models' LOCAL bounds (brush-model brushes live in
    entity-local space): 2,791 recovered (1,675 missile/shot clip 0x2080, 865 detail solid 0x8000001, 112
    player/AI clip...), 1 ambiguous skipped and reported (stage report collision summary).
  * Headless zombies: the zm_test template zone never lists the heads c_zom_dlc0_zombie_hazmat_1 picks from
    xmodelalias c_zom_dlc0_zom_head_als (c_zom_dlc0_zom_head1..4). modzone.character_models adds models named
    by the zone's character/ + xmodelalias/ scripts (setmodel/attach/precachemodel literals + alias arrays)
    that exist in BO2 raw and are not listed. mod.ff relinked with them (not yet confirmed in game).
  * Generators "not solid" (user, 3 times; they expect solid): measured WaW data - generator model and its 5
    part models have NO collision (collSurfs 0, contents 0); brushes: gen1 only a 1-unit back plate (the 0x2080
    clips nearby belong to a neighbouring structure 80 u away), gen2/gen3 a solid core box (~20% of the
    80x93x128 body). The mapper put PLAYER clip (0x8030200) around the imported collision-less perk machines,
    so WaW does not auto-solidify collision-less script_models. Converted collision matches WaW. OPEN: asked
    the user to walk into gen2/gen3 - if they walk through, world BRUSH collision itself is broken in our T6
    build (floors work via terrain triangles, so brushes are unproven); if blocked, behaviour is faithful. A
    bounding-box solid for collision-less script_models was offered as an opt-in, not recommended by default.
  * Door buy text: doors buy fine; triggers identical to stock (zm_prison); BO2 sets default_buy_door/debris
    (registered, strings in en_patch_zm, no "UNABLE TO FIND HINT" in console); no ported script touches them;
    the user's global zzz_*door scripts are map-gated to stock maps. Suspect prompt DISPLAY, not setting.
    OPEN: asked whether ANY trigger shows text (wall buys, perks, box) - no answer yet.
  * Plutonium console after match end: "LUI_ERROR: Error processing event: gametype_update" (lobby Lua,
    after-action report) - not map-related, seen every match end.
  * Relink shortcut when only mod.ff changes: `cli build-mod ...` then `cli package ...` (no full run_bridge).
- Tests: 82 pass.

### Next (in-game checks first; user tests)
1. Get the answers: (a) generators 2/3 walk-through? -> if yes, debug T6 world brush collision in the bridge
   ClipMapLinker (brush kd-tree / leaf contents); (b) do ANY prompts render? -> if none, the zm HUD/LUI prompt
   path; if only doors, door-specific. Confirm zombie heads.
2. Root-cause the T4 cmodel-0 leaf-brush walk (WorldConverterDumperT4.cpp v3 section): decode the node form it
   misses so the unlisted-brush recovery heuristic can go.
3. UNVERIFIED: mod.ff references weapon FX (`fx,,waw/...`) that live in the map zone; mod.ff loads BEFORE the
   map zone (console order), so those references may resolve to defaults. Check muzzle flashes.
4. Knife: WaW knifeModel/worldKnifeModel have no T6 field (T6 melee is its own weapon) - still reported.
5. WaW weapon displayName localization (prompts show display name) and per-weapon WaW hint text; ported
   scripts call setHintString with plain strings (galvaknuckles, _nt_ee...) - check T6 accepts them.
6. Visions, lighting, remaining sound semantics (Session 16 list); "Too many sound entries" alias budget.

## Session 16 (2026-09-29): BO2 cannot take a map sound driver; curves now bound by shape

- Previous (unrecorded) agent work, now recorded: OAT T6 got a lossless driver sidecar
  `sounddriverglobals/<name>.w2bsdg` (`ObjCommon/Game/T6/SoundDriverBinary.h`, dumper hook in
  SndDriverGlobalsDumperT6.cpp, loader `ObjLoading/Game/T6/Sound/RawLoaderSoundDriverT6.cpp`, registered in
  ObjLoaderT6). Stock dump of code_post_gfx_zm: `work/t6_sound_driver/base` (17 curves, 33 pans, 26 groups).
- IDA (t6zm.exe; orphan idalib worker pid 42232 port 51752 holds the IDB lock; `idb_open` fails, so it was
  driven over raw JSON-RPC at http://127.0.0.1:51752/mcp). DB_AddXAsset = 0x7FBD30. When a later zone
  carries an asset already loaded: types 16,30,31,41,45,47-49,56-58 or types with a default-asset name
  (table 0xD41330) may override; every other type, INCLUDING snddriverglobals (0x20), reaches
  Com_Error(ERR_DROP=1, "Attempting to override asset '%s' from zone '%s' with zone '%s'") at 0x7FBEC5
  (switch at 0x7FBE81). Com_Error (0x6DF0B0) only prints instead when g_connectpaths >= 2 (dev/path
  compile dvar, sub_4500D0). Per-type override hooks 0xD41510: type 0x20 has none (so even the dev path is a
  raw memcpy with no audio cache refresh). DB_FindXAssetHeader = 0x6CBF50; no caller passes type 0x20 and
  there is no "singleton" string. Conclusion: a converted map can NOT ship its own driver/curves/pans.
  IN-GAME CONFIRMED under Plutonium r5354: probe mod.ff holding only snddriverglobals,singleton (stock +1 curve,
  OAT-linked, roundtrip exact) crashed at load with "Exceeded limit of 1 'snddriverglobals' assets"
  (pool allocator 0x7FB7D0, pool size table 0xD40E80). It fails even before the override check. Probe mod removed.
- Therefore `sounds.bind_curves` (called in stage_bridge after bind_pcm) binds each original WaW curve to a
  stock T6 curve only when the evaluated piecewise-linear shape matches (tolerance 1e-5, all breakpoints +
  midpoints), never by name. Otherwise UNSUPPORTED_SOUND_CURVE with the nearest shape/error recorded; the
  explicit compat flag `--approximate-sound-curves` binds nearest and reports APPROXIMATED_SOUND_CURVE.
  Driver comes from `--t6-sound-driver` or `<techset-dump>/sounddriverglobals/singleton.w2bsdg`;
  run_bridge.ps1 step 1c now dumps it from code_post_gfx_zm.ff (verified: same SHA as the earlier dump).
  Writes `t6_curves`/`t6_curves_missing` per variant in sound_bound_ir and `sounds.curves.json`.
- Assumption (not measured): both engines evaluate curves over normalized distance between min/max dist.
  T6 defaultmin/allon end with a vertical step at x=1; which value the evaluator returns there is unmeasured
  (entry marked evaluator_dependent). The T6 curve evaluator itself was not located this session.
- Fixture result (work/sound_port): only defaultmin matches exactly (4,110 slots). curve2/linear 3,456 slots
  (nearest cos, max gain error .20), default 409 (cos .089), curve4 148 (cosdelay .31), curve0 67 (steep
  .089), curve3 30 (cos .089). So 0 of 2,055 variants are curve-complete in strict mode.
- Tests 56 pass (4 new generic curve tests); compileall, CLI help and run_bridge.ps1 parse OK. No bridge
  rebuild, install or game launch.

- PLAYBACK (user: "just make it play", then move to viewmodels/custom weapons):
  * `sounds.write_t6_bank` writes an official 60-column BO2 alias CSV `zone_raw/<map>/soundbank/waw_<map>.all.aliases.csv`
    from sound_bound_ir; stage_bridge appends `soundbank,waw_<map>.all` to mod_extra.zone; build-mod adds
    content_source/pcm as an asset search path. Names are `waw/<alias>` (secondary too); units: vol/reverb/center %,
    pitch cents from bind_pcm, dist min/maxDry/maxWet, priorities, thresholds, occlusion, loop, 2d/3d from the
    spatialized flag. NOT translated (reported in sounds.bank.json): bus/group/duck (stock category guessed from the WaW
    bus name), speaker map (stock pan front/default), limit types, chain alias, team/cylinder/move/master/slave.
  * run_bridge passes --approximate-sound-curves (nearest stock curve, each reported).
  * Stock BO2 .sabl/.sabs entries are ALL 48 kHz (18,716 loaded, all streamed sampled); the BO2 Linker warns on non-48k
    loaded sounds. audio.stage now resamples LOADED sounds that aren't 48 kHz with FFmpeg swr (filter_size 128), from the
    true source rate (pitch scale becomes 1.0; original PCM hash kept). Streamed sounds are unchanged.
  * GSC: compat wrappers waw_sound/playsound/playloopsound/playsoundatposition/playlocalsound/playsoundasmaster/
    soundexists (gsc_api compat_wrappers) play "waw/"+alias when soundexists(), else report once and play nothing.
  * Fixture: work/sound_pcm_48k (1,943 PCM, 238 resampled), 2,052 variants / 1,950 aliases in the bank; official Linker
    build work/sound_bank_probe: no rate warnings, unlinker roundtrip 2,052 rows. 59 tests pass.
  * NOT yet: full run_bridge rebuild/install and in-game listening test.

### Next
1. USER DECISION (2026-09-29): drop exact attenuation for now; just make WaW sounds play through BO2 banks,
   then move to viewmodels/custom weapons. Curves use nearest stock shape (reported).
2. Same analysis for speaker maps -> T6 pans and buses -> T6 volume groups (also driver tables, so also
   stock-only): match by data, report the rest.
3. Then generate the T6 alias CSV / banks from the bound IR and continue weapons, visions, lighting.

## Session 15 (2026-09-29): generic alias-to-PCM bindings, no runtime completeness claim

- User reiterated: this must work globally for arbitrary compiled WaW maps; viewmodels, custom weapons,
  sounds, visions and lighting must be automatic, not optional fixture-specific additions.
- Read GUIDELINES.MD completely. No IDA work, game launch, installation or full bridge rebuild this session.
- New `sounds.bind_pcm(project, pcm_root)` automatically called by `stage_bridge` after decoding.
  Uses ONLY current sounds.stage/audio.stage manifests, never discovers stale files by directory glob.
  Writes `sound_bound_ir/<alias>.json` and `sounds.bindings.json`; all original variant fields,
  curve values, speaker matrices and bus metadata remain unchanged.
- Each original variant binds to the actual PCM payload after checking canonical WAVE header, rate,
  channels and PCM SHA. Applies alias_pitch_scale to BOTH original pitch endpoints, computes T6 CSV cents.
  Invalid/reversed/nonfinite pitches or native T6 range overflow are explicit errors, never clamped.
  Bound IR is STILL NOT a runtime sound bank; driver/bus/duck/flags remain unimplemented as Session14 notes.
- audio.stage records loadType for unambiguous current bindings. Older manifests without loadType are
  accepted only where output/hash/scale identity is unique; this was necessary to audit existing native PCM.
- Source sound staging now permits generated outputs to update on rebuild, rejects conflicting payloads
  within the same run using hashes; previously any legitimate source revision hit stale-output conflict.
- Actual fixture audit `work/sound_port` + `work/sound_pcm_complete_native`: 2,052 original alias variants
  bound, three missing-source errors (bf4_mpx_clip_out, fly_gear_reload, rich_box_first_laugh), no pitch errors.
  52 unit tests pass + compileall. New generic tests cover pitch range compensation, source revision,
  stale payload rejection, current-manifest-only eligibility and unsupported pitch refusal.
- Full scope remains incomplete. Next: native original sound driver/alias semantics + runtime banks,
  actual weapon/viewmodel integration, visions, lighting, world/collision/navigation and game validation.
  Do not declare success because IR bindings or neutral diagnostic banks compile.

## Session 14 (2026-09-29): native XWMA decoding implemented; all available fixture audio converted and bank-audited

- Concrete progress; full converter goal remains ACTIVE/incomplete. No installed map rebuild/gameplay test.
  Read IDAPython skill completely. Revalidated ready IDA session b1c7f48d (XAudio2_0.dll) before analysis.
- Verified native xWMA calling conventions from demangled symbols AND actual stack/disassembly. Decompiler
  erroneously adds saved-register/ghost parameters: audecNew2args, GetFormat3, audecInit6, audecDecode4,
  audecGetPCM10, defaultopts1, PCMFormat2WMAFormat2. All stdcall; no hidden this argument.
- Native offline WMA bridge mirrors XAudio2 WMA voice format setup: PCMformat6dwords, WMAformat packed24,
  defaults116bytes, profile92bytes; infer actual original profile with GetFormat, check packet size matches
  source, native input callback delivers one original encoded packet at a time, carries final-buffer flag.
  audec state2 decode,3 getPCM,0 EOS. No engine, sound device, source voice or audible playback created.
- First native fire_tornado result820224 PCMbytes exactly dpds, unlike FFmpeg819200. Full180-original-XWMA
  audit: ALL native decoded lengths exactly dpds, all input consumed, zero failures. FFmpeg sample agreement
  ZERO180, including131 equal-length files. Length deltas48files512frames,1file768frames,131files0frames.
  Report work/xwma_native_audit/xwma.reference.json; each original/native/FFmpeg SHA retained for comparison.
- Prototype audit fixed-offset executable remains work/xaudio_reference/xaudio_wma_reference.exe only.
  Source moved/refactored to `tools/xaudio_wma_decoder.cpp`: native functions now found by UNIQUE32byte
  executable-section signatures, relocation-dependent bytes wildcarded, NO hardcoded internal addresses.
  Requires hash-audited installed XAudio2_0 DLL via Python wrapper (known SHA in prior session). Does not bundle
  Microsoft DLL or original asset payloads. Missing/ambiguous signatures or unaudited DLL version fail closed.
- `src/waw2bo2/xwma.py`: validates original XWMA fmt/packet count/monotonic dpds/frame boundaries, finds
  tools/bin/xaudio_wma_decoder.exe or explicit helper, locates actual Windows32bit system DLL, verifies SHA,
  calls subprocess with private temp output, verifies all source consumed and native bytes exactly match dpds.
  Unsupported DLL versions need new native audit/signature coverage, not FFmpeg fallback.
- `audio.convert` now AUTOMATIC native_xwma (no FFmpeg candidates/fallback); original ADPCM remains native ACM
  verified against XAudio2. Signature-scanned output matched fixed-offset native reference hashes EXACT180/180.
  Optional external FFmpeg discovery no longer runs unless explicitly requested (audits may still use it).
- `stage-bridge --xwma-decoder` override added. `run_bridge.ps1` automatically runs generic
  `tools/build_audio_decoder.ps1`: discovers installed VisualStudio C++ toolchain via vswhere, builds x86
  helper when missing/outdated, or accepts explicit -XwmaDecoder. Build helper supports -Force. Tested FORCE
  build successfully; run_bridge PowerShell syntax parsed without errors. No manual setup needed here.
- Corrected RIFF parser behavior: a complete declared RIFF object can have unrelated trailing file bytes.
  Parse only within declared object, reject truncated/invalid declared bounds/chunks, retain original wholefile,
  report trailing byte count/SHA rather than treating tail as PCM. Fixture garbar_loop has valid RIFF8527076
  bytes with58140 trailing bytes, not a broken internal audio chunk. Its declared original PCM now preserved.
- Actual `work/sound_pcm_complete_native`: 1,943 available audio files:180 native_xwma,1691 native_ms_adpcm,
  72 exact_original_pcm. Only three source misses remain (same original bf4_mpx clip,galilgear,rich_boxlaugh).
  Source aliases/drivers/loop behavior still separate IR and NOT translated runtime sound bank definitions.
- Official BO2 Linker native bank audit + modern Unlinker repeated with ALL1,943 final native PCM:
  work/sound_bank_complete_native_audit/report.json: native_pcm_exact1943,roundtrip_pcm_exact1943,missing0.
  Banks are neutral diagnostic definitions, NOT original alias behavior; do not install those as final map audio.
- Tests50 PASS; includes XWMA packet/profile validation, unknown native DLL refusal before invocation, RIFF
  trailing-data exclusion. compileall/CLI and full bridge rebuild still should be checked before installation.

### Next work (full original scope unchanged)

1. Original SOUND ALIAS semantics: custom driver curves (same-name T4/T6 curves differ), speaker matrices,
   buses/ducking/teams/cylinders; native T6 driver binding/indices and singleton override rules need evidence.
   Apply recorded audio alias_pitch_scale to BOTH pitches. Native raw PCM is now available, not playback-ready.
2. Resolve three missing originals through stock loaded-sound closure; full automatic content bridge integration
   needs a fresh stage/build with current exports. No global scripts/game processes were touched this session.
3. Original custom weapons/viewmodels/animations must actually reach mod.ff + runtime registrations; remaining
   enum/multi-root/physics/melee work, visions, lighting, full native asset roundtrips and in-game validation all
   remain. Do not declare completion based on successful neutral sound bank packing.

## Session 13 (2026-09-29): XAudio2 2.0 ADPCM reference verified; automatic decoder corrected

- Concrete progress toward the full ACTIVE conversion goal; no installed mod changes or game launch.
  Read IDAPython skill and used IDA for actual DLL function/layout inspection.
- Owned live IDA sessions (TTL3600): `58e964ba` installed CoDWaW.exe; `b1c7f48d` private copy of installed
  C:/Windows/SysWOW64/XAudio2_0.dll at work/xaudio_reference/XAudio2_0.dll. CoDWaW .text is encrypted on disk
  (few functions/xrefs), so data inspection alone cannot prove all runtime submission details.
- CoDWaW contains CLSID `{6f6ea3a9-2cf5-41cf-91c1-2170b1540063}` at0x89dab8, which installed32bit registry
  binds to XAudio2_0.dll; searched other2.1/2.2/2.3 release class IDs, absent. Strong static version evidence.
- Installed XAudio2_0 DLL SHA256 e435b73193bdf651f7ae564eba05266595ac672db45e0e22dce92d0bcb3c6513.
  Native symbols available! GetAdpcmDecodeFunction0x44c110 selects mono/stereo8/16/float; short mono0x450ef0,
  stereo0x4514e0. Prediction uses arithmetic >>8, unlike FFmpeg's truncation towardzero. Read actual code.
- New audit-only x86 `tools/xaudio_adpcm_reference.cpp`: loads installed system DLL, checks audited signature,
  obtains native decoder via selector, decodes standard7-coefficient ADPCM blocks directly. No XAudio engine,
  voice, sound device or playback created. Parser rejects invalid predictors/customcoef/incomplete blocks;
  output-size overflow guard. DO NOT ship fixed internal DLL offsets as production converter dependency.
  Build with MSVC vcvarsall x86, output work/xaudio_reference/xaudio_adpcm_reference.exe.
- Extended audit_native_audio --xaudio-helper, requires exact DLL SHA, compares direct XAudio output to ACM.
  Full real-source audit: ALL1,691 valid ADPCM files EXACT XAudio2_0 == Windows ACM. They all differ from
  FFmpeg. Report work/native_audio_audit/native_decoder_audit.json has native/FFmpeg/XAudio hashes.
  One malformed garbar_loop source remains rejected. Audit does not claim observed live WaW playback.
- `audio.convert` now automatically uses installed Windows ACM for original standard7coef Microsoft ADPCM,
  verifies source block frame count/output length. No FFmpeg fallback for ADPCM (known wrong samples).
  Customcoef reported pending XAudio semantics; nonWindows explicit unsupported native decoder, not fake output.
  All source metadata/nonstandard rate->alias_pitch_scale rules retained. Native Windows path needs no FFmpeg.
- Actual fresh `work/sound_pcm_native`: 1,691 native_ms_adpcm,71 exact_original_pcm,131 XWMA length-matching
  candidates,49 XWMA length mismatches excluded,4errors (three missing originals +garbar_loop malformedRIFF).
- Official native bank audit repeated with CORRECTED PCM: work/sound_bank_native_audit/report.json,
  native_pcm_exact1893, roundtrip_pcm_exact1893, missing0. No original alias semantic/runtime audio claim.
- Rebuild behavior fixed in audio.stage: previous generated PCM may legitimately change after decoder/source
  improvement; overwrites previous generated outputs, rejects conflicts between different assets in THIS run.
  Downstream consumers MUST use current manifest, not stale unsupported output files.
- Regression suite46 tests PASS. New negative prediction test distinguishes arithmeticfloor from FFmpeg
  truncation; pipeline test confirms actual native ADPCM selection/sample output. Rebuilt latest x86 helper
  after overflow guard, uber fixture still matches newly staged native PCM exactly.

### XWMA next: useful exact native symbols found

The installed XAudio2_0 DLL embeds its xWMA decoder, so offline native reference should be possible without
audible playback or a running game. IDA session b1c7f48d has types/symbols. Relevant functions:
- CX2SourceVoiceWMA::Initialize0x42e4a0: constructs PCM format, derives original WMA profile via defaults+
  GetFormat, calls audecNew and audecInit with inputcallback. Decompiled in this turn; reread for structs.
- PCMFormat2WMAFormat0x453d30; GetDefaultEncFormatOpts0x454fd0; GetFormat0x454f70;
  audecNew0x455360; audecDelete0x4552c0; audecInit0x45b5c0; audecReset0x455a10;
  audecDecode0x45ae00; audecGetPCM0x45b500; audecGetPCMWrap0x45b250;
  GetWMAInputData0x42e110; prvNewInputBuffer0x470010.
Initialize stack shows PCM struct6dwords, WMA format26bytes, defaultopts116bytes, formatinfo92bytes,
initparams4dwords includingcallback/context. VERIFY actual types/calling conventions; decompiler labels
some stdcall parameters as this and hidden registers incorrectly. Never guess a native call stack.
Native WMA reference needed for49 length mismatches and131 currently length-only candidates; then original
sound-driver/alias semantics, gun runtime bindings, visions and lighting remain the full unfinished scope.

## Session 12 (2026-09-29): native Windows ADPCM sample audit exposes decoder disagreement

- Goal turn is concrete progress: new native reference API, regression cases, full sample comparison.
  Full converter goal remains ACTIVE. No mod installed/started; no bank/runtime completeness claim.
- Added `src/waw2bo2/acm.py`, synchronous OFFLINE Microsoft Windows Audio Compression Manager decoding.
  Uses msacm32.dll and installed msadp32.acm; no sound device opened and no audio played. Correct pointer-sized
  ACMSTREAMHEADER, explicit API prototypes, buffers kept alive, source-consumption checks, unprepare/close.
  API references: https://learn.microsoft.com/en-us/windows/win32/api/msacm/nf-msacm-acmstreamopen and
  https://learn.microsoft.com/en-us/windows/win32/api/msacm/nf-msacm-acmstreamconvert .
- `tests/test_acm.py` native mono/stereo hand-constructed known-block cases verify sample and channel order.
  Full suite44 tests PASS. Native Windows availability is required for those two tests (skip elsewhere).
- `tools/audit_native_audio.py SOURCE PCM OUTPUT`: compares FFmpeg staging to actual Windows ACM output,
  records exact hashes/sample-difference count/maxdelta and writes native candidates SEPARATELY. Current
  report `work/native_audio_audit/native_decoder_audit.json`: ALL1,691 valid ADPCM files differ at sample
  level; no length disagreement. One source invalid (garbar_loop RIFF size). No source candidates replaced.
- Example original uber_shot_st: both90112 PCMbytes; 36,299 of45,056 samples differ, maxdelta69.
  Worst whole-set delta305 (voiceovers/zombie/dlc3/plr0/name_dempsey_00.wav). This disproves the idea that
  matching decoded length proves sample fidelity. Session11 compressed files remain candidate decodes.
- Read-only binary strings in actual installed CoDWaW.exe show XAudio2 references, NOT MSACM32/acmStreamConvert.
  Therefore Windows ACM cannot be declared the exact WaW runtime decoder without further XAudio2/engine audit.
  Do not arbitrarily choose FFmpeg or ACM, and do not call Windows agreement a WaW native roundtrip.
- Attempted native ACM on original fire_tornado XWMA: stream open MMRESULT512 (unsupported conversion).
  This API is not a WMA solution on this machine. XWMA49 length mismatches remain unresolved, unchanged.
- T6 driver inspection confirms dynamic SndDriverGlobals curveCount/pointer; aliases have6-bit curveindices.
  Still no validated runtime custom-slot/global-binding path or original alias semantic conversion. Do not
  name-map default/rcurve/pan settings; their source numerical data differ as documented earlier.

### Next actions

1. Inspect actual WaW sound setup/decoder (XAudio2 version/ADPCM/WMA submission) and capture offline decoded
   reference samples. Could use a native XAudio source-voice effect tap without audible output; ensure stable
   channel/rate, source start/codec delay/tail boundaries. Current Windows ACM/FFmpeg difference needs resolution.
2. Keep original native and FFmpeg candidates separate until reference settles it; extend format support,
   missing-audio recovery, loop-region and nonstandard rate/pitch translation without fabricated samples.
3. Continue original sound-driver/alias semantics (curve slots/channel matrices/ducking), then weapons,
   visions and lighting. Session11/10/9 notes retain the full scope. No new blocker or approval needed.

## Session 11 (2026-09-29): original audio decoding + official native bank payload roundtrip

- Concrete progress; full conversion goal remains ACTIVE and incomplete. No installed mod changes/game test.
- Found already-installed `imageio_ffmpeg` in Python314 roaming packages; runtime discovery returns its
  FFmpeg7.1 executable. No install/download. `audio.find_decoder` tries explicit path, PATH, imageio_ffmpeg.
  `stage-bridge --audio-decoder PATH` overrides detection; PCM staging is automatic in stage_bridge.
- New `audio.py`: strict RIFF chunk parser; canonical PCM16 WAV with fixed44-byte header; source PCM exact,
  original Microsoft ADPCM/XWMA through FFmpeg without resampling/remixing, validates block/packet lengths.
  Extra PRIV/metadata must NEVER become audio samples (native T6 bank writer assumes fixed44byte header).
  Preserves source bytes separately, SHA256, chunk sizes, private metadata, loop metadata; unsupported loop
  regions remain explicitly incomplete. Decoder errors, incomplete blocks, unrecognized formats fail closed.
- T6 sample rate is an enum (8000,12000,16000,24000,32000,44100,48000,96000,192000), unlike original WaW
  WAVs with many slightly nonstandard rates. No invented/resampled PCM: choose nearest bank rate, retain all
  samples and record `alias_pitch_scale=source_rate/bank_rate`. REQUIRED downstream alias translation must
  multiply BOTH source min/max pitches by this factor before cents conversion. Do not omit the correction.
- Actual `work/sound_pcm`: 1,893 PCM files emitted: 71 exact original PCM plus 1,822 compressed decoded files
  with matching expected length (1,691 ADPCM +131 XWMA). Compressed samples still need native decoder audit.
  49 XWMA files have decoded length disagreement and are excluded from output manifest, NOT padded/truncated.
  Example fire_tornado.xwma: dpds expected820224 bytes / PRIV410112 samples, FFmpeg emits819200 bytes (512mono
  samples shorter). Need actual WaW/XAudio/WMA reference to determine delay/tail semantics before claiming fidelity.
  Four errors: previous three missing original files plus malformed RIFF size in music_mainmenu/garbar_loop.wav.
- `tools/audit_audio_bank.py`: explicitly DIAGNOSTIC neutral alias definitions, NOT original alias translation.
  Uses official BO2 `bin/Linker.exe`, stages all1,893 manifest PCM, alternates loaded/streamed to exercise both
  SABL/SABS writers. Verifies every packed payload hash, frames, channel count, rate index, storage/format,
  then modern native Unlinker dumps soundbank and verifies all1,893 recovered PCM hashes too.
  `work/sound_bank_audit/report.json`: native_pcm_exact1893, roundtrip_pcm_exact1893, missing0; native build
  and dump succeeded. Does NOT prove original alias behavior, native decoded samples, or runtime audio.
  Audit command: python tools/audit_audio_bank.py work/sound_pcm --bo2 <BO2> --work work/sound_bank_audit
    --unlinker vendor/OpenAssetToolsT6/build/bin/Release_x86/Unlinker.exe (PYTHONPATH=src).
- Measured official BO2 bank file IDs hash FileSource AFTER Windows slash canonicalization: '/', converted
  to '\\', using T6 SND_HashName. Probe first raw forwardslash hash204368948 vs actualbackslash3175998952.
  Keep this rule for audio IDs; do not assume all unrelated asset name hashes normalize the same way.
- Found/fixed native T6 Unlinker WavWriter bug: it wrote constant RIFF size48, omitting audio payload size.
  Correct RIFF size=dataLen+36. Rebuilt workspace T6 Unlinker with MSBuild Release/Win32 successfully,
  redumped probe: all canonical WAV parsing and PCM hash comparison now pass. Log work/audio_unlinker_build.log.
  Native dumper appends codec extension to full sound asset name, hence FileSource ...wav -> exported ...wav.wav;
  audit accommodates that documented behavior rather than expecting an incorrectly named file.
- Tests now42 PASS incl extra-chunk normalization, exact samples, explicit rate/pitch correction, malformed
  RIFF/truncated PCM refusal. compileall and stage-bridge CLI help parse pass. Full bridge integration still
  not rebuilt/tested in-game. Do not install neutral diagnostic banks as the converted mod.

### Next concrete work

1. Audit XWMA against actual WaW/Windows native decoder (49length mismatches plus131length-only passes).
   Audit MS ADPCM against native decoder as well. Recover missing originals through stock loaded-sound closure.
2. Implement ORIGINAL T4 alias -> T6 behavior translation, custom curve slots/matrices/ducking. PCM rate-pitch
   factor MUST be applied. Current bank probe intentionally does not perform those semantics.
3. Wire original banks/weapon assets into runtime zones only once semantic translation is supported; full
   sound native alias roundtrip and runtime playback still needed. Session10/9 vision and lighting work remains.

## Session 10 (2026-09-29): recursive original dependencies and sound semantic/audio stage; NOT gameplay complete

- User reiterated GLOBAL conversion: all viewmodels, guns, sounds, visions and lighting must be automatic,
  not optional Nuketown fixes. Installed Session 8 mod is unchanged. No game launch/install this session.
- New `assetresolve.py`: deterministic original map/companion > original stock WaW closure, cycles, incoming
  edges, provenance, alternate weapons, models/materials/images/physics, notetrack sounds, sound chains,
  loaded/primed audio, FX dependencies. No BO2 replacements. LoadedSound uses `sound/name` or `.xwma`.
  Accuracy graphs are exported with the owning weapon, not independently indexed XAssets.
- `weapons.stage` can use StockWawAssets and extra model requests; writes `weapons.dependencies.json`.
  CLI stage-weapons now has --waw-root, --t4-unlinker, --waw-stock-dumps and repeatable --viewmodel.
  Actual `work/weapon_resolved` probe (before loaded-audio edges were added): 78 weapons, 510 animations,
  166 models staged (previous 158), 15 missing models, five unsupported; 180 original stock dependencies.
  Graph 1,734 nodes / 4,326 edges, 91 missing. Missing CIA arms and viewmodel_b_knife_d2p_out remain absent;
  no replacement picked. Missing export vs genuinely absent compiled definition still needs distinction.
- `t6bridge.stage_bridge` now initializes stock resolution independently of optional script porting and
  automatically resolves/stages weapon dependencies and script models (incl viewmodels). Weapon stage
  remains content_source, NOT runtime mod.ff. Automatic sound closure includes compiled aliases, weapon
  sounds, converted FX sounds and chain/secondary aliases; stages original payloads/IR without bank claims.
  Full stage_bridge rebuild has NOT been exercised after this integration; tests/compileall pass.
- Read IDAPython skill; verified actual sound schema in official WaW linker_pc.exe (not guesses from T6).
  Owned ready IDA session `a038fb61`, idle TTL3600; AssetViewer `89b3b98f` opened after old session was
  authoritatively absent (server health session-not-found and original process gone). T4 alias packer
  sub_404D50, column dispatcher sub_4034F0, CSV loader sub_404510, alias allocator sub_405180.
  CSV column pointer table 0x50F070, 64 entries. Native snd_alias_t is 184 bytes, flags at132.
- Corrected T4 headers/dumper semantic fields: teamVolMod@104 float, teamPitchMod@108 float, moveTime@120
  FLOAT, limitCount@152 unsigned, limitType@156 enum, entityLimitType@164 enum, cylInnerRadius@172 float,
  cylOuterRadius@176 float, cylOuterLevel@180 float. Existing min/max priority fields at84..96 ARE correct;
  cone fields parsed from CSV but not packed by this linker. Do not repurpose them as cones.
- Sound JSON schema now VERSION2; translator rejects VERSION1. Native T4 Unlinker rebuilt successfully
  with corrected headers; `work/content_mod/soundaliases` all redumped to v2. Other old map/stock roots may
  still have v1 until refreshed. Stock marker `.waw2bo2_dump_v4_sound_schema2`, companions marker
  `.dump_v5_sound_schema2`; both now include physpreset and snddriverglobals.
- Registered native DriverGlobalsJsonDumperT4 in same cpp/header and ObjWriter: dumps complete buses64,
  curves32 (all points/count), speakerMaps32 (four matrices), reverbs64, master effects16, including indices.
  Real globals are in code_post_gfx.ff / code_post_gfx_mp.ff, asset type snddriverglobals, name singleton.
  `work/sound_globals/soundglobals/singleton.w2bsndglobals.json` dumped natively with zero errors.
- Verified T4 flags: bit0 looping,1 master,2 slave,3 fullDryLevel,4 noWetLevel,5 randomLooping,
  6 spatialized,7 REAL_DELAY (NOT doppler),8 distanceLpf,9..10 maturity,11 doppler,12 isBig,
  13..14 loadType,15..21 legacy priority,22..27 bus,28..30 moveType. Bit31 unknown, retained/reported.
- New `sounds.py` decodes flags, binds actual indexed source buses/curves/speaker matrices, retains ALL
  source fields and driver data in semantic IR; namespaces secondary/chain references. Resolves compiled
  loaded payloads first, map IWD then stock IWD streamed payloads; preserves exact bytes and SHA256,
  namespaces audio, reports missing files/aliases/schema errors. XWMA remains explicitly requires_decode.
  No T6 sound CSV/bank or runtime playback claim. Source-only ducking/team/cylinder etc remain preserved.
- Actual `work/sound_port` stage: 1,953 alias lists translated with ZERO schema errors; 1,943 original audio
  payloads preserved (1,763 WAVE/other original files +180 XWMA). Three missing original loaded files:
  weapons/bf4_mpx/bf4_mpx_clip_out.wav, weapons/galil/fly_gear_reload.wav,
  custom_sounds/new_powerups/rich_box_first_laugh.wav. Report includes incoming alias names.
  Full resolver with loaded-audio stock recovery may recover more; this probe used content_mod+sound_globals.
- CRITICAL: T4/T6 curves with SAME NAME have DIFFERENT points! T4 default y=[1,.979592,.918367,.816327,
  .673469,.489796,.265306,0]; T6 default y=[1,.630957,.398107,.251189,.177828,.112202,.070795,0].
  Speaker wpn_all matrices differ too. Do not name-map curves/pans to falsely claim sound fidelity.
  T6 SndDriverGlobals has dynamic curveCount/curves; SndCurve has name32,id,8 vec2 points; alias curve
  indices have six bits. Possible custom driver slots need ENGINE singleton/loading validation, not assumed.
  T6 native CSV parser currently hardcodes names in SoundConstantsT6.h and has no source-only ducking fields.
- Regression tests now 39 PASS; compileall PASS. Added generalized resolver and sound tests (priority,
  recursive dependencies, cycles, path safety, flag layout, v1 refusal, driver values, exact audio bytes).

### Continue from here

1. Run full automatic content integration against fresh v2 dumps; distinguish exporter failure from genuinely
   absent assets. Recover missing loaded sounds from original stock zones; dynamic script references still need
   dependency discovery. Avoid adding every unrelated stock asset merely because its zone was dumped.
2. Decode original XWMA (no ffmpeg on PATH; look for available native/bundled decoder), build verified PCM source
   and official BO2 sound banks. Measure custom driver curves/pan/channel translation and global singleton use,
   preserve ducking/priority/teams/cylinders or explicit runtime behavior translation. Native roundtrip required.
3. Continue Session9 remaining weapon enum/multi-root/melee/physics and Zombies registration work; gun assets,
   animations and IPAK must actually reach runtime mod.ff. No stock wallbuy substitutions as completion.
4. Vision film color math translation and lighting (UV2/page indices, directional lightmaps, grid visibility,
   all primary lights) still NOT implemented. Session9 notes apply. Runtime currently flat/unlit.
5. Native full-zone roundtrip and isolated in-game verification still required. Do not mark tool complete.

## Session 9 (2026-09-29): generic gameplay-content extraction, native animation/gun probes; NOT installed/in-game tested

- User insists this is a GLOBAL converter: automatically port original viewmodels, custom guns, sound,
  visions and lighting for arbitrary maps. This session is progress toward that, NOT completion.
  Installed Session 8 mod is unchanged; no game launch or global-script isolation this session.
- Added T4 native `Sound/SoundAliasJsonDumperT4.{h,cpp}`, registered in ObjWriterT4 and its generated MSBuild
  project. Dumps `soundaliases/<name>.w2bsnd.json` preserving every alias field/flags/unknown gaps and original
  loaded/streamed/primed references. Rebuilt `vendor/OpenAssetTools/build/bin/Release_x86/Unlinker.exe`.
  `work/content_mod` has 1,953 lists / 2,055 rows (336 loaded, 1,719 streamed); loaded payloads include XWMA.
  These are extracted, NOT translated to T6 sound banks. Native asset type is `loadedsound` (not loaded_sound).
- `run_bridge.ps1` now extracts map/companion weapon,xanim,sound,loadedsound,rawfile,comworld automatically;
  companion marker `.dump_v4_content` forces old dumps to refresh. StockWawAssets DUMP_ASSETS extended likewise,
  marker `.waw2bo2_dump_v2_content`. Sources remain compiled-zone first; no IWD weapon sources as authority.
- `src/waw2bo2/weapons.py`: infostring parser/writer, measured T4/T6 schemas, namespace dependencies, alternate
  closure, compiled-source priority, original raw xanim staging, accuracy graph staging, intact model/skin
  staging, dependency and unsupported-field reports. Same key/type alone does NOT guarantee compatible enums!
  Sound pointers -> T6 alias strings supported; adsZoomFov -> adsZoomFov1; autorifle third-person profile ->
  T6 default (measured native M14/M16 definitions); first-person tracks unchanged. Other playerAnimType values
  (pistol/smg/mg etc.) still need measured mappings. knifeModel/worldKnifeModel require separate T6 melee port.
- CLI `stage-weapons OUTPUT --root DUMP [--root DUMP ...] [--weapon NAME ...]` (all by default).
  Optional `--stock-materials work/stock_t6_dump/materials --techset-dump work/stock_t6_dump` translates original
  weapon materials/textures into isolated waw_material/waw_image namespaces, copies compiled technique sets/
  shaders. Missing images are errors, not grey placeholders. Returns partial_source_stage; NOT gameplay-ready.
- Full local source stage `work/weapon_port`: 78 weapons, 510 original animations, one missing animation
  viewmodel_b_knife_d2p_out; 158 model sources staged, 22 missing from supplied dumps, five unsupported (multiple
  skin roots or physics). 111 model material dependencies. Sources: content_map, content_mod, waw_zone_dumps/mod.
  No animated model is made rigid to silence the loader. GLTF images are preview URIs; OAT engine material
  loading uses GLTF material names only. Nonembedded GLTF BUFFERS remain explicitly unsupported.
- `tools/audit_weapon_animations.py`: official BO2 Linker builds ALL 510 animations, modern Unlinker dumps
  them back, all 510 payloads EXACT after excluding only version word (T4 raw17 -> T6 raw19).
  Report `work/weapon_animation_audit/report.json`. Not evidence of runtime weapon/animtree binding.
- `tools/audit_weapon_zone.py`: official native weapon + IPAK probe, FX reference-only (`fx,,waw/<name>`, actual
  FX must come from map zone). Reads weapons.zone.fragment, builds and roundtrip dumps. Source accuracy graphs
  MUST be staged (native linker otherwise emits an error but still creates ff). Converted asset dependencies
  may not be dropped to declare success.
- Successful M14 probe: work/weapon_probe (fs_m14 only), work/weapon_zone_audit. Original three models, 18 xanims,
  six materials, ten textures. Native weapon fields ALL exact; all ten image payloads DDS->IWI exact. Six native
  material/three technique validations pass. Expected lone error: absent maps/waw_weapon_audit.d3dbsp (no world).
  Sound strings are namespaced but no bank yet; knife models explicitly unsupported; no gameplay validation.
  HUD icon reference missing in WaW is reported through existing dangling-material rule, not a custom fake icon.
- Generic HUD rule `techsets.match`: T4 2d -> T6 trivial (measured same reticle_side_small in both engines);
  avoids wrongly using a world shader for HUD. Gun visuals use existing measured world material translator.
- `modzone.link_mod` can search converted project; required extra-zone entries protected from the template's
  auto-drop loop. `_build_mod` passes project root. This is groundwork: gun fragment NOT added to runtime mod.ff.
- `stage_bridge` automatically stages original weapon/model sources under content_source (not runtime!), reports
  compiled sound/vision inventory and partial statuses. `SCRIPT_MODEL_RE` now includes setViewModel literals.
  Dynamic model references and resolving missing viewmodels from stock WaW still need work. Actual map testing()
  calls setViewModel("a1_fs_vm_usa_cia_camo_arms"); absent in current mod model dump, NOT interchangeable with
  precached viewmodel_usa_marine_rolledup_arms3_edit. Do not silently pick the latter or a BO2 arm model.
- Added lossless T4 lighting exports in Maps/WorldConverterDumperT4.cpp: .lighting.json (sun + page names),
  .lightgrid.bin (grid rows/entries/palette with trace bytes), .primarylights.json (all ComWorld fields).
  Registered ComWorldDumperT4. Rebuilt native T4 Unlinker twice successfully; work/lighting_source contains real
  Nuketown data: 54 primary lights, five lightmap pairs, 998 rows, 94,331 entries, 20,421 palettes. Sun angles
  [-58,230,1], light .9, ambient .25. `lighting.py` validates/read preserved grid; stage_bridge copies source
  lighting under content_source/lighting and warns that runtime bridge STILL uses flat lighting.
- IDA skill read; new ready T6zm.exe IDA session 1ea9d797 (owned, worker database .i64 alongside game binary).
  AssetViewer session f2da0a7a is adopted/shared, do not terminate. T6 vision parser sub_7D7F10, field table
  0xD3E4F8, 42 entries ending 0xD3E6F0. Old T4 film contrast/brightness/desaturation/lightTint/darkTint NOT
  supported by T6 parser, so blindly copying visions would silently do nothing. T6 uses vc_ vec4 corrections;
  needs measured color-matrix translation. Supported shared fields include r_filmEnable, primary light tweak
  strengths, revive edge fields, sound filter/ringmod, sunFlareTint, postEmissiveBrightening. Captured inventory
  in exec store visionFields but not persistent filesystem yet; re-read table from binary if needed.
- Tests: all 30 unittest tests pass; compileall and
  PowerShell parser check pass. Native log artifacts are under work/. No claim of all-map functional conversion.

### Next work, in order
1. Weapon dependency resolver must fetch missing ORIGINAL assets from StockWawAssets recursively, including
   the missing xanim and map viewmodel. No guessed alternatives. Add namespaces/script tables for model methods.
2. Translate remaining playerAnimType/other enum values against native schemas, audit all custom guns;
   fix multi-root GLTF loader using actual T6 numRootBones support (do not strip skins), handle physics/melee.
3. T4 sound alias flags need decoding from WaW renderer/linker; T6 flags DIFFER. Export SndDriverGlobals curves/
   bus/speaker lookup tables. Resolve streamed sound IWD files and loaded XWMA -> supported PCM without changing
   content. No ffmpeg in PATH; look for installed decoder or Windows Media Foundation. Use official BO2 sound
   bank loader (CSV60 columns), package .sabl/.sabs, namespace alias references in GSC/FX/weapons.
4. WaW add_zombie_weapon takes six args; T6 takes eight, incl upgrade weapon. Translate original registrations,
   include filters, upgrade relationships and wallbuy entities, then remove stock WALL_WEAPON_MAP substitutions.
   Gun assets go to mod.ff; animations MUST have explicit zone entries (weapon has only indirect anim refs);
   FX reference-only before weapons resolves official linker dependencies, map.ff provides original FX.
   Add weapon IPAK read metadata and packaging. Never use successful field build to claim sounds/gameplay work.
5. Vision: translate T4 film math to T6 vc_ matrices based on shader/engine measurements; find actual requested
   visions via stock/rawfile resolution. Current compat visionsetnaked stub only reports; not implemented.
6. Lighting: world UV2 and per-surface lightmap page preservation (merge_surfaces currently ignores page),
   verified T4 two-page -> T6 three-page directional encoding; T4 grid trace -> T6 visibility semantics; source
   primary lights/attenuation lightdefs, sun direction/color, grid palettes. Preserve source light indices when
   adding all 54 lights (current bridge allocates arrays for only two). Current runtime remains flat/unlit.
7. Full rebuild, native roundtrip all asset types, then isolated in-game test. Installed Session8 remains current.


## Session 8 (2026-09-29): wavelet IWI decoder finished; rebuilt/installed, NOT verified in game
- `src/waw2bo2/wavelet.py`: IWI6 formats 6–10 decode to original pixels, then DDS -> existing IWI27 pipeline.
  Integer 2x2 Haar, LSB-first Huffman, optional parent residual, correlated RGB coefficients, raw thin mips,
  shared bitstream across faces/mips. Parent residual does not change the already-uploaded coarse mip.
  RGB has a 4-byte D3D stride with opaque alpha; DDS masks preserve BGR(A) channel order for the IWI swizzle.
- Tables are signature-scanned from the user's `bin/AssetViewer.exe` and validated as 4096-entry prefix lookup
  tables. No proprietary full tables are embedded. Signatures repeat within tables: only a candidate whose
  ENTIRE 16KB is a valid table is accepted; missing/ambiguous tables fail explicitly.
- Decoder measured in AssetViewer: level function 0x4A1940, loader 0x487660, tables 0x548398/0x54C398/0x550398.
  `tools/wavelet_reference.cpp` is an audit-only x86 harness: executes the actual decoder in a private copy of
  its code/table sections. The EXE has no relocations, so the harness relocates four known absolute references
  plus the switch table. It never modifies AssetViewer or game files. Build with vcvarsall x86 + cl /EHsc /O2
  /Fe:work/wavelet/wavelet_reference.exe /link /BASE:0x10000000 (source comments describe scope).
- `tools/audit_wavelet.py`: decoded every wavelet image from mod + stock IWDs, compared ALL mip bytes against
  actual WaW decoder, validated DDS -> IWI27 swizzles/order. 51 source-priority unique images; 52 including
  shadowed copies (`--all-copies`), every comparison exact. Reports: `work/wavelet/audit/report.json` and
  `work/wavelet/audit_all/report.json`. All five layouts synthetic-tested; real inventory has formats 6–9.
- `IwdRecovery` resolves first matching IWD (mod before stock); no stale decoded DDS cache trusted. Only missing
  DDSs are recovered. `t6bridge.stage_effects` no longer skips wavelet dependencies; `stage_images` recovers them
  too, and records source archive/entry/table binary in each image's `wavelet` report field. Intermediate DDSs
  go in the project's `wavelet_src/images`. Automatic decoder availability does NOT opt in to source FX.
- CLI locates Mod Tools even without `--waw-source-fx`. `run_bridge.ps1 -NoWawSourceFx` still passes Mod Tools
  path for the decoder, but only compiles source-only effects when the source-FX opt-in is enabled.
- Full `tools/run_bridge.ps1` completed using official BO2 linker for gameplay mod.ff/scripts, bridge exit 0,
  zero distinct missing assets. Packaged to `%LOCALAPPDATA%/Plutonium/storage/t6/mods/zm_nuketown_waw`.
  95 effects now convert (was 88); both beam images recovered; no UNSUPPORTED_IMAGE wavelet warnings or stage
  errors. The old official mod.ff warnings remain: missing maps/mod.d3dbsp and zmb_test sound banks; it creates
  mod.ff despite these, as before. Do not describe that linker log as warning-free.
- T6 zone dumped into `work/wavelet/t6_roundtrip`: ALL 95 FX match staged runtime fields (only `_source`
  provenance excluded); beam images roundtrip DDS -> IWI27 byte-exact. Reproduce/report with
  `tools/fx/verify_roundtrip.py <staged project> <dump> --report <json>`.
- Tests: `python -m unittest discover -s tests -p test_wavelet.py` (4 pass), test_fx*.py (13), test_wawsource.py (3).
  Synthetic streams cover formats 6–10, escapes, side-channel, odd parity, RGB alpha, rectangular raw mips,
  shared cube stream, truncation, unsupported volume/no-parent mip, IWD priority/stale-cache rejection.
- Limits: wavelet volume textures explicitly unsupported; dimensions must be powers of two; 16MP safety cap;
  2D NOMIPMAPS wavelets lacking a parent are refused. Unknown executable tables fail explicitly.
- NEXT: check these effects in-game (no launch this session); 15 genuinely absent FX still reported/skipped.
  Then continue real WaW weapon/sound/xanim conversion per GUIDELINES, not stock replacements. Wavelet work
  does not claim to solve the full converter or the existing flat-lighting/source-content limitations.

## Session 7 (2026-09-29): effects compiled from the WaW Mod Tools sources (not verified in game)
- Mod Tools layout here: `wawModTools/` has `bin/` (linker_pc.exe, EffectsEd3, converter...), but `raw/` (2612
  `raw/fx/*.efx`), `zone_source/`, `map_source/` are in the WaW install. There is no `raw/images`.
- `src/waw2bo2/wawsource.py` (GUIDELINES 27 source 4): an effect that no compiled WaW zone defines but that has a
  `raw/fx/<name>.efx` is compiled by **WaW's own linker** into a scratch fastfile, then dumped with the T4 unlinker.
  No .efx compiler is reimplemented. Validation: recompiled stock effects dump identical to the shipped ones
  (fx_zombie_wire_spark, fx_smoke_smolder_md_gry). fx_fire_detail_fade_14 differs only in efPriority, which is
  stored in the .efx: oki3 shipped an older revision of the source.
- Linker workspace (`<dumps>/linker`): cwd `bin/`, junctions `raw` -> the raw dir and `main` -> WaW main
  (fileSysCheck.cfg is inside main/iw_00.iwd), `zone_source/<zone>.csv`, output `zone/english/<zone>.ff`.
  After an UNRECOVERABLE ERROR the linker waits for a key press (0 CPU), so calls have a timeout.
- CLI: `stage-bridge --waw-source-fx` (opt-in) `--waw-mod-tools` (default: found in --waw-root or ./wawModTools)
  `--waw-source-dumps`. `run_bridge.ps1` passes them by default (user asked); `-NoWawSourceFx` turns it off.
  Reported `WAW_SOURCE_ASSET fx <name>`.
- Result: 7 more effects: env/fire/fx_fire_campfire_small, fx_fire_blown_md_blk_smk_distant_w,
  env/light/fx_glow_emergency_red_blink, env/smoke/fx_smoke_smolder_lg_gry, fx_smoke_wood_chimney_med,
  env/weather/fx_snow_blizzard_intense, explosions/fx_default_explosion. 88 converted (68 script-loaded), bridge
  link clean, round trip exact (88/88). tests/test_wawsource.py.
- Still missing: 15 absent everywhere (mapper's never-compiled additions, `path/to/fx/name` from a template, and
  `sniper/fx_smoke_ambiance_indoor`: UGX's preset path is wrong; the source is `maps/sniper/...`, so WaW loaded
  nothing and we do not guess). 7 fail on wavelet IWIs (zombie eyes, lamp, lightray...): the Mod Tools have no
  source image for fxt_light_spot_beam/spotlight_beam, so a WaW wavelet IWI decoder is the only path.

## Session 6 (2026-09-29): no stock BO2 substitutes (user directive, overrides Session 5's FX fallback default)
The user restated GUIDELINES.MD: port the WaW content itself; use stock BO2 only when WaW genuinely cannot supply
it; anything that cannot be found in WaW is removed and reported, never faked with BO2 assets.
- FX fallback is now OFF by default: `run_bridge.ps1 -FxFallback` opts in (the old `-NoFxFallback` is gone).
  Default builds play no stock BO2 effect for a WaW one; WaW effects are converted (section below) and the
  fallback only ever sees effects that did not convert. `compat/overrides/nuketown.json` (the map's custom effects
  -> stock BO2) was deleted.
- The "storm": the map never defines its rain FX (`level._effect["rain_N"]`) and its lightning exploders
  10000-10005 do not exist, so there is nothing to port for rain/lightning visuals. With the fallback off, nothing
  fake plays. What remains is WaW's own data: the `setVolFog` values of `lightning_normal`/`lightning_flash`
  (also the map's base fog), plus reported `SetSunLight` and unconverted thunder/rain sound aliases. The storm's
  ember effect (`env/weather/fx_ash_embers_light_nuke`, loaded by `ugx_weather_handle`) and `env/light_nuke` are
  in no zone of the WaW install (`WAW_FX_ABSENT`), so they stay out. The map's other ember effects
  (`fx_ash_embers_light`, `embers_roof`) are real and converted.
- The "Template maps/Nuketown Remastered 1.2" folder the user added is byte-identical to the installed mod
  (only missingasset.csv differs; WaW writes it at runtime: 5 materials + 148 xanims missing, no FX).
  `nuketown.iwd` also carries raw WaW weapon files, a soundaliases CSV and WAVs (sources for the sound/weapon ports).

### WaW FX conversion (implemented this session; in-game result NOT verified yet)
Compiled-to-compiled, no editor re-authoring, no stock substitutes:
- T4 dumper `vendor/OpenAssetTools/src/ObjWriting/Game/T4/Fx/FxJsonDumperT4.cpp` writes every FxEffectDef as
  `fx/<name>.w2bfx.json` (all fields, samples, raw flags; assets by name). Added to ObjWriting.vcxproj by hand.
- T6 dumper `vendor/OpenAssetToolsT6/src/ObjWriting/Game/T6/Fx/FxJsonDumperT6.cpp` (same schema + T6 fields) and
  T6 loader `.../ObjLoading/Game/T6/Fx/JsonLoaderFxT6.cpp` (bridge Linker builds FxEffectDef from the JSON; loads
  materials/models/referenced fx as dependencies). Both added to the vcxproj files by hand.
- `src/waw2bo2/fx.py` translates. Every rule was measured (see module doc; tools/fx/efx_pair.py pairs stock BO2
  compiled FX with raw/fx .efx sources): element flags shift by one bit from useCollision on; WaW element types are
  0 billboard, 1 oriented, 2 tail, 3 line, 4 trail, 5 cloud, 6 model, 7 omni, 8 spot, 9 sound, 10 decal, 11 runner
  (the vendored T4 header's enum NAMES are wrong, the zone conditions are right); colours BGRA->RGBA; atlas
  `entryCount | 1<<9`; effect flags derived (lit 1, omni 0x10, model 0x20, sound 0x40, runner 0x80, trail 0x100);
  totalSize/msecNonLoopingLife recomputed (formulas exact on stock data). Spot lights are refused (need GfxLightDef).
  Output names `waw/<name>` (stock BO2 zones hold same-named, different effects -> override errors otherwise);
  scripts reach them through `level.waw2bo2_fx` (`_waw2bo2_assets.gsc`).
- `src/waw2bo2/fxmaterials.py`: WaW effect techset features (zfeather/falloff/eyeoffset/outdoor/add/particle cloud/
  distortion) matched to BO2 effect techsets whose features are read from BO2 `raw/techsets` + `raw/techniques`
  (shader inputs); donor = stock material of that techset, blend state/constants/atlas/images from WaW. Reported
  drops: nofog (every BO2 effect technique fogs), zfeather on distortion. Materials are written as `waw_fx/<name>`.
- `src/waw2bo2/wawassets.py`: assets the map only references (e.g. gfx_fxt_light_flare2 in mod.ff) are stock WaW
  content from other WaW zones; indexed once (`waw_zone_dumps/stock/stock_index.json`) and dumped on demand;
  reported `WAW_STOCK_ASSET`.
- Findings: 90 script-loaded FX names; 68 exist in WaW (map, nuketown_patch, mod, common, code_post_gfx, plus
  13 found in stock WaW zones through wawassets). The other 22 exist nowhere in the WaW install: reported
  `WAW_FX_ABSENT` and skipped (both storm effects are among them).

### WaW Mod Tools (superseded by Session 7: now used for effect sources)
- Location: `C:\WawConverter\wawModTools` (the part of the installer that goes into the project: bin, deffiles,
  docs, map_source, model_export, raw, ...). The rest of the Mod Tools install went into the WaW install folder.
- The user wants the tool to take the WaW install and the Mod Tools as inputs (and eventually find them itself) so
  assets that are not compiled into a map can be ported. This also covers stock and DLC WaW maps later.
  GUIDELINES.MD sections 27-31 (added this session) describe the lookup order, the asset classes and reporting
  (`WAW_STOCK_ASSET` / `WAW_SOURCE_ASSET`); follow them.
- Today only `--waw-root` exists (stock WaW zones, wawassets.py). Still to do: `--waw-mod-tools` plus
  auto-location, and readers for the raw sources: `.efx` (an iwfx text reader that produces the same
  sampled data WaW's linker does), raw images, materials, weapon files and sound aliases.

### UGX Easy FX (researched after the build; nothing to implement yet)
- UGX Easy FX (treminaor, UGX-Mods) is ONLY a script, `maps/ugx_easy_fx.gsc`: no .efx, images or materials
  (UGX community wiki: github.com/UGX-Mods/community-wiki, Modding/World-at-War-Modtools/Script/Easy-FX.html).
  Every map ships its own copy. `fx_setup()` loadfx's a list of aliases; `fx_start()` plays them at script_structs
  `targetname "fx"`, `script_noteworthy` = alias, `script_string` = stationary / stationary_loop / use / touch /
  use_loop / touch_loop (loop variants read `speed`). Stationary structs spawn one `tag_origin` script_model each
  and PlayFxOnTag on it.
- The converter already supports it generically: the script ports like any other map script (gscport), and all
  155 `fx` structs of this map survive into BO2 `BSP/entities.json`. Every effect the map shipped converts.
- Why effects are still missing: UGX's built-in presets (fire1-6, smoke1-4, light1-2, electric1-2, bugs1,
  explosion1-2) point at stock WaW effect SOURCES in the WaW Mod Tools `raw/fx`; a mapper must compile each into
  their zone ("add to mapname_patch.csv"). The Nuketown mapper compiled only some. The mapper's own "User-Additions"
  (steamred, steamyellow(2), red_ray, triangle_ray, lightning1 = env/light_nuke, green1 = env/green_gew_drip,
  purplesmoke, lava_pop, hell_smoke, fire_pipe) were never compiled into the released mod, and no UGX download
  has them.
- What WaW itself showed: loadfx of an absent effect returns WaW's default `misc/missing_fx` (code_post_gfx.ff: one
  one-shot sprite, material gfx_fxt_misc_missing_fx, 1.5 s). So ~40 placed structs showed a brief "missing FX"
  placeholder at map start in WaW. Aliases fire_long, nuke, green_gew and fx_ash_embers_light_nuke are placed but
  never defined by any script (server or client), so in WaW they are a script runtime error and nothing plays.
  The port plays nothing for both cases (`waw_loadfx` returns undefined and reports; `waw_playfx*` skip undefined).
- Struct usage in this map: candle 33, fire1 27, godray1 16, fire_long 12, wire1 11, triangle_ray 8, steamyellow2 8,
  green1 5, lightning1 5, red_ray 5, steamred 5, emb1 4, hell_portal 4, purplesmoke 2, smoke5 2, and 1 each of
  fx_ash_embers_light_nuke, fire2, nuke, green_gew, hell_smoke, lava_pop, hell_fire, fire_pipe (looping).
- Possible general feature (discussed with the user, deferred until another map needs it): an OPT-IN, reported
  source mode that reads stock WaW effect sources (`raw/fx/*.efx`) from an installed WaW Mod Tools, so presets a
  map references but never compiled can be ported. It needs a new .efx reader, and it deliberately adds effects the
  WaW build did not show. For Nuketown it would only recover smoke5 (sniper/fx_smoke_ambiance_indoor, 2 structs).
  The WaW Mod Tools are not installed on this machine (no `raw` folder in the WaW install).
- Risk to watch in game: `fx_start` spawns ~150 script_models (one per stationary struct). Earlier builds hit
  `G_Spawn: no free entities`. If it returns, Easy FX is the first suspect; the fix belongs in the converter
  (generic), not in the map script.
- Build result (packaged, not yet tested in game): 81 WaW effects converted (61 script-loaded), bridge link clean,
  round trip exact (T6 Unlinker dump of the built zone == staged JSON for all 81). 7 effects skipped as
  UNSUPPORTED_FX because they use WaW *wavelet*-compressed IWIs (IWI v6 formats 6-10; OAT cannot decode them
  either): fxt_light_spot_beam (stock WaW iw_03.iwd) and spotlight_beam (nuketown.iwd). This drops the zombie eye
  glow (misc/fx_zombie_eye_single), blue_eyes, claymore laser, lightray, hanging lamp. Next: a wavelet IWI decoder.
- run_bridge: companions now nuketown_patch, mod, common, code_post_gfx (dump marker .dump_v3 adds fx); stage gets
  --waw-root/--t4-unlinker/--waw-stock-dumps. tests/test_fx.py covers the rules.
- Next: in-game check of the effects (generator sparks, fire, embers of `embers_roof`...), client-side (csc) FX,
  WaW sound alias + audio conversion, and WaW weapons (then drop `entities.WALL_WEAPON_MAP`).
- Other stock BO2 content still in the build (to be replaced by ported WaW content where WaW has it): wall-buy
  weapons (`entities.WALL_WEAPON_MAP`), zm_nuked sound banks / null music aliases, traversal anims
  (`zm_mantle_over_40`, zm_nuked ASD aliases), skybox material shell (`skybox_dlc0_zm_nuketown` with the WaW cube
  image), zm_test zbarrier asset.

## Session 5 status (2026-09-29): gameplay script translation (read with Session 4 below)

Read `GUIDELINES.MD` first: a general converter, no map-specific hacks, no silent substitution; fallbacks must be
explicit, opt-in and reported.

### WaW GSC -> BO2 translator (new, `src/waw2bo2/`)
- `gsc.py`: lossless GSC tokenizer (each token keeps preceding whitespace/comments) + parser (includes, functions,
  animtree) + `references()` (calls, `::f` pointers, `path::f`, method vs function).
- `t6api.py`: what BO2 provides. Builtins come from `t6zm.exe` itself: 24-byte records
  `{fn; 0; name*; id; minArgs; maxArgs}` in contiguous tables; tables are classified server/client and
  function/method by voting with names only one side/style of the stock BO2 raw scripts uses. Plus every stock BO2
  script's functions, plus (with `--t6-unlinker`) the asset lists of code_pre/post_gfx_zm, common_zm, patch_zm.
  Cached in `<stage>/t6api_cache.json` (CACHE_VERSION; delete it to rebuild).
- `gscport.py`: classifies WaW scripts as *core* (WaW framework BO2's _zm replaces: `compat/gsc_api.json`
  core_scripts/core_patterns/template_scripts) or *portable* (the map's own + non-core stock such as `maps\_weather`),
  ports portable ones recursively to `maps/mp/waw/<path>.gsc`, and resolves every reference: local -> included
  ported script -> BO2 counterpart script function -> compat shim -> T6 builtin (arity + method/function checked) ->
  single-function *extraction* of the WaW framework function (from the map's override, else stock) into
  `maps/mp/waw/_waw2bo2_core.gsc` -> reported stub in `_waw2bo2_stubs.gsc` (prints `WAW2BO2 UNSUPPORTED GSC ...`).
  Also: `(a.b).size` paren fix, `//...\` comment fix (T6 splices lines), extra builtin args dropped, WaW animtrees BO2
  lacks are neutralised (`%anim` -> string, anim builtins -> compat `waw_anim_*`, reported UNSUPPORTED_XANIM).
  The map main `maps\<map>` is split at `maps\_zombiemode::main()` into `waw_main_pre()`/`waw_main_post()`;
  `hook_bo2_main` inserts `maps\mp\waw\<map>::waw_main_pre();` before `maps\mp\zombies\_zm::init();` and
  `level thread ...::waw_main_post();` at the end of the BO2 main (idempotent).
- `compat/_waw2bo2_compat.gsc` (generic WaW API on BO2): loadfx/playfx family through the converted-asset table
  (`_waw2bo2_assets.gsc`, `level.waw2bo2_fx[waw name] = bo2 name`; empty until FX conversion exists -> FX report
  `WAW2BO2 UNSUPPORTED FX <name>` and are skipped), setVolFog 8->18 args, setClientDvar(s) (renderer/cg/ui dvars
  reported, others setdvar), playSound(alias, notify) -> playsoundwithnotify, visionsetnaked (reported: WaW visions
  not converted), SetSunLight/ResetSunLight (reported: no T6 server equivalent), and the power bridge
  (WaW flag `electricity_on` <-> BO2 `power_on` + perk unpause).
- `gscport.link_check()` resolves every reference of the generated scripts like the game's linker; failures are
  stage errors. **The BO2 mod-tools compiler does not check calls at all**: unresolved functions / wrong arity
  only fail at map load (`COM_ERROR ... script error(s)` in console.log, then the game drops to the menu and the
  menu Lua may throw an unrelated LUI error). First in-game run found 2 such errors (method-vs-function); fixed.
- Staging: `t6bridge.port_scripts` (before model staging; models the scripts precache/setmodel are added to the zone
  when a WaW dump has them), `map_scripts` includes `maps/mp/waw/*.gsc`. `run_bridge.ps1` now dumps mod.ff rawfiles
  (`work/waw_zone_dumps/mod_rawfiles`), the stock WaW scripts (`.../stock_scripts`, from common.ff +
  nazi_zombie_*.ff) and mod.ff/common.ff xmodels (`.dump_v2` marker), and passes `--waw-script-root`,
  `--waw-stock-scripts`, `--t6-unlinker`.
- Stage report: `bridge_stage.report.json` `scripts` (ported, extracted, unsupported with locations, rewrites, fx,
  animtrees, core_overrides).

### Status of the priorities (nothing below is user-verified)
1. Scripts: 31 scripts ported, all compile and link-check. Bunker (`maps/mp/waw/_access_bunker.gsc`) translates
   almost verbatim; needs in-game test (grenade the 3 `bunker_generator` script_models; open question: whether BO2
   delivers radius damage to our script_models, which may lack collision). Remaining stubs: dlc3_code framework
   calls, `regret_purchase`, `make_crawler` (uses anims), `has_collectible`, `forceviewmodelanimation`.
   The WaW map overrides many framework scripts (_zombiemode_perks etc.); those changes are not ported (reported).
   `trem_hintstrings` draws hints through a WaW menu + client dvars: hints will not show (triggers still work).
2. FX: not started beyond the reporting compat. Findings: the map's rain FX (`level._effect["rain_N"]`) are never
   defined by its scripts; the lightning exploders 10000-10005 do not exist in its entities or createfx (both
   createfx files are empty), so the WaW "storm" is SetSunLight/setVolFog flashes + thunder sounds + ash embers
   (`ugx_weather_handle`). BO2 raw has 2841 `.efx` sources (iwfx text v2) that the official Linker compiles: the
   real conversion path is WaW FxEffectDef -> iwfx text -> official Linker into mod.ff (the vendored T4 OAT has no
   FX dumper yet). Stock BO2 FX already loaded: 123 in common_zm (`work/common_zm_fx.txt`). User asked for a short-term
   WaW->stock FX mapping; per GUIDELINES it must be an explicit opt-in, reported fallback (not implemented yet).
   **Implemented later this session**: opt-in fallback `stage-bridge --fx-fallback` (`fxmap.py`;
   `compat/fx_fallback.json` = stock WaW effects, `compat/overrides/<waw map>.json` = that map's custom effects).
   Each use is reported `FX_FALLBACK ...`; the rest `UNSUPPORTED_FX`. BO2 effects not in an always-loaded zone go
   to `zone_raw/<project>/mod_extra.zone` -> appended to mod.zone (official Linker compiles them from raw/fx; 13
   linked fine). `run_bridge.ps1` passes `--fx-fallback` by default (the user asked); `-NoFxFallback` disables it.
3. Skybox: cause found (not verified): T6 `mc_skycubemaphdr` reads an HDR intensity from alpha (stock sky DXT5, mean
   alpha ~0.9); the WaW cube (`berlin2_ft`, 64x64 X8R8G8B8, genuinely that small in iw_00.iwd) has no alpha and became a
   24-bit RGB IWI. `_sky_with_hdr_alpha` writes `sky_src/<img>_t6sky.dds` as A8R8G8B8 with alpha
   `SKY_HDR_ALPHA` = 255/6 (tune after testing: too dark -> raise, blown out -> lower).
4. Weapons: the WaW weapon models are in mod.ff (now dumped: `work/waw_zone_dumps/mod/xmodel`, e.g.
   `a1_fs_m14_viewmodel`, `viewmodel_bf4_deagle`). Plan: dump WaW weapon defs (OAT T4 has a Weapon infostring
   writer) + viewmodel xanims (no T4 xanim dumper yet) + sound aliases -> translate WEAPONFILE fields to T6 -> link
   through the OAT bridge (map zone; the official Linker cannot read our GLTF models) and register them in the
   ported scripts' include_weapon calls. `entities.WALL_WEAPON_MAP` (stock substitutes) must then go away.
5. Truck: `nuketown_big_truck` (the model `truck_replace` swaps in) was missing from the zone; it is now staged from
   the mod.ff dump, so the script swap may already fix the truck. Magenta: not started.

Entity budget: the bridge turned every spawns.json point into 11 MP spawn classes (201 gentities); with the WaW
scripts running the map hit `G_Spawn: no free entities`. `entities.zombies_spawns` keeps one FFA point only.

## Session 4 status (2026-09-29): read this first (supersedes the older sections where they disagree)

Build + test loop: `powershell -ExecutionPolicy Bypass -File C:\WawConverter\tools\run_bridge.ps1`, which rebuilds and
packages to `%LOCALAPPDATA%\Plutonium\storage\t6\mods\zm_nuketown_waw`. Close `plutonium-bootstrapper-win32` first
(it locks the package). Launch:
`%LOCALAPPDATA%\Plutonium\bin\plutonium-bootstrapper-win32.exe t6zm "<BO2>" -lan +set fs_game mods/zm_nuketown_waw +devmap zm_nuketown_waw`.
Logs: `%LOCALAPPDATA%\Plutonium\console.log`. Crash dumps: `%LOCALAPPDATA%\Plutonium\crashdumps`
(read them with `python tools\mdmp.py <dmp>` and `python tools\mdis.py <dmp> <hexaddr>...`: context at exception stream +8+152;
capstone is installed; the game image is mapped at 0x400000 inside plutonium-bootstrapper).
The user tests in game. Computer-use access to the game was denied, so never claim a fix works until they confirm it.

Builds: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe <proj> /p:Configuration=Release /p:Platform=Win32`.
Build .vcxproj files one at a time (T6: `build/src/Linker/Linker.vcxproj`, `build/src/Unlinker/Unlinker.vcxproj`;
T4: `build/src/UnlinkerCli/UnlinkerCli.vcxproj`). Under git-bash, set `MSYS_NO_PATHCONV=1`.
`*.cpp.template` files are not regenerated by MSBuild: run
`build/buildtools/Release_x86/RawTemplater.exe -o <tmp> <template>` from `build/src/ObjLoading`, then copy the
`Game/<G>/...` outputs into `build/src/ObjLoading/Game/<G>/...`.
Heredocs in the Bash tool mangle backslash escapes in C++ string literals and markdown; use the Write/Edit tools.

### Confirmed working in game (user-verified)
Map loads and spawns. Geometry renders at distance. Red tint gone (the WaW `*reflection_probe0` was a solid-red
placeholder; now a synthesized near-black 4x4 cube). Collision fixed: BSPCreator deduped collision verts with
generated per-face normals, which overflowed uint16 indices; it now dedups on position only and errors on overflow.
Verify with the Unlinker diagnostic `tri winding` census, which must equal the WaW source (up 10924 / down 25634 /
steep 41490).

### Done this session, NOT yet verified in game
- Running-around crash (0x12D9C75E: memcpy past a per-frame 32-byte-vertex dynamic VB, game fn 0x77C6C0, whose caller
  only warns (code 0x25) on overflow). Root cause: the OAT xmodel loader gave no rigid vert lists to (a) glTFs without
  skins (1029 models) and (b) one-bone-per-vertex skins (its rigid check was `maxWeightCount == 0`). Both are now rigid
  (LoaderXModel.cpp.template), so the game no longer CPU-skins them every frame.
  World surfaces are also capped at stock size (fbx.MAX_MESH_TRIS/VERTS = 1536).
- Path nodes: the T4 dumper writes `gameworldsp` -> `<map>.paths.json` (run_bridge step 0 includes it). `paths.py`:
  T4->T6 node types (+1 from CONCEALMENT_STAND), clearance flags sf|0x300000 and link flags 0x28, pathVis copied
  verbatim (n*(n-1)/8 in both games), and traversal mapping (wall_hop -> stock `zm_mantle_over_40`; jumps -> generated
  `maps/mp/animscripts/traverse/waw2bo2_jump_*.gsc` calling `dosimpletraverse` with zm_nuked ASD aliases).
  `GameWorldMpLinker` links nodes/links/vis plus a kd-tree built like stock (split x/y at the midpoint; child[0] =
  below; child[1] follows its parent). Script strings are registered through the Zone (BSPLinker now takes Zone&).
  node_* entities are kept in the ents string in PathData order, as stock does.
  Round-trip verified with the Unlinker `.paths.txt` diagnostic.
- Spawning: WaW zombie_spawner actors now also emit `spawn_location` / `riser_location` script_structs
  (BO2 zonemgr). console.log showed zombies spawning ("W2B SPAWN" debug prints from the template main).
- Only `lightmap*` images are inline (loadDef) now. An inline mipped cube probe or R8 `$outdoor` crashed the zone
  load (E_INVALIDARG).
- Materials: `techsets.index_donors` skips technique sets used only by animated stock materials (scrolling UVs).

### Diagnostics (T6 Unlinker, `--include-assets mapents,clipmap,gfxworld,gameworldmp`)
Writes `<map>.clipmap.txt` (incl. winding census), `.gfxworld.txt` (surface maxima, image load info), and
`.paths.txt` next to `.ents`. Stock references are in `work/clipdiag/stock3`, `stock5`, `stockpaths`.

### Open items, in the user's priority order (their last message)
1. **Gameplay scripts.** The WaW map's custom scripts live in `mods/Nuketown Remastered 1.2/nuketown.iwd`
   (56 GSC/CSC, already extracted to `work/waw_iwd_scripts`). The map main is `maps/nuketown.gsc`; it threads
   `_access_bunker` (bunker opens after the 3 `bunker_generator` ents take grenade damage), `_nt_power`,
   `_rake_trap`, `_nt_pap_access`, `_blst_nuketown_perks`, `_electric_water_trap`, `_zipline`, `_buildable_shield`,
   `_watershoot`, `_nt_moving_fire`, `_nt_ee`, `ugx_weather_handle` + `_weather` (rain/lightning), `lava_damage`,
   `set_bunker_flag`, and more. Planned generic approach (not started): `gscport.py` translates WaW GSC to BO2
   (include/namespace map: maps\_utility->maps\mp\_utility, maps\_zombiemode_utility->maps\mp\zombies\_zm_utility,
   _zombiemode_X->_zm_X, _laststand->_zm_laststand; custom maps\X -> maps\mp\waw\X). A `waw2bo2_compat.gsc` shims
   WaW-only builtins, driven by BO2 mod-tools Linker compile errors. The WaW map main is split at
   `maps\_zombiemode::main()` into pre/post functions called from the BO2 main around `maps\mp\zombies\_zm::init()`.
   Framework-only lines (level.DLC3.*, include_weapons, zone init) are dropped. Ported scripts must be added to
   `t6bridge.map_scripts` so they compile and link. `_access_bunker.gsc` is nearly valid BO2 GSC already; start there.
2. **FX** (lightning storm, rain, generator sparks, embers...). BO2 errors on unknown loadfx names. Short term: map WaW
   FX names to stock BO2 FX from common_zm/zm_nuked/patch_zm (list them with `Unlinker --list`; the last PowerShell
   attempt failed on a pipe syntax error). Long term: convert WaW FxEffectDef -> T6 (the bridge has no T6 FX loader).
   WaW lightning uses SetSunLight/setVolFog, which need BO2 equivalents.
3. **Skybox** does not work (sky shows black). Staged from BO2 `skybox_dlc0_zm_nuketown` with a WaW cubemap colorMap.
4. **Weapons.** 74 WaW custom weapons in mod.ff were replaced by BO2 ones (entities.WALL_WEAPON_MAP); porting the real
   weapons (models, anims, sounds, weapon files) is a large task.
5. **Truck** (`bo2_mlv_veh_t6_dlc_movingtruck_zmb_low`, and the WaW script swaps in `nuketown_big_truck`) renders with
   scrambled UVs. The glTF vertex/UV pairing checks out (texel-density test); the next step is comparing with WaW.
6. Magenta textures (missing images), real lightmaps, layered blending (vd1), and the long-term official
   cod9map64 + arclight64 + Linker route (needs a WaW BSP -> .map decompiler; the user deferred it: "not yet").
7. Doors/debris buying: not yet tested by the user. The template main has debug prints (W2B SPAWN/DAMAGE); remove them
   once gameplay is stable.

Concurrent editing: another session or the user may also edit these files. Re-read before editing and keep their
changes.

## Session 3 status (2026-09-28) — read this first

`tools\run_bridge.ps1` now runs end to end and installs a Plutonium mod at
`%LOCALAPPDATA%\Plutonium\storage\t6\mods\zm_nuketown_waw` (map ff + ipak, mod.ff, sound banks).
BO2 loads the map, initialises the game and links all scripts. **Players cannot spawn yet**:
~25 "potential infinite loop in script" warnings, source not yet identified.
The log is `%LOCALAPPDATA%\Plutonium\console.log`. The user's global scripts in
`storage\t6\scripts\zm` also run and are suspects; so are zone/spawn setup.

Pipeline: WaW dumps (the T4 dumper now writes clip v3 with brushes, submodels and static-model collision
tris) → stage (world FBX with tangents, brushes.json, submodels.json, models.json, entities.json/spawns.json,
xmodels, materials, IWI) → mod.ff via the BO2 mod-tools Linker (zm_test template + script include closure)
→ compile map GSC with the mod-tools Linker, then extract the bytecode with the OAT unlinker → OAT T6 bridge link →
package.
The bridge C++ was extended: static models (GfxWorldLinker::LoadXModels), native brushes plus a kd-tree
(convention verified against WaW data), brush submodels (cmodels, gfx brush models), MapEnts triggers, and
collision vertex welding. Linker.exe must be LARGEADDRESSAWARE (run_bridge does it). A rare
nondeterministic access violation remains; bridge-link retries it (not root-caused).
Zone graph: copied from the WaW map GSC (zones.py). Known gaps: WaW path nodes → T6 PathData
not yet converted (zombies can't path); lighting is flat; brush-model entities don't render their own surfaces.

## Session 2 findings (2026-09-28) — read this first

The vendored T6 bridge is an exact match for upstream OpenAssetTools branch
`bsp-compilation-2` @ `95b8c68`. It was verified against the source, not guessed.
Several assumptions above turned out to be wrong:

1. **The bridge never writes a `.d3dbsp`.** `LoaderBSP_T6` links
   gfxworld/clipmap/comworld/gameworldmp/mapents straight into
   `zone_out/<zone>/<zone>.ff`. Step 6 below ("stage the native d3dbsp, then run
   the official Linker") cannot happen with this tool. The only way to get the
   official BO2 tools to own the BSP is a real `.map` source for `cod9map64`.
   That is a different pipeline.
2. **The map name is the zone name.** A zone called `bridge` builds
   `maps/mp/bridge.d3dbsp` and needs `maps/mp/bridge{,_amb,_fx}.gsc` +
   `clientscripts/mp/bridge{,_amb,_fx}.csc`. Use `zm_nuketown_waw` as the zone name.
3. **Technique sets are loaded from `techniquesets/<name>.json` + `shader_bin/{ps,vs}_<name>.cso`**
   (the T6 OAT techset dump format), never from `BO2\raw\techsets\*.techset`.
   Stock zm_nuked.ff *includes* its world techsets (it does not reference them), so we dump them
   with the T6 Unlinker and include them too.
4. **Every translated material used `trivial_2d_9zzq26w5`.** That is a 2D/HUD
   technique with depth test off. It came from the single template in `materials.py`. Even
   if it had resolved, the world would have rendered wrong. Replaced with per-material
   selection (`techsets.py`). Each WaW techset (`wc_l_sm_r0c0n0s0`, `l_sm_r0c0_b1c1`, ...) maps
   to the closest stock zm_nuked T6 techset (`wpc_lit_sm_r0c0n0s0_1zzj1138`,
   `lit_sm_r0c0_b1c1n1x1`, ...). The material is cloned from a stock BO2 *donor* material
   that uses that techset, and only the images are swapped in. 209/213 WaW world techsets map.
   Unmapped: `l_(h)sm_r0c0_b1c1_b2c2_b3c3`, `wc_sky`. Every degradation is recorded in
   `bridge_stage.report.json`.
5. **Images are loaded ONLY as `images/<name>.iwi` (IWI v27).** DDS files and
   aliases in the search path are ignored, which is why `lightmap0_secondary`
   could never be found. `iwi.py` converts DDS→IWI27. It is validated against a Python
   mirror of `iwi::LoadIwi27` and handles BGRA→RGBA swizzle, cubemaps, luminance and DXT.
   The linker also hard-requires `reflection_probe0` and `$outdoor`.
6. **`*` materials**: OAT looks for `materials/generated/_<name up to '('>.json`.
   The old translator wrote them to `materials/_<name>.json`.
7. **Search paths are roots**, not asset folders (`<root>/images/...`). Passing
   `final_stage\images` or `BO2\raw\techsets` as roots does nothing.
   `bridge-link` now passes `--asset-search-path` = project root + stock dump only.
   That replaces OAT's defaults, so `BO2\raw` cannot shadow anything.
   `fxanim_props.atr` is copied into the project instead.
8. Code images use OAT references (`,$identitynormalmap`), the same as stock zm_nuked.

### How to run (new)

```powershell
powershell -ExecutionPolicy Bypass -File C:\WawConverter\tools\run_bridge.ps1
```

This runs (1) the T6 Unlinker on `zone\all\zm_nuked.ff` for `material,techniqueset`
→ `work\stock_t6_dump`, (2) `waw2bo2.cli stage-bridge` and (3) `waw2bo2.cli bridge-link`,
logging to `work\final_stage\zm_nuketown_waw_bridge.log`. Staging is fail-closed:
it exits non-zero on any missing source material, image, techset or shader.

### Still open / unverified

- Nothing has been run on Windows yet. The Python was only tested against staged samples.
- Images go into an ipak (`>ipak,zm_nuketown_waw` + `>level.ipak_read`) because
  the bridge's image loader marks every image as streamed. It is unverified at runtime.
- The WaW lightmap and probe are used as the single T6 lightmap/probe. The encodings differ,
  so expect wrong lighting even if it links. `BSP/entities.json` and `spawns.json` are not
  generated yet, so default entities and spawns at 0,0,0 are used.
- Static models: the bridge loads xmodels by name. XModel conversion is not staged yet.
- Scripts are the zm_test boilerplate GSC source. Whether BO2 accepts uncompiled
  GSC inside the ff is unverified.
- Decals get their donor's sort key and polygon offset, so decals may z-fight.

## Objective

Build a converter that accepts a fully compiled World at War custom map and produces a working Black Ops II PC mod/zone. The official BO2 mod tools must be used wherever possible, especially for final BSP/zone compilation and linking.

Input map:

```text
C:\Users\thrif\AppData\Local\Activision\CoDWaW\mods\Nuketown Remastered 1.2
```

BO2 installation:

```text
C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops II
```

BO2 boilerplate mod:

```text
C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops II\mods\zm_test
```

WaW installation/search path:

```text
C:\Program Files (x86)\Steam\steamapps\common\Call of Duty World at War\main
```

## Current implementation

Workspace:

```text
C:\WawConverter
```

Python package:

```text
C:\WawConverter\src\waw2bo2
```

Important modules:

- `world.py`: reads custom OAT WaW gfx/clip binary dumps.
- `fbx.py`: writes intermediate geometry/collision FBX.
- `materials.py`: translates WaW/T4 material JSON into T6-style material JSON, preserving material path hierarchy.
- `cli.py`: extraction, inspection, FBX, material, static-model, conversion, and official-build commands.

OpenAssetTools source/fork:

```text
C:\WawConverter\vendor\OpenAssetTools
```

It contains custom T4 dumpers producing:

- `W2BSP001` gfx dumps.
- `W2CLIP002` collision dumps.

Clean T6 OAT bridge:

```text
C:\WawConverter\vendor\OpenAssetToolsT6\build\bin\Release_x86\Linker.exe
```

This is currently being used only as an intermediate FBX → native T6 BSP bridge. The official BO2 tools remain the final authority.

## Successful extraction evidence

Extraction command used:

```powershell
& 'C:\WawConverter\vendor\OpenAssetTools\build\bin\Release_x86\Unlinker.exe' `
  --no-color `
  --search-path 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty World at War\main' `
  --image-format DDS `
  --model-format GLTF `
  --include-assets 'gfxworld,clipmap,mapents,xmodel,material,image' `
  --output-folder 'C:\WawConverter\work\final_stage' `
  'nuketown.ff'
```

Current extracted counts:

- 441,727 render vertices.
- 929,946 indices / 309,982 triangles.
- 15,373 surfaces.
- 669 unique materials.
- 3,842 static-model placements.
- 391 unique static models, all 391 available in `model_export`.
- 52,359 collision vertices.
- 78,048 collision triangles.
- 16,650 collision brushes.
- 924 DDS images.
- 1,369 GLTF model files.

Static-model report:

```text
C:\WawConverter\work\static_model_report.json
```

It reports 3,842 placements, 391 unique models, and no missing models.

## Current generated staging tree

```text
C:\WawConverter\work\final_stage
```

Important files/directories:

```text
waw2bo2\maps\nuketown.d3dbsp.gfx.bin
waw2bo2\maps\nuketown.d3dbsp.clip.bin
maps\nuketown.d3dbsp.ents
materials\
images\
model_export\
zone_raw\bridge\BSP\map_gfx.fbx
zone_raw\bridge\BSP\map_col.fbx
zone_raw\bridge\materials\
zone_raw\bridge\images\
zone_source\bridge.zone
```

The collision FBX must include a zero UV channel. Without UVs, the T6 bridge previously crashed; with UVs it reaches normal linker diagnostics.

The bridge zone is intentionally minimal:

```text
>game,T6
>map,zm
>level.@00012d3c,0
```

## Official BO2 tool verification

The official BO2 compiler works on the untouched boilerplate map:

```powershell
Set-Location 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops II'
& .\bin\cod9map64.exe -platform pc map_source\zm_test.map
```

It successfully creates a native `zm_test.d3dbsp`.

Official BO2 linker:

```text
BO2\bin\Linker.exe
```

It correctly looks for native assets such as:

```text
maps/mp/zm_nuketown_waw.d3dbsp
```

It cannot consume the intermediate FBX directly. Therefore the current required sequence is:

```text
WaW assets → OAT custom dump → Python FBX/material staging → native T6 BSP bridge → official BO2 Linker → runtime test
```

## Current blocker

The clean T6 bridge reaches material/lightmap compilation but does not yet emit a native BSP.

Latest diagnostics include:

```text
Missing asset "trivial_2d_9zzq26w5" of type "techniqueset"
Missing asset "*123_47" of type "material"
Missing asset "lightmap0_secondary" of type "image"
ERROR! unable to find lightmap image lightmap0_secondary!
BSP link has failed.
```

The stock techniqueset exists here:

```text
C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops II\raw\techsets\trivial_2d_9zzq26w5.techset
```

The extracted lightmaps originally had leading underscores:

```text
images\_lightmap0_primary.dds
images\_lightmap0_secondary.dds
```

Aliases have been staged:

```text
images\lightmap0_primary.dds
images\lightmap0_secondary.dds
zone_raw\bridge\images\lightmap0_primary.dds
zone_raw\bridge\images\lightmap0_secondary.dds
```

The bridge temporarily also reported a missing stock rawfile:

```text
animtrees/fxanim_props.atr
```

That file exists at:

```text
BO2\raw\animtrees\fxanim_props.atr
```

Adding the BO2 raw root resolves that rawfile, but causes the techniqueset/lightmap lookup to regress depending on search-path ordering. Search-path ordering or the exact OAT T6 asset path conventions are now the main investigation target.

## Recommended next steps

1. Re-run the T6 bridge while capturing the complete log to a file, with all of these paths explicitly supplied and in a controlled order:

   ```text
   final_stage\images
   final_stage\zone_raw\bridge\images
   BO2\raw
   BO2\raw\images
   BO2\raw\techsets
   BO2\raw\techniques
   ```

2. Confirm whether the T6 bridge expects `.techset` files from `raw/techsets`, or whether it expects a different source/asset path root. Inspect OAT T6 linker asset search-path handling if necessary.

3. Confirm whether lightmaps need OAT image JSON metadata in addition to DDS. If required, create the minimal T6 image definitions in the staging tree instead of relying only on DDS files.

4. Fix generated material `*123_47`. Windows cannot create a filename containing `*`; the translator currently emits underscore aliases such as `_123_47.json`. Verify the T6 linker’s lookup convention and add an explicit name-normalization layer if necessary.

5. Do not enable generated aliases or missing-image placeholders in the final production path. Those flags are only for bridge diagnostics:

   ```text
   --allow-generated-aliases --allow-missing-images
   ```

   The default converter must remain fail-closed and report missing assets.

6. Once the clean T6 bridge emits a native `.d3dbsp`, stage it under the project’s expected path:

   ```text
   zone_raw\zm_nuketown_waw\maps\mp\zm_nuketown_waw.d3dbsp
   ```

   Then run the official BO2 linker using the generated project zone source and boilerplate scripts.

7. Use the official BO2 linker output as the final artifact. Verify the resulting mod loads in BO2, reaches the map, renders geometry and textures, has collision, and does not show missing-material or missing-image errors.

8. Only after runtime verification should the converter be described as producing a fully working BO2 mod.

## Important honesty constraint

The project is not complete yet. Geometry, collision, models, extraction, and most material translation are working, but there is currently no verified native BSP or playable BO2 output. Do not claim successful conversion until the official BO2-linked zone loads in-game without the known texture/material failures.

# Session 4 (2026-09-28): first in-game view, floor collision and tool-material work

The player now reaches first person, but the initial view still has black/absent ground and the player fell through the map in the first test. The screenshot showed props and sky rendering. The game also exited without a script traceback in that first run. Global Plutonium scripts remain temporarily isolated under `C:\WawConverter\work\global_script_isolation_20260928` (59 files); **restore them to their original `storage\t6\raw\scripts\zm` and `storage\t6\scripts\zm` locations after isolated testing**. The user authorized repeated isolation tests until first spawn is stable, but no permanent edits to global scripts.

- WaW collision triangles under both spawn clusters face down in source data. `src/waw2bo2/fbx.py` no longer reverses them: `BSPCreator.cpp` already reverses FBX triangle indices on import, so reversing twice left downward-facing BO2 floors.
- Source gfx surface `*98n_12` is real ground around the second spawn (~Z 1679), but `wc/caulk_shadow` tool surfaces overlap it. Their WaW technique set is `wc_tools`; the BO2 translation visibly drew them as black geometry. `src/waw2bo2/t6bridge.py` now removes `wc_tools` surfaces from **render geometry only**, as it already does for sky. This latest change has not yet been rebuilt/tested.
- The T6 bridge's world cmodel leaf was initialized with zero terrain/brush contents even though its brush tree and terrain partitions existed. `vendor/OpenAssetToolsT6/src/ObjLoading/Game/T6/BSP/Linker/ClipMapLinker.cpp` now enables terrain contents and attaches the world brush root to cmodel 0. The bridge was rebuilt with MSBuild (Release|Win32), set LARGEADDRESSAWARE, linked, packaged, and launched. The current in-game run (PID 39564 when last checked) has remained alive beyond the previous crash interval, but the user's confirmation on floor collision is pending. Do not claim this fix is verified until the user confirms or a runtime position trace does.
- A second user screenshot showed camera-dependent disappearance of buildings and terrain, consistent with faulty surface culling. `vendor/OpenAssetToolsT6/src/ObjLoading/Game/T6/BSP/Linker/GfxWorldLinker.cpp` previously zeroed `tris.mins/maxs`, `tris.vertexCount`, and `tris.firstVertex` for **every** render surface. It now fills these from actual surface bounds and indices. The linker was rebuilt and the zone was re-linked/packaged. Current verification launch PID 4756 (when last checked); user was asked for confirmation and screenshot.
- The prior diagnostic GSC remains in `work/final_stage/zone_raw/zm_nuketown_waw/maps/mp/zm_nuketown_waw.gsc`: `waw2bo2_damage_probe` and `W2B SPAWN` logging. Remove it when no longer needed.
- The official BO2 mod tools still compile scripts and `mod.ff`; the custom OAT T6 bridge links the world zone. The most recent `run_bridge.ps1` build failed only at packaging because the game locked the previous mod; after the process exited, running `python -m waw2bo2.cli package ...` succeeded.

Latest test result: user explicitly confirmed **no visible or solid ground, falls through, and game crashes after ~3 seconds of movement**. The render-tool omission and triangle metadata changes did not resolve it. The screenshot has noclip ON, so that view is not proof of standing collision. The newest zone was compiled/linked/packaged, but must be considered broken.

The crash dumps are under `%LOCALAPPDATA%\Plutonium\crashdumps`. The three latest `.txt` records (10:06, 10:11, 10:15) all have `0xC0000005` at **the same address `0x12D9C75E`** and no GSC error. Python `minidump` + `capstone` showed this address is a `rep movsb` (`memcpy`); the 10:15 crash attempted to write at `0x60ff6000`, a `MEM_RESERVE`/uncommitted gap, while copying approximately `0x200c0` bytes. This is a native memory fault, not a script traceback. Do not assume which generated structure is corrupt yet. A Windows debugger or PE symbol/disassembly investigation should identify the caller.

Further stack analysis: the return address is `0x0077c73b`, in game render code that calls a copy routine at `0x006d1cd0`. `tools/crash_summary.py` prints registers/stack for the three minidumps. Copy lengths in the three crashes were `0x2b7a0`, `0x6ca0`, and `0x200c0`, all divisible by 32 (the size of `GfxPackedWorldVertex`). The destination ran into an uncommitted gap. This strongly implicates world-vertex upload/allocation, but remains an inference rather than a proved faulty field. The Plutonium EXE is Themida-protected; the relevant game code is dynamically populated in its `.payload` section and present in full dumps, not useful in the on-disk binary alone.

The 59 global scripts were restored successfully (58 in `raw\scripts\zm`, 1 in `scripts\zm`); the backup folder contains zero files. If testing again in isolation, move them out temporarily and restore again as the user authorized, but do not leave them disabled.

Next: investigate BO2 world clipmap traversal/trace and renderer/visibility structures, and identify the native crash caller. Add explicit script traces at spawn (downward bullettrace, origin over time) if useful; official BO2 linker must compile any diagnostic GSC. Do not substitute a cosmetic platform for source collision. There are still stock HUD/vision warnings and unconverted path nodes; map is not complete.


## 2026-09-29: collision index overflow and inline world images
- Collision root cause: BSPCreator loaded map_col.fbx with generate_missing_normals. Per-face normals
  broke vertex dedup (~234k verts), and the uint16 index cast wrapped, scrambling triangles.
  Collision now dedups on position only, with a hard error on >65536 unique vertices.
  Check: the Unlinker clipmap diag prints a "tri winding" census. Ours must equal the WaW source
  (up 10924 / down 25634 / steep 41490); stock zm_nuked floors are also "down".
- LoaderImageT6: lightmap*, reflection_probe*, and $outdoor are stored inline (loadDef, streaming 0,
  semantic 1, category 2/1), as in stock. They were streamed before, which is the suspected red-tint cause.
  Not yet verified in game.
- Correction: an inline cube probe or R8 $outdoor crashes the zone load (E_INVALIDARG). Only the lightmap is inline now.
- Red tint root cause: WaW's dumped *reflection_probe0 is solid pure red (a placeholder). stage_images now
  synthesizes a 4x4 neutral grey cube (NEUTRAL_PROBE_RGBA). Collision confirmed fixed in game by the user.
- Open: collision "a bit wonky" inside the truck (static-model collision is 2-unit triangle prisms, hulls.py).

## 2026-09-29 (later): scroll/sheen, crash, path nodes, spawns
- Probe is near-black now (the grey probe gave a wet sheen). techsets.index_donors skips technique sets used only by
  animated stock materials (e.g. packapunch "moving").
- Crash 0x12D9C75E came back after running around. Stock max world surface is 1536 tris / 1122 verts, ours was 14k.
  fbx.MAX_MESH_TRIS/VERTS = 1536 now (in-game verification pending).
- Path nodes: the T4 dumper writes gameworldsp -> <map>.paths.json. paths.py maps types (+1 from CONCEALMENT_STAND),
  adds clearance flags 0x300000 / link 0x28, copies pathVis (n*(n-1)/8 in both games), and maps traversals
  (wall_hop -> zm_mantle_over_40, jumps -> generated traverse/waw2bo2_jump_*.gsc -> dosimpletraverse).
  GameWorldMpLinker links it, with a kd-tree built like stock. Node entities are kept in the ents string.
- Spawns: WaW zombie_spawner actors emit spawn_location / riser_location script_structs (BO2 zone manager).
- Open: truck UVs look wrong in game (glTF pairing verified OK). Official route available: bin/cod9map64 +
  arclight64 + Linker needs a .map source (would need a WaW BSP -> .map decompiler).
- Crash 0x12D9C75E root cause (from the dump, game fn 0x77C6C0): a per-frame dynamic VB (32-byte verts) is
  overfilled by CPU-skinned model surfaces. The OAT T6 xmodel loader gave no rigid vert lists to (a) unskinned glTFs
  (no weights) and (b) one-bone-per-vertex skins (its rigid check was maxWeightCount == 0). Fixed in
  LoaderXModel.cpp.template (regenerate with build/buildtools/Release_x86/RawTemplater.exe -o <tmp>, then copy
  Game/*/XModel/LoaderXModel*.cpp into build/src/ObjLoading). mdmp.py context offset fixed (r+8+152).
- Zombies do spawn (template's W2B SPAWN prints in console.log); earlier sessions crashed before they arrived.
