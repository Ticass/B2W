# Native shader translation

`waw2bo2.shaders` translates extracted WaW Shader Model 3 vertex and pixel
instructions into HLSL, then compiles Shader Model 5 DXBC with Windows
`d3dcompiler_47.dll`. It preserves register swizzles, masked writes, instruction
ordering, saturation, literal constants, texture dimensions, projected/bias/LOD
sampling, derivatives, discard, comparisons and bounded literal repeat loops.
Unknown instructions, relative addressing and unsupported flow control fail
explicitly. Partial-precision instructions are promoted to float32 and reported.

Run from the project with `PYTHONPATH=src`:

```powershell
python -m waw2bo2.cli translate-shader original.cso translated/my_shader
python -m waw2bo2.cli translate-shader original.cso translated/my_shader --bindings pass_bindings.json
```

Outputs are `.hlsl`, `.cso` and a JSON report with source/output hashes,
profiles, constants, sampler dimensions and binding status. Compiling without
a binding contract uses the original register ABI for offline validation. It
does **not** make that bytecode compatible with a BO2 material pass.

A contract maps each used external constant, declared input/output and sampler
to the target pass. Example schema (illustrative slot numbers, not verified
BO2 bindings):

```json
{
  "constants": {
    "c5": {"buffer": 2, "index": 0},
    "c6": {"buffer": 2, "index": 1},
    "c7": {"buffer": 2, "index": 2}
  },
  "samplers": {"s0": {"texture": 3, "sampler": 4}},
  "inputs": {"v0": {"semantic": "TEXCOORD0", "width": 2}},
  "outputs": {"oC0": {"semantic": "SV_Target0", "width": 4}}
}
```

T4 material dumping retains vertex/pixel bytecode and original technique-pass
arguments in `waw_techniquesets`. Bridge staging writes offline translations to
`content_source/shaders`, then `shaderruntime` activates supported programs in
live `shader_bin` and cloned T6 technique sets referenced by converted materials.
Each activated program has a resource contract and bytecode hash recorded under
`content_source/shaders/runtime`. The bridge report counts bound material passes.

Activation currently supports the common position/color/UV interfaces, material
texture hashes, maintained native constant buffers and single render targets.
Vertex adapters reconstruct WaW packed half-float UV bytes from T6 attributes,
pad three-component positions and supply T6 fog visibility to WaW pixel programs.
Original literal pass arguments become immutable shader constants. Engine
technique slots are matched by meaning, and native argument update frequencies
and vertex declarations are retained.

`build-mod --linker <bridge linker> --techset-dump <stock dump>` first builds a
complete gameplay baseline with the mod-tools linker and stock techniques. It
then relinks that baseline with the native bridge linker and bound technique
sets. This retains the mod tools' material constants, texture names and image
streaming data, and preserves the baseline ipak. Only materials with translated
passes receive new technique references. The final fastfile is extracted again
to check those references, unchanged texture bindings and DXBC programs before
replacing `mod.ff`. `run_bridge.ps1` supplies both linkers and search paths.

## Paired lit passes (Session 21)

`shaderruntime.bind_material` first tries the original WaW vertex **and**
pixel program together, so the varyings between them stay WaW's own; the pixel
input struct mirrors the translated vertex output signature exactly (D3D11
links by register) and `check_linkage` proves it on the compiled DXBC. When
the pair cannot be bound, the older pixel-only path against the native vertex
program is tried. Each adapter below was measured on stock T6 programs:

| Adapter | WaW | T6 |
|---|---|---|
| `waw_ubyte4_vector` (NORMAL, tangent in TEXCOORD2) | biased byte `b/127-1`, scale `w/255+0.7529` | signed byte in unorm (`v>=.5 ? 2v-2 : 2v`); re-encoded with `w=63` (scale 1) |
| `waw_uv_lightmap` | world float4 texcoord = UV + lightmap UV | TEXCOORD0 + TEXCOORD1 |
| `baseLightingCoords` | lp_* VS -> PS model lighting offset | `gridLightingCoordsAndVis` (cb3[4]); `.w` = sun visibility, carried on the same varying |
| `modelLightingSampler` | `2 L C` gamma | `sqrt(hdr 32 L^2 C^2)`: texel * `sqrt(8 hdr)`, alpha = grid sun visibility |
| `fogColor`, `sunDiffuse`, `sunSpecular` | gamma, summed in gamma | linear before `* hdrControl0.x`, `sqrt`: converted per quantity; `sunSpecular` has no T6 field, T6 drives sun specular with `sunDiffuse` |
| `shadowmapSamplerSun` | point-sampled depth, manual compare | same lookup matrix/partitions, comparison sampler: texels read with `Load` (no sampler) |
| `reflectionProbeSampler` | `rgb * a` gamma | `rgb / (a + 1e-6)` linear |
| `lightmapSamplerSecondary/Primary` | two textures | one page: WaW-encoded copy, exact V remap per sub-page |
| `destructibleParms` | WaW destructible fade | neutral 0 (no WaW destructibles) |
| material constants absent from the donor | material literal | embedded as a literal (hash = case-insensitive djb2-xor, seed 0) |

