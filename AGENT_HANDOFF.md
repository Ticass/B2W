# WaW → BO2 Custom Map Converter Handoff

## v0.2.0 release preparation, 2026-10-07

- User explicitly requested pushing and releasing the desktop GUI. Preparing
  v0.2.0 as a development prerelease with the portable Windows ZIP, checksums,
  direct README download link, desktop quick start, and known verification limits.
  Tool/package version updated consistently to 0.2.0. Rebuilding the bundle from
  the release sources before upload; native build and gameplay scope unchanged.

## Desktop mod-tools launcher, 2026-10-07

- User requested a simple native GUI resembling T6 Mod Tools. Added Tk/ttk
  Mod Builder, Setup, Reports, grouped build/install/run controls, stage checklist,
  console, saved paths, Steam discovery, prerequisite checks, and owned-process
  cancellation. Source entry is `Launch Mod Tools.bat` / `python -m waw2bo2.gui`.
- Launcher manages isolated working folders per source and game-installation pair;
  input archive/tool timestamps invalidate cached dumps. Build output stays separate
  until Install is clicked. Required native stages, material audit, and package
  outputs must succeed before a success receipt is recorded.
- PowerShell driver supports explicit Python worker and NoInstall, resolves its
  source folder relatively, creates required folders, and uses already-LAA linkers
  without requiring editbin on the end user's machine. Resource lookup supports
  frozen executables. Existing conversion semantics are unchanged.
- `tools/build_desktop.py` + `desktop.spec` create GUI and console worker EXEs,
  Python/Tk runtime, T4/T6 native tools, decoder, schemas/compat data, and native
  corresponding source archives/licenses. Local portable ZIP:
  `work/desktop_dist/WawConverter-Windows.zip`; app folder alongside it.
- 267 tests pass, including real subprocess output/failure/cancellation and GUI
  gating/event transitions. Packaged self-test confirms Tk, native tool presence,
  compatibility data, and weapon schemas. Real WaW extraction/inspection using
  the portable worker/extractor succeeded; bundled T6 Unlinker + portable audit
  checked 67,022 material arguments, zero violations/missing technique sets.
- UI screenshot: `work/desktop_smoke/launcher.png`. Evidence logs/native extracts
  are under `work/desktop_smoke/`. No new full conversion/playtest was performed;
  installed playable map was only read for the T6 audit and was not replaced.
  No game was launched or closed. GUI/source changes are local and uncommitted;
  no new GitHub release has been published for this request.

## Session 32 (2026-10-02): vision fallback, pipeline audit, reference tool

- INSTALLED BUILD at session start was a leftover DIAGNOSTIC (WAW2BO2_DIAG_LIGHTMAP + FALLBACK_MAGENTA, logs work/diag_*.log, not recorded by Session 31): world drew lightmap only. Always rebuild clean and check a lit program's instruction count before handing a build to the user.
- VISION (root cause of "whole map too bright"): nuketown.gsc calls VisionSetNaked("cargoship"); no WaW zone has it. CoDWaW sub_4629F0 then loads vision/default (film OFF, glow off). The port used to keep mp_kneedeep (light tint 1.37); a first fix this session took cargoship from WaW raw/ (contrast 1.6, +.25) and was far too bright. Now visions.stage aliases any vision absent from zones to WaW's default (report default_fallback, warning VISION_DEFAULT). raw/ is never a runtime source.
- AUDITED (consistent, no change): exposure 0 -> hdrControl0.x 1/4, DISPLAY_SCALE 4 -> unit 1 for light/sun/fog/model grid; film applied once (overlay; LUT only unshoulders). Memory t6-exposure-unit-scale says exposure -2: STALE, linker uses 0.
- MEASURED: WaW uncompressed IWI = BGRA bytes (D3DFMT_A8R8G8B8 only), BO2 IWI fmt 1 = RGBA (stock LUT + hdr_bloom_apply) - both handled right. Spot dir: forward = -dir in both engines (T6 sub_6DA1A0). T6 world matrix and lightPosition share viewOffset (sub_76C4D0). Correct T6 IDA db is the Steam t6zm.exe.i64.
- In-game tests by user: r_lightTweakSunLight 0 -> no change (sun not the flat light); r_spotLightShadows/SModel/Entity 0 -> no change; sm_spotEnable is write-protected.
- Reference tool ("Reference tools/", BO1->BO2 lighting by aidenwrld): BO2 writer side matches ours; new ideas: fit 9 SH coeffs per grid sample (colors path zeroes model SH -> BO2-native lit models reflect at 10%), sun hero light for viewmodels. Not implemented yet (user asked for option 1 = SH coeffs).
- SCRIPT RENDER DVARS (main brightness cause): compat waw_setclientdvar DROPPED every r_ dvar. nuketown.gsc sets per player: r_filmUseTweaks 1 + r_filmTweak* (contrast 1.1, brightness .07, dark tint 0 .08 .13, light tint .847 1 .984), r_contrast 2.4, r_brightness .8, r_fog 0, r_lightTweakSunLight .85 / Ambient .4 / DiffuseFraction .35. Measured CoDWaW sub_6DC1A0 (dvar globals mapped from sub_707A20 registration): tweak values replace the vision film when r_filmUseTweaks; contrast *= r_contrast, brightness += r_brightness, desat = d(d+(1-d)r_desaturation). Sun dvars are loaded from sunParse (sub_6FCDE0) and rebuilt on change (sub_6F6680). Now: visions.script_dvars scans literal setClientDvar(s) values; visions.film_terms applies user dvars to every grade; "_filmtweak" overlay grade (compat selects it on r_filmUseTweaks); r_fog 0 parks setvolfog at 1e6 units (T6 r_fog is cheat-flagged 0x80) and ignores later waw_setvolfog; staged BSP/lighting.json sun takes the script r_lightTweak* values (WaW direct sun .29 instead of .40). Not handled (reported): r_specularColorScale 1.2, glow tweaks (glow disabled), sm_* and r_dlightLimit.
- OPEN: black spots (need user locations; doorstep (-404,570) lightmap data is normal), glow still disabled (GLOW_ENABLED False, 10 ms), lamp-vs-light offset reports unverified.

## Session 31 (2026-10-02): layered materials, scorch, models, primary-light shadows

- USER: "bring the whole lighting system over". Inventory: ~560 layered world materials had no WaW lighting (no vd1), 37 wc/ blocked by BLENDWEIGHT0 (terrain scorch), models partial, shadows off.
- LAYERS: T4 dumper v5 exports vld + per-surface vertexLayerData + worldVertFormat. WaW records = float2 texcoords + RGBA UBYTE4N transforms (stride verified: ranges end exactly at buffer size); T6 = half2 + BGRA transforms (stock bytes form valid 2x2 rotations only as BGRA). FBX LayerUV<k>/LayerNormal<k>; BSPCreator reads; GfxWorldLinker::AppendLayerVertices writes vd1 in the T6 material format (identity transforms where T6 needs more, white vertex colour if T6 material is single-layer). techsets.LAYERED_VERTEX_DATA set by stage_geometry when the dump has layer data. Formats for 0xFF surfaces: technique name (techsets.world_vert_format, verified 638/638) or record spacing (world.layer_formats_from_strides, 473/473), 24-byte tie by generated-name layer count.
- SCORCH: '_sco' programs: BLENDWEIGHT0 neutral 0 (waw_neutral input adapter), terrainScorchTextureSampler constant texel, gameTime neutral (only feeds the zero-weighted fade).
- PROBE/TEXTURES: probe at fixed t15 when donor lacks it (engine binds per surface); relaxed probe regex; add_material_texture adds dropped source maps (hash-ordered table + arg); add_code_texture binds attenuation (code 15). Model grid scale check accepts mad.
- LIGHT ARGS: T6 light uniforms are per-pass type-5 args (value 0x01000000+index, location = byte offset); ensure_code_constant_arg adds missing ones. lightSpotFactors.w = shadow fade (a5 in both setters).
- SHADOWS: T4 dumper v6 exports shadowGeom + lightRegion per light (identical T6 structs). BSP/shadowgeom.json (FBX mesh indices, smodel indices = WaW order, hulls); GfxWorldLinker::LoadShadowGeometry; smodel NO_SHADOW unless in WaW sun list (linker previously forced NO_SHADOW on all). canUseShadowMap kept, shadowed slots reachable, shadowmapSamplerSpot point load (texldp allowed), spotShadowmapPixelAdjust = WaW taps (0.25,0.25,0.5,-0.125) x T6 (1/S,1/S) [DERIVED, T6 sub_76A7A0; WaW sub_737E60]. T6 code const 60 = spotShadowmapPixelAdjust (1472), 61 = dlight variant.
- Offline coverage: world 154/164, layered 471/472, models 326/332 keep WaW lit passes (shadows on). Remaining: 4-layer material on 3-layer T6 technique, charred zombies (__characterCharredAmount), one fog output form, gameTime (2), 31 model materials without donor, depthFromClip slot 1 (shadow depth pass, donor ok).
- DLIGHTS: WaW LIGHT_SPOT/OMNI passes blend invdestalpha,one; WaW lit world passes write alpha 1 (colorWriteAlpha on) -> the light pass adds 0 on world surfaces. T6 DLIGHT_GLIGHT draws the base WaW pass (matches). T6 dlight/glight setter found: sub_729C20 (scene struct = PerSceneConsts + 1040; glights = omni dlights, -1/radius, diffuseColor; spot dlight rows; code consts dlightPosition 132, dlightDiffuse 133, dlightSpotFactors 141). T6 has no multi-pass world technique in stock data (renderer pass loop not verified).
- Later fixes: pixel gameTime -> type 5 buffer 2 location 64 value 0x01000019 (stock routing, 382 passes); __characterCharredAmount neutral 0. Open: foliage mc_ambient_t0c0_sco fog visibility feeds several outputs (adapter overrides one component); 4-layer material on 3-layer T6 technique.
- NOT VERIFIED IN GAME (user playtest pending for layered/models/shadows builds).

## Session 30b (2026-10-01): local primary-light adapter implemented

- T6 per-light setter FOUND: sub_782FA0 (called from sub_7836F0, the light switch; consts at source+2048+16*i). With GfxLight a2: lightPosition = (origin - viewOffset, 1); lightDiffuse = (diffuseColor@+88, 1); lightSpotDir = (dir@+20, dAttenuation@+64); lightFallOffA = (1/(aAbB.x-aAbB.y), 1/(aAbB.z-aAbB.w), 1/(f.z'-f.x), 1/(f.w'-f.y)), lightFallOffB = (-aAbB.y*A.x, -aAbB.w*A.y, -f.x*A.z, -f.y*A.w) with f = falloff@+120, f.z' = max(f.z,f.x) (min window 2^-12), f.w' = min(f.w,f.y); omni lightSpotFactors = cone factors, spot = cookie projection. Found via the version-word bump `inc word ptr [ebx+19D2h]`. WaW r_diffuseColorScale and r_specularColorScale default 1.
- IMPLEMENTED: shaderruntime.LIGHT_ADAPTERS/light_constant/light_falloff_placement (from source primary lights + lightdef + DDS widths, computed before material staging); lighting.t6_light_fields stages falloff/aAbB/dAttenuation; ComWorldLinker reads them (linker rebuilt). Primary lights staged with canUseShadowMap 0 (no shadowGeom/lightRegion, no spot shadow adapter; warning in report), SHADOWED_LIT_SLOTS (8, 14) unreachable via bind_material(light_shadows=False).
- Offline rebind of all 164 world materials: 127 keep WaW lit passes (was 0); lava binds slots 4/5/6/7/13 on the WaW page. Remaining: 135 slot failures packed BLENDWEIGHT0 (layered blends), depthFromClip (slot 1, depth prepass), gameTime (2 materials). 158 tests pass.
- NOT VERIFIED IN GAME. Spot shadows from primary lights are disabled by design until shadow data + adapter exist.
- USER PLAYTEST 1: closer to WaW, lava glow visible, but many surfaces pure white, lava/blue room light weak. CAUSE: ComWorldLinker left roundness 0, so T6 sub_73AC60 turned every spot into SPOT_SQUARE (technique 9), an untranslated donor that read the WaW-encoded page as a T6 page (rgb / direction alpha -> white). FIX: staged roundness 0.5 (stays technique 7). DLIGHT_GLIGHT techniques 15-19/24/25 now draw their base technique's WaW pass (DLIGHT_BASE_SLOTS; dynamic light not added). Rebuilt; awaiting playtest 2.

## Session 30 (2026-10-01): why no world surface keeps WaW lighting (investigation, no code change)

- USER: lava blue in BO2, orange in WaW; "no maps will ever have proper shading" until fixed. Game NOT launched (user playing).
- RULED OUT (measured): primary-light off-by-one. FBX `_pl` = WaW index, BSPCreator/GfxWorldLinker copy it unchanged (material sort copies whole GfxSurface), staged primarylights.json row order = source, lava `_pl47/_pl53` meshes sit 250-550u from lights 47/53 (r700) and 880u+ from blue 46/48. Stock zm_nuked uses the same 0=none/1=sun convention. Lightmap texels under lava are orange in both WaW and T6 pages; reflection_probe0 is 06 06 06; falloff image is grey.
- ROOT CAUSE (converter-wide): 0 of 164 world materials keep WaW lit passes; 127 revert to stock `pimp_technique_lmap` donors. WaW sun/lit slots 4/5/6 translate fine, but the spot/omni slots 7/8/13/14 fail (481x `no native code uniform ... lightFalloffPlacement`, 189x packed BLENDWEIGHT0, 31x lightSpecular), and bind_material then reverts 4/5/6 because donor 7/8/13/14 read the T6-encoded page.
- WAW FACTS (CoDWaW_mapped.exe.i64, session 60181d9c): code const 11 = lightFalloffPlacement, set in sub_7425D0 via sub_6D7120 (consts at source+16*(idx+144)) = (attenImage.width/512, 0, lightDef.lmapLookupStart/512, 0); 1/512 = dword_8AF9B0. The ramp is baked into EVERY secondary lightmap page, row 0: light_point_linear (16 wide, start 1) occupies texels x 0..35 (2-texel borders, 2x bilinear upsample), matching u = 2 + 32*d/r on the 1024-wide page; v = 0. WaW light setter sub_742400 (GfxLight: type@0 color@4 dir@16 origin@28 radius@40 cosOuter@44 cosInner@48 exponent@52): lightPosition = (origin - viewOffset, 1/radius); lightDiffuse = color*dvar(42B70C8); lightSpecular = color*dvar(42B6FBC); lightSpotDir = (dir,0); lightSpotFactors = (1/(cosIn-cosOut), -cosOut/(cosIn-cosOut), exponent, arg).
- T6 FACTS (t6zm.exe.i64): code const names 0 lightPosition, 1 lightDiffuse, 2 lightSpotDir, 3 lightSpotFactors, 4 lightAttenuation, 5 lightFallOffA, 6 lightFallOffB (table 0xd1e920). Consts live at 0x3A2D980+16*i (versions word_3A2E960[i]); only writer is render command sub_742AC0 (table 0xd1c614) -> the CPU-side producer is NOT yet found. sub_73AC60 copies ComPrimaryLight (196 B) into runtime GfxLight (352 B, array at gfx+0x4E750): falloff->+0x78, aAbB->+0x88, dAttenuation->+0x40, angle->+0x48, diffuseColor->+0x58, cookieControl0-2->+0x98; spot matrix at +0xD0. T6 meanings differ from WaW: donor spot/omni PS use lightSpotDir.w as an inverse-square scale, lightFallOffA/B.zw as a smoothstep radius window, lightSpotFactors as attenuation-texture placement. ComWorldLinker leaves falloff/aAbB/angle/dAttenuation/cookie fields ZERO, so donor local-light terms are probably zero today.
- T6 world matrix is camera-relative (donor VS fog distance = |worldPos|); WaW lightPosition is also origin-minus-view-offset, so T6 lightPosition.xyz can feed WaW directly.
- NEXT: find the T6 producer of light code consts (to map ComPrimaryLight fields -> constants), then (a) lightFalloffPlacement adapter = per-lightdef literal (all lights here share light_point_linear; reject mixed defs explicitly), (b) lightPosition.w / spotDir / spotFactors / lightDiffuse expressed from verified T6 constants (diffuse via the sunDiffuse sqrt(4 hdr L) rule), (c) fill ComPrimaryLight precomputed fields so donors also light correctly. Per-(material, light) literal clones were rejected: 1,528 pairs. Origin-keyed tables need T6 eyeOffset semantics first.

## Session 29 (2026-10-01): global light units (exposure/composite), pass states, model unlit

