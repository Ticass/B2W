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
    precacheshader( "waypoint_revive" );
    // WaW zones control spawning; they are not lethal player boundaries.
    // BO2 otherwise queues a kill during scripted teleports above a zone.
    if ( isdefined( level.player_out_of_playable_area_monitor_callback ) )
        level.waw2bo2_out_of_playable_area_callback = level.player_out_of_playable_area_monitor_callback;
    level.player_out_of_playable_area_monitor_callback = ::waw_out_of_playable_area;
    level.waw2bo2_reported = [];
    if ( !isdefined( level.waw_effect ) )
        level.waw_effect = [];
    if ( !isdefined( level.waw2bo2_fx ) )
        level.waw2bo2_fx = [];
    if ( !isdefined( level.waw2bo2_weapons ) )
        level.waw2bo2_weapons = [];
    if ( !isdefined( level.waw2bo2_weapon_names ) )
        level.waw2bo2_weapon_names = [];
    if ( !isdefined( level.waw2bo2_runtime_weapons ) )
        level.waw2bo2_runtime_weapons = [];
    maps\mp\waw\_waw2bo2_assets::init();
    maps\mp\waw\_waw2bo2_visions::init();
    maps\mp\_utility::registerclientsys( "waw_sun" );
    maps\mp\_utility::registerclientsys( "waw_vision_global" );
    maps\mp\_utility::registerclientsys( "waw_vision_player" );
    level.waw2bo2_overlay_global = "_identity";
    level thread vision_overlay_players();
    level thread waw_player_fields();
    level thread waw_revive_waypoints();
    level thread all_players_connected_bridge();
    level thread first_player_ready_bridge();
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

// T6 applies the attacker's custom damage function at the point of a swipe.
// Source contact-kill monitors retain their own proximity checks and kills;
// suppress the competing ASD attack until the monitor returns.
waw_contact_kill_begin()
{
    if ( !isdefined( self.waw_contact_kill_count ) || self.waw_contact_kill_count <= 0 )
    {
        self.waw_contact_kill_count = 0;
        self.waw_contact_previous_damage = undefined;
        if ( isdefined( self.custom_damage_func ) )
            self.waw_contact_previous_damage = self.custom_damage_func;
        self.custom_damage_func = ::waw_contact_kill_damage;
    }
    self.waw_contact_kill_count++;
}

waw_contact_kill_end()
{
    self.waw_contact_kill_count--;
    if ( self.waw_contact_kill_count > 0 )
        return;
    if ( isdefined( self.custom_damage_func ) && self.custom_damage_func == ::waw_contact_kill_damage )
        self.custom_damage_func = self.waw_contact_previous_damage;
    self.waw_contact_previous_damage = undefined;
}

waw_contact_kill_damage( victim )
{
    return 0;
}

waw_out_of_playable_area()
{
    // Called by BO2's native monitor with the player as self. Preserve
    // intentional kill brushes, but do not turn absent/disabled WaW spawn
    // volumes into new death barriers. Source trigger damage stays native.
    if ( !self maps\mp\zombies\_zm::in_kill_brush() )
        return false;
    if ( isdefined( level.waw2bo2_out_of_playable_area_callback ) )
        return self [[ level.waw2bo2_out_of_playable_area_callback ]]();
    return true;
}

// WaW utility helper used by the power bridge; BO2 has no function by
// this name. Wait one standard 20 Hz server frame between its notifies.
wait_network_frame()
{
    wait 0.05;
}

// WaW hide_chest hides every piece, including inactive trigger_use entities.
// Restoring only their origin leaves T6's use trigger hidden after relocation.
waw_enable_trigger()
{
    self maps\mp\zombies\_zm_utility::enable_trigger();
    if ( isdefined( self.classname ) && self.classname == "trigger_use" )
        self show();
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
    // WaW perk scripts wait for these framework events before swapping models.
    // BO2 power_on does not emit them; preserve WaW's network-frame ordering.
    wait_network_frame();
    level notify( "sleight_on" );
    wait_network_frame();
    level notify( "revive_on" );
    wait_network_frame();
    level notify( "doubletap_on" );
    wait_network_frame();
    level notify( "juggernog_on" );
    wait_network_frame();
    level notify( "Pack_A_Punch_on" );
    if ( isdefined( level.flag["power_on"] ) && flag( "power_on" ) )
        return;
    level notify( "electric_door" );
    clientnotify( "power_on" );
    if ( isdefined( level.flag["power_on"] ) )
        flag_set( "power_on" );
    level notify( "power_on" );
    level thread maps\mp\waw\_waw2bo2_perks::perk_unpause_all_perks();
}

