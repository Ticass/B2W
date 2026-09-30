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

## Lightmaps

`lightmaps.py` writes every WaW lightmap page twice (see its docstring):
WaW-encoded (secondary halves + primary sun visibility, stacked in T6's
three-page shape) for surfaces whose lit passes are translated WaW programs,
and T6-encoded (WaW flat-normal lighting squared into T6's rgb/a ambient,
no directional term, visibility in page 3 alpha) for T6 donor surfaces.
`BSP/lightmaps.json` lists both; the bridge GfxWorldLinker gives a surface
page `i` or `n + i`. A material keeps WaW lightmap passes only when every
reachable lit slot (4, 5, 6: the bridge emits only the sun primary light) is
translated; otherwise they revert, so one surface never mixes encodings.
The world FBX carries lightmap UVs as a second UV set and each mesh's page as
a `_lm<N>` name suffix.

Complex normal/tangent varyings, code textures, multi-target and multi-pass
layouts still require adapters. Their exact reasons appear per material in
`shader_runtime.unsupported`; they keep existing donor passes. Compiling all
source instructions does not establish runtime binding or restore lightmaps/grid
lighting. Linked activation also requires visual verification in the game.

Validation: the extracted WaW `code_post_gfx` corpus translates and compiles
137 vertex/pixel shaders without unsupported instructions. This is compilation
coverage for that corpus, not proof of coverage for every custom map or of live
visual equivalence. Unit tests additionally compile real SM3 vertex bytecode,
translate it to SM5, check explicit resource bindings, masked register aliasing,
flow balance, projective sampling, discard, and fail-closed unsupported cases.
