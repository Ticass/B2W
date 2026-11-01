// Optional BO2 Der Riese port fixes. Install in storage/t6/scripts/zm/.
// No imports from maps/mp/waw: safe to load globally on unrelated maps.
#include common_scripts\utility;

init()
{
    if ( getdvar( "mapname" ) != "zm_der_riese_waw" )
        return;
    level thread install_fixes();
}

install_fixes()
{
    // The map publishes these after custom-script init has started. Bound
    // the wait so an incompatible/older release fails closed.
    for ( i = 0; i < 200; i++ )
    {
        if ( isdefined( level.waw2bo2_perk_observer_api ) && isdefined( level.waw2bo2_health_setter ) && isdefined( level.waw_overrideplayerdamage )
            && isdefined( level.waw2bo2_source_player_laststand ) && isdefined( level.callbackplayerlaststand ) )
            break;
        wait 0.05;
    }
    if ( i == 200 )
    {
        println( "Der Riese casual fixes: incompatible map release; fixes disabled." );
        return;
    }
    if ( isdefined( level.casual_fixes_ready ) )
        return;
    level.casual_fixes_ready = true;
    level.casual_map_damage_override = level.waw_overrideplayerdamage;
    level.casual_map_laststand = level.waw2bo2_source_player_laststand;
    // Keep the source callback intact for ordinary/co-op downs. Replace the
    // T6 callback it is normally reached through, before the first down.
    replacefunc( level.callbackplayerlaststand, ::laststand_callback );
    level.waw_overrideplayerdamage = ::damage_override;
    // Equivalent to T4 g_fix_health_sets_max: health writes stop changing
    // maxhealth. Explicit maxhealth writes (Jug/loss of perks) still work.
    replacefunc( level.waw2bo2_health_setter, ::set_health );
    level.waw2bo2_perk_granted_callback = ::perk_granted;
    println( "Der Riese casual fixes: native solo revive and health setter fix enabled." );
}

perk_granted( perk )
{
    if ( perk != "specialty_quickrevive" || getplayers().size != 1 || !flag( "solo_game" ) )
        return;
    if ( !isdefined( level.casual_qr_buys ) )
        level.casual_qr_buys = 0;
    level.casual_qr_buys++;
    if ( level.casual_qr_buys != 3 )
        return;
    // Disable purchases immediately, but leave the third purchased perk on
    // the player. Unlink first: these WaW triggers may follow their machine.
    triggers = getentarray( "zombie_vending", "targetname" );
    foreach ( trigger in triggers )
    {
        if ( isdefined( trigger.script_noteworthy ) && trigger.script_noteworthy == "specialty_quickrevive" )
        {
            trigger unlink();
            trigger trigger_off();
        }
    }
    machines = getentarray( "vending_revive", "targetname" );
    foreach ( machine in machines )
    {
        machine notsolid();
        machine hide();
    }
}

set_health( value )
{
    self.health = value;
}

damage_override( inflictor, attacker, damage, flags, mod, weapon, point, dir, hitloc, modelindex, offset )
{
    if ( isdefined( self.casual_native_revive ) && self.casual_native_revive )
    {
        // Native downed players cannot take normal damage during solo revive.
        return;
    }
    if ( !isdefined( self.casual_revives_used ) )
        self.casual_revives_used = 0;
    if ( getplayers().size == 1 && flag( "solo_game" ) && self.casual_revives_used < 3
        && self hasperk( "specialty_quickrevive" ) && damage >= self.health
        && mod != "MOD_SUICIDE" && mod != "MOD_CRUSH"
        && !( isdefined( level.intermission ) && level.intermission ) )
    {
        self.lives = 3 - self.casual_revives_used;
        self.casual_revives_used++;
        self.casual_native_revive = true;
        self thread clear_revive_state();
        if ( !isdefined( offset ) )
            offset = 0;
        // Let T6 enter actual engine last stand. Do not emulate stance,
        // inventory, vision, or the revive with a timer.
        self finishplayerdamage( inflictor, attacker, damage, flags, mod, weapon, point, dir, hitloc, offset, 0 );
        return;
    }
    self [[ level.casual_map_damage_override ]]( inflictor, attacker, damage, flags, mod, weapon, point, dir, hitloc, modelindex, offset );
}

laststand_callback( inflictor, attacker, damage, mod, weapon, dir, hitloc, offset, duration )
{
    self endon( "disconnect" );
    if ( isdefined( self.casual_native_revive ) && self.casual_native_revive )
    {
        // Native playerlaststand calls level.playerlaststand_func, which
        // starts BO2 wait_and_revive, its grey vision and revive HUD. Native
        // auto_revive calls RevivePlayer and restores inventory/client state.
        self maps\mp\zombies\_zm_laststand::playerlaststand( inflictor, attacker, damage, mod, weapon, dir, hitloc, offset, duration );
        return;
    }
    self [[ level.casual_map_laststand ]]( inflictor, attacker, damage, mod, weapon, dir, hitloc, offset, duration );
}

clear_revive_state()
{
    self endon( "disconnect" );
    self waittill_any( "player_revived", "death", "spawned_player" );
    self.casual_native_revive = undefined;
}