power_bridge_bo2_to_waw()
{
    flag_wait( "power_on" );
    if ( !flag( "electricity_on" ) )
        flag_set( "electricity_on" );
}

// T6 ground movement can consume an upward impulse before leaving the floor.
// Lift only grounded players receiving an upward launch, then apply the
// source velocity in the same frame. Airborne and non-player calls stay native.
waw_setvelocity( velocity )
{
    if ( isplayer( self ) && velocity[2] > 0 && self isonground() )
        self setorigin( self.origin + ( 0, 0, 1 ) );
    self setvelocity( velocity );
}

// ---- FX: WaW effect names resolve through the converted asset table ----

waw_loadfx( name )
{
    if ( isdefined( level.waw2bo2_fx ) && isdefined( level.waw2bo2_fx[name] ) )
    {
        loaded = loadfx( level.waw2bo2_fx[name] );
        if ( isdefined( level.waw2bo2_model_overlay_fx ) && isdefined( level.waw2bo2_model_overlay_fx[name] ) )
        {
            if ( !isdefined( level.waw2bo2_model_overlay_ids ) )
                level.waw2bo2_model_overlay_ids = [];
            level.waw2bo2_model_overlay_ids[loaded] = [];
            models = getarraykeys( level.waw2bo2_model_overlay_fx[name] );
            for ( i = 0; i < models.size; i++ )
                level.waw2bo2_model_overlay_ids[loaded][models[i]] = loadfx( level.waw2bo2_model_overlay_fx[name][models[i]] );
        }
        return loaded;
    }
    report( "FX", name );
    return undefined;
}

waw_playfx( fx, origin, forward, up )
{
    if ( !isdefined( fx ) || !isdefined( origin ) )
        return;
    fx = waw_model_overlay_fx( fx, origin );
    if ( isdefined( up ) )
        playfx( fx, origin, forward, up );
    else if ( isdefined( forward ) )
        playfx( fx, origin, forward );
    else
        playfx( fx, origin );
}

