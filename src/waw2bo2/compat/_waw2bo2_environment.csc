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
    r = float( values[0] );
    g = float( values[1] );
    b = float( values[2] );
    strength = max( r, max( g, b ) );
    color = "0 0 0";
    if ( strength > 0 )
        color = ( r / strength ) + " " + ( g / strength ) + " " + ( b / strength );
    setsaveddvar( "r_lightTweakSunColor", color );
    setsaveddvar( "r_lightTweakSunLight", strength );
}
