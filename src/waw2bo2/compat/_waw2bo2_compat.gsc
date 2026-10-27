// waw2bo2 compatibility layer: World at War script API on the BO2 zombies runtime.
//
// Generic for every converted WaW map. gscport.py rewrites references to WaW
// builtins that BO2 lacks (or implements differently) into calls of the waw_*
// functions below; nothing here names a map, an entity or an asset.
// Unsupported behaviour is reported once per API in the console
// ("WAW2BO2 UNSUPPORTED ..."), never hidden.
#include common_scripts\utility;
#include maps\mp\_utility;

waw_oktospawn()
{
    // T4 0x5175F0 admits fewer than four newly spawned entities and checks
    // its per-client snapshot bytes against 800. T6 owns a different packet
    // budget, unavailable to GSC. Conservatively cap script admissions at
    // four per server frame; never return an undefined gameplay predicate.
    frame = gettime();
    if ( !isdefined( level.waw_spawn_budget_frame ) || level.waw_spawn_budget_frame != frame )
    {
        level.waw_spawn_budget_frame = frame;
        level.waw_spawn_budget_requests = 0;
    }
    if ( level.waw_spawn_budget_requests >= 4 )
        return false;
    level.waw_spawn_budget_requests++;
    return true;
}

// WaW GetEnt indexes script_linkname. T6 loads the field but its native
// lookup returns undefined; scan live entities so switch/hinge links retain
// their authored identities, including repeated names for array queries.
waw_getentarray_linkname( value, key )
{
    matches = [];
    entities = getentarray();
    for ( i = 0; i < entities.size; i++ )
        if ( isdefined( entities[i].script_linkname ) && entities[i].script_linkname == value )
            matches[matches.size] = entities[i];
    return matches;
}

waw_getent_linkname( value, key )
{
    matches = waw_getentarray_linkname( value, key );
    if ( matches.size )
        return matches[0];
    return undefined;
}

init()
{
    if ( isdefined( level.waw2bo2_compat_ready ) )
        return;
    level.waw2bo2_compat_ready = 1;
    // Converted maps run on Plutonium T6. WaW scorebar libraries use this
    // flag to leave TAB's scoreboard to the native client.
    if ( !isdefined( level.isPlutonium ) )
        level.isPlutonium = 1;
    precacheshader( "waypoint_revive" );
    if ( !isdefined( level.get_player_weapon_limit ) )
        level.get_player_weapon_limit = ::waw_player_weapon_limit;
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
    if ( !isdefined( level.waw2bo2_bo2_weapons ) )
        level.waw2bo2_bo2_weapons = [];
    maps\mp\waw\_waw2bo2_assets::init();
    maps\mp\waw\_waw2bo2_hint_defaults::init();
    maps\mp\waw\_waw2bo2_bo2weapons::init();
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
    level thread waw_window_paths();
    level thread power_bridge();
}

// WaW cuts and restores a window's paths through its clip brush, which BO2's
// blocker_init deletes beside a zbarrier (the carpenter's windows.clip);
// waw_window_paths then follows the chunks instead.
// WaW IsNoTarget reads the entity's FL_NOTARGET flag, which only the notarget
// cheat sets; T6 script cannot read it. An undefined result is a runtime error
// in a T6 condition (it aborted the map's is_player_valid callers), so answer
// what WaW answers outside that cheat.
waw_isnotarget()
{
    return false;
}

waw_connectpaths()
{
    if ( isdefined( self ) )
        self connectpaths();
}

waw_disconnectpaths()
{
    if ( isdefined( self ) )
        self disconnectpaths();
}

