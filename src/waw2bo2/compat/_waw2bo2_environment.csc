#include clientscripts\mp\_utility;

init()
{
    registersystem( "waw_sun", ::sun_state );
    level.waw_global_vision = [];
    level.waw_player_vision = [];
    registersystem( "waw_vision_global", ::global_vision_state );
    registersystem( "waw_vision_player", ::player_vision_state );
}

global_vision_state( localclientnum, state )
{
    if ( state == "" )
        return;
    level.waw_global_vision[localclientnum] = int( state );
    apply_vision( localclientnum );
}

player_vision_state( localclientnum, state )
{
    if ( state == "" )
        return;
    level.waw_player_vision[localclientnum] = int( state );
    apply_vision( localclientnum );
}

apply_vision( localclientnum )
{
    index = 0;
    if ( isdefined( level.waw_global_vision[localclientnum] ) )
        index = level.waw_global_vision[localclientnum];
    if ( isdefined( level.waw_player_vision[localclientnum] ) && level.waw_player_vision[localclientnum] >= 0 )
        index = level.waw_player_vision[localclientnum];
    setdvar( "vc_LUT", index + 1 );
}

// ---- WaW client builtins for the map's own client scripts (clientport) ----

// WaW light intensity scales the light's gamma colour (ComPrimaryLight colour
// = _color * intensity). Converted primary lights are staged linear (colour
// squared, lighting.stage_primary_lights), so the T6 intensity is its square.
waw_setlightintensity( intensity )
{
    self setlightintensity( intensity * intensity );
}

waw_getlightintensity()
{
    return sqrt( self getlightintensity() );
}

// T6 client scripts have no volumetric fog; the server applies the map's
// client fog moments (gscport client_fog_monitors).
waw_client_setvolfog( start, halfway, halfheight, baseheight, r, g, b, transition )
{
}

// T6 syncs client system states itself: BO2's waitforclient has no such call.
waw_syncsystemstates( client )
{
}

// WaW restores splitscreen profile dvars; T6 has no client builtin for it.
waw_updatedvarsfromprofile( client )
{
}

// T6 has no client sound volume query: a WaW fade-out loop ends at once.
waw_getsoundvolume( id )
{
    return 0;
}

waw_setclientdvar( name, value )
{
    setdvar( name, value );
}

// WaW script profiler markers.
waw_prof( name )
{
}

// WaW's engine zombie-eye state; T6 actors draw BO2's own eye glow.
waw_haseyes()
{
    return 0;
}

// Both sun tweak dvars are SAVED (0x1200): plain setdvar is refused for them.
sun_state( localclientnum, state )
{
    if ( !isdefined( level.waw_saved_sun ) )
    {
        level.waw_saved_sun = spawnstruct();
        level.waw_saved_sun.color = getdvar( #"r_lightTweakSunColor" );
        level.waw_saved_sun.light = getdvar( #"r_lightTweakSunLight" );
    }
    if ( state == "" )
        return;
    if ( state == "reset" )
    {
        setsaveddvar( "r_lightTweakSunColor", level.waw_saved_sun.color );
        setsaveddvar( "r_lightTweakSunLight", level.waw_saved_sun.light );
        return;
    }
    values = strtok( state, " " );
    if ( values.size != 3 )
        return;
    // WaW sun values are gamma; T6 sun light is linear (as the world sun)
    r = float( values[0] ) * float( values[0] );
    g = float( values[1] ) * float( values[1] );
    b = float( values[2] ) * float( values[2] );
    strength = max( r, max( g, b ) );
    color = "0 0 0";
    if ( strength > 0 )
        color = ( r / strength ) + " " + ( g / strength ) + " " + ( b / strength );
    setsaveddvar( "r_lightTweakSunColor", color );
    setsaveddvar( "r_lightTweakSunLight", strength );
}
