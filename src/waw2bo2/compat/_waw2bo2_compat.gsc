// waw2bo2 compatibility layer: World at War script API on the BO2 zombies runtime.
//
// Generic for every converted WaW map. gscport.py rewrites references to WaW
// builtins that BO2 lacks (or implements differently) into calls of the waw_*
// functions below; nothing here names a map, an entity or an asset.
// Unsupported behaviour is reported once per API in the console
// ("WAW2BO2 UNSUPPORTED ..."), never hidden.
#include common_scripts\utility;
#include maps\mp\_utility;

init()
{
    if ( isdefined( level.waw2bo2_compat_ready ) )
        return;
    level.waw2bo2_compat_ready = 1;
    level.waw2bo2_reported = [];
    if ( !isdefined( level.waw2bo2_fx ) )
        level.waw2bo2_fx = [];
    if ( !isdefined( level.waw2bo2_weapons ) )
        level.waw2bo2_weapons = [];
    if ( !isdefined( level.waw2bo2_weapon_names ) )
        level.waw2bo2_weapon_names = [];
    maps\mp\waw\_waw2bo2_assets::init();
    maps\mp\waw\_waw2bo2_visions::init();
    maps\mp\_utility::registerclientsys( "waw_sun" );
    maps\mp\_utility::registerclientsys( "waw_vision_global" );
    maps\mp\_utility::registerclientsys( "waw_vision_player" );
    level thread stuck_zombie_monitor();
    level thread power_bridge();
}

report( api, detail )
{
    key = api;
    if ( isdefined( detail ) )
        key = api + " " + detail;
    if ( !isdefined( level.waw2bo2_reported ) )
        level.waw2bo2_reported = [];
    if ( isdefined( level.waw2bo2_reported[key] ) )
        return;
    level.waw2bo2_reported[key] = 1;
    println( "WAW2BO2 UNSUPPORTED " + key );
}

// WaW's zombiemode framework turns the power on with flag "electricity_on";
// BO2's with flag "power_on" (+ "electric_door" / client "power_on" notifies
// and unpausing the perk machines). Keep both frameworks in step.
power_bridge()
{
    if ( !isdefined( level.flag ) || !isdefined( level.flag["electricity_on"] ) )
        flag_init( "electricity_on" );
    level thread power_bridge_bo2_to_waw();
    flag_wait( "electricity_on" );
    if ( isdefined( level.flag["power_on"] ) && flag( "power_on" ) )
        return;
    level notify( "electric_door" );
    clientnotify( "power_on" );
    if ( isdefined( level.flag["power_on"] ) )
        flag_set( "power_on" );
    level notify( "power_on" );
    level thread maps\mp\zombies\_zm_perks::perk_unpause_all_perks();
}

power_bridge_bo2_to_waw()
{
    flag_wait( "power_on" );
    if ( !flag( "electricity_on" ) )
        flag_set( "electricity_on" );
}

// ---- FX: WaW effect names resolve through the converted asset table ----

waw_loadfx( name )
{
    if ( isdefined( level.waw2bo2_fx ) && isdefined( level.waw2bo2_fx[name] ) )
        return loadfx( level.waw2bo2_fx[name] );
    report( "FX", name );
    return undefined;
}

waw_playfx( fx, origin, forward, up )
{
    if ( !isdefined( fx ) || !isdefined( origin ) )
        return;
    if ( isdefined( up ) )
        playfx( fx, origin, forward, up );
    else if ( isdefined( forward ) )
        playfx( fx, origin, forward );
    else
        playfx( fx, origin );
}

waw_playfxontag( fx, ent, tag )
{
    if ( !isdefined( fx ) || !isdefined( ent ) )
        return;
    playfxontag( fx, ent, tag );
}

waw_playloopedfx( fx, delay, origin, cull, forward, up )
{
    if ( !isdefined( fx ) )
        return undefined;
    if ( !isdefined( cull ) )
        cull = 0;
    if ( !isdefined( forward ) )
        forward = ( 1, 0, 0 );
    if ( !isdefined( up ) )
        up = ( 0, 0, 1 );
    ent = spawnfx( fx, origin, forward, up );
    ent thread waw_loop_fx( delay );
    return ent;
}

waw_loop_fx( delay )
{
    self endon( "death" );
    for (;;)
    {
        triggerfx( self );
        wait( delay );
    }
}

