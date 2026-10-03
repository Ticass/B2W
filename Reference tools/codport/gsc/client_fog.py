"""
Black Ops' per-client fog, on Black Ops II's client fog builtins.

Black Ops client scripts set one local client's fog with

    setVolFogForClient( localClientNum, <setVolFog's values> )

and the engine fades to it over the transition time. zombie_coast does this for its whole weather
cycle -- the blizzard's dense exterior fog, a lighter fog inside the lighthouse and in each part of
the ship, the clear-weather fog -- from client-side triggers at the doorways. Black Ops II has no
setVolFogForClient (only a leftover name string in t6zm.exe), so the translator used to delete
every call, and the port kept whichever fog it was built with everywhere, all the time.

Black Ops II does have the same fog, split in two (t6zm.exe, `codport.bo2.exe_builtins`):

* `setclientvolumetricfog( <setVolFog's values> )` (0x7cd750) stores one client-side fog. It takes
  setVolFog's values in setVolFog's order -- start, half distance, half height, base height, red,
  green, blue, then either the transition time (8 values) or scale, sun colour, sun direction, sun
  start and stop angles, transition time and maximum opacity (18) -- checks them the same way and
  keeps colour and scale apart as Black Ops does (fog colour .w). It ignores the transition time.
* `switchtoclientvolumetricfog( localClientNum )` (0x7cdcc0) applies it to that client at once
  (0x74ab20 with a zero-length transition).

So `setVolFogForClient` becomes a call to a helper written into the same script, which steps Black
Ops' fade itself and applies each step with those two builtins.
"""

from __future__ import annotations

#: The Black Ops call, lower-cased, and the helper it becomes.
SOURCE_NAME = "setvolfogforclient"
HELPER_NAME = "codport_setvolfogforclient"

#: Seconds between fade steps.
FADE_STEP = 0.05

HELPER_SOURCE = f"""
// codport: Black Ops' setVolFogForClient( localClientNum, <setVolFog's values> ) (see
// codport/gsc/client_fog.py). Black Ops II stores the fog with setclientvolumetricfog, which takes
// setVolFog's values without the client, and applies it at once with switchtoclientvolumetricfog;
// Black Ops' fade over the transition time is stepped here.
{HELPER_NAME}( localclientnum, start_dist, half_dist, half_height, base_height, fog_r, fog_g, fog_b, fog_scale, sun_r, sun_g, sun_b, sun_x, sun_y, sun_z, sun_start, sun_stop, transition, max_opacity )
{{
	if ( !isdefined( sun_r ) )
	{{
		// setVolFog's short form: start, half distance, half height, base height, colour, transition.
		transition = fog_scale;
		fog_scale = 1;
		sun_r = 0;
		sun_g = 0;
		sun_b = 0;
		sun_x = 0;
		sun_y = 0;
		sun_z = 0;
		sun_start = 0;
		sun_stop = 0;
		max_opacity = 1;
	}}
	if ( !isdefined( transition ) )
		transition = 0;
	if ( !isdefined( max_opacity ) )
		max_opacity = 1;
	target = [];
	target[0] = start_dist;
	target[1] = half_dist;
	target[2] = half_height;
	target[3] = base_height;
	target[4] = fog_r;
	target[5] = fog_g;
	target[6] = fog_b;
	target[7] = fog_scale;
	target[8] = sun_r;
	target[9] = sun_g;
	target[10] = sun_b;
	target[11] = sun_x;
	target[12] = sun_y;
	target[13] = sun_z;
	target[14] = sun_start;
	target[15] = sun_stop;
	target[16] = max_opacity;
	// The same fog asked for again (a repeated trigger or callback) leaves the fade running; restarted
	// on every request, a ten-second fade never finished (zombie_coast v78).
	if ( !isdefined( level.codport_client_fog_target ) )
		level.codport_client_fog_target = [];
	last = level.codport_client_fog_target[localclientnum];
	if ( isdefined( last ) )
	{{
		same = 1;
		for ( j = 0; j < 17 && same; j++ )
		{{
			if ( last[j] != target[j] )
				same = 0;
		}}
		if ( same )
			return;
	}}
	level.codport_client_fog_target[localclientnum] = target;
	// One console line per fog change (Plutonium's console.log): what the port asked for and when.
	println( "codport fog: client " + localclientnum + " start " + start_dist + " half " + half_dist + " colour " + fog_r + " " + fog_g + " " + fog_b + " scale " + fog_scale + " opacity " + max_opacity + " over " + transition + "s" );
	level notify( "codport_fog_" + localclientnum );
	level thread codport_client_fog_fade( localclientnum, target, transition );
}}

codport_client_fog_fade( localclientnum, target, transition )
{{
	level endon( "codport_fog_" + localclientnum );
	if ( !isdefined( level.codport_client_fog ) )
		level.codport_client_fog = [];
	from = level.codport_client_fog[localclientnum];
	if ( isdefined( from ) && transition > 0 )
	{{
		steps = int( transition / {FADE_STEP} );
		for ( i = 1; i < steps; i++ )
		{{
			frac = i * 1.0 / steps;
			// A new array each step: the last one applied is held in level.codport_client_fog.
			current = [];
			for ( j = 0; j < 17; j++ )
				current[j] = from[j] + ( target[j] - from[j] ) * frac;
			codport_client_fog_apply( localclientnum, current );
			waitrealtime( {FADE_STEP} );
		}}
	}}
	codport_client_fog_apply( localclientnum, target );
}}

codport_client_fog_apply( localclientnum, v )
{{
	level.codport_client_fog[localclientnum] = v;
	setclientvolumetricfog( v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], 0, v[16] );
	switchtoclientvolumetricfog( localclientnum );
}}
"""
