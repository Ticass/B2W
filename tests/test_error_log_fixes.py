"""Map build failures from the Discord error-logs forum (2026-11)."""
import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gsc, gscport, t6bridge, zombiebehavior
from waw2bo2.launcher import map_fastfiles, map_zone


class ByteOrderMarkTests(unittest.TestCase):
    def test_script_saved_with_a_bom_tokenizes(self):
        # Crazy Place / Library *_fx.gsc: "cannot tokenize '﻿#include maps\\_util'"
        script = gsc.parse('﻿#include maps\\_utility;\nmain()\n{\n}\n', 'maps\\library_fx')
        self.assertIn('main', script.functions)
        self.assertTrue(gsc.emit(script.tokens).startswith('#include'))


class IntegerLiteralTests(unittest.TestCase):
    def wrap(self, source):
        tokens = gsc.tokenize(source)
        count = gscport.wrap_integer_literals(tokens)
        return count, gsc.emit(tokens)

    def test_literals_past_32_bits_keep_the_low_32_bits(self):
        # Return to Stairway to Hell _zombiemode_mysterybox: "stoi argument out of range"
        self.assertEqual(self.wrap('x = 4294967301;'), (1, 'x = 5;'))
        self.assertEqual(self.wrap('x = 9999999999;'), (1, 'x = 1410065407;'))
        self.assertEqual(self.wrap('x = 2147483648;'), (1, 'x = ( -2147483647 - 1 );'))
        self.assertEqual(self.wrap('x = 3000000000;'), (1, 'x = ( -1294967296 );'))

    def test_32_bit_literals_and_floats_are_unchanged(self):
        source = 'x = 2147483647 + 99999999999.5 + 0;'
        self.assertEqual(self.wrap(source), (0, source))


class LoadingZoneTests(unittest.TestCase):
    def test_a_loading_zone_pick_builds_the_map_zone(self):
        # Decagonal Demise: decagon_load.ff was chosen; it holds no world
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for name in ('decagon.ff', 'decagon_load.ff', 'decagon_patch.ff', 'mod.ff'):
                (folder / name).write_bytes(b'')
            self.assertEqual(map_zone(folder / 'decagon_load.ff'), folder / 'decagon.ff')
            self.assertEqual(map_zone(folder / 'decagon.ff'), folder / 'decagon.ff')
            self.assertEqual(map_fastfiles(folder), [folder / 'decagon.ff'])

    def test_a_map_named_load_without_a_sibling_stays(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'zm_overload.ff'
            path.write_bytes(b'')
            self.assertEqual(map_zone(path), path)
            lone = Path(tmp) / 'nazi_zombie_load.ff'
            lone.write_bytes(b'')
            self.assertEqual(map_zone(lone), lone)


class LoadTimeGuardTests(unittest.TestCase):
    def fold(self, guard, assigned=frozenset()):
        words = [t.low for t in gsc.tokenize(guard)[:-1]]
        return zombiebehavior.fold_level_guard(words, 'nightclub', set(assigned))

    def test_reaper_asylum_guard_folds_for_another_map(self):
        # Nightclub _zombiemode::init_standard_zombie_anims (Reaper's Map Pack)
        guard = 'isdefined(level.anim_use_asylum) || isdefined(level.script) && level.script == "nazi_zombie_asylum"'
        self.assertIs(self.fold(guard), False)
        self.assertIsNone(self.fold(guard, {'anim_use_asylum'}))
        self.assertIs(self.fold('level.script == "nightclub"'), True)
        self.assertIs(self.fold('!(level.script != "nightclub")'), True)
        self.assertIs(self.fold('isdefined(level.x) && level.script == "a"', {'x'}), False)

    def test_runtime_guards_stay_unknown(self):
        self.assertIsNone(self.fold('level.round_number > 3'))
        self.assertIsNone(self.fold('isdefined(level.x) && flag("power_on")', {'x'}))


class GeneratedBlendTests(unittest.TestCase):
    def test_generated_blend_reference_never_takes_a_bo2_raw_namesake(self):
        # Leviathan "*4_5n": a ,reference the map zone does not define; BO2 raw
        # materials/generated/_4_5n.json belongs to a BO2 map (techset not shipped)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, native, bo2 = root / 'source', root / 'native' / 'materials', root / 'bo2'
            for path in (bo2 / 'raw/materials/generated/_4_5n.json', source / 'materials/generated/_4_7.json'):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps({'techniqueSet': 'lit_sm_r0c0s0_b1c1n1s1v1', 'textures': []}))
            native.mkdir(parents=True)
            report = t6bridge.StageReport('any_map')
            _, fallbacks = t6bridge.recover_material_sources(report, {'*4_5n'}, [source], None, None, native, bo2)
            self.assertEqual(fallbacks, {})
            sub = t6bridge.dangling_substitute('*4_5n', [source])
            self.assertEqual(sub[0], '*4_7')


if __name__ == '__main__':
    unittest.main()
