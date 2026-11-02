// Passive diagnostics for the reported 1-HP/no-regeneration bug.
// No converter imports or gameplay writes; safe in the global scripts folder.
init()
{
    if ( getdvar( "mapname" ) != "zm_der_riese_waw" )
        return;
    level thread watch_players();
}

value( x )
{
    if ( !isdefined( x ) )
        return "undefined";
    return "" + x;
}

watch_players()
{
    for ( ;; )
    {
        foreach ( player in getplayers() )
        {
            if ( isdefined( player.casual_health_audit_started ) )
                continue;
            player.casual_health_audit_started = true;
            player thread watch_damage();
            player thread watch_health();
        }
        wait 1;
    }
}

watch_damage()
{
    self endon( "disconnect" );
    self.casual_health_last_damage_time = gettime();
    self.casual_health_last_damage = "none";
    for ( ;; )
    {
        self waittill( "damage", amount, attacker, dir, point, mod );
        self.casual_health_last_damage_time = gettime();
        self.casual_health_last_damage = "amount=" + value( amount ) + " mod=" + value( mod );
        if ( isdefined( attacker ) && isplayer( attacker ) )
            self.casual_health_last_damage += " attacker_weapon=" + attacker getcurrentweapon();
    }
}

downed()
{
    return ( isdefined( self.laststand ) && self.laststand )
        || isdefined( self.revivetrigger )
        || ( isdefined( self.casual_native_revive ) && self.casual_native_revive );
}

snapshot()
{
    return "t=" + gettime() + " hp=" + self.health + "/" + self.maxhealth
        + " session=" + value( self.sessionstate )
        + " down=" + self downed()
        + " jug=" + self hasperk( "specialty_armorvest" )
        + " qr=" + self hasperk( "specialty_quickrevive" )
        + " casual=" + value( level.casual_fixes_ready )
        + " revives_used=" + value( self.casual_revives_used )
        + " damage=" + value( self.casual_health_last_damage );
}

watch_health()
{
    self endon( "disconnect" );
    history = [];
    position = 0;
    low_since = gettime();
    reported = false;
    previous = "";
    for ( ;; )
    {
        state = self snapshot();
        history[position] = state;
        position = ( position + 1 ) % 32;
        active = isalive( self ) && self.sessionstate == "playing" && !( self downed() );
        if ( !active || self.health > 2 )
        {
            low_since = gettime();
            reported = false;
        }
        else if ( !reported )
        {
            cap_collapsed = self.maxhealth <= 1;
            quiet_low = gettime() - low_since >= 12000
                && gettime() - self.casual_health_last_damage_time >= 12000;
            if ( cap_collapsed || quiet_low )
            {
                reason = "low_health_without_recent_damage";
                if ( cap_collapsed )
                    reason = "max_health_collapsed";
                println( "WAW_HEALTH_AUDIT BEGIN player=" + self getentitynumber() + " reason=" + reason );
                for ( i = 0; i < 32; i++ )
                {
                    index = ( position + i ) % 32;
                    if ( isdefined( history[index] ) )
                        println( "WAW_HEALTH_AUDIT " + history[index] );
                }
                println( "WAW_HEALTH_AUDIT END" );
                reported = true;
            }
        }
        wait 0.25;
    }
}
