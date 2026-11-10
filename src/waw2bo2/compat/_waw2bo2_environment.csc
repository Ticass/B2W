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
// Every alias the map's client scripts play is the map's own (waw/); names
// they build at runtime ("pa_audio_link_" + count) reach the bank here.
waw_client_alias( alias )
{
    if ( getsubstr( alias, 0, 4 ) == "waw/" )
        return alias;
    return "waw/" + alias;
}

// waw_client_sound_debug 1: every sound the map's client scripts start goes
// to the console (WAW CLIENT SOUND), so a probe can see the client side ran.
waw_client_sound_log( kind, alias, origin )
{
    if ( getdvar( "waw_client_sound_debug" ) != "1" )
        return;
    where = "";
    if ( isdefined( origin ) )
        where = " " + origin;
    println( "WAW CLIENT SOUND " + kind + " " + waw_client_alias( alias ) + where );
}

waw_client_playsound( clientnum, alias, origin )
{
    if ( !isdefined( alias ) || alias == "" )
        return -1;
    waw_client_sound_log( "play", alias, origin );
    if ( isdefined( origin ) )
        return playsound( clientnum, waw_client_alias( alias ), origin );
    return playsound( clientnum, waw_client_alias( alias ) );
}

// WaW clientscripts\_audio::playLoopAt: a looping sound on a fake entity of
// its own, returned to the caller (BO2's playloopat takes no client and
// returns nothing).
waw_playloopat( clientnum, alias, origin, fade )
{
    if ( !isdefined( fade ) )
        fade = 0;
    fake_ent = spawnfakeent( clientnum );
    setfakeentorg( clientnum, fake_ent, origin );
    waw_client_sound_log( "loop", alias, origin );
    playloopsound( clientnum, fake_ent, waw_client_alias( alias ), fade );
    return fake_ent;
}

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

// WaW client SpawnFx + TriggerFX. T6's client spawnfx drops the given axis (a
// createfx effect plays on the default one: Der Riese's eclipse sprite seen
// edge-on) and no stock client script calls it; BO2 plays createfx through
// playfx (clientscripts\mp\_fx create_triggerfx). The returned handle is what
// WaW scripts keep and delete (waw_client_delete stops the effect). It is a
// struct, not a client entity: T6 has 768 client entities per local client
// (CG_Spawn), and a map with many createfx one-shots ran out of them.
waw_spawnfx( clientnum, fx, origin, delay, forward, up )
{
    ent = spawnstruct();
    ent.origin = origin;
    ent.waw_fx_client = clientnum;
    ent.waw_fx = fx;
    ent.waw_fx_delay = delay;
    ent.waw_fx_forward = forward;
    ent.waw_fx_up = up;
    return ent;
}

waw_triggerfx( ent, time )
{
    if ( !isdefined( time ) )
        time = ent.waw_fx_delay;
    if ( isdefined( ent.waw_fx_playing ) )
        deletefx( ent.waw_fx_client, ent.waw_fx_playing );
    if ( !isdefined( ent.waw_fx_forward ) )
        ent.waw_fx_playing = playfx( ent.waw_fx_client, ent.waw_fx, ent.origin );
    else if ( !isdefined( ent.waw_fx_up ) )
        ent.waw_fx_playing = playfx( ent.waw_fx_client, ent.waw_fx, ent.origin, ent.waw_fx_forward );
    else
        ent.waw_fx_playing = playfx( ent.waw_fx_client, ent.waw_fx, ent.origin, ent.waw_fx_forward, ent.waw_fx_up, time );
}

// The map's client Delete(): a waw_spawnfx handle stops its effect, any other
// entity is deleted as before.
waw_client_delete()
{
    if ( isdefined( self.waw_fx_client ) )
    {
        if ( isdefined( self.waw_fx_playing ) )
            deletefx( self.waw_fx_client, self.waw_fx_playing );
        self.waw_fx_playing = undefined;
        return;
    }
    self delete();
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
