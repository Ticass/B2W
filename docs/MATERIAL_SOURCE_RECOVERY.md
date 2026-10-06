# Recovering missing model materials

The converter resolves world, model, HUD and weapon material definitions through
the same lookup before staging their textures:

1. Map and companion WaW zone dumps.
2. Stock WaW zones.
3. WaW Mod Tools raw material sources.
4. Exact-name BO2 definitions or explicit shared equivalents, when WaW has no
   material definition. Each use is reported as `BO2_FALLBACK`.

Raw WaW materials do not usually have the `mc/`, `wc/` or `mlv/` draw-family
prefix used by compiled assets. Source lookup checks both the exact path and
the unprefixed path. Engine `$` material references with a family prefix use
the original unprefixed built-in definition. In particular, `mc/$default3d`
keeps WaW's `default` color map on a model unlit pass; it must not be substituted
with `mc/mtl_default`, whose `default_c` texture can be a mod's pink OWN3D image.
Geometry and model material assignments remain intact.

The source compiler checks that the requested asset was actually extracted.
WaW's linker can write a fastfile even when loading a material failed; that
fastfile alone is not evidence of a recovered definition.

BO2 definitions may exist in the Mod Tools raw directory even when a stock
zone dump contains only another draw-family variant. The converter checks raw
definitions as well as the dump, and obtains each image from the stock image
dump or the definition's own asset root. Native images retain their pixels,
alpha and mip chain, under `waw_world/bo2_fallback/` or
`waw_image/bo2_fallback/`. Weapon staging preserves these already staged native
bindings instead of looking for their pixels in WaW.

Shared imported foliage, television screen and perk-bottle material equivalents
are recorded in `src/waw2bo2/compat/bo2_equivalents.json`. These rules contain
no map names, coordinates or weapon-specific geometry changes. A real WaW
definition always wins over an equivalent. Other unresolved references remain
reported; this does not make every arbitrary missing custom material recoverable.
