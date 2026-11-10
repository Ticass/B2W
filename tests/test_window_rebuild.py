import struct
import unittest

from waw2bo2 import barriers

# WaW _zombiemode_blockers_new::replace_chunk (Der Riese patch), trimmed
REPLACE_CHUNK = '''
replace_chunk( chunk, has_perk, via_powerup )
{
	only_z = ( chunk.origin[0], chunk.origin[1], chunk.og_origin[2] );
	if( has_perk == true )
	{
		chunk RotateTo( ( 0, 0, 0 ),  0.15 );
		chunk waittill_notify_or_timeout( "rotatedone", 1 );
		wait( 0.1 );
	}
	else
	{
		chunk MoveTo( only_z, 0.15);
		chunk RotateTo( ( 0, 0, 0 ),  0.3 );
		chunk waittill_notify_or_timeout( "rotatedone", 1 );
		wait( 0.2 );
	}
	if( has_perk == true )
	{
		chunk MoveTo( chunk.og_origin, 0.05 );
	}
	else
	{
		chunk MoveTo( chunk.og_origin, 0.1 );
	}
}

add_new_zombie_spawners()
{
	wait( 9 );
}
'''


def stock_repair(start, rest, q0, q1, frames=4):
    """A raw v19 board animation: root keys at the first and last frame, one
    bone (tag_animate) with rotation keys at both ends."""
    def trans(a, b):
        mins = [min(a[k], b[k]) for k in range(3)]
        rng = [abs(a[k] - b[k]) for k in range(3)]
        key = lambda v: [round((v[k] - mins[k]) / rng[k] * 65535) if rng[k] else 0 for k in range(3)]
        return (struct.pack('<H', 2) + bytes([0, frames - 1]) + bytes([0]) + struct.pack('<6f', *mins, *rng)
                + struct.pack('<6H', *key(a), *key(b)))
    return (struct.pack('<3H2BH', 19, frames, 1, 2, 2, 30) + struct.pack('<H', 0) + trans(start, rest)
            + bytes([0, 0]) + b'tag_animate\0'
            + struct.pack('<H', 2) + bytes([0, frames - 1]) + struct.pack('<6h', *q0, *q1)
            + struct.pack('<H', 0) + b'\0')


class WindowRebuildTests(unittest.TestCase):
    def test_timing_is_the_maps_plain_branch(self):
        self.assertEqual(barriers.replace_chunk_timing(REPLACE_CHUNK),
                         {'lift': 0.15, 'turn': 0.3, 'hold': 0.2, 'push': 0.1})
        self.assertIsNone(barriers.replace_chunk_timing('replace_chunk( a )\n{\n\twait( 1 );\n}\n'))

    def test_board_lifts_turns_holds_then_pushes(self):
        start, rest = (-40.0, 0.0, -31.0), (28.0, 0.0, 50.0)
        raw = stock_repair(start, rest, (16080, 16905, 16670), (-3670, 3670, 22877))
        timing = barriers.replace_chunk_timing(REPLACE_CHUNK)
        out = barriers.waw_repair_animation(raw, timing)
        version, frames, bones, flags, kind, fps = struct.unpack_from('<3H2BH', out)
        self.assertEqual((version, frames, bones, flags, fps), (19, 19, 1, 2, 30))   # 0.6 s
        decoded = barriers._decode_repair(out)
        self.assertEqual(decoded['bone'], b'tag_animate')
        for got, want in zip(decoded['root'], (start, rest)):
            for g, w in zip(got, want):
                self.assertAlmostEqual(g, w, places=2)
        # every frame keyed: read the root track (after the empty root rotation)
        mins, rng = struct.unpack_from('<3f', out, 15), struct.unpack_from('<3f', out, 27)
        root = [tuple(mins[k] + rng[k] * v[k] / 65535 for k in range(3))
                for v in (struct.unpack_from('<3H', out, 39 + 6 * i) for i in range(frames))]
        self.assertAlmostEqual(root[5][2], rest[2], places=1)        # lifted by 0.15 s (frame 4.5)
        self.assertAlmostEqual(root[5][0], start[0], places=1)       # still where it lay
        self.assertAlmostEqual(root[15][0], start[0], places=1)      # held until 0.5 s
        self.assertAlmostEqual(root[18][0], rest[0], places=1)       # pushed in by 0.6 s


if __name__ == '__main__':
    unittest.main()
