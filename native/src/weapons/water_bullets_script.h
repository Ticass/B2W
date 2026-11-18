#pragma once

#include <string_view>

namespace w2b::weapons
{
    inline constexpr std::string_view waterBulletsScript = R"gsc(// waw2bo2: WaW bullets go on through water; T6 never damages what stands in it.
// See waw2bo2/waterbullets.py. Each row: hip stand/ducked/prone min, hip
// stand/ducked/prone max, decay rate, fire add, move add, ADS spread, pellets,
// WaW water penetration depth.

init()
{
    level.waw_water_bullets = [];
    // WEAPON_TABLE
    level thread waw_water_bullet_players();
}

waw_water_row( a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 )
{
    row = [];
    row[0] = a0;
    row[1] = a1;
    row[2] = a2;
    row[3] = a3;
    row[4] = a4;
    row[5] = a5;
    row[6] = a6;
    row[7] = a7;
    row[8] = a8;
    row[9] = a9;
    row[10] = a10;
    row[11] = a11;
    return row;
}

waw_water_bullet_players()
{
    for (;;)
    {
        level waittill( "connected", player );
        player thread waw_water_bullets();
    }
}

waw_water_bullets()
{
    self endon( "disconnect" );
    self.waw_water_spread = 0;
    self.waw_water_spread_time = gettime();

    for (;;)
    {
        self waittill( "weapon_fired", weapon );

        if ( !isdefined( weapon ) || !isdefined( level.waw_water_bullets[weapon] ) )
            continue;

        row = level.waw_water_bullets[weapon];
        spread = self waw_water_spread( row );
        eye = self geteye();
        angles = self getplayerangles();
        forward = anglestoforward( angles );
        right = anglestoright( angles );
        up = anglestoup( angles );
        cone = tan( spread );

        for ( i = 0; i < row[10]; i++ )
        {
            radius = randomfloat( 1 ) * cone;
            turn = randomfloat( 360 );
            dir = vectornormalize( forward + right * ( cos( turn ) * radius ) + up * ( sin( turn ) * radius ) );
            trace = bullettrace( eye, eye + dir * 8192, 1, self );

            if ( !isdefined( trace["surfacetype"] ) || trace["surfacetype"] != "water" || isdefined( trace["entity"] ) )
                continue;

            start = trace["position"] + dir * 0.5;
            magicbullet( weapon, start, start + dir * row[11], self );
        }
    }
}

// Hip spread as both engines evolve it: stance minimum, plus the fire add
// decaying at its rate, plus the move add; blended to ADS spread.
waw_water_spread( row )
{
    stance = self getstance();
    low = row[0];
    high = row[3];

    if ( stance == "crouch" )
    {
        low = row[1];
        high = row[4];
    }
    else if ( stance == "prone" )
    {
        low = row[2];
        high = row[5];
    }

    now = gettime();
    self.waw_water_spread = max( 0, self.waw_water_spread - row[6] * ( now - self.waw_water_spread_time ) / 1000 );
    self.waw_water_spread_time = now;
    velocity = self getvelocity();
    moving = min( 1, length( ( velocity[0], velocity[1], 0 ) ) / max( 1, getdvarfloat( "g_speed" ) ) );
    hip = min( high, low + self.waw_water_spread + row[8] * moving );
    self.waw_water_spread = min( high - low, self.waw_water_spread + row[7] );
    ads = self playerads();
    return hip + ( row[9] - hip ) * ads;
}
)gsc";
}