- USER: lighting "dark and faded", nothing like WaW (their WaW screenshot: bright, contrasty); HUD trap icons white boxes; power arrow black box; blue box glow / lava tint / lab neon missing; pitch-black item parts; nuke smoke reacting to movement. Fixes must be converter-wide.
- ROOT CAUSE (global): BO2 hdrControl0 = (s,0,1/s,1/s), s = 2^-(exposure+2) (IDA sub_728180). Lit/unlit native programs write sqrt(s*L); every hdr_bloom_apply composite (hq/normal/null) shows sqrt(4*buffer^2 + bloom), then a shoulder above 0.75 (0.75+0.25(1-e^(4.328-5.771x))), then the LUT. In-game exposure sweep: -2 stops = exactly 2x pixel. Linker hard-coded mp_dig exposure 2.5 -> native-lit world at 0.42x WaW; translated WaW programs (sky/unlit/fx) wrote display colour -> 2x and shoulder-squashed. Stock: zm_nuked exposure 4.8 sun 15, zm_transit 3 / 12 (T6 Unlinker gfxworld dump now prints sun/fog).
- FIX: GfxWorldLinker exposure 0 when converter lighting.json is present (lighting.json "exposure" overrides; template fog rescaled). Translated WaW pixel programs write 0.5*rgb (contract output_rgb_scale; not for depth slots, 2d HUD, or blends using src colour as multiplier). Adapters convert to display units (fog/sun sqrt(4hdr L), modelLighting sqrt(8*4hdr), probe sqrt(4hdr rgb/a)). Vision LUT bakes the inverse shoulder (visions.unshoulder). Unit rule: WaW gamma g -> T6 linear g^2 for sun, primary light colours (lighting.stage_primary_lights), waw_setvolfog RGB, SetSunLight CSC. Memory: t6-exposure-unit-scale.
- PASS STATES: shaderruntime.apply_source_pass_states gives each T6 pass the WaW blend/alpha-test of its feeding technique when the pass runs WaW's program, or when WaW is additive and donor is not (2d HUD: srcalpha/invsrcalpha + gt0 instead of premultiplied donor -> white boxes; wc/mpl_chalk additive). Model mc_unlit/mc_objective now use T6 mc_unlit_replace/add/blend and mc_objective donors (MODEL_UNLIT_MAP); source_slot routes UNLIT/EMISSIVE through WaW lit aliases (objective). Translator: sincos.
- CRASH found+fixed during session: 0x7777F9 sub_777790 type-6 (pixel material const) lookup is unbounded like 0x77C253; mod.ff mc/mtl_zombie_vending_vent_power_off kept donor scaleRGB arg its mod-tools-rebuilt material lacked. Paired passes now drop material-constant args neither translated program reads (prune_missing_material_constants unread=True). tools/audit_material_args.py now replays type 6 too: final map 0 violations; mod.ff must be re-audited after each build.
- CAPTURE TOOLING (scratchpad, not in repo): Plutonium launched with +set w2b_shots "x y z yaw pitch [dvar=v];..." + temporary storage/t6/scripts/zm/w2b_capture_tmp.gsc (teleports, setdvar per shot, logprints W2BSHOT) + PrintWindow capture synced on games_mp.log. WaW reference capture via cfg+devmap failed: mod scripts raise fatal "undefined is not an array" under devmap; WaW D3D9 window cannot be PrintWindow-captured.
- 151 tests pass. Installed build: exposure 0 + output halving + inverse shoulder; loads without crash; labs/spawn visibly correctly exposed (no clipping). NOT verified vs WaW side-by-side.
- OPEN: WaW glow (mp_kneedeep r_glow 1, cutoff .26, intensity 1, radius 5) not mapped to T6 bloom (curve dvars r_bloom*; T6 bloom adds inside sqrt); lab neon/blue box glow depend on it. Primary light shadows: lightRegion/shadowGeom empty in linker. Power arrow not identified (need location). Nuke smoke: elements are WaW line(3) + billboards at 15-39k units, flags 0x12000050/0x60; needs in-game observation. Layered world blends (~500 materials) still drop layer 1. Pitch-black floor patch at spawn doorstep (-404,570) persists.

## Session 28 (2026-10-01): bunker shaft grey portal material

- User identified the plate over the circular bunker shaft/Bowie knife/ladder, rather than an underground points door. Generic converter changes only; collision and entity files unchanged.
- Source render surface14224 spans x[-1428,-1348], y[1252,1336], z1613 and uses wc/hdrportal_lighten / wc_unlit_distfalloff. It is static visual geometry, not an unlock entity. Source collision dump has no triangle across this plane. Outdoor bunker_door_clip *106 has no render surfaces; its notSolid/connectPaths calls already survive translation.
- Portal was translated with fence alpha blending and a native vertex shader: distance falloff was rejected (UV.xy subtraction and inverseWorldViewMatrix unsupported), giving a grey sheet. Shared unlit state preservation and EMISSIVE routing now include distance falloff. UV adapter accepts the measured xy camera subtraction; inverse-world-view translation-column adapter reconstructs object-space camera from native inverseViewMatrix and worldMatrix, including rotation/scale, rejecting unsupported full-matrix reads. Source falloff constants are embedded, original VS/PS paired in slots2/3, no unsupported passes. No named map/target checks or entity removal.
- 147 tests pass; linked roundtrip preserves portal blend/sort and both VS/PS programs exactly; 43,903 arguments checked with zero violations/absent techsets. Rebuilt world and packaged successfully while game closed. Evidence work/bunker_portal_material_audit.json, bunker_portal_roundtrip, bunker_portal_args.log, bunker_portal_tests.log. No live playtest: removal of visual grey sheet and physical hatch traversal still need user verification; do not claim the collision symptom was reproduced or fixed.

## Session 27 (2026-10-01): gun/HUD namespace regression, missing wood, transparent lab decals

- USER: guns became white, crosshair four white squares, OWN3D purple house trim, blue/black lab panel. Generic converter fixes only; no collision edits. Game closed for final installation.
- ROOT CAUSE guns/HUD: stage_materials added waw_world/ before weapons.stage_visuals tried to resolve the original DDS names. Every lookup missed; colorMap fell back to $white. Shared stage now accepts image_prefix; weapons resolve unprefixed source names and apply waw_image/ themselves. Code images bypass lookup. 246 images restored; ALL 246 roundtrip DDS pixels match original source, zero absent/mismatched (work/material_weapon_pixel_audit.json). User's later location screenshot shows crosshair restored.
- ROOT CAUSE purple house trim: default_c is literally the magenta OWN3D image from WaW mod.ff. mc/bo2_wood_burned_blend (shutters/trellis) was absent, previously substituted with mc/mtl_default. Generic material closure now consults stock WaW and raw source lookup before BO2 fallback. Shared material compatibility table includes imported burned-wood and damaged-TV material aliases. Native fallback images are namespaced waw_world/bo2_fallback/ and packaged with their original native material, reported BO2_FALLBACK; real WaW source wins. Native image extraction added to pipeline. Remaining unavailable TV/gobo definitions are NOT claimed fixed.
- ROOT CAUSE blue panel confirmed with new user coordinates (-1691,-140,1385), yaw259: source ray intersects coplanar wc/art_twotone surface10184 and wc/dyn_lights surface14639, ~134 units ahead. lights DDS has alpha0 background with hidden RGB; source dyn_lights is a DECAL, alpha blend, no depth writes, sort43, despite technique wc_unlit. Converter selected opaque unlit_replace (depthWrite true, sort4), exposing hidden RGB and covering the wall. techsets.material_techset reads source UNLIT state to select native blend donor; build_material preserves source blend/depth/sort behavior for unlit world materials. Native donor camera region/stencil/flags retained. All 8 affected world unlit materials restaged; dyn_lights/blue_element now emissiveTrans/sort43. Simple source shaders also bind native EMISSIVE slot3 when WaW exposes UNLIT but no explicit EMISSIVE pass (opaque neon remains emissiveOpaque).
- VERIFIED: 145 tests pass; final world 43,903 material args, gameplay 48,390 args: zero violations/missing techsets. Final roundtrip material state, camera region, sort, texture routing, VS+PS bytes verified for dyn_lights/blue_element/pel1_glow; burned-wood native image payloads match. Evidence work/material_final_binding_audit.json, material_final_world_args.log, material_fix_mod_args.log. 937 source world/model top mips also matched prior roundtrip. Source pixels/mips were not rewritten to hide issues.
- BUILD: first namespace correction pipeline work/material_regression_build.log installed successfully. Recovery pipeline work/material_recovery_build.log linked successfully; package initially failed while user reopened game. Final emissive/transparency world relinks work/material_emissive_link.log, material_transparency_link.log; FINAL PACKAGE succeeded after user closed game. Installed mod.ff and weapon IPAK from work/mod_build/out, world FF/IPAK from work/final_stage/zone_out/zm_nuketown_waw. No live final playtest: user should restart and inspect gun/HUD, house trim, identified lab panel/neon. Local-light/directional-lightmap adapters and lava-room lighting parity still incomplete; do NOT claim every rendering issue solved. Bunker entity lifecycle investigation remains pending, no collision changes this session.

## Session 26 (2026-10-01): generic brush rendering, unlit UVs, world image isolation

### Follow-up: grey plate after bunker door unlock (investigation, not fixed)
- User explicitly wants entity/prop lifecycle investigation, NO collision conversion work. Asked asynchronously whether outdoor generator bunker door or underground points door; location not yet identified.
- Outdoor source _access_bunker::destroy_gen rotates the script_origin pivot, waits rotatedone, then bunker_door_clip notSolid/connectPaths. All calls survive translation. blocker *106 has ZERO source and linked render surfaces, so cannot explain a visible grey plate by itself. Destination helper bunker_door_moveto is hidden by the source and translated scripts; its notSolid state is not assumed. Crossing trigger flag_set_bunker *150 also has zero render surfaces; source/converted set_bunker_flag deletes it after trigger and sets enter_lab_zone1.
- Underground source lab2_clip cleanup waits enter_lab_zone2, then notSolid/connectPaths/delete; all calls survive. BO2 debris code also deletes/moves linked blockers using original target groups. No rendered source brush belongs to a trigger/info_volume (all 176 rendered brushes are script_brushmodel). No entity or collision mutation made without identifying the plate. Need exact doorway/location or screenshot before choosing a converter-wide correction.

- USER: underground wrong textures, missing elevator and geometry, missing red/white/blue lighting; reiterated fixes must apply to the converter across maps. No map-name or targetname special cases introduced.
- ROOT CAUSE (geometry): GfxWorldLinker assigned zero surfaces to every brush submodel. The source has 341 brush surfaces on 176 script_brushmodels; elevator_door *351 has six surfaces and pap_key_elevator *353 has nine. T4 render interchange v4 appends original GfxBrushModel ranges. Reader validates bounds/overlaps and accepts empty UINT32_MAX sentinel ranges; legacy dumps require re-extraction when collision submodels exist. Surface.brush_model survives sky/tool filtering and separates FBX merge buckets. FBX `_bm<N>_pl<N>_lm<N>` ownership reaches BSPSurface; linker sorts owner first, retains contiguous brush ranges, and restricts static camera groups/AABB visibility to model 0. Stock zm_nuked independently confirms 9,865 static surfaces plus two surfaces owned by model 124.
- ROOT CAUSE (unlit): original vertcol_simple_fog declares UV float4 but only copies xy. Previously rejected as unknown packing, leaving mismatched donor varyings. New generic adapter accepts only direct xy-copy reads, supplies decoded UV.xy with unused zw zero, and compiles the original pair. wc/pel1_glow now has no unsupported passes; lava's simple pass also binds its source vertex/pixel pair. Remaining local-light/directional-lightmap adapters are STILL incomplete; red glow/blue shine parity is NOT established.
- Imported world/model/HUD material images now use waw_world/<source image>, resolved to original WaW pixels only at staging. Engine code images remain references; effects retain their separate namespace. Prevents source wallbuy/skin images resolving to different common_zm pixels.
- Full build exposed stale extraction files in modzone.activate_mod_shaders (hud_icon_monkey absent from fresh baseline but left in older dumps). Generated baseline dump, overlay, verification dump and shader output are cleared within the verified work root per run. Fresh rebuild passed; no stale files accepted as evidence.
- T4 Unlinker, T6 Linker and diagnostic Unlinker built; T4 patch regenerated, reverse --check passes. 140 tests pass. tools/audit_brush_render.py compares staged FBX to linked diagnostics and validates range coverage: 176 models, 1,796 triangles, 5,753 static / 6,093 total surfaces, zero errors. 43,860 linked material args checked: zero violations or absent techsets. Source lava/pel1_glow DDS pixels, material bindings and translated shader pairs match linked output exactly (reference marker canonicalized).
- Rebuilt/installed package; all eight binary package files hash-match staging. Evidence: work/underground_brush_audit.json, underground_texture_audit.json, underground_material_audit.log, underground_installed_hashes.json; build logs run_bridge_underground.log (first attempt fails stale verification), underground_mod_rebuild.log, underground_compile.log, underground_link.log, underground_package.log. NO in-game visual or elevator movement verification this session. User should retest underground on the new build; remaining lighting approximations are reported, not claimed fixed.

## Session 25 (2026-10-01): sky crash and square effect sprites fixed (user-verified in game)