// WaW keeps a boarded window shut to path-finding: its clip brush
// disconnects the window's traversal until every chunk is destroyed and again
// on the first repair (_zombiemode_blockers). T6's blocker_disconnect_paths
// and blocker_connect_paths are empty, and blocker_init deletes the clip
// beside a zbarrier, so zombies already in the playable area outside a window
// (Shi No Numa's swamp walkways) walked its traversal through all six boards
// (measured: find_flesh, zm_traverse_barrier, 6/6 closed). Same rule here.
// Measured in T6: unlinknodes leaves a negotiation link in place; disabling
// the inside end node cuts the traversal (findpath outside -> inside fails)
// while the begin node and the attack spots outside stay reachable. That end
// is the window's own node (paths.split_window_ends): T6 picks a disabled
// nearest node anyway, so disabling the shared hub failed every path to a
// player standing at the window.
waw_window_paths()
{
    level endon( "end_game" );
    while ( !isdefined( level.exterior_goals ) )
        wait 0.05;
    waittillframeend;
    for ( ;; )
    {
        foreach ( goal in level.exterior_goals )
        {
            if ( !isdefined( goal ) || !isdefined( goal.zbarrier ) || !isdefined( goal.neg_start ) || !isdefined( goal.neg_end ) )
                continue;
            open = maps\mp\zombies\_zm_utility::all_chunks_destroyed( goal, goal.barrier_chunks );
            if ( isdefined( goal.waw_path_open ) && goal.waw_path_open == open )
                continue;
            goal.waw_path_open = open;
            setenablenode( goal.neg_end, open );
        }
        wait 0.05;
    }
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

// WaW setSavedDvar sets a server dvar that is also sent to clients; T6 has
// no such builtin, and its server-side setdvar is the equivalent.
waw_setsaveddvar( dvar, value )
{
    setdvar( dvar, value );
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

waw_array_remove( array, value )
{
    result = [];
    for ( i = 0; i < array.size; i++ )
        if ( array[i] != value )
            result[result.size] = array[i];
    return result;
}

waw_contact_kill_damage( victim )
{
    return 0;
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
    // a window's clip: BO2's blocker_init deletes it beside a zbarrier
    if ( !isdefined( self ) )
        return;
    self maps\mp\zombies\_zm_utility::enable_trigger();
    if ( isdefined( self.classname ) && self.classname == "trigger_use" )
        self show();
}

// WaW replaces the client visibility mask with this one player. T6 only
// clears that player's hidden bit, retaining every previous visible player.
waw_setvisibletoplayer( player )
{
    self setinvisibletoall();
    self setvisibletoplayer( player );
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
    waw_clientnotify( "power_on" );
    if ( isdefined( level.flag["power_on"] ) )
        flag_set( "power_on" );
    level notify( "power_on" );
    level thread maps\mp\waw\_waw2bo2_perks::perk_unpause_all_perks();
}

// WaW maps (the DLC3 template's DLC3_threadCalls2 among them) start the zone
// manager themselves. The generated BO2 map main already runs
// _zm_zonemgr::manage_zones with the zones read from the map script; a second
// manager would double every zone's spawner bookkeeping.
waw_manage_zones( initial_zones )
{
}

// WaW's template installs the SP/coop engine callbacks here. T6's Zombies
// gametype already installs its own callbacks; replacing them would bypass
// _zm's player lifecycle and damage handling. Keep this entry point as a
// deliberate no-op. Map damage overrides use the converter's _zm hooks.
waw_setupcallbacks()
{
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

// WaW ragdolls a living actor, then kills it (StartRagdoll; LaunchRagdoll;
// DoDamage). T6 only ragdolls a dead actor: every stock caller (thundergun,
// springpad, riotshield) damages first, so a live call is lost and the kill
// plays the normal death animation. Defer ragdoll and launch until the death.
// A WaW ragdoll stops blocking players at once; drop the actor's player
// collision now, as BO2's zombie_ragdoll_then_explode does, so the frame
// before the deferred ragdoll does not leave an invisible wall.
waw_startragdoll( arg )
{
    if ( isai( self ) && isalive( self ) )
    {
        if ( !isdefined( self.waw_ragdoll_pending ) )
        {
            self.waw_ragdoll_pending = 1;
            self setplayercollision( 0 );
            self thread waw_ragdoll_after_death();
            self thread waw_ragdoll_doomed();
        }
        return;
    }
    if ( isdefined( arg ) )
        self startragdoll( arg );
    else
        self startragdoll();
}

waw_launchragdoll( force, bone )
{
    if ( isai( self ) && isalive( self ) )
    {
        self.waw_ragdoll_launch = force;
        self.waw_ragdoll_launch_bone = bone;
        self waw_startragdoll();
        return;
    }
    if ( isdefined( bone ) )
        self launchragdoll( force, bone );
    else
        self launchragdoll( force );
}

// A WaW ragdolled live actor is out of play at once: a limp body thrown by
// the launch until its script kills it (flogger and KF2 0.75 s, bridge 1 s,
// zipline and shield/baby-gun kicks in the same frame; the delayed kills all
// pass no attacker). T6 cannot ragdoll a living actor, so a zombie its script
// did not kill within the frame dies at frame end without an attacker, and
// its body takes the launch. A same-frame kill keeps its own attribution.
waw_ragdoll_doomed()
{
    self endon( "death" );
    waittillframeend;
    if ( isdefined( self ) && isalive( self ) )
        self dodamage( self.health + 666, self.origin );
}

// WaW _zombiemode_utility::enemy_is_dog tests self.type == "dog"; a T6
// zombie dog's type is "zombie_dog" (measured), and _zm_ai_dogs sets isdog.
// Without this the Tesla chain treats dogs as zombies and asks them for
// zombie bones (j_spineupper), which ends the arc thread.
waw_enemy_is_dog()
{
    if ( isdefined( self.isdog ) && self.isdog )
        return true;
    return isdefined( self.type ) && ( self.type == "dog" || self.type == "zombie_dog" );
}

// T6 dogs use BO2's zombie_wolf, whose skeleton differs from WaW's: WaW's
// Tesla arc asks the target for the source's bone (j_spineupper on a dog,
// j_spine1 on a zombie). A missing tag is a runtime error and leaves the
// arc's target position undefined, so that link of the chain never damages
// its target. A bone the model (or an attached model) lacks gives the origin.
waw_gettagorigin( tag )
{
    if ( !isdefined( self.model ) || self.model == "" || waw_model_has_tag( self.model, tag ) )
        return self gettagorigin( tag );
    for ( i = 0; i < self getattachsize(); i++ )
    {
        if ( waw_model_has_tag( self getattachmodelname( i ), tag ) )
            return self gettagorigin( tag );
    }
    return self.origin;
}

waw_model_has_tag( model, tag )
{
    if ( !isdefined( level.waw_model_tags ) )
        level.waw_model_tags = [];
    if ( !isdefined( level.waw_model_tags[model] ) )
    {
        tags = [];
        count = getnumparts( model );
        for ( i = 0; i < count; i++ )
            tags[tolower( getpartname( model, i ) )] = 1;
        level.waw_model_tags[model] = tags;
    }
    return isdefined( level.waw_model_tags[model][tolower( tag )] );
}

// WaW scripts damage their ragdolled actor again after a wait; T6 has
// already removed it at the earlier death (see waw_ragdoll_doomed).
waw_dodamage( amount, origin, attacker, inflictor, hitloc, mod, flags, weapon )
{
    if ( !isdefined( self ) )
        return;
    // WaW's Tesla takes the origin from the arc's source enemy, which a dog's
    // exploding death may already have removed; T6 dodamage needs a vector.
    if ( !isdefined( origin ) )
        origin = self.origin;
    if ( isdefined( weapon ) )
        self dodamage( amount, origin, attacker, inflictor, hitloc, mod, flags, weapon );
    else if ( isdefined( flags ) )
        self dodamage( amount, origin, attacker, inflictor, hitloc, mod, flags );
    else if ( isdefined( mod ) )
        self dodamage( amount, origin, attacker, inflictor, hitloc, mod );
    else if ( isdefined( hitloc ) )
        self dodamage( amount, origin, attacker, inflictor, hitloc );
    else if ( isdefined( inflictor ) )
        self dodamage( amount, origin, attacker, inflictor );
    else if ( isdefined( attacker ) )
        self dodamage( amount, origin, attacker );
    else
        self dodamage( amount, origin );
}

waw_ragdoll_after_death()
{
    self waittill( "death" );
    // Measured in game: StartRagdoll inside the death notify is ignored even
    // though IsAlive is already false; at the end of the frame it launches.
    waittillframeend;
    if ( !isdefined( self ) )
        return;
    self startragdoll();
    if ( !isdefined( self.waw_ragdoll_launch ) )
        return;
    if ( isdefined( self.waw_ragdoll_launch_bone ) )
        self launchragdoll( self.waw_ragdoll_launch, self.waw_ragdoll_launch_bone );
    else
        self launchragdoll( self.waw_ragdoll_launch );
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

// BO2 runtime copies (dog rounds) load the same paths WaW's scripts did; the
// map zone carries WaW's converted effect, not BO2's (a Tranzit asset).
waw_loadfx_native( name )
{
    if ( isdefined( level.waw2bo2_fx ) && isdefined( level.waw2bo2_fx[name] ) )
        return waw_loadfx( name );
    return loadfx( name );
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

waw_set_zombie_var( var, value, div, is_float )
{
    // WaW: fourth argument is a float flag, third is a divisor.
    // BO2's same-named function uses a float flag followed by a table column.
    // Only source table overrides may replace the authored default.
    if ( isdefined( level.waw_zombie_var_overrides ) && isdefined( level.waw_zombie_var_overrides[var] ) )
    {
        if ( isdefined( is_float ) && is_float )
            value = float( level.waw_zombie_var_overrides[var] );
        else
            value = int( level.waw_zombie_var_overrides[var] );
    }
    if ( isdefined( div ) )
        value = value / div;
    level.zombie_vars[var] = value;
}

// BO2 keeps some zombie vars per team and reads level.zombie_vars[team][var];
// the map's code keeps them global. Called after each source write of one.
waw_team_zombie_var( var )
{
    if ( !isdefined( level.teams ) )
        return;
    foreach ( team in level.teams )
    {
        if ( !isdefined( level.zombie_vars[team] ) )
            level.zombie_vars[team] = [];
        level.zombie_vars[team][var] = level.zombie_vars[var];
    }
}

// T6 MP's entity rope anchors are visual followers, not physical joints.
// Fixed-world source suspensions use an authoritative pendulum. Other models
// retain native physics. This does not replace general rigid-body collision.
// Script kicks give the suspended body the velocity the engine gives a free
// body of the same model: none measurable. Free copies of Peter's and the
// swamp lamp's models launched with forces up to 50000 (centred or 20 units
// off centre) only fell under gravity, never moving sideways. Adding the
// source's +-20 kicks as velocity (no mass, no damping) piled energy up until
// Peter spun around his rope.
waw_physicslaunch( point, impulse )
{
    definition = undefined;
    if ( isdefined( level.waw_rope_suspensions ) && isdefined( self.targetname ) )
        definition = level.waw_rope_suspensions[self.targetname];
    if ( !isdefined( definition ) )
    {
        if ( isdefined( point ) && isdefined( impulse ) )
            self physicslaunch( point, impulse );
        else
            self physicslaunch();
        return;
    }
    if ( !isdefined( self.waw_rope_motion ) )
    {
        state = spawnstruct();
        state.definition = definition;
        state.forward = anglestoforward( self.angles );
        state.up = anglestoup( self.angles );
        state.position = self.origin + state.forward * definition.point[0]
            - anglestoright( self.angles ) * definition.point[1] + state.up * definition.point[2];
        state.rest = vectornormalize( state.position - definition.anchor );
        state.velocity = ( 0, 0, 0 );
        self.waw_rope_motion = state;
        self thread waw_rope_motion();
    }
}

waw_rope_rotate( vector, axis, cosine )
{
    // Rodrigues' shortest rotation from the authored rope direction.
    return vector + vectorcross( axis, vector )
        + vectorcross( axis, vectorcross( axis, vector ) ) / max( 0.0001, 1 + cosine );
}

waw_rope_motion()
{
    self endon( "death" );
    state = self.waw_rope_motion;
    definition = state.definition;
    dt = 0.0125;
    for ( ;; )
    {
        for ( step = 0; step < 4; step++ )
        {
            gravity = getdvarfloat( "phys_gravity" );
            state.velocity += ( 0, 0, gravity ) * dt;
            if ( definition.damp > 0 )
                state.velocity *= max( 0, 1 - definition.damp * dt );
            state.position += state.velocity * dt;
            displacement = state.position - definition.anchor;
            distance = length( displacement );
            if ( distance > definition.length )
            {
                direction = displacement / distance;
                state.position = definition.anchor + direction * definition.length;
                // Remove outward velocity, retaining tangential motion.
                state.velocity -= direction * max( 0, vectordot( state.velocity, direction ) );
            }
        }
        direction = vectornormalize( state.position - definition.anchor );
        axis = vectorcross( state.rest, direction );
        cosine = vectordot( state.rest, direction );
        forward = waw_rope_rotate( state.forward, axis, cosine );
        up = waw_rope_rotate( state.up, axis, cosine );
        angles = vectortoangles( forward );
        roll = vectortoangles( ( vectordot( up, anglestoup( angles ) ),
            vectordot( up, anglestoright( angles ) ), 0 ) );
        angles = ( angles[0], angles[1], angleclamp180( roll[1] ) );
        local_point = definition.point;
        origin = state.position - anglestoforward( angles ) * local_point[0]
            + anglestoright( angles ) * local_point[1] - anglestoup( angles ) * local_point[2];
        self moveto( origin, 0.05 );
        self rotateto( angles, 0.05 );
        wait 0.05;
    }
}

waw_spawn( classname, origin, flags, radius, height )
{
    if ( isdefined( height ) )
        ent = spawn( classname, origin, flags, radius, height );
    else if ( isdefined( radius ) )
        ent = spawn( classname, origin, flags, radius );
    else if ( isdefined( flags ) )
        ent = spawn( classname, origin, flags );
    else
        ent = spawn( classname, origin );
    // T6's spawned radius volumes can overlap live AI without dispatching
    // the WaW AI trigger event. Keep native geometry and player contacts.
    if ( isdefined( ent ) && classname == "trigger_radius" && isdefined( flags ) && ( int( flags ) & 7 ) )
        ent thread waw_radius_ai_contacts( int( flags ) );
    return ent;
}

// Actors a WaW trigger's AI flags (1 axis, 2 allies, 4 neutral) respond to:
// every species of the team, dogs included. T6 getaiarray( team ) returns
// only the team's human species (measured: a zombie_dog is in
// getaispeciesarray( "axis" ) but not in getaiarray( "axis" )), so dogs
// never set off a Bouncing Betty.
waw_trigger_ai( flags )
{
    candidates = [];
    if ( flags & 1 )
        candidates = arraycombine( candidates, getaispeciesarray( "axis", "all" ), 0, 0 );
    if ( flags & 2 )
        candidates = arraycombine( candidates, getaispeciesarray( "allies", "all" ), 0, 0 );
    if ( flags & 4 )
        candidates = arraycombine( candidates, getaispeciesarray( "neutral", "all" ), 0, 0 );
    return candidates;
}

waw_radius_ai_contacts( flags )
{
    self endon( "death" );
    for ( ;; )
    {
        // Allow the source caller to begin its waittill after spawn returns.
        wait 0.05;
        candidates = waw_trigger_ai( flags );
        foreach ( actor in candidates )
            if ( isdefined( actor ) && isalive( actor ) && actor istouching( self ) )
                self notify( "trigger", actor );
    }
}

waw_playfxontag( fx, ent, tag )
{
    if ( !isdefined( fx ) || !isdefined( ent ) )
        return;
    // WaW plays on "tag_origin" of models without that bone (the power-ups'
    // glow on zombie_bomb, whose only bone is fatman); in T6 a missing tag is
    // a runtime error. Play on a tag_origin carrier riding the entity instead.
    if ( isdefined( tag ) && isdefined( ent.model ) && ent.model != "" && !waw_entity_has_tag( ent, tag ) )
    {
        ent = waw_tag_carrier( ent );
        tag = "tag_origin";
    }
    if ( isdefined( level.waw2bo2_bolted_loops ) && isdefined( level.waw2bo2_bolted_loops[fx] ) )
    {
        // T6's transient PlayFXOnTag event cannot own an indefinitely looping
        // source effect. A script FX entity gives the bolt an explicit lifetime.
        carrier = spawnfx( fx, ent gettagorigin( tag ), anglestoforward( ent.angles ), anglestoup( ent.angles ) );
        carrier.waw_fx = fx;
        triggerfx( carrier );
        level thread waw_bolted_fx_lifetime( carrier, ent, tag );
        return carrier;
    }
    playfxontag( fx, ent, tag );
}

waw_entity_has_tag( ent, tag )
{
    if ( waw_model_has_tag( ent.model, tag ) )
        return 1;
    for ( i = 0; i < ent getattachsize(); i++ )
    {
        if ( waw_model_has_tag( ent getattachmodelname( i ), tag ) )
            return 1;
    }
    return 0;
}

waw_tag_carrier( ent )
{
    if ( isdefined( ent.waw_tag_carrier ) )
        return ent.waw_tag_carrier;
    carrier = spawn( "script_model", ent.origin );
    carrier.angles = ent.angles;
    carrier setmodel( "tag_origin" );
    carrier linkto( ent );
    ent.waw_tag_carrier = carrier;
    level thread waw_tag_carrier_lifetime( carrier, ent );
    return carrier;
}

waw_tag_carrier_lifetime( carrier, ent )
{
    while ( isdefined( ent ) )
        wait 0.1;
    if ( isdefined( carrier ) )
        carrier delete();
}

waw_bolted_fx_lifetime( carrier, parent, tag )
{
    // A triggered FX entity keeps playing where it was triggered: moving its
    // origin leaves the effect behind (the Wunderwaffe's travelling bolt
    // between zombies, the mystery box sky marker). Once the parent moves,
    // the effect rides the parent's tag natively, as BO2's moving effects do,
    // and ends with the parent.
    start = carrier.origin;
    while ( isdefined( parent ) && isdefined( carrier ) )
    {
        if ( distancesquared( parent gettagorigin( tag ), start ) > 1 )
        {
            fx = carrier.waw_fx;
            carrier delete();
            playfxontag( fx, parent, tag );
            return;
        }
        wait 0.05;
    }
    if ( isdefined( carrier ) )
        carrier delete();
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

// WaW PrecacheString with an undefined string precaches nothing and the
// caller goes on (School registers most powerups with no hint); in T6 it is a
// runtime error that ended the map's init_powerups
waw_precachestring( text )
{
    if ( isdefined( text ) )
        precachestring( text );
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
    // level.zombie_sounds holds the map's aliases already bound (waw/music_box)
    if ( getsubstr( alias, 0, 4 ) == "waw/" && soundexists( alias ) )
        return alias;
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
    {
        // Nothing plays, so nothing ends: still end it, or a waiter holds its
        // lock for good (one missing quote line silenced every later quote
        // behind level.player_is_speaking).
        if ( isdefined( notifyname ) )
            self thread waw_sound_missing( notifyname );
        return;
    }
    if ( isdefined( notifyname ) )
    {
        self playsoundwithnotify( name, notifyname );
        self thread waw_sound_wait( notifyname );
    }
    else
        self playsound( name );
}

waw_sound_missing( notifyname )
{
    self endon( "death" );
    waittillframeend;
    self notify( notifyname );
}

// WaW's IsWaitingOnSound: the entity still has a sound playing whose notify
// has not fired (maps\_utility::delete_on_death_wait_sound waits for it
// before deleting the entity). T6 has no such builtin.
waw_sound_wait( notifyname )
{
    self endon( "death" );
    if ( !isdefined( self.waw2bo2_sounds_waiting ) )
        self.waw2bo2_sounds_waiting = 0;
    self.waw2bo2_sounds_waiting++;
    self waittill( notifyname );
    self.waw2bo2_sounds_waiting--;
}

waw_iswaitingonsound()
{
    return isdefined( self.waw2bo2_sounds_waiting ) && self.waw2bo2_sounds_waiting > 0;
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

waw_get_number_variants( prefix )
{
    for ( i = 0; i < 100; i++ )
        if ( !waw_soundexists( prefix + "_" + i ) )
            return i;
    return 100;
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
// neither table may be BO2's own (the BO2 framework can hand one to a WaW
// script): it passes through when level.waw2bo2_bo2_weapons (generated in
// _waw2bo2_bo2weapons from the linked mod.ff) lists it. T6 fails on an
// unknown weapon, so any other weapon (an uncarried WaW weapon, or one a mod
// script names but the map does not ship) is reported and the call skipped.

waw_weapon( name )
{
    // WaW's "none" is weapon 0, an empty def (clip 0, class "none"); T6
    // weapon queries reject the name ("Weapon name "none" is not valid")
    if ( !isdefined( name ) || name == "none" )
        return undefined;
    if ( isdefined( level.waw2bo2_weapons ) && isdefined( level.waw2bo2_weapons[name] ) )
    {
        if ( level.waw2bo2_weapons[name] == "" )
        {
            report( "WEAPON", name );
            return undefined;
        }
        return level.waw2bo2_weapons[name];
    }
    // an empty table: the build did not record mod.ff's weapons (lowercase
    // keys: the engine matches weapon names case-insensitively)
    if ( isstring( name ) && isdefined( level.waw2bo2_bo2_weapons ) && level.waw2bo2_bo2_weapons.size && !isdefined( level.waw2bo2_bo2_weapons[tolower( name )] ) )
    {
        report( "WEAPON", name );
        return undefined;
    }
    return name;
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

waw_weaponmaxammo( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return 0;
    return weaponmaxammo( weapon );
}

waw_weaponstartammo( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return 0;
    return weaponstartammo( weapon );
}

waw_weaponfuellife( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return 0;
    return weaponfuellife( weapon );
}

waw_weaponfiretime( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return 0;
    return weaponfiretime( weapon );
}

waw_weaponreloadtime( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return 0;
    return weaponreloadtime( weapon );
}

waw_weaponissemiauto( name )
{
    weapon = waw_weapon( name );
    return isdefined( weapon ) && weaponissemiauto( weapon );
}

waw_weaponisboltaction( name )
{
    weapon = waw_weapon( name );
    return isdefined( weapon ) && weaponisboltaction( weapon );
}

waw_weaponisgasweapon( name )
{
    weapon = waw_weapon( name );
    return isdefined( weapon ) && weaponisgasweapon( weapon );
}

waw_isweaponcliponly( name )
{
    weapon = waw_weapon( name );
    return isdefined( weapon ) && isweaponcliponly( weapon );
}

waw_isweapondetonationtimed( name )
{
    weapon = waw_weapon( name );
    return isdefined( weapon ) && isweapondetonationtimed( weapon );
}

waw_weaponinventorytype( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return "none";
    return weaponinventorytype( weapon );
}

waw_weaponaltweaponname( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return "none";
    return waw_weapon_name( weaponaltweaponname( weapon ) );
}

waw_weapondualwieldweaponname( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return "none";
    return waw_weapon_name( weapondualwieldweaponname( weapon ) );
}

waw_givestartammo( name )
{
    weapon = waw_weapon( name );
    if ( isdefined( weapon ) )
        self givestartammo( weapon );
}

waw_getfractionstartammo( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return 0;
    return self getfractionstartammo( weapon );
}

waw_getfractionmaxammo( name )
{
    weapon = waw_weapon( name );
    if ( !isdefined( weapon ) )
        return 0;
    return self getfractionmaxammo( weapon );
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

// The Zombies helper owns its HUD count and uses raw Destroy, rather than
// the HUD utility's parent/child cleanup. Retain that helper on valid HUDs.
waw_destroy_hud( element )
{
    if ( isdefined( element ) )
        element maps\mp\zombies\_zm_utility::destroy_hud();
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
    // the map's own registry (its _zombiemode_weapons::include_zombie_weapon,
    // keyed by WaW name) for its box, wall buys and Pack-a-Punch
    if ( isdefined( level.waw_include_zombie_weapon ) )
        [[ level.waw_include_zombie_weapon ]]( name, in_box, weighting_func );
    // Native tactical helpers use a different inventory name. Precache and
    // include it for their startup guards, without a second mystery-box entry.
    if ( isdefined( level.waw2bo2_runtime_weapons ) && isdefined( level.waw2bo2_runtime_weapons[name] ) )
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
    if ( isdefined( hint ) && ( !isstring( hint ) || hint != "" ) )
        report( "WEAPON_HINT (prompt built from display name + cost)", name );
    hint = &"ZOMBIE_WEAPONCOSTONLY";
    if ( !isdefined( weaponvo ) )
        weaponvo = "";
    maps\mp\zombies\_zm_weapons::add_zombie_weapon( weapon, upgrade, hint, cost, weaponvo, "", ammo_cost );
    // This must precede the native tactical initializer: it checks
    // level.zombie_weapons before registering its script-model animtree.
    if ( isdefined( level.waw2bo2_runtime_weapons ) && isdefined( level.waw2bo2_runtime_weapons[name] ) )
    {
        maps\mp\zombies\_zm_weapons::add_zombie_weapon( level.waw2bo2_runtime_weapons[name], undefined, hint, cost, weaponvo, "", ammo_cost );
        waw_native_only_weapon( level.waw2bo2_runtime_weapons[name] );
    }
}

// WaW's opaque pause menu covered script HUD; T6's draws under it. WaW HUD
// elements therefore hide in menus (gscport lower_hud_fields does the same
// for explicit hidewheninmenu assignments).
waw_create_simple_hud( client )
{
    hud = maps\mp\zombies\_zm_utility::create_simple_hud( client );
    hud.hidewheninmenu = 1;
    return hud;
}

waw_newhudelem()
{
    hud = newhudelem();
    hud.hidewheninmenu = 1;
    return hud;
}

waw_newclienthudelem( client )
{
    hud = newclienthudelem( client );
    hud.hidewheninmenu = 1;
    return hud;
}

// The map's effect-only client listeners run on the server (gscport
// client_fx_listeners) and wait for "waw_client_" + msg, a server notify the
// native clientnotify never raises.
waw_clientnotify( msg )
{
    clientnotify( msg );
    level notify( "waw_client_" + msg );
}

// A server copy of a client listener spawns its effect once, for everyone.
waw_server_local_players()
{
    players = [];
    players[0] = level;
    return players;
}

// Client script calls in a server copy of a client listener (gsc_api
// client_listener_calls); the local client number is already dropped.
// WaW CSC Spawn( client, origin, classname ) creates a client entity; the
// server entity is networked to every client instead.
waw_client_spawn( origin, classname )
{
    if ( !isdefined( classname ) )
        classname = "script_model";
    return spawn( classname, origin );
}

// A CSC "fake entity" is a client-side sound source placed with
// SetFakeEntOrg; on the server it is a script_origin playing the alias.
waw_client_spawnfakeent()
{
    return spawn( "script_origin", ( 0, 0, 0 ) );
}

waw_client_setfakeentorg( ent, origin )
{
    if ( isdefined( ent ) )
        ent.origin = origin;
}

waw_client_deletefakeent( ent )
{
    if ( isdefined( ent ) )
        ent delete();
}

waw_client_playloopsound( ent, alias, fadetime )
{
    if ( isdefined( ent ) )
        ent waw_playloopsound( alias, fadetime );
}

waw_client_stoploopsound( ent, fadetime )
{
    if ( !isdefined( ent ) )
        return;
    if ( isdefined( fadetime ) )
        ent stoploopsound( fadetime );
    else
        ent stoploopsound();
}

// WaW drops a power-up only where it lands inside a "playable_area" trigger
// (_zombiemode_powerups::powerup_drop). BO2 checks its zone volumes instead,
// which converted maps synthesize around spawner groups, reaching outside.
waw_powerup_drop( drop_point )
{
    areas = getentarray( "playable_area", "targetname" );
    if ( areas.size )
    {
        probe = spawn( "script_model", drop_point + ( 0, 0, 40 ) );
        valid = 0;
        for ( i = 0; i < areas.size; i++ )
        {
            if ( probe istouching( areas[i] ) )
                valid = 1;
        }
        probe delete();
        if ( !valid )
            return;
    }
    maps\mp\zombies\_zm_powerups::powerup_drop( drop_point );
}

// The map's GetArrayKeys( level.zombie_weapons ): its own registry is
// level.waw_zombie_weapons (gscport.WAW_LEVEL_FIELDS); BO2's level.zombie_weapons
// holds BO2's names, which the map's box cannot show or give. Entries only
// BO2 needs (its framework last-stand pistol, native tactical helpers, the
// equipped alt melee) stay out of the map's key lists (the box chooser).
waw_native_only_weapon( name )
{
    if ( !isdefined( level.waw2bo2_native_only_weapons ) )
        level.waw2bo2_native_only_weapons = [];
    level.waw2bo2_native_only_weapons[name] = 1;
}

waw_zombie_weapon_keys()
{
    if ( !isdefined( level.waw_zombie_weapons ) )
        return [];
    keys = getarraykeys( level.waw_zombie_weapons );
    if ( !isdefined( level.waw2bo2_native_only_weapons ) )
        return keys;
    out = [];
    for ( i = 0; i < keys.size; i++ )
    {
        if ( !isdefined( level.waw2bo2_native_only_weapons[keys[i]] ) )
            out[out.size] = keys[i];
    }
    return out;
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
        self thread waw_inventory_size();
    }
    // WaW sets score_total when a player connects (_zombiemode::onPlayerConnect),
    // BO2 only at spawn; the map's power-up drop watcher sums it meanwhile
    if ( !isdefined( self.score_total ) )
    {
        self.score_total = 0;
        if ( isdefined( self.score ) )
            self.score_total = self.score;
    }
    if ( isdefined( self.stats ) )
        return;
    self.stats = [];
    self.stats["kills"] = 0;
    self.stats["score"] = 0;
    self.stats["downs"] = 0;
    self.stats["revives"] = 0;
    self.stats["perks"] = 0;
    self.stats["headshots"] = 0;
    self.stats["zombie_gibs"] = 0;
}

// WaW Mule Kick packs keep self.InventorySize (2, or 3 with the perk) for the
// map's own box and weapon scripts to read. With BO2 stock perks that pack no
// longer runs: BO2's Mule Kick decides the limit, as for BO2's wall buys.
waw_inventory_size()
{
    self endon( "disconnect" );
    waittillframeend;
    if ( !( isdefined( level.waw2bo2_stock_perks ) && level.waw2bo2_stock_perks ) )
        return;
    for ( ;; )
    {
        self.inventorysize = bo2_weapon_limit( self );
        wait 0.1;
    }
}

bo2_weapon_limit( player )
{
    if ( player hasperk( "specialty_additionalprimaryweapon" ) && isdefined( level.additionalprimaryweapon_limit ) )
        return level.additionalprimaryweapon_limit;
    return 2;
}

// BO2's wall buys, box and too-many-weapons monitor only count BO2's Mule
// Kick. A WaW Mule Kick (its own perk, specialty_extraammo in community
// packs) raises self.InventorySize instead: honor whichever is higher.
waw_player_weapon_limit( player )
{
    limit = bo2_weapon_limit( player );
    if ( isdefined( player.inventorysize ) && player.inventorysize > limit )
        return player.inventorysize;
    return limit;
}

// WaW's framework maintains these session counters in stats; T6 keeps
// them in pers. Reads from converted scripts use the native live values.
waw_player_stat( player, key )
{
    native_key = key;
    if ( key == "perks" )
        native_key = "perks_drank";
    else if ( key == "zombie_gibs" )
        native_key = "gibs";
    else if ( key == "score" )
    {
        if ( isdefined( player.score_total ) )
            return player.score_total;
        if ( isdefined( player.stats ) && isdefined( player.stats[key] ) )
            return player.stats[key];
        return 0;
    }
    if ( isdefined( player.pers ) && isdefined( player.pers[native_key] ) )
        return player.pers[native_key];
    if ( isdefined( player.stats ) && isdefined( player.stats[key] ) )
        return player.stats[key];
    return 0;
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
                // T6 draws a waypoint at waypointIconWidth * width / 8 (8 is
                // BO2's own objpoint size); WaW's native revive icon is the
                // plain 36-unit waypointIconWidth, so 8x8 matches it.
                marker.hud setshader( "waypoint_revive", 8, 8 );
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

// Maps use perk names T6's perk table lacks (T6 SetPerk/HasPerk/UnsetPerk
// raise "Unknown perk"): WaW-only ones (specialty_boost = Electric Cherry,
// specialty_shades = Who's Who, specialty_altmelee = bowie) and custom markers
// (School's specialty_marathon). They carry no engine effect the scripts rely
// on, so the player keeps them in a script-side table. Every name outside
// T6's table (the 60 specialty_ names in t6zm.exe) is emulated.
waw_perk_emulated( perk )
{
    switch ( perk )
    {
        case "specialty_additionalprimaryweapon":
        case "specialty_armorpiercing":
        case "specialty_armorvest":
        case "specialty_bulletaccuracy":
        case "specialty_bulletdamage":
        case "specialty_bulletflinch":
        case "specialty_bulletpenetration":
        case "specialty_deadshot":
        case "specialty_delayexplosive":
        case "specialty_detectexplosive":
        case "specialty_disarmexplosive":
        case "specialty_earnmoremomentum":
        case "specialty_explosivedamage":
        case "specialty_extraammo":
        case "specialty_fallheight":
        case "specialty_fastads":
        case "specialty_fastequipmentuse":
        case "specialty_fastladderclimb":
        case "specialty_fastmantle":
        case "specialty_fastmeleerecovery":
        case "specialty_fastreload":
        case "specialty_fastweaponswitch":
        case "specialty_finalstand":
        case "specialty_fireproof":
        case "specialty_flakjacket":
        case "specialty_flashprotection":
        case "specialty_gpsjammer":
        case "specialty_grenadepulldeath":
        case "specialty_healthregen":
        case "specialty_holdbreath":
        case "specialty_immunecounteruav":
        case "specialty_immuneemp":
        case "specialty_immunemms":
        case "specialty_immunenvthermal":
        case "specialty_immunerangefinder":
        case "specialty_killstreak":
        case "specialty_longersprint":
        case "specialty_loudenemies":
        case "specialty_marksman":
        case "specialty_movefaster":
        case "specialty_nokillstreakreticle":
        case "specialty_nomotionsensor":
        case "specialty_noname":
        case "specialty_nottargetedbyairsupport":
        case "specialty_nottargettedbysentry":
        case "specialty_pin_back":
        case "specialty_pistoldeath":
        case "specialty_proximityprotection":
        case "specialty_quickrevive":
        case "specialty_reconnaissance":
        case "specialty_rof":
        case "specialty_scavenger":
        case "specialty_showenemyequipment":
        case "specialty_showonradar":
        case "specialty_sprintrecovery":
        case "specialty_stalker":
        case "specialty_stunprotection":
        case "specialty_twogrenades":
        case "specialty_twoprimaries":
        case "specialty_unlimitedsprint":
            return 0;
    }
    return 1;
}

waw_setperk( perk )
{
    // With BO2 stock perks a perk the player does not own yet is BO2's
    // give_perk (HUD, effects, loss on down). WaW scripts also take an owned
    // perk away for a moment and give it back (d2p and galvaknuckles toggle
    // Speed Cola around their animations): that stays an engine toggle.
    if ( isdefined( level.waw2bo2_stock_perks ) && level.waw2bo2_stock_perks && !waw_perk_emulated( perk )
        && !( isdefined( self.perks_active ) && isinarray( self.perks_active, perk ) ) )
    {
        self maps\mp\waw\_waw2bo2_perks::give_perk( perk, false );
        return;
    }
    if ( !waw_perk_emulated( perk ) )
    {
        self setperk( perk );
        // WaW's native perk only speeds up reviving another player; a custom
        // map's own solo revive (if any) stays in its scripts. Only retail
        // maps get the BO-style solo life (stock_map_policies).
        if ( isdefined( level.waw2bo2_solo_revive ) && perk == level.waw2bo2_solo_revive["perk"]
            && flag( "solo_game" ) && !( isdefined( level.waw2bo2_stock_perks ) && level.waw2bo2_stock_perks ) )
            self waw_stock_solo_revive_bought();
        return;
    }
    if ( !isdefined( self.waw2bo2_perks ) )
        self.waw2bo2_perks = [];
    self.waw2bo2_perks[perk] = 1;
    if ( perk == "specialty_altmelee" )
        self waw_altmelee_weapon();
}

// WaW's engine reads specialty_altmelee to melee with the bowie (the map's
// _zombiemode_bowie only sets the perk). T6 melees with the player's melee
// weapon instead: equip BO2's bowie the way BO2's flourish end does
// (_zm_melee_weapon::do_melee_weapon_flourish_end).
waw_altmelee_weapon()
{
    weapon = "bowie_knife_zm";
    if ( self hasweapon( weapon ) )
        return;
    self giveweapon( weapon );
    self maps\mp\zombies\_zm_melee_weapon::change_melee_weapon( weapon, self getcurrentweapon() );
    if ( self hasweapon( "knife_zm" ) )
        self takeweapon( "knife_zm" );
}

// WaW's HUD netfield -86 sends round((scale - 1) * 10) in six bits.
// Its client decodes 1 + bits * 0.1; e.g. source scale 8 displays as 1.6.
waw_fontscale( scale )
{
    encoded = int( floor( ( scale - 1 ) * 10 + 0.5 ) );
    return 1 + ( encoded & 63 ) * 0.1;
}

waw_unsetperk( perk )
{
    if ( !waw_perk_emulated( perk ) )
        self unsetperk( perk );
    else if ( isdefined( self.waw2bo2_perks ) )
        self.waw2bo2_perks[perk] = undefined;
}

// WaW assigns .maxhealth (Jugg: 160). A T6 player's cap lives in its player
// state and only setmaxhealth changes it; AI keep the field.
waw_set_maxhealth( value )
{
    if ( isplayer( self ) )
        self setmaxhealth( value );
    else
        self.maxhealth = value;
}

// WaW BloodImpact( bool ); T6 takes "normal", "hero" or "none" and errors on
// anything else, which ended WaW's magic_bullet_shield before its health loop.
waw_bloodimpact( on )
{
    if ( isstring( on ) )
        self bloodimpact( on );
    else if ( on )
        self bloodimpact( "normal" );
    else
        self bloodimpact( "none" );
}

waw_hasperk( perk )
{
    if ( !waw_perk_emulated( perk ) )
        return self hasperk( perk );
    return isdefined( self.waw2bo2_perks ) && isdefined( self.waw2bo2_perks[perk] );
}

// WaW SetOrigin on a player is a teleport its client never interpolates
// (the teleporter's black room -> mainframe core); see waw_linkto.
waw_setorigin( origin )
{
    if ( isplayer( self ) )
        self dontinterpolate();
    self setorigin( origin );
}

// ---- linked trigger volumes ----

// WaW teleports players by linking them to a script_origin and assigning its
// origin in the same frame (Der Riese's black room); its client shows the jump.
// T6's client interpolates a linked player's move: the view slid ~2000 units
// through the walls in one snapshot. dontinterpolate marks this frame's move
// a teleport, as T6's own scripts do before moving players.
waw_linkto( parent, tag, origin, angles )
{
    if ( isplayer( self ) )
        self dontinterpolate();
    if ( isdefined( angles ) )
        self linkto( parent, tag, origin, angles );
    else if ( isdefined( origin ) )
        self linkto( parent, tag, origin );
    else if ( isdefined( tag ) )
        self linkto( parent, tag );
    else
        self linkto( parent );
    if ( isdefined( self.classname ) && self.classname == "trigger_multiple" )
        self thread waw_linked_trigger_contacts( parent );
}

// WaW tests a linked brush trigger against each entity box with the brush
// rotated by its parent. T6 carries the rotated volume (its absolute bounds
// follow the parent), but a fast-spinning one (Shi No Numa's flogger log,
// ~470 deg/s) misses most contacts its oriented box has with an actor's
// live bounds. While the parent rotates, entities the authored spawnflags
// admit (1 axis, 2 allies, 4 neutral AI, 8 no players) that the engine
// reports as not touching are tested against the oriented box; a contact
// sends the trigger notify WaW would have sent. A still volume is the engine's.
waw_linked_trigger_contacts( parent )
{
    self notify( "waw_linked_trigger_contacts" );
    self endon( "waw_linked_trigger_contacts" );
    self endon( "death" );
    parent endon( "death" );
    mins = self getmins();
    maxs = self getmaxs();
    offset = distance( self.origin, parent.origin );
    flags = 0;
    if ( isdefined( self.spawnflags ) )
        flags = int( self.spawnflags );
    last = self.angles;
    for ( ;; )
    {
        wait 0.05;
        angles = self.angles;
        if ( angles == last )
            continue;
        previous = last;
        last = angles;
        // trigger_off parks the volume far from its parent
        if ( abs( distance( self.origin, parent.origin ) - offset ) > 1000 )
            continue;
        candidates = waw_trigger_ai( flags );
        if ( !( flags & 8 ) )
            candidates = arraycombine( candidates, getplayers(), 0, 0 );
        // A fast rotor (the flogger, ~23 degrees a frame) can pass over an
        // actor between frames: test the angles swept since the last frame.
        steps = [];
        for ( s = 1; s <= 4; s++ )
            steps[steps.size] = waw_lerp_angles( previous, angles, s / 4 );
        foreach ( ent in candidates )
        {
            if ( !isdefined( ent ) || !isalive( ent ) || ent istouching( self ) )
                continue;
            lo = ent getabsmins();
            hi = ent getabsmaxs();
            if ( isai( ent ) )
            {
                // WaW actors are 72 units tall and 15 in radius; T6's zombie
                // bounds are 50 tall, missing hits WaW would register.
                lo = ent.origin + ( -15, -15, 0 );
                hi = ent.origin + ( 15, 15, 72 );
            }
            foreach ( step in steps )
            {
                if ( waw_box_touches_oriented( lo, hi, self.origin, anglestoforward( step ), anglestoright( step ),
                    anglestoup( step ), mins, maxs ) )
                {
                    self notify( "trigger", ent );
                    break;
                }
            }
        }
    }
}

waw_lerp_angles( from, to, t )
{
    out = [];
    for ( i = 0; i < 3; i++ )
    {
        delta = to[i] - from[i];
        while ( delta > 180 )
            delta -= 360;
        while ( delta < -180 )
            delta += 360;
        out[i] = from[i] + delta * t;
    }
    return ( out[0], out[1], out[2] );
}

waw_box_touches_oriented( lo, hi, origin, forward, right, up, mins, maxs )
{
    // A cheap reject on the oriented box's radius, then 27 samples of the
    // entity box (corners, edge/face midpoints, centre).
    centre = ( lo + hi ) * 0.5;
    reach = length( maxs - mins ) * 0.5 + length( hi - lo ) * 0.5;
    if ( distance( centre, origin + forward * ( ( mins[0] + maxs[0] ) * 0.5 )
        - right * ( ( mins[1] + maxs[1] ) * 0.5 ) + up * ( ( mins[2] + maxs[2] ) * 0.5 ) ) > reach )
        return false;
    for ( i = 0; i < 3; i++ )
    {
        for ( j = 0; j < 3; j++ )
        {
            for ( k = 0; k < 3; k++ )
            {
                point = ( lo[0] + ( hi[0] - lo[0] ) * i * 0.5, lo[1] + ( hi[1] - lo[1] ) * j * 0.5,
                    lo[2] + ( hi[2] - lo[2] ) * k * 0.5 );
                delta = point - origin;
                x = vectordot( delta, forward );
                y = 0 - vectordot( delta, right );
                z = vectordot( delta, up );
                if ( x >= mins[0] && x <= maxs[0] && y >= mins[1] && y <= maxs[1] && z >= mins[2] && z <= maxs[2] )
                    return true;
            }
        }
    }
    return false;
}

// A HUD material name built at run time ("hud_chalk_" + n): converted
// materials renamed out of BO2's names resolve through the generated table.
waw_hud_material( name )
{
    if ( !isdefined( level.waw2bo2_hud_materials ) )
        level.waw2bo2_hud_materials = maps\mp\waw\_waw2bo2_hud_materials::table();
    if ( isdefined( name ) && isdefined( level.waw2bo2_hud_materials[name] ) )
        return level.waw2bo2_hud_materials[name];
    return name;
}

// ---- WaW dog rounds (gsc_api zombie_runtime dog_rounds) ----

// BO2's _zm_ai_dogs runs the rounds; the source keeps spawn selection and health.
waw_dog_rounds( spawn_logic, source_health, enable_rounds )
{
    if ( isdefined( spawn_logic ) )
    {
        level.waw_dog_spawn_logic = spawn_logic;
        level.dog_spawn_func = ::waw_dog_spawn_location;
    }
    if ( isdefined( source_health ) && isdefined( level.dog_spawners ) )
    {
        level.waw_dog_source_health = source_health;
        // after BO2's dog_init, which set the native table's health
        array_thread( level.dog_spawners, maps\mp\zombies\_zm_utility::add_spawn_function, ::waw_dog_source_health );
    }
    [[ enable_rounds ]]();
}

// WaW picks among the dog spawners the map has opened (add_area_dog_spawners
// fills level.enemy_dog_spawns as areas unlock), not every dog spawner.
// Ported WaW code reads that list as level.waw_enemy_dog_spawns.
waw_dog_spawn_location( spawners, favorite_enemy )
{
    if ( isdefined( level.waw_enemy_dog_spawns ) && level.waw_enemy_dog_spawns.size > 0 )
        spawners = level.waw_enemy_dog_spawns;
    return [[ level.waw_dog_spawn_logic ]]( spawners, favorite_enemy );
}

waw_dog_source_health()
{
    [[ level.waw_dog_source_health ]]();
    self.maxhealth = level.dog_health;
    self.health = level.dog_health;
}

// ---- WaW framework calls with WaW semantics ----

// A script calling WaW's _zombiemode::player_damage_override directly (Shi
// No Numa's flogger in solo) asks "does this hit down the player?". WaW's
// stock body never takes health: a hit below current health only marks the
// attacker and returns; otherwise the player goes down. BO2's function of
// that name is its damage callback (it needs a weapon name and returns the
// damage to apply), so a downing hit goes through native damage instead,
// letting BO2's last stand, solo revive and game over decide the rest.
waw_player_damage_override( einflictor, eattacker, idamage, idflags, smeansofdeath, sweapon, vpoint, vdir, shitloc, modelindex, psoffsettime )
{
    if ( !isdefined( idamage ) || idamage < self.health )
    {
        if ( isdefined( eattacker ) )
            eattacker.sound_damage_player = self;
        return;
    }
    if ( self maps\mp\zombies\_zm_laststand::player_is_in_laststand() )
        return;
    origin = self.origin;
    if ( isdefined( vpoint ) )
        origin = vpoint;
    self dodamage( self.health + 100, origin );
}

// ---- stock WaW map policies (gscport stock_map_policies; never custom maps) ----

waw_stock_perk_cost( perk, cost )
{
    policy = level.waw2bo2_solo_revive;
    if ( isdefined( policy ) && perk == policy["perk"] && flag( "solo_game" ) )
        return policy["cost"];
    return cost;
}

// BO-style solo Quick Revive on retail WaW maps: the machine's hint shows
// the solo price, each purchase registers the one life BO2's last stand
// spends on a self revive, and after the limit the machine stops selling.
waw_stock_solo_revive()
{
    policy = level.waw2bo2_solo_revive;
    flag_wait( "initial_players_connected" );
    if ( !flag( "solo_game" ) )
        return;
    if ( !isdefined( level.solo_lives_given ) )
        level.solo_lives_given = 0;
    machines = getentarray( "zombie_vending", "targetname" );
    foreach ( machine in machines )
        if ( isdefined( machine.script_noteworthy ) && machine.script_noteworthy == policy["perk"] )
            machine thread waw_stock_solo_revive_machine( policy );
}

waw_stock_solo_revive_machine( policy )
{
    self endon( "death" );
    level endon( "waw2bo2_solo_revive_spent" );
    self thread waw_stock_solo_revive_spent();
    for ( ;; )
    {
        // after the source's own power-on hint
        level waittill( policy["perk"] + "_power_on" );
        waittillframeend;
        if ( level.solo_lives_given < policy["limit"] )
            self sethintstring( policy["hint"], policy["cost"] );
    }
}

waw_stock_solo_revive_spent()
{
    self endon( "death" );
    level waittill( "waw2bo2_solo_revive_spent" );
    self sethintstring( "" );
    // trigger_off moves the trigger 10000 units down, which a linked trigger
    // ignores: Shi No Numa links each vending trigger to the script_origin
    // its random spot moves (nazi_zombie_sumpf_perks), so the machine kept
    // selling. Unlink first; the source never moves it again after that.
    self unlink();
    self trigger_off();
}

waw_stock_solo_revive_bought()
{
    self.lives = 1;
    level.solo_lives_given++;
    if ( level.solo_lives_given >= level.waw2bo2_solo_revive["limit"] )
        level notify( "waw2bo2_solo_revive_spent" );
}

// ---- miscellaneous WaW builtins ----

// The map's own string tables (its zones and patch: mp/zombiemode.csv holds
// costs and zombie vars) ship as waw/<path>; WaW code reads them, never BO2's
// table of the same name (level.waw2bo2_tables, _waw2bo2_assets).
waw_table( table )
{
    if ( isdefined( level.waw2bo2_tables ) && isdefined( table ) && isdefined( level.waw2bo2_tables[tolower( table )] ) )
        return level.waw2bo2_tables[tolower( table )];
    return table;
}

waw_tablelookup( table, search_column, search_value, return_column )
{
    return tablelookup( waw_table( table ), search_column, search_value, return_column );
}

waw_tablelookupistring( table, search_column, search_value, return_column )
{
    return tablelookupistring( waw_table( table ), search_column, search_value, return_column );
}

// The native AI's point awards (owned _waw2bo2_spawner) go to the map's own
// _zombiemode_score::player_add_points for the events WaW scores: a kill and
// a hit (WaW has one hit event; BO2's light-hit split is its own). Events WaW
// never scores (BO2 barrier rebuild, power-up grants) stay BO2's.
waw_player_add_points( event, mod, hit_location, is_dog, zombie_team, damage_weapon )
{
    if ( isdefined( level.waw_player_add_points ) )
    {
        if ( event == "death" || event == "damage" || event == "damage_ads" )
        {
            self [[ level.waw_player_add_points ]]( event, mod, hit_location, is_dog );
            return;
        }
        if ( event == "damage_light" )
        {
            self [[ level.waw_player_add_points ]]( "damage", mod, hit_location, is_dog );
            return;
        }
    }
    self maps\mp\zombies\_zm_score::player_add_points( event, mod, hit_location, is_dog, zombie_team, damage_weapon );
}

// WaW's isdefined( player.is_drinking ) on BO2's drink counter (0 = idle).
waw_is_drinking( player )
{
    return isdefined( player.is_drinking ) && player.is_drinking > 0;
}

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

// WaW animscripts\dog_init fills anim.dogAnims, the tables WaW's raw-animation
// dog scripts play. A T6 dog plays its animation state machine instead (the
// traversal table is converted by traversals.py), so nothing reads them.
waw_dog_animation_tables()
{
}

waw_dog_animation_set( animset )
{
}

// WaW bbPrint( format, args... ) writes a telemetry line; T6's bbprint takes
// ( table, format, args... ), so a WaW call hands T6 a number as its format.
// Telemetry only (no gameplay): the source loop's call must not stop it.
waw_bbprint( format, a, b, c, d, e, f, g )
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