// Source effects keep their own IDs in level.waw_effect. Reuse an identical
// model already rendered by a map entity; unmatched effects remain complete.
waw_model_overlay_fx( fx, origin )
{
    if ( !isdefined( level.waw2bo2_model_overlay_ids ) || !isdefined( level.waw2bo2_model_overlay_ids[fx] ) )
        return fx;
    entities = getentarray( "script_model", "classname" );
    for ( i = 0; i < entities.size; i++ )
    {
        ent = entities[i];
        if ( isdefined( ent.model ) && distance( ent.origin, origin ) <= 4
            && isdefined( level.waw2bo2_model_overlay_ids[fx][ent.model] ) )
        {
            if ( !isdefined( level.waw2bo2_model_overlay_logged ) )
                level.waw2bo2_model_overlay_logged = [];
            if ( !isdefined( level.waw2bo2_model_overlay_logged[fx] ) )
            {
                println( "WAW2BO2 FX MODEL REUSE " + ent.model );
                level.waw2bo2_model_overlay_logged[fx] = 1;
            }
            return level.waw2bo2_model_overlay_ids[fx][ent.model];
        }
    }
    println( "WAW2BO2 FX MODEL UNMATCHED id=" + fx + " origin=" + origin );
    return fx;
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
// WaW lerps gamma colour to its fog colour; T6 lerps linear colour and
// writes sqrt(hdr * result) with the converted world's hdr scale of 1.
waw_setvolfog( start, halfway, halfheight, baseheight, r, g, b, transition )
{
    if ( !isdefined( transition ) )
        transition = 0;
    if ( isdefined( level.waw2bo2_fog_off ) && level.waw2bo2_fog_off )
        return;
    setvolfog( start, halfway, halfheight, baseheight, r * r, g * g, b * b, transition );
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

// ---- animation ----

// T6 has no StopUseAnimTree; WaW callers clear their animations before it,
// which leaves a T6 script model at its bind pose as well.
waw_stopuseanimtree()
{
}

// WaW ClearAnim's blend time is optional; T6 requires it.
waw_clearanim( animname, time )
{
    if ( !isdefined( time ) )
        time = 0;
    self clearanim( animname, time );
}

// ---- hint strings ----

// WaW stores cost-bearing IDs (default_buy_door_750); BO2 normally stores
// the base ID and substitutes a separate cost. Keep the source registry.
waw_add_zombie_hint( ref, text )
{
    if ( !isdefined( level.waw2bo2_hints ) )
        level.waw2bo2_hints = [];
    precachestring( text );
    level.waw2bo2_hints[ref] = text;
}

waw_get_zombie_hint( ref )
{
    if ( isdefined( level.waw2bo2_hints ) && isdefined( level.waw2bo2_hints[ref] ) )
        return level.waw2bo2_hints[ref];
    return maps\mp\zombies\_zm_utility::get_zombie_hint( ref );
}

waw_set_hint_string( ent, default_ref )
{
    ref = default_ref;
    if ( isdefined( ent.script_hint ) )
        ref = ent.script_hint;
    self sethintstring( waw_get_zombie_hint( ref ) );
}

// trem_hintstrings (UGX) re-sent hints every frame through WaW menu client
// dvars because WaW's setHintString did not update; BO2's does, and draws
// &&1 as the use key itself.
waw_native_hintstring( string )
{
    if ( isdefined( string ) )
        self sethintstring( string );
}

// ---- client dvars ----

// WaW sets per-client dvars from the server; BO2 has no such builtin. WaW
// renderer tuning (r_*, sm_*) does not mean the same thing to T6's renderer,
// and cg_*/ui dvars belong to WaW menus. Everything else goes to the server
// dvar (the listen-server host shares it).
waw_setclientdvar( name, value )
{
    lname = tolower( name );
    // T6 caps the global dvar at 12.8 s, but its per-player duration accepts
    // WaW's longer perk durations. Keep these local to the purchasing player.
    if ( lname == "player_sprinttime" && isplayer( self ) )
    {
        self setsprintduration( float( value ) );
        return;
    }
    // r_filmUseTweaks: WaW films with the r_filmTweak* values (CoDWaW
    // sub_6DC1A0), baked at conversion into the "_filmtweak" overlay.
    if ( lname == "r_filmusetweaks" )
    {
        self.waw2bo2_filmtweaks = ( value != "0" && value != "" );
        if ( isdefined( self ) && isplayer( self ) )
            self vision_overlay_update();
        return;
    }
    // r_fog 0 removes WaW's fog; BO2's r_fog is a cheat dvar, so the fog is
    // moved past the view instead and later setVolFog calls stay there.
    if ( lname == "r_fog" )
    {
        level.waw2bo2_fog_off = ( value == "0" );
        if ( level.waw2bo2_fog_off )
            setvolfog( 1000000, 1000001, 1000, 0, 1, 1, 1, 0 );
        return;
    }
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

// weapon queries take the WaW name; T6 only knows the converted weapon
// (the mystery box cycles GetWeaponModel of WaW names)
waw_getweaponmodel( name, model_index )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return "tag_origin";
    if ( isdefined( model_index ) )
        return getweaponmodel( weapon, model_index );
    return getweaponmodel( weapon );
}

waw_weaponclass( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return "none";
    return weaponclass( weapon );
}

waw_weapontype( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return "none";
    return weapontype( weapon );
}

waw_weaponclipsize( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return 0;
    return weaponclipsize( weapon );
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

waw_switchtooffhand( name )
{
    weapon = waw_weapon( name );
    if ( isdefined( weapon ) )
        self switchtooffhand( weapon );
}

waw_getcurrentoffhand()
{
    return waw_weapon_name( self getcurrentoffhand() );
}

waw_hasweapon( name )
{
    weapon = waw_weapon( name );
    return isdefined( weapon ) && self hasweapon( weapon );
}

// Preserve authored equipment slots and user bindings; only map the weapon
// name and normalize the blank sentinel used to clear a source weapon slot.
waw_setactionslot( slot, type, name )
{
    if ( type == "weapon" )
    {
        // Source buildables clear the slot with a blank weapon sentinel.
        if ( !isdefined( name ) || name == "" || name == " " )
        {
            self setactionslot( slot, "" );
            return;
        }
        weapon = waw_weapon( name );
        if ( isdefined( weapon ) )
            self setactionslot( slot, type, weapon );
        return;
    }
    if ( isdefined( name ) )
        self setactionslot( slot, type, name );
    else
        self setactionslot( slot, type );
}

// A repeated source HUD cleanup must not abort the surrounding buildable
// thread before it restores weapon cycling/offhands. Pass the HUD as an
// argument: an undefined method receiver cannot safely enter this helper.
waw_destroy_hud_elem( element )
{
    if ( isdefined( element ) )
        element maps\mp\gametypes_zm\_hud_util::destroyelem();
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
    // Native tactical helpers use a different inventory name. Precache and
    // include it for their startup guards, without a second mystery-box entry.
    if ( isdefined( level.waw2bo2_runtime_weapons[name] ) )
        maps\mp\zombies\_zm_weapons::include_zombie_weapon( level.waw2bo2_runtime_weapons[name], 0 );
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
    // This must precede the native tactical initializer: it checks
    // level.zombie_weapons before registering its script-model animtree.
    if ( isdefined( level.waw2bo2_runtime_weapons[name] ) )
        maps\mp\zombies\_zm_weapons::add_zombie_weapon( level.waw2bo2_runtime_weapons[name], undefined, hint, cost, weaponvo, "", ammo_cost );
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


// WaW _zombiemode sets "all_players_connected" once the starting players are
// in; BO2's framework (which replaces it) sets "initial_players_connected".
// Stock WaW utilities (set_all_players_visionset, ...) wait on the WaW flag.
all_players_connected_bridge()
{
    if ( !isdefined( level.flag ) || !isdefined( level.flag["all_players_connected"] ) )
        flag_init( "all_players_connected" );
    flag_wait( "initial_players_connected" );
    flag_set( "all_players_connected" );
}

// WaW _callbackglobal notifies "first_player_ready" when the first player
// connects (maps\_utility::wait_for_first_player waits on it); BO2 does not.
first_player_ready_bridge()
{
    waittillframeend;
    players = getplayers();
    if ( players.size )
        player = players[0];
    else
        level waittill( "connected", player );
    level notify( "first_player_ready", player );
}

// WaW _zombiemode's connect handler gives every player a stats array
// (kills/score/downs/revives/perks) that map scripts increment; BO2 has none.
waw_player_fields()
{
    players = getplayers();
    for ( i = 0; i < players.size; i++ )
        players[i] waw_player_stats();
    for ( ;; )
    {
        level waittill( "connected", player );
        player waw_player_stats();
    }
}

waw_player_stats()
{
    if ( !isdefined( self.waw2bo2_grenade_monitor ) )
    {
        self.waw2bo2_grenade_monitor = 1;
        self thread waw_grenade_diagnostics();
    }
    if ( isdefined( self.stats ) )
        return;
    self.stats = [];
    self.stats["kills"] = 0;
    self.stats["score"] = 0;
    self.stats["downs"] = 0;
    self.stats["revives"] = 0;
    self.stats["perks"] = 0;
}

// Record the actual missile selected by the inventory, including after give all.
// Server-side evidence only: model names do not establish client delivery or
// rendering. Observe entity allocation and lifetime without changing missiles.
waw_grenade_diagnostics()
{
    self endon( "disconnect" );
    level endon( "end_game" );
    count = 0;
    for ( ;; )
    {
        self waittill( "grenade_fire", grenade, weapon );
        count++;
        model = "<unset>";
        if ( isdefined( grenade ) && isdefined( grenade.model ) )
            model = grenade.model;
        if ( !isdefined( weapon ) )
            weapon = "<unset>";
        number = -1;
        if ( isdefined( grenade ) )
            number = grenade getentitynumber();
        entities = getentarray();
        println( "WAW2BO2 GRENADE throw=" + count + " weapon=" + weapon + " model=" + model + " entity=" + number + " time=" + gettime() + " script_entities=" + entities.size );
        self thread waw_grenade_lifecycle( grenade, count, weapon, gettime() );
    }
}

waw_grenade_lifecycle( grenade, count, weapon, start )
{
    self endon( "disconnect" );
    level endon( "end_game" );
    wait 0.25;
    if ( isdefined( grenade ) )
    {
        model = "<unset>";
        if ( isdefined( grenade.model ) )
            model = grenade.model;
        println( "WAW2BO2 GRENADE STATE throw=" + count + " weapon=" + weapon + " elapsed=" + ( gettime() - start ) + " entity=" + grenade getentitynumber() + " model=" + model + " origin=" + grenade.origin );
    }
    else
        println( "WAW2BO2 GRENADE STATE throw=" + count + " weapon=" + weapon + " elapsed=" + ( gettime() - start ) + " removed=1" );
}

// WaW's co-op revive marker must also work when BO2's native teammate
// indicator is unavailable. Only actual revive targets get a waypoint;
// custom solo revives and Who's Who fake bodies keep their source HUD.
waw_revive_marker_visible( viewer, target )
{
    if ( !isdefined( viewer ) || !isdefined( target ) || viewer == target )
        return false;
    if ( !isalive( viewer ) || !isalive( target ) )
        return false;
    if ( !isdefined( viewer.sessionstate ) || viewer.sessionstate != "playing" )
        return false;
    if ( !isdefined( viewer.team ) || !isdefined( target.team ) || viewer.team != target.team )
        return false;
    return isdefined( target.laststand ) && target.laststand &&
        isdefined( target.revivetrigger );
}

waw_revive_waypoints()
{
    // Keep ownership on the level, so disconnected players do not abort
    // cleanup threads and leave HUD elements behind for their teammates.
    markers = [];
    for ( ;; )
    {
        active = [];
        for ( i = 0; i < markers.size; i++ )
        {
            marker = markers[i];
            if ( waw_revive_marker_visible( marker.viewer, marker.target ) )
                active[active.size] = marker;
            else if ( isdefined( marker.hud ) )
                marker.hud destroy();
        }
        markers = active;
        players = getplayers();
        for ( v = 0; v < players.size; v++ )
        {
            for ( t = 0; t < players.size; t++ )
            {
                viewer = players[v];
                target = players[t];
                if ( !waw_revive_marker_visible( viewer, target ) )
                    continue;
                found = false;
                for ( i = 0; i < markers.size; i++ )
                {
                    if ( markers[i].viewer == viewer && markers[i].target == target )
                    {
                        found = true;
                        break;
                    }
                }
                if ( found )
                    continue;
                marker = spawnstruct();
                marker.viewer = viewer;
                marker.target = target;
                marker.hud = newclienthudelem( viewer );
                marker.hud.alpha = 1;
                marker.hud.archived = 1;
                marker.hud.hidewheninmenu = 1;
                marker.hud.immunetodemogamehudsettings = 1;
                marker.hud setshader( "waypoint_revive", 32, 32 );
                marker.hud setwaypoint( 1 );
                markers[markers.size] = marker;
            }
        }
        for ( i = 0; i < markers.size; i++ )
        {
            marker = markers[i];
            marker.hud.x = marker.target.origin[0];
            marker.hud.y = marker.target.origin[1];
            marker.hud.z = marker.target.origin[2] + 40;
        }
        wait 0.1;
    }
}

// WaW film and glow run in a full-screen HUD pass over BO2's resolved frame
// (glow.py): one material per vision, below every other HUD element.
vision_overlay_players()
{
    // players can connect before this runs (the host normally does)
    players = getplayers();
    for ( i = 0; i < players.size; i++ )
        players[i] thread vision_overlay_player();
    for ( ;; )
    {
        level waittill( "connected", player );
        player thread vision_overlay_player();
    }
}

vision_overlay_player()
{
    self endon( "disconnect" );
    if ( !isdefined( level.waw2bo2_vision_overlays ) || isdefined( self.waw2bo2_overlay ) )
        return;
    hud = newclienthudelem( self );
    hud.horzalign = "fullscreen";
    hud.vertalign = "fullscreen";
    hud.x = 0;
    hud.y = 0;
    hud.sort = -10000;
    hud.foreground = 0;
    hud.hidewheninmenu = 0;
    hud.alpha = 1;
    self.waw2bo2_overlay = hud;
    self vision_overlay_update();
}

vision_overlay_update()
{
    if ( !isdefined( self.waw2bo2_overlay ) )
        return;
    vision = level.waw2bo2_overlay_global;
    if ( isdefined( self.waw2bo2_overlay_vision ) )
        vision = self.waw2bo2_overlay_vision;
    if ( isdefined( self.waw2bo2_filmtweaks ) && self.waw2bo2_filmtweaks )
        vision = "_filmtweak";
    if ( !isdefined( level.waw2bo2_vision_overlays[vision] ) )
        vision = "_identity";
    self.waw2bo2_overlay setshader( level.waw2bo2_vision_overlays[vision], 640, 480 );
}

// WaW names resolve to translated rawfiles. T6's player API has a different name.
waw_visionsetnaked( vision, time )
{
    if ( !isdefined( time ) )
        time = 0;
    vision = tolower( vision );
    key = vision;
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
        self.waw2bo2_overlay_vision = undefined;
        if ( key != "" )
            self.waw2bo2_overlay_vision = key;
        self vision_overlay_update();
    }
    else
    {
        level.waw2bo2_overlay_global = "_identity";
        if ( key != "" )
            level.waw2bo2_overlay_global = key;
        players = getplayers();
        for ( i = 0; i < players.size; i++ )
            players[i] vision_overlay_update();
        maps\mp\_utility::setclientsysstate( "waw_vision_global", "" + index );
        if ( vision == "" )
            vision = "waw/_identity";
        visionsetnaked( vision, time );
    }
}

// _waw2bo2_precache precaches the scripts' named models in the first frame;
// a WaW script repeating it later (T6: "precacheModel must be called before
// any wait statements") is then skipped.
waw_precachemodel( name )
{
    if ( !isdefined( level.waw2bo2_precached ) )
        level.waw2bo2_precached = [];
    if ( isdefined( level.waw2bo2_precached[name] ) )
        return;
    level.waw2bo2_precached[name] = 1;
    precachemodel( name );
}

// ---- perks ----

// BO2 already initializes its controllers; source framework init calls in
// the explicit stock-perks mode must not start a second purchase controller.
stock_perks_noop()
{
}

stock_perks_owned_by_bo2( perk )
{
    // give_perk starts native perk_think and updates the native HUD once.
}

stock_perks_no_refund( trigger, perk, cost )
{
    // Opting into stock BO2 purchases also opts into its no-refund behavior.
}

stock_perks_no_money()
{
    self maps\mp\zombies\_zm_audio::create_and_play_dialog( "general", "perk_deny", undefined, 0 );
}
// A known absent source model must not terminate the rest of the source
// thread. Keep the current model; the converter reports the missing asset.
waw_missing_model( name )
{
    report( "XMODEL", name );
}

// WaW's perk table has names T6's lacks (measured from both executables;
// T6 SetPerk/HasPerk/UnsetPerk raise "Unknown perk"). Custom perk scripts use
// them as markers (specialty_boost = Electric Cherry, specialty_shades = Who's
// Who, specialty_altmelee = bowie): they carry no engine effect the scripts
// rely on, so the player keeps them in a script-side table instead.
waw_perk_emulated( perk )
{
    switch ( perk )
    {
        case "specialty_altmelee":
        case "specialty_boost":
        case "specialty_exposeenemy":
        case "specialty_fraggrenade":
        case "specialty_gas_mask":
        case "specialty_greased_barrings":
        case "specialty_leadfoot":
        case "specialty_null":
        case "specialty_ordinance":
        case "specialty_shades":
        case "specialty_specialgrenade":
        case "specialty_water_cooled":
        case "specialty_weapon_bazooka":
        case "specialty_weapon_bouncing_betty":
        case "specialty_weapon_flamethrower":
            return 1;
    }
    return 0;
}

waw_setperk( perk )
{
    if ( isdefined( level.waw2bo2_stock_perks ) && level.waw2bo2_stock_perks )
    {
        if ( !self hasperk( perk ) )
            self maps\mp\waw\_waw2bo2_perks::give_perk( perk, false );
        return;
    }
    if ( !waw_perk_emulated( perk ) )
    {
        self setperk( perk );
        return;
    }
    if ( !isdefined( self.waw2bo2_perks ) )
        self.waw2bo2_perks = [];
    self.waw2bo2_perks[perk] = 1;
}

waw_unsetperk( perk )
{
    if ( !waw_perk_emulated( perk ) )
        self unsetperk( perk );
    else if ( isdefined( self.waw2bo2_perks ) )
        self.waw2bo2_perks[perk] = undefined;
}

waw_hasperk( perk )
{
    if ( !waw_perk_emulated( perk ) )
        return self hasperk( perk );
    return isdefined( self.waw2bo2_perks ) && isdefined( self.waw2bo2_perks[perk] );
}

// ---- miscellaneous WaW builtins ----

waw_being_revived( player )
{
    // Source custom revive handlers can own this state too.
    if ( isdefined( player.being_revived ) && player.being_revived )
        return true;
    return isdefined( player.revivetrigger ) &&
        isdefined( player.revivetrigger.beingrevived ) &&
        player.revivetrigger.beingrevived;
}

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