- SKY CRASH (dump 23:44, 0x77C253): T6 sub_77C210 resolves MTL_ARG_MATERIAL_CONST (type 0) by scanning the material constant table (32-byte entries) with no end bound, continuing from the previous match; sub_77C150 does the same for type-2 samplers over the texture table (16-byte entries). The four source sky materials kept a donor constant 0xd8e38a65 they do not have. `shaderruntime.prune_missing_material_constants` removes it; it now hashes names with `t6_hash` (T6 R_HashString ORs 0x20 per byte, so '_' differs from WaW's lower()). `tools/audit_material_args.py <extracted zone> [techset roots]` replays both lookups: stock zm_nuked 175,770 args / 0 violations; crashing build 8 (sky only); installed map FF and mod.ff 0. USER: loaded, sky fine, no crash.
- Weather CSC: r_lightTweakSunLight/SunColor are registered 0x1200 (SAVED); script setdvar refuses dvars without 0x4000, setsaveddvar needs 0x1000 -> `_waw2bo2_environment.csc` uses setsaveddvar.
- SQUARE NUKE / RAY GUN SPRITES: ROOT CAUSE was asset NAME COLLISION. 38 of 68 effect images (fxt_smk_def_3, fxt_smk_gen, fxt_fx_raygun_ring, ...) exist in stock common_zm, which loads first, so BO2's own textures (other sizes/atlas layouts; ring colour is grey with the shape only in alpha) were drawn under WaW UVs. Effect images are now `waw_fx/<name>` (t6bridge.FX_IMAGE_PREFIX; stage_images strips it to find the WaW source). USER: fixed. Shaders, UVs, atlas, blend and texture data were all verified identical to stock/WaW first; don't revisit them for this.
- Effect image streaming: stock effect images (125/125) are streaming 2 with a header-only loadDef (format, resourceSize 0); world/model images are 1. Staging writes `images/streaming.json` and LoaderImageT6 applies mode 2 to listed images. Inlining effect images (loadDef with pixels, levelCount = mip count) was REJECTED by the game with E_INVALIDARG at 0x74C0E0 (CreateTexture2D in sub_74BFC0, called by sub_74C680); stock mipped inline images use loadDef levelCount 0. Not pursued.
- Unlinker image dumper logs `ImageMeta` lines (streaming, loadDef, levels) for comparing linked images with stock.
- Remaining name collisions with common_zm outside effects: fxt_zmb_wep_wallbuy_03, limbs_n, skin_pore_detail (not renamed).
- LAUNCH: the bootstrapper must start with working directory %LOCALAPPDATA%\Plutonium (else "no binary"): `Start-Process ...\plutonium-bootstrapper-win32.exe -ArgumentList 't6zm "<BO2>" -lan +set fs_game mods/zm_nuketown_waw +devmap zm_nuketown_waw' -WorkingDirectory $env:LOCALAPPDATA\Plutonium`.
- 133 tests pass. NEXT (user): door hints and the mystery box do not work.

### Session 25 continued: hints, WaW mystery box, menu crash, player timing (NOT yet playtested except as noted)
- HINTS: the map routes hints through UGX `trem_hintstrings` (WaW menu client dvars BO2 never draws). gsc_api `library_functions` maps `maps\trem_hintstrings::_sethintstring` -> compat `waw_native_hintstring` (native setHintString; BO2 draws &&1 as the use key).
- WAW MYSTERY BOX (user chose full WaW box over BO2's): the box lives in the map's `_zombiemode_weapons` override, which BO2 replaces; BO2 _zm_magicbox found no chests. gscport WEAPON_REGISTRATION["box"] extracts `treasure_chest_init`; `extract_level_state` keeps the init() assignments to level fields the box reads (level.boxAnim, box_moved); generated `_waw2bo2_weapons::start_box()` is hooked after `waw_main_post` (hook_bo2_box). Sibling calls inside extracted framework code now resolve to the WaW sibling (after BO2 counterpart/compat), not to an unrelated BO2 script (bug: treasure_chest_think resolved to _zm_magicbox's). level.chests/chest_index -> level.waw_chests/waw_chest_index (WAW_LEVEL_FIELDS; BO2 fire sale iterates level.chests). dlc3_code `*_weighting_func` extractable (gsc_api extract_exceptions; ray gun odds). Compat waw_stopuseanimtree (no-op), waw_clearanim (default time 0).
- BOX ANIMATIONS: `t6bridge.stage_core_animtrees` (box script only; not d2p/rake/generic_human) writes `animtrees/nuketown.atr` (flat list, header-marked, cleaned per run) and copies WaW raw xanims; mod_extra.zone gets `rawfile,animtrees/<tree>.atr` + `xanim,<name>` (official linker reads WaW raw xanims). WITHOUT the rawfile the map ERR_DROPs "unknown anim tree" at load. lukkie_magic_box_fake_a/b have no xanim anywhere (reported; never played). Linked box model keeps j_hinge/tag_animate skin.
- MAIN-MENU LUI CRASH (ui_mp/T6/MainMenuOG.lua:8 after a game): reproduced on our map, not on stock zm_nuked (tools: scratch menu_return_test.py drives the bootstrapper console via stdin: devmap, `disconnect`). Our zm/mapstable.csv row had empty frontend columns; stage_lobby_map_table fills them from the base-game row (column 11 == 0). Verified: load + return to menu clean.
- PLAYER TIMING (generic): WaW _zombiemode::main blocks on flag_wait("all_players_connected"), so map mains loop getPlayers() after it. `waw_main_post` now starts with flag_wait("initial_players_connected"). Expected to fix the underground teleporter (per-player threads) and other per-player setup. Not playtested.
- PACK-A-PUNCH: BO2 _zm_perks handles WaW `zombie_vending_upgrade` ("old_packs"); _zm_power powers it via script_noteworthy specialty_weapupgrade. In-game probe (temporary raw script, removed): 32 base weapons upgradable, fs_colt passes can_buy/can_upgrade/included. User reported "machine doesn't take the gun" on the PREVIOUS build; latest build not yet tested by the user's tester.
- 135 tests pass.

## Session 24 (2026-09-30): source sky layers, fog API and sun split installed

- USER: continue lights/shaders/visions/weather; sky too sunny, black spots, lighting unlike WaW. No gameplay verification this session. Black spots are NOT confirmed fixed; remaining directional lightmap/model lighting/exposure work still matters.
- Replaced the cubemap-on-BO2-dome staging path when source worldspawn has skyboxmodel. Generic stage_source_skybox imports the actual source model/GLTF buffers, all its layered materials, textures, original alpha/depth states, and paired translated SM3 shaders. Current source skybox_ber2 has four layers: background, top/bottom animated clouds, horizon. The real berlin_2_dome_bkgd image is now included. No alpha42 HDR cubemap substitution in this source-model path.
- Sky shader adapters: T6 full model vertex COLOR0/UV streams explicitly added to vertexDecl (merely changing HLSL inputs was insufficient); translation-free sky matrix composition; original uvScroll literals; CONST_SRC_CODE_GAMETIME index25 bound as ONE row, CB2 row15, stable argument count incremented. Source Texture2D replaces donor TextureCube only for a complete source sky pair. Fog distance is extracted from source shader's normalized-direction path (7700 here), rather than using the physical ~931-unit dome radius. All four shader pairs compile and bind.
- Found actual weather bug in compatibility GSC: setvolfog was passed 17 arguments. Native T6 sub_851590 accepts only 8 or 18 (despite its usage string omitting the eighteenth). Changed adapter to its supported WaW-compatible eight-argument form, which normalizes RGB and retains magnitude as fogColorScale. Native evidence from work/T6_mapped_vision.exe; new IDA worker session 7902d598. Do not blindly square script fog RGB.
- Removed inherited template createart vision selection for nonexistent vision/<project>.vision and light-grid tweaks. Source createart controls fog/grade; unchanged previous template files migrate, manually edited files preserved. Source mp_kneedeep and existing LUT conversion retained. Global renderer r_contrast/r_brightness support is still incomplete.
- SunParse now implements measured T4 direct strength (sunLight - ambientScale)*(1-diffuseFraction), then squares for T6 linear units. Here 0.403 gamma direct strength instead of 0.9. Angles/color retained. EXPOSURE remains mp_dig 2.5; diffuse/sky ambient mapping and exposure calibration are not finished. Scripted lightning sunlight units/timer encoding parity still require native/runtime confirmation.
- Built T6 Linker and applied LAA; compiled all map GSC/CSC; linked zero missing assets. Finished FF roundtrip verifies all four source textures, 3 vertex streams, full argument counts and single-row time binding. Grid byte-identical to staged T6 grid; all 78,035 collision triangles/positions/winding/contents/flags match source. 129 tests pass. Logs: work/lighting_source_sky_link.log; extraction: work/lighting_source_sky_roundtrip; sky report: work/source_sky_stage.json (also merged into bridge_stage.report.json).
- Packaged to Plutonium mods/zm_nuketown_waw. Installed/build map FF SHA256 3959F6D58C61E3F8C5831F2B20FE3B41EE56E6209EB5B294E60EEEF97ED5EDFF. Previous installed map FF/IPAK backed up in work/lighting_source_sky_backup. Persistent mod.ff remains 0EF746AB45817D0A91C8FA4943F2C9EA2CE287DD901012259084BE7750584865, preserving match-end LUI table fix. No game process was running during installation.
- NEXT: compare this build in game to source sky/fog/clouds; diagnose remaining black spots with exact location/material and shader/lightmap/grid checks. Native T6 lightmap shader was rechecked: it really samples rgb/a ambient + directional term and sqrt(hdr * linear); current flattened T6 pages are not accidentally double-squared. Directional WaW lightmap passes remain reverted because reachable local-light donor passes still consume T6 page encoding. No arbitrary lights/brightness boosts introduced.

## Session 23 (2026-09-30): walkable edge bits and script_model collision boxes

- USER (playtest of Session 22): player-only invisible wall at the yellow-house living room -> kitchen arch (approx. x 800-830, y 435-535, floor z 1688) and on the stairs; grenades and bullets pass it. It appeared with the Session 21 partition compaction, survived the clip-material fix. Also "climbing" a rubble pile outside the house; grenades still pass doors and the debris rocks. User: stop hunting individual objects; it is a structural regression. Darker WaW-like lighting with purple patches (possible RGBA decode) is still open, lower priority.
- Collision DATA is unchanged since Session 21 (brush set diffed identical; triangles audited). Compaction only changed which triangles the engine reaches: T6 gathers at most 256 leaves (sub_6F6C70 -> sub_4F5010, buffer {count, 256}) and 512 partitions per swept query, so before compaction dense areas silently dropped triangles.
- ROOT CAUSE (IDA, T6_mapped_vision.exe): sub_881940 (capsule vs triangle; the player path) copies the triangle's `triEdgeIsWalkable` bit into the trace when the capsule touches an edge; sub_6D8770 derives walkable from normal.z >= 0.7 ONLY when that bit is clear. The bridge wrote all-ones (upstream OAT placeholder): every edge contact was ground, so steep rubble is climbable and edge contacts on arch/stair geometry misbehave. Stock zm_nuked sets 36,587/73,506 (50%), zm_transit 53%; WaW nuketown itself 84,615/234,144 (36%); ours was 234,105/234,105. Rays (bullets/grenades, sub_5D1230) never read the bits, which is why the wall was player-only.
- FIX: T4 clip dump v6 appends triEdgeIsWalkable (WorldConverterDumperT4, patch updated, reverse --check passes). `world.read_collision` reads it (`edge_walkable`). `t6bridge.write_collision_edges` writes `BSP/collisionedges.bin` (uint32 count; per active triangle 9 float corners + edge bits byte). `ClipMapLinker::LoadWalkableEdges` matches linked triangles by quantised corners under cyclic rotation (the linker Morton-reorders triangles and the FBX path rotates corners) and rotates the bits with them; unmatched triangles keep bits clear (engine slope rule); no file -> old all-ones with a warning.
- DOORS/ROCKS: WaW SP_script_model (sub_531C40) gives every script_model contents 0x2080 | collSurf contents and the model's bounds; without collSurfs the entity collides as that box. T6 SP_script_model (sub_5485E0) sets the same contents but traces collSurfs, and the converted door/rock/debris models have none (WaW door: contents 0, collSurfs 0). WaW grenade clipmask measured 0x280E091 (T6 0x280E893), neither has player clip, so door clip brushes (0x30200) never stopped grenades in either game. FIX: `stage_models` gives every map-placed script_model xmodel without collSurfs one 12-triangle box collSurf (contents 0x2080) of its LOD0 bounds (GLTF axes measured: game = (x, -z, y)); triangle convention measured on WaW collSurfs (normal (c-a)x(b-a), outward). Report key `script_model_collision_boxes`. Script-spawned script_models are NOT boxed yet.
- Tests: 125 pass (new tests/test_collision_semantics.py). T4 Unlinker + T6 Linker rebuilt. Full run log `work/run_bridge_walkable_edges.log`.
- NOT VERIFIED IN GAME. If the arch wall persists with WaW edge bits, next suspects: player capsule height vs arch (arch jambs ~62 units, BO2 stand height ~70; test by crouching), and the 256-leaf gather cap.

## Session 23 (2026-09-30): match-end LUI globe metadata fix installed

- User: match ending crashes with LUI error. Exact evidence in installed mod `console_zm.log.001` lines 4759+: GameGlobeZombie.lua:120 `operator * is not supported for nil * number`, called by MoveToUpDirectly -> PrivateGameLobby_Project.PopulateButtons_Project_Zombie -> OpenAfterActionReportIfNeeded, event `gametype_update`. The separate 09:00 native dump is NOT this LUI error (it records an internal sun-dvar script error).
- UI code reads globe rotation coordinates via Engine.TableLookup/GetCurrentMapTableName. Stock raw zm/mapstable.csv has no converted map row; ui_mapname remains the custom map when its world zone unloads. Added generic modzone.stage_lobby_map_table and CLI build-mod integration: stock rows preserved, custom project metadata added with finite neutral globe coordinates in columns 16/17/18, map index/count updated. Linked as `stringtable,zm/mapstable.csv` in persistent mod.ff, not the unloadable map FF. No global Lua overrides or stock UI source changes.
- Existing mod.ff relinked through loaded asset pools with only the table overlay. Keep ImpactFx exclusion and weapon name normalization from the shader overlay routine. Native roundtrip verified custom row numeric (0,0,0), count 8, zero removed asset declarations and exactly one added asset: stringtable,zm/mapstable.csv. New mod.ff installed via package; old backed up to work/lui_endmatch/original_mod.ff. Other map build data unchanged by this fix.
- Logs/build evidence under work/lui_endmatch (linker.log, check.log, package.log). 126 tests passed, including table stock-row preservation, valid custom coordinates and idempotent staging (work/lui_endmatch_tests.log).
- Runtime return-to-lobby is not yet verified. Restart the mod/game to load updated mod.ff and end a match. If it still fails, capture newest exact LUI traceback before changing broader UI behavior; check actual active map table and custom-map start-location metadata.

## Session 22 (2026-09-30): generic collision ownership, native LightGrid, and effect shaders installed

Current installed build: `work/final_stage/zone_out/zm_nuketown_waw/zm_nuketown_waw.ff`, packaged successfully to `%LOCALAPPDATA%/Plutonium/storage/t6/mods/zm_nuketown_waw`. Build log `work/lighting_fx_final_link.log`, package log `work/lighting_fx_final_package.log`. No gameplay verification this session. Do NOT claim yellow-house obstruction, remaining projectile misses, lighting fidelity, or nuke artifacts are confirmed fixed.

- User clarified yellow-house wall after first open door, but requests a general WaW-to-BO2 fix. Latest request: after collision, adapt LightGrid, source lighting/weather, and nuke black-square/bleed-through artifacts.
- T4 brush ownership walker now visits the shared following branch for negative leafBrushCount as well as offset children. Clip schema v5 guarantees complete ownership; complete dumps no longer use bounds-based unlisted-brush recovery. Fresh source has 16,218 unique world brushes, 432 entity-owned brushes, zero unlisted brushes. Only one extra entity-owned brush was recovered; world geometry did not materially change, so runtime yellow-house blocking remains unresolved.
- Native collision roundtrip exporter writes `.collision.json`; new `tools/audit_collision_roundtrip.py` compares oriented triangle coordinates plus contents/surface flags. Final build: all 78,035 source active triangles exactly match, no additions/missing/changed triangles. Brush position-tree audit had zero violations. Ordinary 96-unit nearby-query samples max 370 combined brush/partition candidates, below 512; do not continue blaming current ordinary queries on that cap without runtime evidence.
- T4/T6 packed grid row decoders measured identical in native code (T4 sub_71C3D0, T6 sub_7584D0). Python decoder validates empty runs, 3/4-byte populated runs, four-byte row offsets and entry bounds. Native loader now imports 998 rows, 94,331 entries, 20,421 palettes. T6 palette decode (sub_755810) is 32*(byte/255)^2; bytes are converted by round(c/sqrt(8)) for WaW model shader gamma scale 2*c/255. Entries preserve palette and primary-light indices. T4 needsTrace is a corner-bit obstruction mask, not T6 visibility: converted visibility is 255 for a selected nonzero light; 40,770 source points still need a faithful corner-obstruction adapter. No coefficients/sky-light regions are invented.
- Final native `.t6lightgrid.bin` byte-equals BSP/lightgrid.bin; SHA256 07422f498c6719e3f9f653d4f594c55e56d6fa923cff36d0859b3a19f4af8224. Roundtrip folder `work/lighting_fx_final_roundtrip`.
- Imported 54 source primary lights, with WaW omni enum 3 -> T6 5. T4 lightdef dump, T6 dependency loader, and actual source `light_point_linear` + `falloff_linear` attenuation texture added. No stock attenuation substitution. Source per-surface/model primary-light indices preserved (gfx dump v3, FBX `_pl` metadata, merge key includes primary light); 3,842 placements. Source sun angles/color/ambient metadata now overlays mp_dig defaults, but gamma calibration, diffuse fraction and exposure are NOT complete (exposure still 2.5). Weather scripted sunlight unit conversion and source outdoor matrix/image remain open.
- Effect materials previously bypassed the shader translator entirely. They now call shaderruntime.bind_material. Added source vertex material-constant embedding (T4 type 0), D3D11 projected clip-coordinate mapping without DX9 half-texel bias, and reciprocal depth -> camera depth adapter measured against native soft-particle PS (`zNear.x/abs(floatZ)`). All five nuke materials now use paired original WaW vertex/pixel programs. 78/83 effect materials active; unsupported include outdoorMapSampler on lightning ground/smk_gen_z200 and absent original/native metadata for three effects. Report `work/fx_runtime_session22.json`. Source smoke DDS alpha was intact and target IWI matched the converter exactly; missing alpha was not established as the cause.
- Imported local primary lights make previously unreachable stock spot/omni passes reachable. Expanded REACHABLE_LIT_SLOTS to 4,5,6,7,8,13,14. Mixed WaW/native lightmap readers cannot share a page encoding: 131 material bindings were revalidated and WaW lightmap passes reverted where necessary. Current BSP/lightmaps.json wawMaterials is empty, and native converted T6 lightmap pages are used. These approximate flat-normal lighting; original directional lightmap fidelity requires remaining spot/omni shader adapters or separate compatible material/page routing. Do not quietly restore old sun-only reachability to hide this conflict.
- T4 changes persisted into vendor/OpenAssetTools.patch; reverse apply --check passed. run_bridge dumps lightdefs and caches companion lightdefs as v9. Preserve prior Session 21 ImpactFx named-reference fix and sourceShader activation. T6 native tools rebuilt; reapply LARGEADDRESSAWARE after every Linker build.
- Verification: 121 Python tests pass (`work/lighting_latest_tests.log`), native builds pass, final link has zero missing assets, git diff --check clean. Installed build needs game checks: yellow-house interior after first door, grenades against doors/rocks/furniture/bus, floor gaps, static-prop lighting, lightning and nuke alpha/occlusion. Tests cannot establish live-game fixes.

### Next (current)
1. Obtain in-game evidence for collision and this lighting/effect build; inspect any new crash dump. Remaining collision misses and yellow-house obstruction remain open.
2. Finish source sun/diffuse/exposure/weather unit calibration, outdoor visibility mapping, and T4 grid corner-obstruction semantics. Native T4 sub_705920 computes primary sun gamma color as (sunLight-ambientScale)*(1-diffuseFraction)*sunColor; current SunParse overlay does NOT yet reproduce that whole contract. Do not blindly square raw source parameters and call this exact.
3. Finish local-light shader/attenuation/shadow adapters so directional WaW lightmaps can remain active without mixed-encoding errors. Current converted pages are an explicit approximation.
4. Verify nuke source paired shaders in game; examine depth/clip/alpha contracts if artifacts persist. No map-specific fake geometry or effect replacement.

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

- BLACK MODELS ROOT CAUSE: world lightmap data/UVs verified consistent against WaW's own data (as-is sampling: 138/15,262 surfaces dark, flipped: 1,121). Black rocks/planks/furniture are static models lit by the T6 LIGHT GRID, which GfxWorldLinker::LoadLightGrid still writes as the upstream OAT placeholder (200x200x50-cell box at the origin, colours memset only rowDataStartSize*2 bytes). The WaW grid (lightgrid.bin, preserved in content_source/lighting) is not converted. NEXT: measure T6 GfxLightGrid (rows/entries/compressed colours/coeffs vs stock zm_nuked dump) and convert WaW's grid.

### Next
0. Convert the WaW light grid (see above); then A/B again.
0b. A/B: compare the two installed mods at the same spots; then bisect model (mc_) vs world (wc_) programs if the noise follows the WaW programs.
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

## Session 33 (2026-10-02)

Measured and fixed:
- T6 binds the per-surface lightmap (t13) / reflection probe (t15) only when a pass's
  `customSamplerFlags` has bit 1 / bit 0 (all 8003 stock passes). Translated passes inherited
  donor flags and read stale textures (teal "fullbright" look). `shaderruntime` now ORs
  `CUSTOM_SAMPLER_BITS` for every per-surface read. `r_lightMapSecondary` is never read by T6.
- WaW vertex colour is D3DCOLOR (B,G,R,A); T6 reads R8G8B8A8_UNORM (input layout table
  byte_D1F7E0, sub_7314B0). `world._color` now swaps; this fixed blue lava/yellow teleporter.
- WaW spot-shadow PS (2818 measured) compare the raw z of TEXCOORD4; T6's lookup matrix needs
  z / w (stock spot-shadow PS divides xyz by w). New `projective_depth` input adapter.
  Black ground patches were all LIT_SPOT_SHADOW (technique 8) surfaces of light 39.
- World brushes with only DETAIL/STRUCTURAL/TRANSPARENT contents collide with nothing in WaW;
  `hulls.collision_brushes` drops them (133 on nuketown, incl. the bunker hatch portal brush).

Verified (no change needed): WaW sub_705A70 and T6 sub_73AC60 copy primary-light dir
unchanged; T6 sub_782FA0 rows equal WaW's uniforms; primary light indices match WaW.

Open:
- The "misplaced lights" the user sees are UGX Easy-FX effects (script_structs targetname
  "fx", ugx_easy_fx.gsc), not primary lights: their orientation is wrong in BO2. A probe
  script (Plutonium scripts/zm/waw2bo2_fxprobe.gsc) logs struct and fx-model angles to
  mods/zm_nuketown_waw/games_mp.log.
- Bunker hatch: a player trace at (-1388,1310) falls through; need the user's blocked spot.
- Diagnostic: WAW2BO2_DIAG_SLOT_COLORS now gives each lit technique a unique colour.
- Unlinker surfverts dump now has a vertex colour column.

## 2026-10-04: geyser launch and physics brush vertices
- Geyser source waits for the elevated push trigger (`getEnt(pad.target,"targetname")`). `gscport.bridge_launch_triggers` recognizes chained trigger velocity launchers by structure and includes their activation pad in the touching test. No map-name condition. `setvelocity` now routes through `waw_setvelocity`, lifting grounded players one unit for upward impulses before calling native setvelocity in the same frame.
- Latest crash dump: `plutonium-r5354-t6zm-2026-10-04_04-01-16.dmp`, EIP 0x006B836B reads `[edi+4]`, EDI=0. Physics support routine reads the vertex count at object+0x50 and pointer at +0x54, matching T6 cbrush_t. The referenced brush memory is absent from the dump, so the exact brush and causal connection to geysers are unproved. The last GSC error mentions powerup_hud_monitor; do not equate it with this native fault.
- Staged world has 27 plane-only brushes with empty verts. `hulls.world_brush` now reconstructs missing convex vertices by intersecting original planes and checking all half-spaces (0.01 tolerance); invalid hulls fail rather than emit a null pointer. No AABB replacement. All 27 repaired successfully, no empty submodel brush hulls.
- 28 relevant regression tests passed (launch, hull recovery, GSC port, collision semantics). Official script compilation passed; T6 bridge linked with exit 0 and no missing assets. Installed zone SHA256 matches rebuilt zone. Installed material audit: 66156 arguments, 0 violations, 0 missing techsets.
- Installed update at Plutonium/storage/t6/mods/zm_nuketown_waw. Backup of staged files and previous installed map fastfile: work/geyser_fix_backup_20261004. Runtime validation still pending: stand on each geyser without jumping, verify lift on eruption and ride/landing repeatedly without a crash.

## 2026-10-04: global WaW perk ownership
- User explicitly requires WaW perk gameplay only, implemented in the converter for every map. `gsc_api.json.preserved_scripts` makes `maps/_zombiemode_perks` portable even for stock WaW sources, overriding framework-name patterns. The map override (IWD/rawfile priority) or stock WaW source owns its machines, purchases, bottles, HUD and loss handlers. Existing framework-hook extraction now starts standard WaW perks together with its extensions once, before the player wait.
- `gscport.stage_bo2_perk_support` creates a project-local BO2 `_zm_perks` override. Its init keeps required client-field registration, support arrays and flags but starts no BO2 machine spawns, vending purchases, power listeners, Pack-a-Punch controllers or host migration. BO2 pause/unpause-all entry points cannot alter WaW machines. Other native framework support exports remain callable; the source WaW scripts route to their own translated framework. No installed BO2 raw files changed.
- `t6bridge.stage_bridge` stages this automatically for converted scripts; `bo2_scripts`/`map_scripts` include the override in compilation and zone output. Documentation: docs/PERK_CONVERSION.md. Added tests/test_perk_ownership.py (stock preservation, extensions, routing, controller suppression, client fields, compilation list). Two new tests and 13 GSC regression tests pass.
- Full tools/run_bridge.ps1 rebuild completed with exit 0: zero stage errors, source Pack-a-Punch FX newly discovered/converted, official mod/script compilation, bridge linking, packaging. Installed audits: 66545 map arguments and 67090 mod arguments, both zero violations/missing techsets. Both installed perk scripts were extracted from the actual installed map fastfile and match the freshly compiled bytecode exactly (WaW 25475 bytes; BO2 support 67801 bytes).
- Logs: work/perk_rebuild_20261004.log, work/perk_installed_scripts_20261004.log. Runtime checks still pending: restart map, check single purchase/charge, source HUD, power/availability, perk loss after downing/death and Pack-a-Punch. Previous geyser global changes survived the full rebuild; staged world has zero vertex-less brushes.

## 2026-10-04: remove one-sided traversal collision globally
- User explicitly asked to remove collision from one-sided traversal sheets instead of attempting WaW one-way behavior. The converter's collision_material_slots now omits triangles identified by oneway.one_way_sheets: player-blocking, non-solid, steep, with no opposing face. Both FBX collision and walkable-edge export use this filter; solid geometry, floors and genuinely opposing-face sheets remain. Same-facing duplicate triangles are still one-sided and removed.
- Deleted the generated player movement/teleport emulation. oneway_source produces only a no-op init so older generated map hooks stay valid. stage_geometry reports collision_triangles_one_sided_removed and ONE_WAY_COLLISION_REMOVED; port_scripts no longer claims emulation. Documented the user-directed policy in docs/COLLISION_CONVERSION.md section 7.
- Current map test case: 484 terrain triangles removed (190 clip, 38 clip_nosight_rock, 252 berlin_window_glass, 4 com_glass_dirty_stain2), leaving 77551 terrain triangles and 232653 exported edges. Regenerated collision mesh, material slots and edges through converter functions from the original WaW clip dump; disabled the old generated script; compiled, linked and packaged successfully with no missing assets.
- 28 relevant tests passed (one-way removal/duplicate orientation/edge consistency, collision semantics, hull recovery, GSC port). Installed material audit: 66545 arguments, zero violations/missing techsets. Installed one-way script bytecode matches the compiled 122-byte no-op. Extracted actual installed collision JSON: 77551 triangles, zero of the 484 removed geometry keys remain. In-game traversal still requires confirmation after restarting the map.
- Artifacts: work/oneway_removal_backup_20261004, work/oneway_compile_20261004.log, work/final_stage/oneway_removed_bridge.log, work/oneway_removed_audit, work/oneway_removed_geometry/maps/mp/zm_nuketown_waw.d3dbsp.collision.json. The WaW-only perk change remains in the rebuilt zone.

## 2026-10-04: duplicate perk HUD client callback fix
- User's screenshot still showed both WaW and BO2 quick-revive icons after the server-only ownership fix. Stock client `_zm_perks.csc` was still installing native LUI perk callbacks. Confirmed with the IDA skill and t6zm.exe.i64: `sub_7CD8A0` (setupclientfieldcodecallbacks) attaches `sub_6FD5D0`, which dispatches field-name LUI events with oldValue/newValue used by hudperkszombie.lua.
- Global `stage_bo2_perk_support` now also emits `clientscripts/mp/zombies/_zm_perks.csc`. Original registrations, including schema/order and FX callbacks, remain exactly intact; perk_init_code_callbacks is empty and native custom-perk threads are disabled. Native setperk remains so WaW purchases retain engine effects. WaW keeps its own HUD. Both overrides automatically enter map_scripts/zone generation for every converted map.
- Two expanded perk ownership tests and 13 GSC regression tests pass. Official client/server script compilation and bridge link passed (zero missing assets). Installed after user closed the game to release the fastfile lock. Extracted actual installed scripts match compiled bytes: client override 4914, server override 67801, source WaW perks 25475, one-way no-op 122. Installed fastfile SHA256 matches rebuilt output. Actual stock client registration function compared byte-for-byte equal after staging; no code callback calls remain in perk_init_code_callbacks.
- Docs: docs/PERK_CONVERSION.md. Backup: work/perk_client_backup_20261004. Logs: work/perk_client_compile_20261004.log, work/final_stage/perk_client_bridge.log, work/perk_client_package_20261004.log, work/perk_client_installed_scripts_20261004.log. In-game verification pending: relaunch, purchase a perk, verify only WaW icon and loss behavior. Previous geometry and geyser fixes retained; no geometry restaging in this script-only update.

## 2026-10-04: buildable revive checks, perk power events and sprint duration
- User reported rake/shield failure, machines not lighting with power, and screenshot of repeated player_sprintTime=15 rejection (T6 global dvar max12.8). Optional question about failure phase received no answer during this work; exact in-game failure phase remains unverified.
- Both source buildables directly negate player.being_revived during collection/construction. T6 _zm_laststand instead stores revivetrigger.beingrevived. gscport.bridge_revive_reads translates source reads globally to compat waw_being_revived, preserving explicit isdefined probes and custom source writes. Helper honors source custom revival=true and otherwise reads guarded native trigger state. Current stage: 14 translated reads across rake/shield, C4, claymore, tactical insertion and perk scripts.
- power_bridge now sends WaW's standard sleight_on/revive_on/doubletap_on/juggernog_on/Pack_A_Punch_on events with network-frame ordering when electricity_on is set, including when BO2 power_on was already true. Source model-swap threads were waiting on these missing events; source DLC3 framework entry is replaced/stubbed by BO2. No new machine controller or coordinate/map-specific change. Current source double-tap starts with its _on model already, so its commented source setmodel is intentionally preserved.
- waw_setclientdvar routes player_sprintTime on player receivers to setsprintduration(float(value)), preserving 15 seconds per player without global dvar rejection. Native t6zm.exe sub_6CE860 verified in IDA: accepts seconds below16383, converts to ms at player state+1228; 15 is valid. Existing character hook's initial4-second sprint remains; source perk updates use per-player duration.
- Three player-state tests plus 13 GSC tests and two perk-ownership tests pass. Official script compilation and bridge link passed with zero missing assets. Installed package successfully without a lock. Extracted installed bytes match compilation: compat14851, rake9302, shield10625, WaW perks25487, BO2 client override4914, one-way no-op122. Installed fastfile SHA256 matches rebuilt output. Script-only change retains earlier geometry and geyser fixes.
- Backup work/buildable_power_backup_20261004; logs work/buildable_power_compile_20261004.log, work/final_stage/buildable_power_bridge.log, work/buildable_power_package_20261004.log, work/buildable_power_installed_scripts_20261004.log. Runtime check pending: collect parts, build/pick/equip/use both buildables, turn on both power sources and check lit machine models/FX, verify Stamin-Up duration and no sprint console spam. Do not claim in-game validation yet.

## 2026-10-04: explicit namespaced perk client startup
- User confirmed Quick Revive still shows native BO2 and WaW icons after disabling same-name client code callbacks. The previous same-name override was insufficient in game; its installed bytecode was verified, but stock client dependency/cache precedence remains the likely explanation, not a runtime-proven cause. Native callback analysis also verified registerclientfield arg8 controls initial zero-value callbacks (field+40), rather than enabling LUI by itself; did not alter the field schema or native effects.
- stage_bo2_perk_support now stages unique clientscripts/mp/waw/_waw2bo2_perks.csc (same compatibility client source) and _waw2bo2_zm.csc (stock BO2 client bootstrap with all perk includes/qualified calls routed to the unique compatibility script). hook_bo2_perk_client explicitly routes every converted map's client _zm::init call to the unique bootstrap. t6bridge.stage_bridge invokes this hook globally; existing map_scripts includes both mp/waw CSCs automatically. This avoids relying on same-name stock script replacement; other BO2 client systems remain in the bootstrap.
- Perk tests expanded for explicit startup routing, unchanged non-perk initializer, unique script staging/compile-list inclusion and hook idempotence. Two perk tests, 13 GSC regressions and three player-state tests pass. Official CSC/GSC compilation and bridge linking pass with zero missing assets.
- Installed after user closed game to release fastfile lock. Actual extracted installed scripts match compilation: map CSC14457, unique bootstrap29194, unique perks4126. Verified installed bytecode map imports owned bootstrap; bootstrap imports owned perks and contains no native _zm_perks import; owned perk script has no setupclientfieldcodecallbacks symbol. Compat14851, shield10625, rake9302, one-way122 remain exact. Installed fastfile SHA256 equals rebuilt zone. In-game Quick Revive validation is still pending; do not claim verified disappearance of duplicate icons.
- Backup work/perk_bootstrap_backup_20261004; logs work/perk_bootstrap_compile_20261004.log, work/final_stage/perk_bootstrap_bridge.log, work/perk_bootstrap_package_20261004.log, work/perk_bootstrap_installed_scripts_20261004.log. Docs/PERK_CONVERSION.md describes why startup now calls unique scripts explicitly.
# 2026-10-04: global Zombies globe/co-op selection
- User confirmed "globe option" means the BO2 Zombies globe menu in a co-op lobby. Implemented globally in modzone/cli/package, with no map gameplay/world edits.
- stage_lobby_gametype_table preserves stock rows and adds one category-5 default location plus a category-6 zclassic/content0/YES default mode per project. Allocates noncolliding indices and fixes both count bounds; repeat staging is idempotent. Existing stage_lobby_map_table supplies map identity, coordinates and no-DLC metadata.
- link_lobby creates frontend-only mod_load.ff containing both tables and five custom-map material aliases (background, blur, mode card and two loading-screen names). WaW lacks BO2 globe artwork; the stock empty map frame supplies interface scaffolding. Native map/location navigation and party/game-mode selection remain in charge. Tables also persist in mod.ff. Package now requires mod_load.ff and derives its mod.json name from the project rather than hardcoding Nuketown. Default printed launch opens the online Zombies lobby; explicit direct_map=True retains LAN/devmap testing.
- Actual extraction caught OAT's sorted search paths selecting stock CSVs instead of generated tables. Corrected frontend root priority and a table-only gameplay override layer while retaining stock gameplay lookup priority; shader activation copies those tables too. verify_lobby_tables extracts both built files and refuses builds lacking the custom map/location/default mode.
- Interrupted build temporarily left 1313 staging materials on baseline shaders. Restored runtime techniques from active conversion reports and reran shader activation/validation before final install. Actual installed gameplay asset list contains every pre-update asset, with only zm/gametypestable.csv added; no gameplay assets removed. Bound shader/texture roundtrip checks passed. Installed map ff remains byte-identical to the existing map output, retaining earlier scripts/collision/geyser work.
- Eight relevant tests pass (six modzone, two package), compileall passes; verified full build-mod exit0 and extracted table registrations. Final package installed successfully. Installed mod.ff and mod_load.ff hashes match builds. Material audit: 67090 gameplay arguments and 5 frontend arguments, zero violations/missing techsets. Both installed tables contain custom map index7, location16/default and mode20/zclassic/content0/YES (counts8/17/21).
- Logs: work/globe_build_verified_20261004.log, work/globe_shader_verify_20261004.log, work/globe_package_20261004.log. Actual extraction: work/globe_final_gameplay_audit_20261004 and work/globe_final_frontend_with_loading_20261004. Backup: work/globe_backup_20261004. Docs: docs/LOBBY_SELECTION.md.
- Runtime menu/co-op session is not tested. User route: load converted mod in Zombies -> Custom Games -> Change Map -> zm_nuketown_waw -> default play area/mode. Joining players need the same converted mod. Earlier reported Quick Revive/buildable runtime fixes remain pending user validation; do not claim in-game validation.

## 2026-10-05: remaining OWN3D materials

- User identified M14/M16A2, the spawn-area tree and a TV screen; requires global converter fixes. Their staged materials used the mod's pink `default_c` through `mc/mtl_default` substitution. M14/M16A2 source GLTFs name `mc/$default3d` on small helper surfaces; its actual WaW raw built-in is `$default3d`, with `default` colorMap and the `default` technique. Source lookup missed draw-family-prefixed raw material names, and the material lookup did not cover weapon materials before visual staging.
- Shared `recover_material_sources` now covers world/HUD/model/weapon definitions in WaW-first order, including raw sources. Draw-family aliases of engine `$` materials use the actual unprefixed source. `default` maps to a model unlit donor while retaining the original colorMap; the staged builtin also binds the original WaW textured_simple VS/PS pair in slot 2. WaW compilation verifies the requested asset exists in a fresh dump rather than accepting a fastfile alone.
- Imported foliage, TV screen and Double Tap bottle definitions are absent from the whole WaW lookup. Shared equivalent table maps them to their corresponding native BO2 material. The mc foliage definition exists only in BO2 raw, while its native technique/images are dumped from stock; both roots are now searched. Native image bytes remain namespaced and weapon staging retains these bindings. Existing WaW definitions still win. No map-name, location or weapon-geometry rules.
- Full restaging exposed animtree hook idempotence with the existing namespaced perk client bootstrap. `hook_bo2_animtrees` accepts that initializer on reruns, with a regression test. Reapplying the fixed hook changed no startup calls; the sole staging error was then cleared after validation, and downstream builds resumed. Earlier staging failure remains recorded in `work/pink_material_build_20261005.log`.
- 210 tests pass. Backup: `work/pink_material_backup_20261005`. Documentation: `docs/MATERIAL_SOURCE_RECOVERY.md`. Build/installed roundtrip validation is recorded below when complete. Runtime appearance still requires user testing; do not claim a playtest.
- Completed official gameplay build, script compilation, bridge link (exit 0, zero missing assets) and package installation. Re-extracted final world/gameplay materials and images: four repaired material bindings and seven image pixel/mip payloads match staging exactly, no selected material binds `default_c`. Material argument audits: world 66,674 and gameplay 67,095 arguments, zero violations/absent techsets. Five installed FF/IPAK hashes match built outputs.
- Restaging also attempted to add one more pre-existing environment startup call. The hook now inserts only when absent; restored the existing map CSC from backup and recompiled/relinked before installing. Final script-content comparison has no changes (eight source GSCs differ only in whitespace). M14/M16 viewmodel GLTF bytes, collision brush/model metadata and entities remain unchanged. This session does not remove pre-existing environment duplicates or claim runtime validation.
- Evidence: `work/pink_material_audit_20261005.json`, `work/pink_material_roundtrip_20261005/{map_final,mod}`, `work/pink_material_{mod,scripts,world,package}_20261005.log`, `work/final_stage/pink_material_bridge_20261005.log`. Other unresolved community weapon/perk materials remain explicitly reported; the identified M14/M16/tree/TV definitions and Double Tap bottle are recovered.
- Final regression suite: 211 tests passed, including raw WaW recovery taking precedence over native equivalents and corrected dependency-report provenance. `git diff --check` passes.
### 2026-10-05: original perk-lighting compatibility fix installed (runtime pending)

- User explicitly rejected BO2 model substitutes for Cherry/Mule Kick/Stamin-Up. Preserve original WaW assets; do not add native machine equivalents or invent lit variants.
- Fixed script model lookup filtering before complete WaW search: `recover_script_models` now searches compiled map roots, stock WaW zones, then compiled WaW raw xmodel sources. `WawSourceAssets.source` supports binary `raw/xmodel` definitions. Recovered roots feed normal conversion; setModel-only resolved models now enter first-frame precache.
- Added generic regression tests; 216 unittest tests pass. New source changes are uncommitted; earlier pink-material changes are also uncommitted and must be preserved.
- Actual original map audit: all 5 FF files, both IWDs, stock WaW zone index, and raw definitions lack `p6_zm_vending_electric_cherry_on`, `bo1_zombie_vending_marathon_on`, `bo1_zombie_vending_triple_gun_on`, `zombie_vending_marathon_on`, `zombie_vending_triple_gun_on`. Evidence `work/perk_waw_source_audit_20261005.json`, script `work/audit_perk_waw_sources_20261005.py`.
- Missing-model precaches/swaps are explicitly reported and guarded, retaining existing WaW machines and letting original power-on omni-light FX and notifications execute. Source-backed models are converted and precached normally. Runtime appearance still needs verification; do not claim a playtest.
- User confirmed these machines light in original WaW. Purchase-state question remains unanswered. Original source light FX contain looping omni lights; missing model precaches/swaps precede startup/thread lighting and can stop T6 execution. Added a generic explicit missing-model guard preserving existing models and subsequent source light FX/notifications. Full build completed with exit 0 and zero missing linker assets, log `work/perk_lighting_build_20261005.log`; 216 tests pass.
- Documentation `docs/SCRIPT_MODEL_SOURCE_RECOVERY.md`.

- Installed/re-extracted validation: original three machines' model definitions, GLTF geometry, materials, and owned image bytes match backup; three original power-on FX match compiled roundtrip (excluding `_source` provenance metadata), including looping omni elements. Both perk scripts and compat bytecode match extraction; five installed FF/IPAK hashes match outputs. World/gameplay material audits: 66,986 / 67,095 arguments, zero violations or absent techsets. Evidence `work/perk_lighting_audit_20261005.json`, `work/perk_lighting_roundtrip_20261005`, audit script `work/audit_perk_lighting_20261005.py`. Backup `work/perk_lighting_backup_20261005`. No BO2 machine fallback added.

## 2026-10-05: default reverb echo correction
- User put LightGrid corner work aside, requires standard Plutonium support. No LightGrid source changes made. Audio task: echo on most sounds, especially gunshots; cutoff improvement already confirmed by user.
- Found source WaW driver DEFAULT I3DL2 room/reflections/reverb = -10000 millibels (silent), while BO2 default RAD preset has audible reflections. Generated client ambient script previously declared no room and inherited BO2 default.
- sounds.default_reverb_mix reads staged source globals; t6bridge.write_amb_csc declares default ambient room dry=1/wet=0 and initializes that mix only for a silent source default. Normal room controller can select explicit rooms. Missing/audible custom source defaults are reported, never guessed silent. Alias gain/sends/PCM/storage unchanged. Explicit WaW room preset I3DL2-to-RAD translation still incomplete and now reported.
- 219 tests pass; full mod/world builds succeeded, world linker zero missing assets. Installed nine packaged files verified against build hashes. Native client script bytecode matches extraction of map fastfile. Both fastfiles extract successfully. All four SAB files byte-identical to prior installed build, preserving loaded weapon audio cutoff fix.
- Evidence/backup: work/sound_echo_fix_20261005/{audit.json,default_reverb.json,installed_before,compile.log,build_mod.log,bridge.log,tests.log}; audit.py reproduces checks. In-game echo improvement unverified. New audio changes remain uncommitted alongside pink/perk fixes. Do not claim full source room acoustics preservation.

## 2026-10-05: source localization and WaW hint registry
- User reports unavailable perk/trap localization keys and weapon_ppsh instead of readable UI text. Added generic localization import, owned key rewriting, weapon display field localization, provenance reporting and missing-key conversion errors. WaW map sources take priority over stock/raw and explicit BO2 fallback. No models or weapon behavior substituted.
- Found a separate generic-hint cause: source cost-bearing hint IDs had been routed to BO2's base-ID helper without the source table. Extract WaW init_strings during port_map; run from framework_level_state before map pre-init. Adapt add/get/set hint helpers and direct source level.zombie_hints accesses to an isolated WaW registry. Local map helper definitions retain their implementation.
- Both zone lists include source strings. Native gameplay shader relink now copies english/localizedstrings into its overlay, otherwise this relink cannot resolve the catalog despite the official baseline linker succeeding.
- Nuketown staged catalog: 140 values, zero unresolved references. ZOMBIE_FLAMES_UNAVAILABLE = The power must be activated first; WEAPON_PPSH = PPSh-41; upgraded = The Reaper. Two explicitly reported absent-source fallbacks: PATCH_ZOMBIE_MONKEY -> compatibility text Cymbal Monkey; ZOMBIE_WEAPON_M1911 -> exact BO2 localization. Literal custom labels retain exact wording.
- 229 tests pass; compiled scripts and both native builds pass, world linker zero missing assets. Extracted both fastfiles and compared all 140 localization values; 94 compiled weapon display fields resolve. Source core/compat bytecode matches map extraction. Nine installed FF/IPAK/SAB hashes match builds, all four sound banks unchanged. No in-game playtest.
- Evidence: work/hint_strings_fix_20261005/{audit.py,audit.json,stage.py,tests.log,compile.log,build_mod.log,bridge.log,installed_before,source_before}. Selective stage.py preserves previously converted model/FX guards while adding source hint registry. Full normal bridge pipeline performs this automatically. Documentation docs/LOCALIZATION.md. Changes remain uncommitted alongside earlier fixes.

## 2026-10-05: Pack-a-Punch duplicate regression investigation (OPEN)
- User screenshot shows another Pack-a-Punch machine during use; user says it stays permanently and did not happen before. Initial FX explanation is only a candidate, not confirmed: source FX contains a 10-second model particle, but this does not explain permanent persistence.
- Native extraction of pre-hint installed map vs hint build: both PaP xmodel definitions, all 8 geometry GLBs, collision dumps and PaP FX JSON are byte-identical. Source upgrade-script diff contains only localization references. Extracted gameplay weapon definitions differ only in display fields; no gun/model/behavior field changes. Evidence work/pap_regression_20261005/{before,after,mod_before}.
- Installed a DIAGNOSTIC build, not a claimed fix. Temporary staged compat/perk scripts log source weapon -> resolved model, nearby script_model entities at start/12s/25s, and output-entity cleanup. No source FX elements removed or models hidden. Runtime reproduction is required to distinguish effect persistence from a wrong weapon display model or cleanup failure. Scripts compile, link_check has zero errors, native world linker has zero missing assets; both probe scripts' bytecode and installed fastfile hash verified (probe_audit.json).
- These probes exist ONLY in work/final_stage staged files. Their baseline backups are work/pap_regression_20261005/source_before; work/pap_regression_20261005/probe.py reproducibly injects them. Remove probes after diagnosis and implement confirmed generic fix in converter source. Do not overwrite this staging with older hint audit.py before collecting evidence.
- Current installed map is diagnostic. Other packaged assets remain the hint build. Actual recent game logs are C:/Users/thrif/AppData/Local/Plutonium/console.log and storage/t6/main/console_zm.log (12:15 Oct5); installed mod-folder console_zm.log is stale Oct3. No game process is currently running. Next step: user restart/reproduce once, wait 25s, then inspect WAW2BO2 PAP log lines and runtime errors. Do not claim this regression fixed.

## 2026-10-05: FX model asset isolation installed
- User corrected persistence: the extra machine eventually disappears. Tester screenshot reportedly shows BO2's battery-topped machine overlapping WaW's original. Found model references inside namespaced effects remained unnamespaced, allowing binding to different preloaded BO2 models.
- fx.convert_elem now namespaces every model visual with waw_fx_model/. t6bridge.stage_fx_models emits independent model assets using the already converted WaW definitions and exact source LOD/material dependencies, reports provenance, and errors on missing models. Normal bridge zone includes all owned FX models. No effect element, model geometry, transform, lifetime or source visual behavior removed.
- Removed temporary diagnostic probes by restoring source_before compat/perk files. Installed clean rebuilt world zone; gameplay mod.ff remains the verified hint build. 232 tests pass, script compiler succeeds, native linker zero missing assets.
- Native audit verifies five owned FX models with identical definitions/geometry, seventeen effects changed only in model references/storage sizes, all 140 localized strings, compiled source scripts match extraction, and all nine installed output hashes. Evidence work/pap_regression_20261005/{isolate_models.py,model_isolation.stage.json,audit_isolation.py,model_isolation.audit.json,isolation_bridge.log,isolation_roundtrip}. Documentation docs/FX_MODEL_ISOLATION.md.
- Current installed build is no longer diagnostic. In-game disappearance/appearance with the isolated models still needs user confirmation. These source changes remain uncommitted with prior fixes.

## Grenade collision, 2026-10-05

- User reports normal frags passing white house fences, buyable doors, house pillars, school bus and almost all rocks. Requests a global fix for geometry solid to players/zombies. User states original WaW fence grenades bounce; no original runtime reproduction captured this session.
- Proven model defect: native T6 XModelTraceLine 0x40DFD0 returns -1 when signed model+198 collLod is negative, before checking collSurfs. Entity caller 0x580940 uses that path. The 55 synthesized WaW script-model boxes had surfaces/0x2080 but no collLod. stage_models now selects 0 for synthesized boxes, preserving authored collision LODs.
- Explicit user collision policy: retained PLAYERCLIP|MONSTERCLIP (0x30000) volumes gain MISSILECLIP (0x80). These bits were compatible between engines; this is deliberate grenade-blocking behavior, not a proven engine-bit mismatch. Shapes, other contents, surface flags and ownership remain unchanged. Applied consistently in world brushes, brush submodels, side/axial masks, terrain, xmodel collSurfs and static-model masks. Player-only/monster-only/compile-only masks unchanged. Existing one-sided traversal removal retained.
- Staged 1,210 world and 33 entity brush masks plus two terrain materials changed. Source report counts added. 236 tests passed. tools/audit_collision_roundtrip.py now accounts for the intentional traversal omission and grenade-mask policy.
- Native link zero missing assets; 77,551/77,551 source walkable-edge matches. Native roundtrip proves all 55 collLod values and box surfaces, original brush shapes, 33 changed native brush-model masks, all 77,551 retained triangle positions/winding/intended masks; brush-tree position contract violations zero.
- Evidence work/grenade_collision_20261005/audit.json, staged.json, mask_stage.json, masks_roundtrip and installed_hashes.json. Packaged/installed all nine files; SHA256 map FF ef9c2f56e349cb3ef6ab0111ff54abdc76839b03e2d05ccd6f1ecf30a9659e3b. Runtime bounce behavior NOT playtested. Need a fresh-session frag retest against fences, both house doors, pillars, school bus and rocks. No grenade GSC watcher/teleport and no map/model-name exception introduced.

## Pack-a-Punch stationary FX model reuse, 2026-10-05

- User confirms isolated FX-model fix failed; battery appears only while upgrading. Extracted effect model geometry matches original WaW. Foreign BO2 geometry was not established as the cause.
- Original PaP effect has a ten-second stationary complete powered-machine model while the map entity already displays that model. Added generic conditional variants through fx.entity_model_variants and t6bridge.stage_entity_model_fx. Only single, stationary, unit-scale model elements without collision/physics/child effects qualify. Original FX unchanged; other seven PaP elements retained exactly. Only PaP qualifies in this map.
- Generated asset table maps original FX/source model to variant. waw_loadfx registers native IDs; waw_playfx selects variant only when an existing script_model of that source model is within four units. Original effect remains for unmatched positions/models. No machine hiding/swapping, BO2 replacements, or map/perk-name exception.
- 239 tests pass; script compiler and native linker succeed, zero missing assets. Roundtrip verifies exact seven retained elements, original FX unchanged, compiled scripts match, grenade collision JSON byte-identical. Evidence work/pap_model_reuse_20261005/{audit.json,audit_fix.py,bridge.log,roundtrip,installed_hashes.json,staged.json}.
- Packaged and installed all nine files, hashes verified. Current map FF SHA256 f3d5e6881ceafcc12aabe6bb277b1364d5f41f11d0e766f4791713fa8684d007. Runtime entity matching and visual behavior NOT playtested. User must restart session to test upgrade with power on. Source changes remain uncommitted with preceding fixes.

## Pack-a-Punch server initialization and FX registry correction, 2026-10-05

- User confirms last model-reuse fix also failed. Battery exists on idle machine; new screenshots show second shell/sign/rollers during use. Do not treat battery as proof of a foreign model. Earlier geometry audit found original WaW geometry.
- Investigated script paths: only client used uniquely named perk/bootstrap scripts; server map still called native _zm::init, relying on same-name _zm_perks override. Native initializer starts BO2 machine and purchase controllers and writes stock packapunch_fx in shared level._effect. Earlier runtime execution wasn't captured, so cached native initialization is a concrete competing path, not a proven captured trace.
- stage_bo2_perk_support now emits unique maps/mp/waw/_waw2bo2_perks.gsc and _waw2bo2_zm.gsc. hook_bo2_perk_server routes map server init and perk availability calls to owned scripts; power_bridge also calls owned unpause. Animtree hooks support both owned server/client initialization. Native framework helpers/other initializers remain.
- WAW_LEVEL_FIELDS now isolates source _effect as waw_effect for translated scripts/core/level-state. Compat initializes level.waw_effect. Staged 24 source scripts rewritten; source and BO2 effects cannot overwrite each other through the shared table.
- REMOVED failed stationary model-reuse functions, generated metadata and runtime suppression. Full original eight-element PaP effect restored/preserved, FX model isolation remains. No map/perk-name exceptions or model replacements.
- 237 tests pass. Native compilation/link succeeds with zero missing assets; audit verifies 75 extracted bytecode files match compiler outputs, explicit owned server call chain, isolated source FX registry, complete unchanged source effect and no conditional variant, byte-identical grenade collision JSON. Evidence work/pap_server_ownership_20261005/{staged.json,audit.py,audit.json,compile.log,bridge.log,roundtrip,installed_hashes.json}; staging script work/stage_pap_server_ownership.py.
- Packaged/installed all nine files, hashes verified. Current map FF SHA256 43eba3d7880edfb2d0a1de2334f2f9af941346d416ed6103215f05afa72907e7. Gameplay NOT playtested. Need fresh-session PaP use test; if still doubles, investigate original moving shader/FX rendering and actual runtime model/effect IDs rather than claiming geometry origin from screenshot alone. New changes remain uncommitted with previous fixes.

## Grenade changes fully removed at user request, 2026-10-05

- User reports invisible walls now bounce grenades and explicitly requests complete removal for now. Reverted BOTH shared player/zombie MISSILECLIP promotion and collLod=0 activation for synthesized script-model boxes. Original pre-existing box synthesis remains, with original disabled LOD behavior.
- Restored hulls.py, grenade test additions and collision docs to pre-change state; removed t6bridge mask/LOD changes and grenade stage report fields. Kept independent one-way omission handling in roundtrip audit, with source masks again expected.
- Restored 59 staged assets from exact pre-grenade backups: four BSP collision metadata files and 55 xmodels. Model differences validated limited to grenade LOD/mask fields before restoration. Current Pack-a-Punch server bootstrap/isolated source effects and all other preceding fixes preserved.
- 233 tests pass. Native link succeeds with zero missing assets. Native extraction confirms restored model LODs/contents, map collision JSON identical to pre-mask backup, clipmap diagnostic identical except runtime pointer addresses, all 77,551 retained triangle positions/winding/source masks match. All 75 current compiled scripts preserved.
- Rebuilt, packaged and installed all nine files; hashes verified. Evidence work/grenade_rollback_20261005/{restored.json,audit.py,audit.json,bridge.log,roundtrip,installed_hashes.json}; rollback script work/rollback_grenades_20261005.py. Do not re-enable grenade changes without further user authorization. Gameplay not playtested; fresh session loads rollback.

## PaP overlay reuse after source effect ownership correction, 2026-10-05

- User reports ongoing duplication and suspects unlit-perk missing-FX fix. Subsequent screenshot/user confirms correct model now spawns: model-selection half is fixed, overlap remains. Preserve owned server bootstrap and isolated source effect registry.
- Compared pre-lighting backup: PaP original/_on definitions, all source geometry LODs and materials are identical to current; its eight-element effect already included ten-second full `_on` model. Only later FX namespace references differ. Native WaW/T6 render analysis did not establish an additional flag/shader defect; don't claim lighting guard inserted those assets or completed gameplay fix.
- Added fx.model_overlay_variants and t6bridge.stage_model_overlay_fx: eligible stationary, unit-scale single-spawn one-shot model overlays have conditional variants omitting only that repeated render instance. Original full FX remain for unmatched model/location. Moving, physical, random-rotation, delayed, scaled, child-effect cases excluded. Only PaP qualifies here, with seven other elements exact.
- wav_loadfx registers variant native IDs by original source-owned FX ID; waw_playfx checks nearby (<=4 units) script_model exact source model. This relies on preceding source `level.waw_effect` isolation; earlier reuse attempt preceded that correction and failed user testing. Existing powered WaW entity stays visible. Console-only first-selection log WAW2BO2 FX MODEL REUSE <model>; unmatched log WAW2BO2 FX MODEL UNMATCHED id=<id> origin=<origin>. Next runtime test should capture this evidence if overlap persists.
- 236 tests pass, compilation/link succeeds with zero missing assets. Native audit verifies 75 scripts, original effect unchanged, conditional seven remaining elements exact, source IDs isolated, grenade rollback collision byte-identical. All nine packaged/installed files hash-match. Evidence work/pap_overlay_owned_ids_20261005/{staged.json,audit.py,audit.json,compile.log,bridge.log,roundtrip,installed_hashes.json}, staging work/stage_fx_overlay_owned_ids_20261005.py.
- Current installed map FF SHA256 def921b37d4065e0ee4b6a4047f78f7ad46e678c7d35a5b3bb3b6cc9cc2a8e71. Runtime reuse branch NOT playtested. Need fresh-session upgrade test, retain actual console selection/unmatched line. Grenade changes remain fully removed per user; do not re-enable. Source edits remain uncommitted.

## Monkey tactical startup registration, 2026-10-05

- User CONFIRMED Pack-a-Punch fixed. Captured actual game log contains `WAW2BO2 FX MODEL REUSE zombie_vending_packapunch_on`. Preserve that implementation. New issue: throwing monkey crashes with unrecognized animtree `zombie_cymbal_monkey`.
- Native monkey helper checks `level.zombie_weapons["cymbal_monkey_zm"]` before initializing model/effects/arrays and ScriptModelsUseAnimTree. Source weapon include/add registered only `zombie_cymbal_monkey`, while existing native box grant gives `cymbal_monkey_zm`. Server init therefore skipped the entire setup. Client already includes native runtime name before its init.
- gscport.assets_source now generates supported tactical runtime alias metadata (only for included, nonempty source weapon mappings). Compat include/add registers that helper alias before native monkey init, with in_box=0, retaining original source definition/box entry. Reverse inventory mapping returns source name. No BO2 model substitution: existing legacy flag still selects weapon_zombie_monkey_bomb.
- 237 tests pass; native script compilation and link succeed with zero missing assets. Extracted 75 compiled scripts match compiler bytes. Native startup guard/registration order audited. Original PaP eight-element effect and conditional seven-element variant retained exactly; collision JSON remains byte-identical to grenade rollback.
- Packaged/installed nine files with matching hashes. Current map FF SHA256 7315bf7e59a0559acdca5f1482245b87b542041317028799cc8612dee9432e84. Monkey gameplay NOT retested; requires fresh map/session and throw. Evidence work/monkey_animtree_20261005/{game_before.log,native_monkey.gsc,audit.json,compile.log,bridge.log,tests.log,roundtrip,installed_hashes.json}. Docs docs/TACTICAL_WEAPON_REGISTRATION.md. New changes remain uncommitted alongside earlier fixes.

## Physical prop grenade collision repair, 2026-10-06

- User explicitly distinguishes physical props (rocks, doors, pillars, white fences) from player-only invisible barriers (zombie windows and map boundaries). User confirms affected props block grenades in WaW. NEVER restore yesterday's global player/monster MISSILECLIP promotion.
- Reverified native T6 XModelTraceLine 0x40DFD0: negative collLod skips collSurfs. Synthesized map-placed script_model boxes now select LOD0; authored LODs/masks unchanged. Enabled 55 staged models, including buyable doors and movable rock blockers. Existing source box shapes preserved; no grenade GSC watcher introduced.
- Added projectilecollision.py for static props without authored model collision: intersect visible LOD0 mesh faces with original world movement brush halfspaces, then emit independent MISSILECLIP-only static collision. Eligibility uses hard surface types or imported default opaque lit model replace materials; excludes glass/foliage/grass/water/default-alpha/unlit/missing metadata. Complete source ownership required; entity-owned brushes excluded. Source masks, original brushes, submodels, terrain and authored collision models unchanged. No map/model-name exception.
- This is an explicit geometry-based compatibility inference, NOT proof of recovered original WaW collision. Source fence/bus/column/most rock models have no collSurfs/contents in their dumps. Do not claim source data proved their original projectile trace path. Checked missing native dynentities: 12 authored locker doors, not these props; diagnostic T4 dumper edit was reverted and rebuilt.
- 1,663 placements receive mesh surfaces, including all listed physical prop classes. Gaps remain geometric gaps; no brush/bounding-box faces added for static props. 350 spatially batched clipmap-only xmodels conserve the asset pool, preserving each placement's separate surface bounds. Each collision asset uses a real recovered triangle as its required render LOD, never placed in GfxWorld. Native staged/extracted 1,040,706 faces verified with source proxy geometry, mask 0x80, LOD0 and bone0. Existing 52 static colliders retained.
- Added native T6 dumper .collsurfs.bin diagnostic for contents/LOD/bone/bounds/triangles. Rebuild instructions: tools/retemplate_oat.py vendor/OpenAssetToolsT6 ObjWriting/XModel/XModelDumper.cpp.template; build build/src/Unlinker/Unlinker.vcxproj, Release/Win32 with SolutionDir set. Native extract must include `script` (not scriptparsetree) to verify compiled scripts.
- 239 tests pass, native link zero missing assets; all 77,551 retained world triangles/winding/source masks exact and collision JSON byte-identical to previous monkey build. Brush-tree contract violations zero. All 75 compiled scripts and 133 FX match; confirmed PaP fix and monkey startup registration preserved. All 9 installed package hashes match outputs; ONLY map FF changed, including unchanged sound banks/gameplay mod/image packs.
- Current installed map FF SHA256 0d26e178e314a181a7107b74036e036e5d613ed2ed2d72cc106a074adf7d7046. Evidence work/grenade_prop_collision_20261006/{staged.json,audit.py,audit.json,bridge_batched.log,roundtrip,installed_hashes.json,installed_before}; selective stage.py establishes initial backups, final restaging/batching performed with the same source module. No in-game launch/playtest this session. Need fresh map/session throws at doors, rocks, fences, pillars, plus zombie-window and boundary pass-through checks. Changes uncommitted.

## Intermittent grenade clipping and silent later throws, 2026-10-06

- User playtest/recording confirms previous repair still clips at some spots/angles; throws 2/3 sometimes invisible and silent. They are unsure whether this also happens before give all. Recording: C:/Users/thrif/Videos/NVIDIA/Desktop/Desktop 2026.10.06 - 12.44.33.01.mp4. Do not claim gameplay fixed without another runtime test.
- Corrected an investigation error: native WeaponDef.isRollingGrenade is 0x698; 0x738 is plantable (verified with MSVC x86 offsetof using the actual T6 schema, work/grenade_video_20261006/offsets.txt). Helper 0x42C950 box-traces planted explosives; ordinary frag path is a point trace and supports static model collSurfs. Discarded experimental sweptprops.py completely; NO native brush changes or weapon physics switch installed.
- Recovery now uses source movement-brush intersection only to qualify physical placements; then keeps the complete hard LOD0 mesh. Previous intersection trimming left visible fence tops/rock faces uncovered. Recovered faces have both windings because native XModelTraceLine is one-sided. Source masks/brushes/terrain stay unchanged; gaps remain mesh gaps. Tests simulate the native plane/barycentric trace from both directions, above the movement clip, diagonally, and through picket gaps.
- Also fixed a latent batch-identity bug: chunk hashes include placement transforms/bounds, not only source model names. Regression covers two 64-placement chunks of the same models in one spatial cell. Single-placement assets may legitimately reuse local geometry at different world transforms.
- T4 offhandClass needs T6 offhandSlot: Frag -> Lethal; Smoke/Flash -> Tactical. Installed source fraggrenade/stielhandgranate/zombie_cymbal_monkey slots corrected. No source/stock model or physics replacement. Added event-driven grenade_fire diagnostics to compat player initialization: console WAW2BO2 GRENADE throw=N weapon=... model=...; no per-frame traces or missile motion changes. Compiles natively. This is to identify the disappearing model issue, which remains unproven.
- WFT_BOUNCE_SOUND is a surface-alias prefix. It previously remained grenade_bounce while the bank holds waw/grenade_bounce_wood etc. Converter now namespaces the prefix and discovers existing alias-family dependencies (weapons.py + assetresolve.py). Selective installed build changes fraggrenade/stielhandgranate prefixes only: their 25 source material aliases already exist in the bank. C4/claymore/source-monkey prefix fields retain previous installed values until a FULL sound dependency stage carries their currently absent families. Do not blindly apply new prefixes to this old staged bank for those weapons.
- 241 tests pass. Native map link succeeds with zero missing assets. Roundtrip verifies original 52 static colliders, 55 entity boxes, unchanged 77,551 terrain faces/masks and 484 intentional one-way omissions; 0 brush-tree contract violations; 75 compiled scripts match; 133 effects preserved. Full physical repair covers 1,663 placements / 2,975,354 placed faces (2,909,848 unique asset faces), using 336 unique collision assets. Reused local assets account for the count difference. 1,166 translated map material technique routes verified against source after linking.
- Native gameplay roundtrip must retain all 193 weapons; only expected offhandSlot and frag bounceSound differences allowed. Projectile model remains the imported WaW Mk2 with both original LODs. Blank projExplosionSound alone is NOT a proven silence cause: T6 impactType grenade_explode uses a global sound table (0x5BFF60), as did T4. No guessed explosion sound/model substitution added.
- Evidence: work/grenade_full_faces_20261006/{audit.json,audit.py,weapon_audit.json,placement_audit.json,staged.json,bridge.log,build_mod.log,compile.log,tests.log,roundtrip,mod_roundtrip,installed_before,installed_hashes.json}. Native audit does not establish actual bounce/render/audio in a running game. Next test: fresh session, three starting grenade throws before give all, then three after; inspect console diagnostics if an invisible throw remains.
- Final installed package: all 9 hashes match verified outputs. Map FF SHA256 a58e8dead72d207651dbba86629e9b2fc5b061796563400660b345fe0fc1627e; mod FF 1a429ace001bb1b7548ed9ac40608bec89ac74e3b64b859414f5837b7d24edf6. No gameplay launch this session; invisible/silent model issue not claimed solved. New changes remain uncommitted.

## Native BO2 primary frags, 2026-10-06

- User explicitly requests replacing ordinary primary grenades with BO2 grenades, overriding default source preservation for this role. Monkeys and all other special grenades must remain. Do not extend replacement to arbitrary offhand-class weapons: special/custom equipment can share frag classification.
- weapons.stage_runtime maps standard lethal fraggrenade and stielhandgranate to native frag_grenade_zm. Original weapon definitions are parked outside linker search paths and omitted from mod.ff, preventing give all from exposing their invisible/silent versions. Retained weapons' alternate references use the same mapping. Stock frag definition/model/audio/physics remain unchanged.
- gscport.assets_source emits one deterministic reverse name for shared runtime weapons. Existing monkey startup alias and PaP model-overlay FX remain intact. Physical-prop collision changes and original window/boundary masks are retained.
- Selective stage migration: work/use_bo2_primary_frags.py. Evidence: work/bo2_primary_frags_20261006. 244 tests pass. Native build/install verification will be recorded there; do not infer an in-game playtest from package verification.
- Completed native verification: 193 -> 191 weapons, exactly fraggrenade/stielhandgranate removed, every remaining definition unchanged including stock BO2 frag and both monkey weapons. All 75 compiled scripts and 1,166 translated material routes verified. Map linker reports zero missing assets. Packaged/installed all nine files with matching hashes (audit.json, installed_hashes.json). No live gameplay test; restart the mod/map for user testing. Changes remain uncommitted.

## Invisible BO2 frag follow-up, 2026-10-06

- User confirms real grenades now bounce correctly, but invisible throws remain; suspects WaW GSC grants. Actual LATEST log is storage/t6/main/console_zm.log (2026-10-06), NOT the stale mods/zm_nuketown_waw/console_zm.log (2026-10-03). Saved work/bo2_frag_visibility_20261006/game_before.log. All six grenade_fire events report frag_grenade_zm and t6_wpn_grenade_frag_projectile. Do not claim these are remaining WaW projectiles or that grant remapping alone resolves invisibility.
- WaW grants/refills already use waw_weapon mapping. Found loadout save/restore offhand calls bypassing it; added getcurrentoffhand/switchtooffhand API adapters and regenerated the three staged calls. Generic regression test; 245 tests pass.
- Targeted experimental visibility repair: existing grenade_fire watcher calls show/setvisibletoall on frag_grenade_zm immediately and after the spawn frame. Does not change missile model, movement, collision, damage or grenade inventory, and skips all special grenades. No WaW ordinary frag definitions reintroduced. This is NOT a proven root cause or gameplay fix; requires fresh user test. Build/install evidence will be in work/bo2_frag_visibility_20261006.
- Native compile/link succeeded with zero missing assets; all 75 compiled scripts verified. Gameplay mod FF remains byte-identical to BO2 primary-frag replacement (191 definitions unchanged; old ordinary WaW frags absent). All nine installed hashes verified. This visibility experiment has not been playtested. Optional user question about fresh-session versus give all remained unanswered during work.

## Visibility experiment failed; delivery investigation, 2026-10-06

- User confirms show/setvisibletoall experiment FAILED: BO2 grenades remain randomly invisible; indicator sometimes appears, sometimes not. Removed the visibility-reset calls/helper completely. Do not reintroduce them or claim grenade fixes based solely on server model names/native asset roundtrip.
- Latest main/console_zm.log saved to work/bo2_grenade_delivery_20261006/game_before.log. Four throws again report frag_grenade_zm/t6_wpn_grenade_frag_projectile. Stock grenade model has three nonempty LODs. Stock grenade indicators are enabled, range 250; missing indicator alone is not proof of missing network delivery because range/speed gates also apply.
- Native IDB re-open now works: session 0583667b for work/T6_mapped_vision.exe.i64. Old 4178f68a worker was unreachable. Snapshot gather sub_893560 loops live entities and copies up to 1,023 qualifying entity states (breaks when increment reaches 1,024); checks inuse +248, NO_CLIENT bit0 at +250, field +188 != -1. Called by sub_44CC10. Snapshot ring allocator sub_518F20 uses 200*maxclients*(packetbackup/4) ring entries, NOT a 200-entity per-snapshot cap. Do not misreport that as the cause. Actual qualifying count is unknown; 3,536 source entity records include 2,248 path nodes/negotiations and 613 script structs, so source-record count does NOT equal network entity count. Native spawning sub_54FCC0 is a Plutonium thunk into 0x12C2A5D0 outside this dump.
- World uses single fully visible server cluster/camera cell, so no recovered multi-cluster PVS bug demonstrated. No WaW ordinary-frag grant path found bypassing adapters; loadout offhand save/restore adapters retained. No ordinary-frag hide/delete handler found.
- New diagnostics are observers only: grenade_fire prints entity number, server time, script_entities count; one delayed sample after 250ms prints elapsed/model/origin or removed=1. Does NOT move/show/hide/recreate missiles or affect special grenades. Script entity count is not snapshot count. This is an investigation build, NOT a proven fix. Need user throw sequence with visible/invisible order to correlate actual allocation/lifetime/client delivery. No game process available for live snapshot inspection during this turn.
- User asks why stock BO2 zombie core does not already handle this. Explained server spawning/collision works, but converted map/client environment still surrounds it; current evidence does not establish client receipt/rendering. ROOT CAUSE UNIDENTIFIED. No engine or BO2-core defect proved. Avoid further guessed visibility/inventory patches.
- Observer/rollback build compiled and linked with zero missing assets; 75 compiled scripts verified; 245 tests pass; all nine installed package hashes match. Only map FF changed; gameplay mod and other eight package files byte-identical to native-primary-frag replacement. Evidence audit.json/installed_hashes.json/package.log in work/bo2_grenade_delivery_20261006. Await actual throw numbers/lifetime measurements; no gameplay fix claimed.

## Saved for later at user's request, 2026-10-06

- User is going out and explicitly asks to save this for later. Stop active investigation; no automation or further game launches requested. Invisible grenades remain unresolved. Latest installed build adds client missile-arrival/shutdown observers to the stock client entityspawned missile branch, without changing visibility, physics, inventory, or specials. Server observer logs entity IDs and 250ms lifetime samples. Next step when user returns: normal launch, several throws with visible/invisible throw numbers, then correlate server entity IDs with client missile arrivals/shutdowns in storage/t6/main/console_zm.log.
- Client probe is selective staged scratch code, NOT a permanent generated pipeline change: work/stage_client_grenade_probe.py modifies work/final_stage/zone_raw/zm_nuketown_waw/clientscripts/mp/waw/_waw2bo2_zm.csc. Full bootstrap restaging would overwrite it. Backup/evidence: work/bo2_client_grenade_probe_20261006/bootstrap_before.csc, compile.log, bridge.log, map_roundtrip, installed_hashes.json. Native compile/link succeeded; all 75 compiled scripts match native extraction; all nine installed hashes verified, with only map FF changed relative to primary-frag replacement.
- Controlled hidden local launch never reached the map, so live_snapshot.json has NO valid gameplay samples and establishes no cause. Closed only the owned test PID 39600 and removed the temporary w2b_missile_autotest.gsc raw script after verifying ownership/hash. No auto-throw helper remains installed; no test process remains running. Do not repeat this failed launch as gameplay evidence. The normal user launcher is required for the next reproduction.
- Read-only native snapshot reader retained at work/bo2_client_grenade_probe_20261006/read_snapshot.py. Native grenade spawn sub_6842A0 calls sub_672CD0, which initializes eType=4 at ushort +216, contents 0x2180, and byte +250=4 (NO_CLIENT bit clear). Server model/DObj initialization and normal trajectory paths confirmed, but client receipt/render state remains unknown. No engine defect or entity-cap exhaustion established.
- No source fix or additional push claimed. Preserve user's confirmed physical collision repair, stock BO2 primary frags, all special grenades, monkey startup alias, and PaP fixes. Resume investigation from the installed observer build when requested.

## User reproduction and client snapshot limit, 2026-10-06 evening

- User returned and ran the diagnostic build. Saved latest log to work/bo2_client_grenade_probe_20261006/user_test_1940.log. Throw 1: server entity 1002 at time 29150, script_entities=900; alive with BO2 model/origin after 300ms; NO client missile arrival. Throw 2: server entity 761 at 31600, script_entities=903; client arrival at 31650 and shutdown after 3350ms. Pending user confirmation whether first was invisible/second visible. No game process running when checked after reproduction.
- Concrete native discovery: CL_GetSnapshot sub_4C33E0 truncates snapshot entity count v5[2698] to 512 before copying states for gameplay/rendering. Saved decompile native_client_snapshot.c in evidence dir. Server gather cap 1023 is separate. 900 script entities does NOT prove >512 snapshot entities, but overflow is now a strong suspect consistent with high-numbered missing grenade and lower-numbered delivered grenade. Do not claim proven root cause until actual client packet count/omitted IDs measured.
- User asks whether network frame could cause it. Explained snapshot delivery plausible; avoid guessing sv_network_fps changes. Asked user to leave normal map running and say ready for live read-only snapshot capture. Reader now records client packet entities and first512/truncated IDs, and fixes prior eType parsing to ushort offset216 (dword offset4 was eFlags). Gentity stride validation tightened to >=252. Script syntax checked, actual live read still pending.
- Current IDA session 5b0167b1 for work/T6_mapped_vision.exe.i64; worker idle TTL3600. Client active struct pointer dword119CB84; header [13] snapshot ring, [15] snapshot mask, [24] latest sequence; snapshot stride10824; snapshot [2698] entitycount/[2702] first ring index; client header [3] entityringbase/[4] capacity. CL_GetSnapshot copies first min(packetcount,512) states, each248bytes. Filtering may also apply after truncation. Preserve all gameplay/collision assets while determining exact overflow cause.

## Confirmed client truncation and stationary emitter repair, 2026-10-06

- Live normal game PID42364 captured read-only. Evidence live_user_throws.json (433 valid samples) in work/bo2_client_grenade_probe_20261006. Server qualifying network entities 427..544; actual client packet entity count426..543. Three invisible grenades were present in every sampled incoming packet but outside CL_GetSnapshot's first512 entities: entity1005 at indices532..534, entity1003 ~533..534, entity1004 ~534..538. None entered gameplay/render snapshot. Fourth entity332 was inside cutoff at indices109..111 for all32 samples; client observer lifetime3350ms. User confirms first3 invisible, fourth visible then disappearing on ground. Snapshot truncation PROVES first3 cause; fourth's perceived early disappearance remains separately unverified (fuse timing may matter), do not claim that detail proved solved.
- Snapshot load: last sampled network type6 scriptmovers516, plus actor/control entries. Read-only live_scriptmovers.json maps172 tag_origin models;142 exactly match authored script_struct targetname=fx stationary emitters. They are permanent UGX-style inert FX carriers. Another105 source skull models and143 model-less brush movers remain untouched. No speculative networking/frame-rate dvar patch made.
- Generic semantic rewrite lower_stationary_fx_carriers in gscport.py matches complete stationary carrier idiom (spawn script_model at self.origin; model tag_origin; copy origin/angles; link to self; exactly6 uses of self.fx; single playfxontag; tag_origin literal; script_noteworthy effect alias; excludes waittill/move/delete/endon). Removes only inert carrier statements and calls compat waw_playfx at source origin/angle basis. Original asset itself preserves looping; existing explicit loop branch retains speed/origin and native loop-FX behavior. Triggered/moving or extra-referenced carriers remain unchanged. No map/model-name exception or grenade pipeline added.
- Applied selectively via work/stage_stationary_fx.py to staged ugx_easy_fx.gsc. Backup/evidence work/bo2_snapshot_fx_20261006/{ugx_before.gsc,user_four_throws.log,tests.log,compile.log,bridge.log,map_roundtrip,audit.json,installed_hashes.json,package.log}. 247 tests pass; native compile/link zero missing assets; all75 compiled scripts match extraction, and ONLY ugx_easy_fx.gsc compiled bytes differ from client-probe baseline. All9 installed hashes match outputs; only mapFF changes, eight other files remain byte-identical to primary-frag replacement. Confirmed physical collision/special grenades/BO2 native frag retained.
- Asked user to restart map, leave it open, and say ready for post-fix capture. This build is INSTALLED but has NOT yet had a fresh runtime test. Do not mark whole grenade issue solved until snapshot headroom and repeated visible grenades verified. Do not close user's game. If process changes, re-query PID before capture. Current reader native offsets valid in PID42364. No new Git push or commit made.
- Fresh post-fix user session now PID42184. Read-only capture confirms snapshot load fell to383..390 entities (533 valid samples in work/bo2_snapshot_fx_20261006/live_after_throws.json). Latest game log has14 server throws and14 corresponding client missile arrivals, none missing. Six grenade lifetimes overlap the timed native capture: every packet containing those grenades also includes them in the first512 gameplay states; truncated missile samples=0. Evidence runtime_audit.json/game_after.log. User visual confirmation still pending; do not invent it. Game left open. This updates the preceding NOT-playtested status: delivery repair is now LIVE verified, full visual symptom resolution still pending.
- Source helper hardened to retain original FX-argument tokens for subsequent field/dependency remapping, match original token body indices without reparsing mutated token counts, and require exactly3 attachment arguments. Native installed stationary script is semantically identical; these source hardenings do not change installed script. 247 full tests pass after index/field hardening;12 focused appearance tests pass after argument guard. No further package change needed.
- USER CONFIRMED: "Okay the grenades work now, they still clip through some things in some minor spots, but I guess it will do for now." Visibility issue accepted as fixed after fresh-session playtest; remaining minor physical collision gaps explicitly deferred by user. Stop further collision expansion/experiments for this request. Root cause was CL_GetSnapshot truncation above512 entities, repaired by eliminating permanent stationary FX anchor models while preserving authored effects. Installed build accepted. No game process closed; no automation created; latest changes remain uncommitted/unpushed.

## Authored map frontend installed, 2026-10-06 evening

- User requested vanilla-style Blit/Large/blur alignment, a proper title/description, a Nuketown icon, and the supplied zombie photo for the lobby miniature. Installed title **Nuketown Remastered**, description **Return to the ruined test town. Fight through shattered homes, burning streets, and secrets buried beneath the fallout.** Native Nuketown atom emblem copied into project namespace; no reliance on DLC map fastfiles.
- Used imagegen skill/built-in generator for art. Source and generated PNGs, packed previews, preparation/link/audit scripts, and round-trip evidence: `work/menu_art/nuketown_20261006/`. Initial landscape Large/blur are drafts; final art uses `nuketown_large_aligned.png` and `nuketown_blur_aligned.png`, technically resized to native2048-square canvases. Blit is512x256 with genuine alpha and stock anchor; its material uses native blend/sampler states. Lobby screenshot is packed256-square; loading version1024-square. Actual UI alignment STILL requires a fresh-session visual check. Do not claim it has been playtested.
- Generic optional `menu.json` in staged project carries title/description/icon/Blit/material names. `modzone.stage_menu_assets` validates authored assets, generates namespaced localized strings, and marks menu images native streaming mode2. Stock CSV projection constants retained. CLI includes assets in gameplay mod.ff and link_lobby includes them in mod_load.ff; packaging writes authored mod.json. See `docs/MAP_MENU_ASSETS.md`. Reusable authored input snapshot at `work/menu_art/nuketown_20261006/project_assets/`; copy to a newly restaged project if staging replaces custom inputs.
- All six images must be declared AFTER `>ipak,<project>_menu` to enter the menu pack. First build's placement captured only2; corrected declaration ordering, native extraction recovers all6 pixel-exactly in both zones. Additional installed file `zm_nuketown_waw_menu.ipak` (identical between frontend/gameplay builds). Existing gameplay/map image packs and sound banks retained.
- Gameplay relink used native baseline overlay with original complete shader zone roots, only fresh menu materials/images/tables/locale exposed. Full native asset-list roots are unsuitable: nested FX handling failed. Raw full staged root is unsuitable for selective overlay: legacy mc material JSON could override native data. Final verify confirms all191 weapon files,21 gameplay map scripts,2 client scripts,930 materials,138 techsets,1376 shader binaries byte-identical to accepted baseline. Both zones' menu table/locale/bindings match staging; all6 decodedDDS images match prepared pixels. Evidence `verified.json`, `verify_mod_final/`, `verify_lobby_final/`.
- Installed native mod.ff/mod_load.ff, menuIPAK, and titled mod.json. `install.py` checks all installed outputs; map FF remains byte-identical to accepted grenade build. Original mod.ff/mod_load.ff/mod.json backed up in `previous_installed/`. Packaging skips identical files, avoiding unnecessary rewrite of running game's open image packs. Game PID42184 left running; existing session has old frontend loaded. Restart BO2 through normal launcher to evaluate selector/globe/lobby alignment. No new commit/push performed. 250 full tests pass; git diff --check passes (existing CRLF warnings only).

## Menu scale and blue terrain correction installed, 2026-10-06

- User supplied `Desktop 2026.10.06 - 20.30.24.02.mp4` and clipboard `babac1d0-ee2d-4c28-b1f0-8ca20d193d67.png`: first art had a small Blit over a much larger town in Large. User additionally requested blue tint on ALL terrain to remove gray lunar clash. First installation's canvas-only alignment claim was insufficient.
- Measured actual stock/native screenshot projections using SIFT, plus user's old custom Blit screenshot (81 inlier matches). Large→1920x1080 screenshot: sx1.875, sy1.0546875, origin(-960,-540). Blit→screenshot: sx=sy1.171875, origin(660,390). Thus Blit→Large UV sx0.625, sy10/9, origin(864,881.7778). Original independently generated Large town was ~2.8x too wide. Native CSV location projection already correct; no speculative CSV coordinate changes.
- Built-in imagegen edited the master to a compact town and applied blue/teal nighttime tint across soil/craters/roads/hills, then generated matching blur. Final source `nuketown_blue_master_v3.png`, `nuketown_blue_blur_v3.png` in `work/menu_art/nuketown_20261006/`. `prepare_matched.py` packs that authored master onto native2048 UV canvas; Blit color channels are sampled from the same decoded Large using measured UV relation, retaining authored alpha channel. No independent Blit render. `prepare.py` is obsolete first-draft preparer, not the current art source.
- Registration audit `blue_alignment_verified.json`:686 inlier landmarks, maximum projected corner/center deviation0.049px. `blue_alignment_projection_preview.png` is SOFTWARE-rendered native projection, NOT a game capture. `alignment.json` records mapping/canvas fit/authoring prompt summary. Preview shows one coherent blue town and blue background. Actual fresh-session visual confirmation is still pending.
- All6 images recovered pixel-exactly from BOTH native zones (`verify_mod_blue/`, `verify_lobby_blue/`, `verified_blue.json`); native191weapons/21GSC/2CSC/930materials/138techsets/1376shaderbinaries still byte-identical to accepted gameplay baseline. 250 tests pass; diff check clean. Installed only changed mod.ff/mod_load.ff and NEW `zm_nuketown_waw_menu_blue_v3.ipak`; prior mapFF/sounds/weaponIPAK and title/lobbyphoto unchanged. `installed_blue.json` verifies all hashes. Previous authored frontend files backed up in `previous_installed_blue/`.
- Optional generic menu.json `image_pack` permits versioned image-pack names so update does not overwrite a pack held open by the running game. Default remains `<project>_menu`. Older menu pack still installed and unchanged; new fastfiles reference the blue_v3 pack. No user game killed, no commit/push. Restart BO2 normally to load and visually validate this revision.

## 2026-10-07: BO2-native dry sound mix and layer balance
- User requests closer WaW feel using BO2-supported sound controls; gunshots still echo and some sounds are too loud. Asked whether stock BO2 clips were intended; no reply during work, proceeded with original clips through BO2-native settings and stated assumption.
- Added optional validated authored mix profile (`compat/sound_mix_waw.json`, CLI stage-bridge --sound-mix). Staged at content_source/sound_mix.json and retained across rebuilds; remove staged file and regenerate bank to return to source volume mapping. Profile is intentional subjective tuning, NOT proof of 1:1 playback.
- Converted alias sends are zero to make dry playback independent of room priority/startup overrides. Original recorded tails and secondary aliases retained. This opt-in profile also suppresses custom rooms; do not silently enable globally. Individual WaW noWetLevel honored globally without profile.
- Layer role trims: weapon -2 dB, reload -3, decay/distant weapon -6, impacts -4, explosion/physics -3, voice -5, ambience -2, other effects -1, music/UI 0. Source bus layer roles precede weapon dependency membership. 206 nonspatial variants use stock BO2 wpn_all/wpn_fnt/wpn_rear/music_all pans; spatial panning stays unchanged. Pan matrices remain approximations.
- Rebuilt gameplay mod.ff against exact currently installed baseline using native --load and full extracted zone definition, with only sound CSV and identical localized strings supplied fresh. Did not rebuild map geometry or frontend. Native gain error 1.53912e-5; all 4014 variants verified. Both generated SAB files byte-identical to previous installation. 191 weapons, 23 scripts, 937 materials, AI/character/accuracy/xmodelalias exports unchanged. Existing external xanim references and two non-48k death-music warnings remain. 120 existing sound-bank conversion errors unchanged.
- Installed ONLY mod.ff and synchronized work/mod_build/out/mod.ff, with SHA guards against concurrent installation changes. Other 10 package files unchanged, including mod_load.ff, blue menu IPAK, map FF, and four sound banks. Previous complete install backed up at work/sound_mix_20261007/before. 275 tests passed; 29 sound tests rechecked after profile validation hardening. Evidence stage.py/staged.json, verify.py/verified.json, install.py/installed.json, native baseline/roundtrip, link.log/tests.log.
- No game process was running; no launch/listening test performed. User needs fresh map/session to hear profile. Do not claim acoustics, bus compressors, surround channels or dynamic ducking are fully 1:1. Other concurrent desktop/menu source modifications belong to separate work; preserved them.

## 2026-10-07: rake axe equip recovery installed
- User clarified rake failure: cannot equip it as an axe. Initial action-slot hypothesis was revised after inspecting actual weapon table and bindings: nt_rake_trap is NOT renamed and user key5 already binds +actionslot3. No slot remap or binding change was installed.
- Found source construction completion destroys progress_hud twice, setting remove_rake_huds between the calls. BO2 native _hud_util::destroyElem accesses self.children and destroys self, so a second call on the deleted receiver can terminate the construction thread before EnableOffhandWeapons/EnableWeaponCycling. The separate buildable pickup watcher can start because remove_rake_huds is already true, leaving a visible/pickable rake with weapon cycling disabled. This is a source/runtime analysis, not a captured runtime traceback.
- gscport now lowers zero-argument unqualified native HUD destroyElem calls with simple/dotted receivers into compat waw_destroy_hud_elem(element), which guards isdefined before calling the native HUD helper. Native helper resolution is required; custom local implementations remain unchanged. Passing the HUD by argument makes repeated cleanup safe even when the receiver became undefined. More complex indexed/call receivers and explicitly qualified calls are not handled by this narrow lowering.
- Added generic setActionSlot namespace wrapper with preserved source slots/types, weapon-name mapping, and safe blank sentinel clearing. Rake retains slot3 and key5. The earlier tentative slot3→slot1 approach was removed before staging; do not report it as the installed fix.
- Selectively staged all3 rake progress-HUD cleanup calls and2 active rake action-slot calls; updated staged compat source. Both compile through official BO2 script tooling. Native map link exit0/zero missing assets;277 tests pass. Native before/after checks all75 map scripts: ONLY _rake_trap and _waw2bo2_compat change, all installed bytes match compilation.
- Installed ONLY zm_nuketown_waw.ff (SHA256 4e47d2876bcd68ff083db8d5f7ba0d21e3214dd454c3d45ea81aeaef92820591); other10 package files unchanged, including latest sound mix mod.ff, allSABs, menu frontend andIPAKs. Installation guarded against concurrent baseline changes. Evidence/backups work/rake_equip_20261007/{before,staged.json,compile.log,link.log,tests.log,native_before,native_after,verified_installed.json}. Initial extraction attempted before linker finished was discarded and rerun successfully after completion; final native verification is valid.
- No game process running and no playtest/launch performed. Fresh map build/pickup + key5 test remains needed. Source trap world animtree still unsupported; no trap animation/damage changes made in this equip-specific repair. Do not claim all trap phases are verified.

## 2026-10-07: sound tuning REVERTED at user request
- User reported sound nearly inaudible and explicitly ordered immediate rollback. Restored exact pre-tuning mod.ff from work/sound_mix_20261007/before/mod.ff to installed mod folder and work/mod_build/out/mod.ff. Verified SHA256 6a73b4e5c178e3e3a888c7f2f144b007427d2802cd16e7307499978160a61648.
- Restored original staged alias CSV and sounds.bank.json; removed staged content_source/sound_mix.json. Reverted this turn's sound-profile code, CLI option, docs and sound tests to Git baseline; removed compat/sound_mix_waw.json. No sound tuning should be reapplied on rebuild.
- Other10 packaged files verified unchanged, including rake-fixed mapFF, all sound banks, frontend/menuIPAKs. Rake source/HUD cleanup and action-slot name wrapper changes preserved. No game process was running when restoring. Work/sound_revert_20261007/restored.json records verification; disabled profile and previous tuned fastfile retained there for forensic reference only, not for reinstatement.
- Earlier sound-tuning installation instructions above are superseded. User rejected the tuning; do not reintroduce volume trims or zero sends without a new request. Restart/reload map needed to hear restored playback if an existing session held the tuned fastfile.

## 2026-10-07: light alignment coordinate audit (investigation pending)

- User shows three bunker fixtures with bright patches away from the skull markers; requests alignment and asks whether BO2 world coordinates match WaW.
- Read installed map FF with native Unlinker (mapents/comworld/gfxworld) into `work/light_alignment_20261007/installed`. `audit_coordinates.py` compares all 439,837 linked surface vertex records with original WaW gfx.bin: zero failures, maximum component difference 0.005 units (native text dump rounds to two decimal places). All 3,842 staged render-model origin/axis/scale tuples exactly equal source; all 54 staged primary-light origin/dir tuples exactly equal source. Evidence `coordinates.json`. No global coordinate scale/translation mismatch established.
- `scripts/zm/waw2bo2_lightmarkers.gsc` is a leftover external diagnostic: first skull at primary light origin, second 32 units along -dir. These are PRIMARY-LIGHT markers, not Easy-FX attachment points. Earlier handoff assertion that these white patches must be misoriented UGX FX was not established and is too narrow.
- Authored nearby triangle_ray/red_ray/green_gew_drip effects are absent from WaW zone/Mod Tools source, report WAW_FX_ABSENT and are undefined/skipped by compat. Those do not produce the screenshots' bright white patches in current build. Source light1/lightray aliases have no authored FX structs in this map. Preserve converter-wide fixes, do not arbitrarily shift source light coordinates.
- Asked user whether white patch moves with camera or stays fixed to distinguish specular highlight from fixed lighting. Answer pending. No installed files or converter behavior changed for this investigation. Sound rollback and accepted grenade carrier reduction/rake script fix preserved. No active game process found during read-only audit.

### Fixed patch follow-up

- User confirms patch stays fixed with camera movement. This alone does not distinguish stationary FX from baked/direct light; no root cause established.
- Native installed xmodel GLTF audit compares every vertex POSITION in all four LODs of fluorescent yellow and tinhatcage lamp against original WaW dump: exact coordinate multisets in all eight LODs. Evidence `audit_lamp_models.py`, `lamp_models.json`. No lamp pivot/geometry displacement established.
- `audit_surface_lighting.py` compares all 439,837 native linked vertex records with source vertex position, lightmap page, UV and assigned primary-light ID. Zero failures; maximum UV difference 0.00000766 (UNORM16 rounding). Source no-lightmap surfaces map to native page0 and their unused UVs are excluded. Evidence `surface_lighting.json`. No reordered lighting association/UV shift established.
- Read-only T6 IDA native sub_73AC60 and sub_782FA0 reaffirm ComPrimaryLight field offsets/copy and camera-relative position/dir. Source spotlight uniforms are already translated on relevant current materials. DO NOT analyze stale work/lighting_fx_latest_roundtrip as current shader bindings: latest installed *123_13 uses runtime_7aa5e13bd2bd08d0, fully translated WaW passes, rather than stale runtime_ad55db19cb3f942c donor passes.
- Asked one further runtime isolation test: view white patch, console `fx_draw 0`; report whether disappears/stays/command rejected; restore `fx_draw 1`. Native executable verifies fx_draw exists at string0xd25b04. Answer pending; no new package or runtime-diagnostic scripts installed. Do not claim fixed or coordinate-shift anything based on screenshot alone. All authorized read-only audits completed, a rendering change needs identification of which path actually produces patch.


## Nuketown perk mode and box relocation, 2026-10-07

- Added saved Mod Builder `Use BO2 stock perks` option (default false), CLI
  `--bo2-stock-perks`, PowerShell `-Bo2StockPerks`, and worker forwarding.
  Matching build receipts gate Install when the option changes.
- Opt-in routes source perk controllers to native BO2 ownership, translates
  community perk IDs/groups/machine links, registers native Electric Cherry/PhD
  server/client modules under owned names, retains native LUI/solo revive and
  perk-loss code. Source free grants call native give_perk; duplicate source
  HUD/perk_think and refund handlers are suppressed. Unknown machine perks or
  unsupported source perk functions fail staging. Normal WaW ownership remains
  the default. Native perk dependencies are protected mod_extra.zone entries.
- BO2 raw lacks legacy Deadshot/PhD machine model names; opt-in uses shipped
  prison machine variants for those missing names. Native optional modules and
  client callbacks have matching registration/ownership.
- Box enable_trigger routes through compat to restore hidden trigger_use
  visibility after moving inactive chest pieces back to their origins. It does
  not clear the buyer-only visibility mask. WaW box relocation flags use the
  waw_ namespace to avoid BO2 controller writes to the same flags.
- Validation: 333 unit tests pass; actual Nuketown translation/link checks have
  zero errors; native BO2 compiler compiled all 75 current scripts. Native perk
  asset-only zone resolves dependencies and links (expected no-BSP diagnostic).
  Evidence: work/gameplay_fix_20261007/{tests.log,verification.json,asset_check/}
  and compile_stage/script_build/linker.log.
- No full map rebuild, install, GUI binary packaging, or gameplay playtest was
  performed. Rebuild/install with the new option to apply it. Gameplay checks:
  buy Quick Revive solo, down/revive three times, power, perk loss, free perks,
  Pack-a-Punch, and multiple box moves including returning to an earlier spot.
- Preserved pre-existing equipment/HUD changes in gscport/compat/gsc_api and
  tests/test_equipment_slots.py; did not commit or publish.

## Rancid user build failure and portable diagnostics, 2026-10-07

- User supplied Downloads/waw2bo2-build (1).log. Both recorded builds stop
  resolving viewmodel_usa_double_barrel_sawed_off_grips_lod0.gltf: custom-map
  XModel JSON exists but its exported mesh is in a stock WaW zone dump.
- Resolve exact exported mesh paths across owning/source/stock roots, fetching
  the named model's stock zone when needed. Preserve map JSON and source-root
  priority. Weapon, world-model and source-sky staging use the shared lookup.
  Missing geometry retains dependency edges and explicit diagnostics instead
  of leaking a FileNotFoundError. No map-specific replacements or cache edits.
- Failed GUI builds/extractions create local diagnostics ZIPs. Reports includes
  Save Diagnostics. Archive includes console, referenced generated native logs,
  reports and settings, excludes game assets, and caps each file at 2 MiB.
  Extraction failure handling works without a selected map.
- Validation: all 337 tests pass (work/rancid_recovery_check/tests.log).
  Real stock WaW shotgun geometry recovery/staging passed using deliberately
  partial map metadata (verification.json). Fresh frozen CLI worker repeats
  the same recovery with no missing/unsupported models (frozen_stage/).
  Packaged --self-test passes GUI/schema/native tool/resource checks.
- Rebuilt work/desktop_dist/WawConverter-Windows.zip with current Python fixes,
  including prior Nuketown opt-in perks/box and equipment work. Native source
  ZIPs reused from work/desktop_native_sources_for_rebuild. No publishing,
  commit, game install or gameplay playtest. Only the user's log was available;
  full Rancid map conversion is unverified. The initial frozen-worker command
  omitted '-m waw2bo2.cli' and opened GUI; terminated only that test PID and
  reran with correct dispatch successfully. This was a test invocation error.

## Bank Job reported build failures, 2026-10-07 (portable 0.2.8)

- User supplied seven staging errors and Downloads/build.log (Wine, Z:/home/humpy).
  Treated logs as diagnostic data. No Bank Job fastfiles/assets available locally.
- Corrected visibility size validation in Python and native T6 GameWorldMpLinker:
  accept floor/ceiling of n*(n-1)/8, retain exact source bytes. The 530-node case
  preserves 35047 bytes; do not truncate its last two meaningful bits.
- Found TWO entity bugs: desktop WAW_ASSETS omitted mapents and stage_bridge
  searched the exporter folder instead of the ordinary maps/*.d3dbsp.ents dump.
  Added mapents to desktop and PowerShell extraction; lookup checks exact names
  across prioritized source roots. Asset-selection receipt changes automatically
  invalidate old custom caches. Shared stock caches do not need resetting.
- Move link_check from port_scripts to completed stage_bridge after generation
  of owned perk/bootstrap modules. Both stock-perk flag modes link cleanly.
- Generate the missing server _amb main entry only when no authored/template
  server ambient exists. Imported WaW scripts own actual server ambience; client
  ambience still uses the real template. Main/fx scripts remain required.
- Localization: derive standard price-family door/area/debris text only from
  consistent authored WaW wording, replacing encoded cost and preserving button
  tokens. Exact custom source strings win. Add community Mule Kick shared-key
  mapping to BO2 ZOMBIE_PERK_ADDITIONALPRIMARYWEAPON only after WaW lookup fails.
  Unknown keys still fail; never broadly humanize localization identifiers.
- Validation: 343 tests pass; old-cache automatic refresh regression passes.
  Real WaW/BO2 raw files resolve all three reported keys. Both perk ownership
  modes pass real API link checks. Native extractor creates the expected .ents
  from the real Nuketown FF with mapents selection. Native linker successfully
  builds a scratch Nuketown-world fixture with 530 path nodes and 35047 bytes.
  This validates the native layout, not Bank Job gameplay or a full Bank Job build.
- Evidence: work/bankjob_recovery_check/{tests.log,verification.json,
  native_build.log,native_link.log,mapents_extract.log,frozen_self_test.log,
  package_verification.log}. Native build requires SolutionDir set to the build
  directory for RawTemplater custom rules when building a vcxproj directly.
- Version metadata synchronized to 0.2.8. Rebuilt desktop ZIP and GUI self-test
  passes; archive integrity verified, bundled native corresponding-source ZIP
  contains modified GameWorldMpLinker.cpp. Reused unchanged T4 archive, updated
  T6 source entry (native_sources/). No commit, publication, game install or playtest.

## WaW ambient tokenizer hotfix released as v0.2.9, 2026-10-07

- Reported error exactly matches stock WaW raw/maps/ber1_amb.gsc: an opening
  block comment at line 237 ends with standalone *\ at line 255, before the
  commented radio_location line. This is a malformed comment terminator, not
  an ordinary line continuation. Do not broadly strip backslashes from code.
- Tokenizer recognizes this standalone ending only as a block-comment fallback;
  valid */ comments have priority. Lossless emit retains the original source.
  fix_syntax repairs only malformed block endings in token comment prefixes,
  preserves disabled code and valid comments/strings, and reports a syntax fix.
- Parse token errors include script name; tokenizer error line accounts for
  pending whitespace/comment newlines. Unknown executable backslashes still fail.
- 346 tests pass, including LF/CRLF, valid comments with internal *\ text,
  exact roundtrip, repair idempotence, disabled functions and filename/line errors.
  Real ber1_amb source parses with exactly one repair. BO2 native compiler accepts
  a repaired fixture using its actual disabled comment content. Frozen GUI/native
  self-test passes. Evidence work/comment_recovery_check/.
- Pushed 66cada8 and annotated v0.2.9 to Ticass/B2W. Published preview release
  https://github.com/Ticass/B2W/releases/tag/v0.2.9 with Windows/Linux archives
  and checksums. Linux workflow 37715290647 succeeded, including comment tests,
  native Linux GUI/CLI and Wine-worker smoke checks. Full affected map gameplay
  remains unverified. No installed map changes. AGENT_HANDOFF retained locally.

## 2026-10-08: Nuketown gameplay reports -> converter-wide fixes
- User reports (Nuketown Remastered): rake trap can't be used "as an axe", bowie knife free, electric
  trap fills screen with checkerboard ~2 s, zombie shield on back gives no protection. User asked that
  every fix apply to all maps. All four fixes are pattern-based, none names Nuketown.
- BOWIE FREE: core map _zombiemode_bowie -> BO2 _zm_weap_bowie (template). BO2 melee_weapon_think
  charges `self.stub.cost`; only BO2 struct wallbuys have stubs. WaW trigger_use bowie_upgrade -> cost
  undefined -> minus_to_player_score(undefined) returns early. compat melee_wallbuy_costs() gives every
  stub-less melee wallbuy trigger a stub carrying its registered cost after think threads start.
- ELECTRIC CHECKERBOARD: SetElectrified draws hard-coded material zombie_electric_shock_overlay
  (string in both CoDWaW.exe and t6zm.exe); BO2 loads it only in zm_transit/buried. t6bridge
  ENGINE_BUILTIN_MATERIALS stages WaW's material when ported scripts call setelectrified. Its techset
  flamethrowerfx_color_distort_overlay_bloom had no rule: techsets.match now maps unlit sets with no
  family rule to the same-named T6 donor set (T6 only appends a hash).
- SHIELD ON BACK: protection lives in the map's _zombiemode::player_damage_override (core, never run in
  BO2). gscport.extract_damage_prelude ports the statements a map override puts before the stock body
  as a BO2 register_player_damage_callback (bare return -> return 0). Stock reference = every variant
  (zone-dump stock AND WaW/Mod Tools raw/: zone _zombiemode is an older release whose override starts
  `if( iDamage < self.health )`). Overrides with finishPlayerDamage in the prelude are reported, not ported.
- RAKE AXE: nt_rake_trap is inventoryType item with its own meleeAnim/meleeDamage 2500. T4 always swings
  the held weapon; T6 swings the melee weapon (knife) unless useAsMelee (BO2 riotshield_zm has it).
  weapons.convert sets useAsMelee 1 for item weapons with meleeAnim + meleeDamage > 0. Earlier
  HUD/actionslot rake fix (2026-10-07) addressed equip, not the melee.
- PRE-EXISTING BLOCKER found on rebuild: BO2_FALLBACK material mc/bo2_t5_foliage_dry_branch_gobo uses
  techset mc_treecanopy_sm_q8e8z12f (zm_prison only, not in donor view) -> staging error. Added
  all2raw.required_bo2_techset (lazy per-zone dump, like required_bo2_image); verify_techsets copies the
  set + shaders into the project.
- Tests: tests/test_map_gameplay_bridges.py. Not installed, no playtest yet.

## 2026-10-08: Bank Job follow-up (pending runtime verification)

- General zombie-spawner conversion now sets script_forcespawn=1. WaW's
  spawn_zombie has a DoSpawn fallback; BO2's helper only creates an actor in
  its script_forcespawn branch. Bank Job's runtime had queued=6, AI=0,
  freeactors=32, active start_zone and 19 spawners, excluding actor exhaustion.
- ScoreSolo reads of stock WaW player.stats keys now use live BO2 counters
  through waw_player_stat; playername becomes name. Added headshots and
  zombie_gibs defaults. Custom keys, writes and defined probes survive.
- Bank Job's player_damage_override edits the first stock MOD_FALLING branch.
  Damage-prelude extraction now recognizes its complete condition as the
  stock-body boundary, preserving the leading self_revive rule and allowing
  the build. Other framework-body edits still appear in core_overrides.
- Native HUD inspection: WaW mapped HUD setup 0x44C480 and BO2 0x7A2ED0
  use identical base multipliers (0.5 bigfixed, 1/3 smallfixed, 0.25 other).
  Ordinary solo fontscale conversion is identity, not a blanket reduction.
  Objective font face selection differs; visual fidelity remains unverified.
  See docs/WAW_PLAYER_STATE_AND_SPAWN.md.
- All 400 tests pass. Fresh translation and native BO2 compilation pass for
  Nuketown (73 scripts), Empty Walls (71), Abandoned School (67), Asylum v2 (88),
  Alcatraz (101). Evidence work/bankjob_20261008/regressions.json.
- Bank Job full rebuild includes the pre-existing uncommitted gameplay fixes.
  Evidence work/bankjob_20261008/build_final.log. The earlier build_fixed.log
  succeeds but predates the spawner/playername changes; do not test that FF.
- Empty Walls exposed a zero-damage finishPlayerDamageWrapper followed by
  return in its solo-revive prelude. This exact idiom now becomes callback
  cancellation. Nonzero finishes remain rejected. Tests cover both cases.
- Final Bank Job rebuild passed native script compilation, zero missing assets,
  and 77,725 material argument checks with zero violations. Installed its
  latest FF/IPAK/mod.ff into Plutonium for the next gameplay check; previous
  changed files are backed up in work/bankjob_20261008/installed_before.
  Receipt work/bankjob_20261008/install_receipt.json. Temporary Bankjob probe
  removed (backups spawnprobe_before.gsc and spawnprobe_last.gsc in evidence).
- Computer Use was stopped with physical Escape; no further UI actions or
  game relaunches were performed after that. Final gameplay checks, commit
  and release remain pending. Preserve this distinction when resuming.

## 2026-10-08: v0.2.13 gameplay and build follow-up

- Supersedes the earlier font-scale identity conclusion: WaW's -86 HUD netfield
  rounds (fontscale-1)*10 into six bits and decodes 1+bits/10. Source 8 displays
  as 1.6. Literal and calculated assignments now preserve that effective scale.
- Original actor classes select original WaW characters/body/head arrays.
  Generated T6 server/client actor scripts retain native state/animation support
  without Nuketown character selection. Bank Job runtime used German honor-guard
  bodies; Empty Walls used char_rus_guard_body1_1_zm.
- Animated models no longer receive the static-prop flag 0x200000. Stock T6
  zombie bodies/heads use 0x80000; native renderer 0x724E1A branches on the static
  bit. Source LOD meshes and distances survive native roundtrip. The specific
  live/dead visual comparison after the flag fix remains unverified.
- Solo source Quick Revive grants one BO2 life. Both Bank Job and Empty Walls
  runtime probes returned health=100 and lives=0 after a down/revive.
- Source weapon hints and early weapon-table lookups are guarded; no-display
  wall buys have an unmatched native target. Unrotated script-struct endpoints
  receive zero angle vectors, fixing Bank Job's game-over rotateto error.
- Empty Walls full build passed (zero missing native assets): 96,096 map plus
  68,294 mod material arguments, zero violations/missing techniques. Cold
  dependency extraction and weapon staging also completed without exceptions.
  Original blank-error staging failure did not recur; CLI now prints the
  exception type as well as its message. The damage-prelude build regression
  remains covered by original-control-expression and zero-damage tests.
- Empty Walls runtime exposed additional issues: dlc3 include_powerups callback
  lost its parameter to a stub; scorebar read undefined level.isPlutonium;
  native weapon registration dropped on missing rumble/flamethrower. The source
  callback now extracts with native powerup registration, compat supplies the
  environment flag, and rumble profiles plus graphs stage as rawfile assets.
- Final unit suite: 411 passing. Final native script regression compilation:
  Nuketown 82, Empty Walls 94, Abandoned School 73, Asylum 97, Alcatraz 110.
  Evidence: work/bankjob_20261008/final_regressions.json, tests_release.log,
  release_bankjob_runtime.log, empty_walls_trace.log, empty_cold_phase.log,
  verify_dynamic_models.log, finalize_runtime_dependencies.log.
- Portable Windows v0.2.13 rebuilt with current T6 native binaries and matching
  source archives. GUI/schema/native-tool self-test and both CLI help commands
  pass. User explicitly authorized GitHub push and release after checks.

- Final Empty Walls runtime: source Russian body, health 100/lives 0 after revive,
  EXE_MATCHENDED after second down, zero script runtime errors and COM_ERROR.
  Evidence release_empty_runtime.log. All agent-created probes removed and
  agent-started game processes stopped before release.
- 2026-10-08 INSTALLED full rebuild (all 11 package files) with the blue menu. Blue art sources were
  gone from work/; recovered the 7 menu materials + 6 IWIs from the installed mod.ff/blue_v3 ipak and
  staged them as a project-authored menu.json (image_pack zm_nuketown_waw_menu_blue_v3), so the
  separate lobby photo/atom icon are kept (the GUI 3-image workflow would derive them from Large).
  All 6 images byte-identical in new mod.ff and mod_load.ff. Previous install backed up in
  builds/zm_nuketown_waw_75bbf9ff_aa295372/installed_backup_20261008. Sound .sabs is smaller than
  before (781 MB vs 992 MB): current converter sound output, not tuned by this session. No playtest.
- 2026-10-08 GAME-OVER NUKE CAMERA. Nuketown's _zombiemode override sets level.custom_intermission to its
  own player_intermission: links player + nt_camera_rocket to a script_origin and MoveTo's it 9 s down the
  intermission struct path; its end_game hides the rocket before waiting for "end_game". BO2's _zm::init
  re-set custom_intermission to BO2's camera path (20 u/s, rocket never moves) -> view sinking inside a
  still rocket. gscport now: extract_custom_intermission (map-edited target only) sets it in
  waw_main_post; extract_end_game runs end_game's pre-wait statements there; bridge_intermission_camera
  adds a networked tag_origin script_model linked to the mover + camerasetposition/cameraactivate (BO2's
  intermission view API; a script_origin is never sent to clients). Player LinkTo kept. BO2 linker compiles
  the grafted scripts; NOT rebuilt/installed, NOT playtested. extract_end_game also ports the map's
  wait between intermission() and stop_intermission (Nuketown: waittill end_it_pls + wait 2) as a thread
  that then does the shared WaW/BO2 exit (stop_intermission, player_exit_level, 1.5 s, exitlevel);
  BO2's fixed 15 s zombie_intermission_time is raised to END_GAME_FALLBACK 30 s (fires only if the
  cutscene never notifies). No more ~7 s black screen; timing now equals WaW's.
- 2026-10-08 REMASTER OPTION (user request). Measured: Nuketown Remastered's BO2-ported textures already have BO2
  resolution (259/259), the loss is in materials (WaW shaders, 48/239 lost spec/gloss etc.). New opt-in
  "Remaster with BO2 materials" (--remaster-bo2-materials, Settings.remaster): remaster.py indexes every
  zone/all fastfile (materials/techsets/images, ~20 s, cached), matches mc/ model materials by name behind
  bo2_/bo1_/t6_/t5_ prefixes (same mc/ class only; world wc/ keeps WaW lighting path), extracts each material +
  images + techset + shaders into asset_cache/bo2_*/dependencies/remaster/sets/<key> (~150 MB, ~5 min first run).
  OAT opens only base/mp|zm|so + ipak_read packs: DLC map pixels (mp_downhill -> dlc1.ipak, file
  mp_downhill.ipak absent) are recovered by hard-linking dlc*/dlczm* packs under the pack names OAT failed to
  open. Short work paths (Windows 260-char limit broke 22 shaders). thermalMaterial dropped from remastered
  materials (WaW has none; linker needed thermal_gradient2). Nuketown: 268/412 model materials remastered,
  audit 0 violations, INSTALLED (separate build root builds/zm_nuketown_waw_remaster). Pre-remaster install
  backed up in builds/zm_nuketown_waw_75bbf9ff_aa295372/installed_backup_pre_remaster. NOT playtested: BO2
  model shaders under the converted lighting grid/probe (near-black probe 6,6,6) are unverified.
- 2026-10-08 COOP HOST CRASH (remaster build, 0xC0000005 at 0x4BC1CF, read 0x1C). Use the Plutonium IDB
  (%LOCALAPPDATA%\Plutonium\games\t6zm.exe.i64; the Steam i64 is a different build). 0x4BC1B0 = model trace per
  XSurface rigid vert list: vertList[i].collisionTree->nodes, tree NULL. Chain sub_814E30 -> sub_754670 ->
  sub_754290 -> sub_7538A0 -> sub_7536A0 -> 0x4BC1B0. OAT LoaderXModel CreateCollisionTree returned nullptr for
  vert lists with vertices but no triangles (302/409 rigid Nuketown primitives, incl. a1_fs_m16_world, zombie
  heads), past 0x8000 tris, or oversize trees. Fixed: EmptyCollisionTree (1 leaf node, 0 tris) always emitted;
  limit cases warn with the model name. Linker rebuilt (MSBuild Tools\Linker Release Win32, LAA kept).
  Nuketown remaster relinked + INSTALLED 23:09-23:10. Pre-existing for all converted maps; coop likely exposes
  it by tracing the other player's held weapon world model. Not yet re-tested in coop.