waw_spawnfx( fx, origin, forward, up )
{
    if ( !isdefined( fx ) )
        return undefined;
    if ( isdefined( up ) )
        return spawnfx( fx, origin, forward, up );
    if ( isdefined( forward ) )
        return spawnfx( fx, origin, forward );
    return spawnfx( fx, origin );
}

waw_triggerfx( ent, delay )
{
    if ( !isdefined( ent ) )
        return;
    if ( isdefined( delay ) )
        triggerfx( ent, delay );
    else
        triggerfx( ent );
}

// ---- world lighting / fog ----

// WaW: setVolFog(start, halfway, halfheight, baseheight, r, g, b, transition)
// T6 sub_851590 accepts the same eight parameters, normalizing RGB and
// retaining its magnitude as fogColorScale. Its extended form needs 18;
// a 17-argument call errors and leaves the previous map fog in place.
waw_setvolfog( start, halfway, halfheight, baseheight, r, g, b, transition )
{
    if ( !isdefined( transition ) )
        transition = 0;
    setvolfog( start, halfway, halfheight, baseheight, r, g, b, transition );
}

// BO2 renderer controls live on clients. The state is retained for late joins.
waw_setsunlight( r, g, b )
{
    maps\mp\_utility::setclientsysstate( "waw_sun", r + " " + g + " " + b );
}

waw_resetsunlight()
{
    maps\mp\_utility::setclientsysstate( "waw_sun", "reset" );
}

// ---- client dvars ----

// WaW sets per-client dvars from the server; BO2 has no such builtin. WaW
// renderer tuning (r_*, sm_*) does not mean the same thing to T6's renderer,
// and cg_*/ui dvars belong to WaW menus. Everything else goes to the server
// dvar (the listen-server host shares it).
waw_setclientdvar( name, value )
{
    lname = tolower( name );
    prefix = getsubstr( lname, 0, 3 );
    if ( getsubstr( lname, 0, 2 ) == "r_" || prefix == "sm_" || prefix == "cg_" || prefix == "ui_" || prefix == "bg_" )
    {
        report( "setClientDvar", name );
        return;
    }
    setdvar( name, value );
}

waw_setclientdvars( n1, v1, n2, v2, n3, v3, n4, v4, n5, v5, n6, v6, n7, v7, n8, v8 )
{
    self waw_setclientdvar( n1, v1 );
    if ( isdefined( n2 ) )
        self waw_setclientdvar( n2, v2 );
    if ( isdefined( n3 ) )
        self waw_setclientdvar( n3, v3 );
    if ( isdefined( n4 ) )
        self waw_setclientdvar( n4, v4 );
    if ( isdefined( n5 ) )
        self waw_setclientdvar( n5, v5 );
    if ( isdefined( n6 ) )
        self waw_setclientdvar( n6, v6 );
    if ( isdefined( n7 ) )
        self waw_setclientdvar( n7, v7 );
    if ( isdefined( n8 ) )
        self waw_setclientdvar( n8, v8 );
}

// ---- animations of WaW animtrees that are not converted yet ----
// gscport routes the animation builtins of such scripts here (their %anim
// references become strings); the scripted behaviour runs without the motion.

waw_anim_useanimtree( tree )
{
    report( "XANIM animtree", tree );
}

waw_anim_setanim( animname, weight, time, rate )
{
    report( "XANIM", animname );
}

waw_anim_clearanim( animname, time )
{
}

waw_anim_getanimlength( animname )
{
    report( "XANIM", animname );
    return 0.05;
}

waw_anim_animscripted( notifyname, origin, angles, animname, mode, root, rate, blend )
{
    report( "XANIM", animname );
    self thread waw_anim_end( notifyname );
}

waw_anim_end( notifyname )
{
    self endon( "death" );
    wait 0.05;
    self notify( notifyname, "end" );
}

// ---- sounds / visions ----

// Converted WaW aliases live in the map's own bank as "waw/<alias>" so they
// never collide with a stock BO2 alias of the same name. An alias the
// conversion lacks plays nothing (as WaW does for an undefined alias) and is
// reported once, instead of letting BO2's same-named sound play.
waw_sound( alias )
{
    if ( !isdefined( alias ) )
        return undefined;
    name = "waw/" + alias;
    if ( soundexists( name ) )
        return name;
    report( "SOUND", alias );
    return undefined;
}

