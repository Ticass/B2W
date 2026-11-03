// Optional fixes (compat/waw_fixes.json): console dvars a player turns on by
// hand, off by default so the map plays as its WaW code does. Each fix reads
// its dvar when its hook runs, so it can be toggled mid-game.
#include common_scripts\utility;

// The manifest's dvars, registered so the console accepts "g_fix_... 1".
// A value the player set before the map loaded is kept.
register()
{
    names = [];
    names[names.size] = "g_fix_tesla_bug";
    names[names.size] = "g_fix_last_stand";
    foreach ( name in names )
    {
        if ( getdvar( name ) == "" )
            setdvar( name, 0 );
    }
    level.waw2bo2_perk_granted_callback = ::perk_granted;
}

enabled( name )
{
    return getdvarint( name ) == 1;
}

// g_fix_last_stand. Called where WaW registers player damage, after the map's
// framework set its damage override (_zombiemode::main).
wrap_player_damage()
{
    if ( !isdefined( level.waw_overrideplayerdamage ) || isdefined( level.waw2bo2_fix_map_damage ) )
        return;
    level.waw2bo2_fix_map_damage = level.waw_overrideplayerdamage;
    level.waw_overrideplayerdamage = ::player_damage;
}

player_damage( inflictor, attacker, damage, flags, mod, weapon, point, dir, hitloc, modelindex, offset )
{
    // a player in the fix's native last stand takes no damage until revived
    if ( native_laststand() )
        return;
    if ( enabled( "g_fix_last_stand" ) && self solo_revive_available( damage, mod ) )
    {
        if ( !isdefined( self.waw2bo2_fix_revives ) )
            self.waw2bo2_fix_revives = 0;
        self.lives = 3 - self.waw2bo2_fix_revives;
        self.waw2bo2_fix_revives++;
        self.waw2bo2_fix_native_laststand = true;
        self thread clear_native_laststand();
        if ( !isdefined( offset ) )
            offset = 0;
        // T6 enters its engine last stand (callback_playerlaststand glue);
        // stance, inventory, vision and the revive are T6's own.
        self finishplayerdamage( inflictor, attacker, damage, flags, mod, weapon, point, dir, hitloc, offset, 0 );
        return;
    }
    self [[ level.waw2bo2_fix_map_damage ]]( inflictor, attacker, damage, flags, mod, weapon, point, dir, hitloc, modelindex, offset );
}

solo_revive_available( damage, mod )
{
    if ( getplayers().size != 1 || !flag( "solo_game" ) || damage < self.health )
        return false;
    if ( isdefined( self.waw2bo2_fix_revives ) && self.waw2bo2_fix_revives >= 3 )
        return false;
    if ( mod == "MOD_SUICIDE" || mod == "MOD_CRUSH" || ( isdefined( level.intermission ) && level.intermission ) )
        return false;
    return self hasperk( "specialty_quickrevive" );
}

// The owned callback_playerlaststand glue sends this down to T6's
// _zm_laststand::playerlaststand instead of the map's Callback_PlayerLastStand.
native_laststand()
{
    return isdefined( self.waw2bo2_fix_native_laststand ) && self.waw2bo2_fix_native_laststand;
}

clear_native_laststand()
{
    self endon( "disconnect" );
    self waittill_any( "player_revived", "death", "spawned_player" );
    self.waw2bo2_fix_native_laststand = undefined;
}

// compat waw_setperk calls this when a player gains a perk.
perk_granted( perk )
{
    if ( perk != "specialty_quickrevive" || !enabled( "g_fix_last_stand" ) || getplayers().size != 1 || !flag( "solo_game" ) )
        return;
    if ( !isdefined( level.waw2bo2_fix_qr_purchases ) )
        level.waw2bo2_fix_qr_purchases = 0;
    level.waw2bo2_fix_qr_purchases++;
    if ( level.waw2bo2_fix_qr_purchases != 3 )
        return;
    // The third purchase stays on the player; the machine stops selling.
    // Unlink first: WaW perk triggers may follow their machine.
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
