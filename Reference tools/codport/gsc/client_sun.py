"""
Black Ops' scripted sunlight, on Black Ops II's sun dvar.

Black Ops client scripts flash the sun with

    SetSunLight( r, g, b );   ...   ResetSunLight();

-- zombie_coast flickers it a dozen times when the power comes on, and again with every lightning
strike (25 calls). Black Ops II has neither builtin, so the translator deleted them and the power
and the storm lost their flashes. Black Ops II's own storm does the same thing another way: zm_tomb's
`_lightning_thread` sets `r_lightTweakSunLight` for the flash and lerps it back to the value it read
at startup (`getdvarfloat( #"r_lightTweakSunLight" )`).

SetSunLight's values are a sun colour, compared with the map's own (worldspawn `suncolor`): Coast's
is (.567, .613, .653), so its `SetSunLight( 2, 2, 2 )` is about three times the normal sun and
`( .4, .4, .4 )` about two thirds of it. The helper scales the startup `r_lightTweakSunLight` by that
ratio -- the mean channel over the source's mean channel -- and ResetSunLight restores it.
"""

from __future__ import annotations

import re

#: The Black Ops calls, lower-cased, and the helpers they become.
HELPERS: dict[str, str] = {
    "setsunlight": "codport_setsunlight",
    "resetsunlight": "codport_resetsunlight",
}


def sun_reference(worldspawn_keys: dict[str, str] | None) -> float:
    """The source sun colour's mean channel (`suncolor`), 1.0 when the map does not say."""
    raw = str((worldspawn_keys or {}).get("suncolor") or "")
    # Black Ops' own worldspawn writes ".567 .613.653": a missing space between two decimals.
    values = [float(t) for t in re.findall(r"\d*\.\d+|\d+", raw)]
    values = [v for v in values if v > 0][:3]
    return sum(values) / len(values) if values else 1.0


def helper_source(reference: float) -> str:
    reference = reference if reference > 0 else 1.0
    return f"""
// codport: Black Ops' SetSunLight( r, g, b ) and ResetSunLight() (see codport/gsc/client_sun.py).
// Black Ops II scales its sun with r_lightTweakSunLight, as zm_tomb's lightning does; the colour is
// taken relative to the source map's own sun colour ({reference:.4f} per channel).
codport_setsunlight( r, g, b )
{{
	if ( !isdefined( level.codport_sunlight ) )
		level.codport_sunlight = getdvarfloat( #"r_lightTweakSunLight" );
	// The dvar's domain is 0 to 32; a value past it is refused with a console error and the flash is lost.
	value = level.codport_sunlight * ( ( r + g + b ) / 3 ) / {reference:.4f};
	if ( value > 32 )
		value = 32;
	setsaveddvar( "r_lightTweakSunLight", value );
}}

codport_resetsunlight()
{{
	if ( isdefined( level.codport_sunlight ) )
		setsaveddvar( "r_lightTweakSunLight", level.codport_sunlight );
}}
"""