// WaW: ent playSound( alias, notifyname ) notifies when the sound ends.
waw_playsound( alias, notifyname, stoppable )
{
    name = waw_sound( alias );
    if ( !isdefined( name ) )
        return;
    if ( isdefined( notifyname ) )
        self playsoundwithnotify( name, notifyname );
    else
        self playsound( name );
}

waw_playloopsound( alias, fadetime )
{
    name = waw_sound( alias );
    if ( !isdefined( name ) )
        return;
    if ( isdefined( fadetime ) )
        self playloopsound( name, fadetime );
    else
        self playloopsound( name );
}

waw_playsoundatposition( alias, origin )
{
    name = waw_sound( alias );
    if ( isdefined( name ) )
        playsoundatposition( name, origin );
}

waw_playlocalsound( alias )
{
    name = waw_sound( alias );
    if ( isdefined( name ) )
        self playlocalsound( name );
}

waw_playsoundasmaster( alias )
{
    name = waw_sound( alias );
    if ( isdefined( name ) )
        self playsoundasmaster( name );
}

waw_soundexists( alias )
{
    return isdefined( alias ) && soundexists( "waw/" + alias );
}

// Conversion diagnostic: a zombie that has not moved 32 units in 15 s while
// not traversing is reported once per spot (position, AI state, whether it
// has entered the playable area), so path problems can be located.
stuck_zombie_monitor()
{
    level endon( "end_game" );
    for ( ;; )
    {
        wait 3;
        zombies = getaiarray( level.zombie_team );
        foreach ( zombie in zombies )
        {
            if ( !isalive( zombie ) )
                continue;
            if ( !isdefined( zombie.waw2bo2_last_pos ) || distancesquared( zombie.origin, zombie.waw2bo2_last_pos ) > 1024 || is_true( zombie.is_traversing ) )
            {
                zombie.waw2bo2_last_pos = zombie.origin;
                zombie.waw2bo2_last_time = gettime();
                continue;
            }
            if ( gettime() - zombie.waw2bo2_last_time < 15000 || is_true( zombie.waw2bo2_stuck_reported ) )
                continue;
            zombie.waw2bo2_stuck_reported = 1;
            state = "?";
            if ( isdefined( zombie.ai_state ) )
                state = zombie.ai_state;
            println( "WAW2BO2 STUCK zombie at " + zombie.origin + " ai_state " + state + " entered_playable " + is_true( zombie.completed_emerging_into_playable_area ) );
        }
    }
}

// ---- weapons ----
// level.waw2bo2_weapons (generated in _waw2bo2_assets): every WaW weapon of the
// map -> the weapon mod.ff carries for it, or "" when it is not carried.
// level.waw2bo2_weapon_names is the reverse (BO2 name -> WaW name). A name in
// neither table is BO2's own (the BO2 framework can hand one to a WaW script)
// and passes through. T6 fails on an unknown weapon, so an uncarried WaW
// weapon is reported and the call skipped.

waw_weapon( name )
{
    if ( !isdefined( name ) || !isdefined( level.waw2bo2_weapons ) || !isdefined( level.waw2bo2_weapons[name] ) )
        return name;
    if ( level.waw2bo2_weapons[name] == "" )
    {
        report( "WEAPON", name );
        return undefined;
    }
    return level.waw2bo2_weapons[name];
}

// BO2 weapon name -> the WaW name the map scripts compare against
waw_weapon_name( weapon )
{
    if ( isdefined( weapon ) && isdefined( level.waw2bo2_weapon_names ) && isdefined( level.waw2bo2_weapon_names[weapon] ) )
        return level.waw2bo2_weapon_names[weapon];
    return weapon;
}

waw_precacheitem( name )
{
    weapon = waw_weapon( name );
    if ( isdefined( weapon ) )
        precacheitem( weapon );
}

waw_giveweapon( name, model_index, a, b )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return;
    if ( isdefined( model_index ) )
        self giveweapon( weapon, model_index );
    else
        self giveweapon( weapon );
}

waw_takeweapon( name )
{
    weapon = waw_weapon( name );
    if ( isdefined( weapon ) )
        self takeweapon( weapon );
}

waw_switchtoweapon( name )
{
    weapon = waw_weapon( name );
    if ( isdefined( weapon ) )
        self switchtoweapon( weapon );
}

waw_hasweapon( name )
{
    weapon = waw_weapon( name );
    return isdefined( weapon ) && self hasweapon( weapon );
}

waw_givemaxammo( name )
{
    weapon = waw_weapon( name );
    if ( isdefined( weapon ) )
        self givemaxammo( weapon );
}

