import unittest

from waw2bo2 import gsc, gscport


class ReviveStateTests(unittest.TestCase):
    def test_buildable_checks_read_native_state_but_custom_writes_survive(self):
        source = '''collect() {
            if (players[i] UseButtonPressed() && !players[i].being_revived)
                take_part();
            while (player UseButtonPressed() && !player.being_revived) wait .3;
            self.being_revived = true;
            if (isdefined(self.being_revived)) self.being_revived = false;
        }'''
        tokens = gsc.tokenize(source)
        self.assertEqual(gscport.bridge_revive_reads(tokens), 2)
        result = gsc.emit(tokens)
        parsed = gsc.parse(result, 'collect')
        calls = [r for r in gsc.references(parsed.tokens)
                 if r.name == 'waw_being_revived']
        self.assertEqual(len(calls), 2)
        self.assertTrue(all(r.qualifier == gscport.COMPAT for r in calls))
        self.assertIn('waw_being_revived( players[i] )', result)
        self.assertIn('self.being_revived = true;', result)
        self.assertIn('isdefined(self.being_revived)', result)
        self.assertEqual(gscport.bridge_revive_reads(gsc.tokenize(result)), 0)

    def test_nested_receiver_and_parenthesized_condition(self):
        tokens = gsc.tokenize('f() { if (!(team.players[index + 1].being_revived)) ok(); }')
        self.assertEqual(gscport.bridge_revive_reads(tokens), 1)
        result = gsc.emit(tokens)
        self.assertIn('waw_being_revived( team.players[index + 1] )', result)
        gsc.parse(result, 'test')

    def test_unrelated_fields_and_strings_are_unchanged(self):
        source = 'f() { self.other = "being_revived"; if (!self.other) ok(); }'
        tokens = gsc.tokenize(source)
        self.assertEqual(gscport.bridge_revive_reads(tokens), 0)
        self.assertEqual(gsc.emit(tokens), source)


if __name__ == '__main__':
    unittest.main()
