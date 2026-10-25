import unittest

from waw2bo2 import gsc, gscport


def lower(source: str) -> tuple[int, str]:
    tokens = gsc.tokenize(source)
    count = gscport.lower_maxhealth_assignments(tokens)
    return count, gsc.emit(tokens)


class MaxHealthLoweringTests(unittest.TestCase):
    SET = gscport.COMPAT + "::waw_set_maxhealth("

    def test_player_jugg_assignment_uses_setmaxhealth_helper(self):
        count, text = lower('f()\n{\n    if ( perk == "x" )\n    {\n        player.maxhealth = 160;\n'
                            '        player.health = 160;\n    }\n}\n')
        self.assertEqual(count, 1)
        self.assertIn(f"player {self.SET} 160 );", text)
        self.assertIn("player.health = 160;", text)

    def test_builtin_call_after_the_rewrite_is_not_a_method(self):
        # Der Riese dog_init: self.maxhealth = int( level.dog_health * m ); the
        # int call was read as `x int( ... )` and stubbed (undefined health)
        tokens = gsc.tokenize('f()\n{\n    self.maxhealth = int( a * m );\n}\n')
        gscport.lower_maxhealth_assignments(tokens)
        refs = [r for r in gsc.references(tokens) if r.name.lower() == "int"]
        self.assertEqual(len(refs), 1)
        self.assertFalse(refs[0].method)

    def test_consecutive_indexed_and_else_receivers(self):
        count, text = lower('f()\n{\n    self.maxhealth = 100;\n    level.players[i].maxhealth = int( a * ( b + 1 ) );\n'
                            '    if ( x ) y = 1;\n    else self.maxhealth = 5;\n}\n')
        self.assertEqual(count, 3)
        self.assertIn(f"self {self.SET} 100 );", text)
        self.assertIn(f"level.players[i] {self.SET} int( a * ( b + 1 ) ) );", text)
        self.assertIn(f"else self {self.SET} 5 );", text)
        gsc.parse(text, "<lowered>")

    def test_reads_and_comparisons_are_untouched(self):
        source = 'f()\n{\n    if ( self.maxhealth == 100 ) x = 1;\n    z = self.maxhealth;\n    a = b.maxhealth * 2;\n}\n'
        self.assertEqual(lower(source), (0, source))


class HudFieldLoweringTests(unittest.TestCase):
    def test_edge_anchors_use_safe_area_and_hud_hides_in_menus(self):
        source = ('f()\n{\n\thud.alignX = "left";\n\thud.horzAlign = "left";\n\thud.vertAlign = "bottom";\n'
                  '\thud.horzAlign = "center";\n\thud.vertAlign = "middle";\n\thud.hidewheninmenu = false;\n'
                  '\tif ( hud.horzAlign == "left" ) x = 1;\n}\n')
        tokens = gsc.tokenize(source)
        self.assertEqual(gscport.lower_hud_fields(tokens), 3)
        text = gsc.emit(tokens)
        self.assertIn('hud.alignX = "left";', text)          # element alignment is unchanged
        self.assertIn('hud.horzAlign = "user_left";', text)
        self.assertIn('hud.vertAlign = "user_bottom";', text)
        self.assertIn('hud.horzAlign = "center";', text)
        self.assertIn('hud.hidewheninmenu = true;', text)
        self.assertIn('if ( hud.horzAlign == "left" )', text)


if __name__ == "__main__":
    unittest.main()