Code textures are matched by reflected name (the code-texture enums differ).
Fog visibility is located by component-exact data flow from `fogConsts`.
Unit conversions are recorded per bound pass (`unit_conversions`).

## Local primary lights (spot/omni)

WaW lit_spot/lit_omni programs read per-light code uniforms set by WaW
`sub_742400`/`sub_7425D0`; T6 sets its own from `ComPrimaryLight` fields in
`sub_782FA0`. Both keep the light position camera-relative, like the world
matrix. The adapter rebuilds each WaW uniform from T6 rows:

| WaW uniform | WaW value | Rebuilt from |
|---|---|---|
| `lightPosition` | (origin - view, 1/radius) | T6 `lightPosition.xyz`, w = -`lightFallOffA.w` |
| `lightDiffuse`, `lightSpecular` | colour (both colour scales default 1) | T6 `lightDiffuse`, sqrt(4 hdr L) |
| `lightSpotDir` | (dir, 0) | T6 `lightSpotDir.xyz` |
| `lightSpotFactors` | (1/(cosIn-cosOut), -cosOut/(cosIn-cosOut), exponent) | `lightFallOffA.x`, `lightFallOffB.x`, `lightFallOffA.y` |
| `lightFalloffPlacement` | (attenWidth/512, 0, lmapLookupStart/512, 0) | literal of the shared light def |

`lighting.t6_light_fields` stages `falloff = (near, radius, near, 0)`,
`aAbB = (cosIn, cosOut, 1/exponent, 0)` and a large `dAttenuation`, so T6's
formulas yield exactly those values (and T6 donors get a 1 - d/radius window).
WaW reads the attenuation ramp from row 0 of every secondary lightmap page;
the placement samples that row's centre. A map whose local lights use
several light defs is rejected for these passes (reported). Spots are staged
with roundness 0.5: T6 `sub_73AC60` turns roundness 0 into SPOT_SQUARE
(technique 9) and 1 into SPOT_ROUND (11), donor techniques that would decode
the WaW-encoded page as a T6 page (white surfaces in game). The
`*_DLIGHT_GLIGHT` techniques (15-25, a dynamic light also touches the surface)
draw their base technique's WaW pass; dynamic light is not added yet. Primary-light
shadow maps are not converted, so lights are staged with `canUseShadowMap` 0
and the shadowed slots (8, 14) are unreachable.

## Layered world materials

WaW layered surfaces (`*a_b` generated materials, `l_sm_*_b1c1...`) read their
extra layers from the vertex layer buffer. The T4 render dump (v5) exports it
with each surface's offset and `MaterialWorldVertexFormat`
(`TEX_<t>_NRM_<n>`, same enum in WaW and T6). WaW records are `t-1` float2
texcoords then `n-1` 4-byte normal transforms (UBYTE4N RGBA, `2v-1` = 2x2 layer
tangent rotation). The world FBX carries them as `LayerUV<k>` / `LayerNormal<k>`
UV sets; the bridge linker writes T6 `vd1` in the final material's format:
half2 texcoords, then transforms with bytes 0 and 2 swapped (T6 reads
B8G8R8A8, verified on stock rotations), identity where T6 expects a layer
normal map WaW lacks. Stock T6 layered techniques (`lit_sm_*_b1c1...`) are the
donors; WaW programs read the layer streams directly (`TEXCOORD3..6`). A
surface dumped without a format (technique in another zone) gets it from the
technique name or the layer buffer's record spacing.

## Terrain scorch

WaW `_sco` world programs weight `terrainScorchTextureSampler0` by a
`BLENDWEIGHT` stream the engine fills after explosions (zero on an unscorched
world). T6 has no such system: the stream is supplied as zero (exact for the
unscorched state) and the scorch texture as a constant.