waw_setweaponammoclip( name, amount )
{
    weapon = waw_weapon( name );
    if ( isdefined( weapon ) )
        self setweaponammoclip( weapon, amount );
}

waw_setweaponammostock( name, amount )
{
    weapon = waw_weapon( name );
    if ( isdefined( weapon ) )
        self setweaponammostock( weapon, amount );
}

waw_getweaponammoclip( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return 0;
    return self getweaponammoclip( weapon );
}

waw_getweaponammostock( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return 0;
    return self getweaponammostock( weapon );
}

// ---- Zombies weapon registration ----
// WaW include_weapon( name, in_box ) / add_zombie_weapon( name, hint, cost,
// weaponVO, variation_count, ammo_cost ) -> BO2 _zm_weapons, whose
// add_zombie_weapon takes ( name, upgrade_name, hint, cost, weaponvo,
// weaponvoresp, ammo_cost ). WaW's Pack-a-Punch upgrades <name> to
// <name>_upgraded; BO2 records that pair on the base weapon.

waw_weapon_known( name )
{
    return isdefined( name ) && isdefined( level.waw2bo2_weapons ) && isdefined( level.waw2bo2_weapons[name] ) && level.waw2bo2_weapons[name] != "";
}

waw_include_weapon( name, in_box, collector, weighting_func )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return;
    if ( !isdefined( in_box ) )
        in_box = 1;
    maps\mp\zombies\_zm_weapons::include_zombie_weapon( weapon, in_box, collector, weighting_func );
}

waw_add_zombie_weapon( name, hint, cost, weaponvo, variation_count, ammo_cost )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return;
    upgrade = undefined;
    if ( waw_weapon_known( name + "_upgraded" ) )
        upgrade = level.waw2bo2_weapons[name + "_upgraded"];
    if ( !isdefined( ammo_cost ) )
        ammo_cost = maps\mp\zombies\_zm_utility::round_up_to_ten( int( cost * 0.5 ) );
    // WaW hints are plain text; BO2 precaches the hint as a localized string.
    // With level.monolingustic_prompt_format (set by include_weapons) BO2
    // builds the wall-buy prompt from the weapon's display name and cost.
    if ( isdefined( hint ) && hint != "" )
        report( "WEAPON_HINT (prompt built from display name + cost)", name );
    hint = &"ZOMBIE_WEAPONCOSTONLY";
    if ( !isdefined( weaponvo ) )
        weaponvo = "";
    maps\mp\zombies\_zm_weapons::add_zombie_weapon( weapon, upgrade, hint, cost, weaponvo, "", ammo_cost );
}

waw_getcurrentweapon()
{
    return waw_weapon_name( self getcurrentweapon() );
}

waw_getweaponslistprimaries()
{
    list = self getweaponslistprimaries();
    for ( i = 0; i < list.size; i++ )
        list[i] = waw_weapon_name( list[i] );
    return list;
}


// WaW names resolve to translated rawfiles. T6's player API has a different name.
waw_visionsetnaked( vision, time )
{
    if ( !isdefined( time ) )
        time = 0;
    vision = tolower( vision );
    index = 0;
    if ( vision != "" )
    {
        if ( !isdefined( level.waw2bo2_visions[vision] ) )
        {
            report( "VISION_ABSENT", vision );
            return;
        }
        index = level.waw2bo2_vision_indices[vision];
        vision = level.waw2bo2_visions[vision];
    }
    if ( isdefined( self ) && isplayer( self ) )
    {
        if ( vision == "" )
            index = -1;
        maps\mp\_utility::setclientsysstate( "waw_vision_player", "" + index, self );
        self setvisionsetforplayer( vision, time );
    }
    else
    {
        maps\mp\_utility::setclientsysstate( "waw_vision_global", "" + index );
        if ( vision == "" )
            vision = "waw/_identity";
        visionsetnaked( vision, time );
    }
}

// ---- miscellaneous WaW builtins ----

waw_getcurrentweaponclipammo()
{
    return self getweaponammoclip( self getcurrentweapon() );
}

waw_assertex( condition, message )
{
}

waw_getstat( index )
{
    report( "getStat" );
    return 0;
}

waw_setstat( index, value )
{
    report( "setStat" );
}

waw_settimescale( scale )
{
    report( "SetTimeScale" );
}

waw_playerpositionvalid( ent )
{
    return 1;
}