## Model lighting

Model passes use the same light adapters. The light-grid scale check accepts
the fused `mad` form; `attenuationSampler` binds T6 code texture 15 (the
staged WaW light def's own attenuation image, read raw) through an added
type-4 argument; per-surface textures (lightmap t13, reflection probe t15)
are bound by the engine at fixed slots; a source texture the donor dropped
(e.g. a layer specular map) is added to the material and pass in name-hash
order (T6's texture lookup scans without an end bound).

## Primary light shadows

The v6 render dump exports WaW's `shadowGeom` (per light: static surfaces and
models drawn into its shadow map) and `lightRegion` hulls, identical
structures in T6. Staging maps surfaces to FBX meshes (`BSP/shadowgeom.json`);
the linker fills `shadowGeom` / `lightRegion` and marks static models outside
the sun's list `STATIC_MODEL_FLAG_NO_SHADOW` (previously every model was).
Lights keep WaW's `canUseShadowMap`; the shadowed techniques (8, 14, 19, 25)
translate. `shadowmapSamplerSpot` is point-loaded like the sun map (projective
taps allowed). `spotShadowmapPixelAdjust`: WaW's tap pattern
(0.25, 0.25, 0.5, -0.125 per 1/1024 tile unit) times T6's (1/S, 1/S) - a
derived tap spacing, not a measured equivalence. `lightSpotFactors.w` is the
shadow fade both setters pass through.

Light uniforms are per-pass in T6: type-5 arguments (`0x01000000 + index`,
location = byte offset) copy them into the constant buffer, so every row a
translated program reads gets its argument.

## Dynamic lights

WaW draws a dynamic light as an extra pass (`TECHNIQUE_LIGHT_SPOT` /
`LIGHT_OMNI`, same `light*` uniforms) blended `invdestalpha, one`. WaW's lit
world passes write alpha 1 with alpha writes on (measured material state), so
on a surface the base pass drew that light pass adds `(1 - 1) * colour = 0`.
T6's `*_DLIGHT_GLIGHT` techniques therefore draw the base WaW pass, matching
WaW's world surfaces. For reference, T6 `sub_729C20` fills the scene dlight
rows (`PerSceneConsts` + 1040): omni dynamic lights become four glights
(position - view, falloff -1/radius, diffuseColor), the spot dlight
`dlightPosition` (w = -1/radius), `dlightDiffuse`, `dlightSpotDir`,
`dlightSpotFactors` (cone, shadow fade) and its def's attenuation image.

Other neutral or routed uniforms: pixel `gameTime` goes to T6 code constant
0x19 at buffer 2 row 4 (stock pixel routing); `__characterCharredAmount` is 0
(static models carry no burn amount; no T6 pass routes one).

## Exposure and light units

T6 builds `hdrControl0 = (s, 0, 1/s, 1/s)` with `s = 1 / 2^(exposure + 2)`
(sub_728180). Lit programs write `sqrt(s * linear)` and no later pass rescales
it: in game, every 2 stops lower exposure exactly doubles the final pixel. WaW
writes gamma colour directly. The converted world therefore uses exposure -2
(`s = 1`, GfxWorldLinker; `lighting.json` `exposure` overrides), which is the
scale the lightmap, light grid and shader adapters assume. With `s = 1` a WaW
gamma quantity `g` becomes T6 linear `g * g`: sun colour/strength, primary
light colours, script fog (`waw_setvolfog`) and scripted sun (`SetSunLight`).
The template fog is rescaled so it keeps its on-screen colour until map fog
arrives. The previous fixed mp_dig exposure 2.5 drew every lit surface at
`2^-2.25`, about 21% brightness.

## Film and glow overlay

WaW grades (film) and glows the finished frame in 8-bit gamma space; BO2's
bloom adds light inside its HDR composite and cannot reproduce it. The
resolved frame is BO2 code texture 9 (stock distortion binds it) and is still
bound when HUD elements draw, holding the final displayed image. The vision
LUT is therefore identity apart from the composite shoulder inverse, and one
full-screen HUD material per vision (`glow.py`, drawn first in the HUD,
switched by `waw_visionsetnaked`) evaluates WaW's film and glow exactly:
film per pixel; glow source on the `screen >> 2` grid from four 2x2-average
taps (cutoff on raw luma, film-graded colour, desaturation); WaW's truncated
separable Gaussian (`sub_74A550`, sigma = radius * (quarterW/screenW) *
screenH / 480, eight tap pairs, pairs under 0.01 dropped, clamp addressing);
bilinear upsample; `+ intensity`. The four pixels of a 2x2 quad share their
quarter-resolution neighbourhood and exchange partial sums with
`ddx_fine`/`ddy_fine`. Not reproduced and reported: god rays, chained passes
above 6.4977 quarter texels, the tiny-radius 2D pass, timed vision fades.

## Pass state

Blend semantics belong to the program writing the colour. After binding,
each T6 pass takes the WaW blend/alpha-test state of the technique feeding it
when it runs the original WaW program (`2d` HUD materials draw straight alpha,
not the donor's premultiplied blend), and when WaW blends additively while the
donor does not (an additive layer drawn with alpha blending shows its black
background). WaW unlit and objective model materials use T6's model unlit /
objective donors instead of being lit by the grid; WaW objective programs feed
T6 UNLIT/EMISSIVE through their lit aliases.

## Lightmaps

`lightmaps.py` writes every WaW lightmap page twice (see its docstring):
WaW-encoded (secondary halves + primary sun visibility, stacked in T6's
three-page shape) for surfaces whose lit passes are translated WaW programs,
and T6-encoded (WaW flat-normal lighting squared into T6's rgb/a ambient,
no directional term, visibility in page 3 alpha) for T6 donor surfaces.
`BSP/lightmaps.json` lists both; the bridge GfxWorldLinker gives a surface
page `i` or `n + i`. A material keeps WaW lightmap passes only when every
reachable lit slot (4, 5, 6, 7, 8, 13, 14, including local primary lights;
8 and 14 only when lights keep shadow maps) is translated; otherwise they
revert, so one surface never mixes encodings.
The world FBX carries lightmap UVs as a second UV set and each mesh's page as
a `_lm<N>` name suffix.

Complex normal/tangent varyings, code textures, multi-target and multi-pass
layouts still require adapters. Their exact reasons appear per material in
`shader_runtime.unsupported`; they keep existing donor passes. Compiling all
source instructions does not establish runtime binding or restore lightmaps/grid
lighting. Linked activation also requires visual verification in the game.

Simple world unlit programs can declare UV float4 while only copying its xy
channels. The vertex adapter accepts this only when all reads are direct xy
copies, supplies the decoded UV pair, and zeroes the unused channels. It does
not interpret these coordinates as packed half bytes or require lightmap UVs.
Imported world/model/HUD material images use `waw_world/` names; lookup strips
the prefix only when resolving the original WaW pixels. Engine code images
remain references. This avoids stock BO2 images winning lookup by shared name.

Weapon staging owns the separate `waw_image/` namespace and resolves source
names before applying it; the shared material stage can leave image names
unprefixed for that caller. Code-image references bypass pixel recovery.

Unlit shader names do not necessarily describe blending: WaW can use `wc_unlit`
for an alpha-blended decal. Donor selection reads the source UNLIT state and
chooses the native transparent family when appropriate. The source blend,
depth-write behavior and sort order survive conversion. Both native UNLIT and
EMISSIVE routes bind the original shader pair. Otherwise a coplanar decal's
hidden RGB can appear as an opaque colored panel over the wall beneath it.

World/model material dependencies now consult the stock WaW resolver before
declaring a source absent. Missing materials may use reported BO2 equivalents
from the shared compatibility table; their native images are packaged in an
isolated namespace and checked for real pixel data. Available WaW materials
always take priority.

Validation: the extracted WaW `code_post_gfx` corpus translates and compiles
137 vertex/pixel shaders without unsupported instructions. This is compilation
coverage for that corpus, not proof of coverage for every custom map or of live
visual equivalence. Unit tests additionally compile real SM3 vertex bytecode,
translate it to SM5, check explicit resource bindings, masked register aliasing,
flow balance, projective sampling, discard, and fail-closed unsupported cases.
Distance-falloff world unlit passes preserve source blending and draw order,
including multiplicative HDR portal materials. Their UV.xy camera subtraction
uses the decoded UV stream. Translation-column reads from the original inverse
world-view matrix recover the camera in object space from the native inverse
view and world transforms, including rotation and scale. Other inverse matrix
reads remain unsupported. Both UNLIT and EMISSIVE routes bind the original
shader pair and its material falloff constants; no portal surface is deleted.

