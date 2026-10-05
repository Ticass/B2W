import unittest

from waw2bo2 import gsc, gscport


class ContactKillTranslationTests(unittest.TestCase):
    source = '''kick(duration, owner)
    {
        for (n = 0; n < duration; n++)
        {
            players = getplayers();
            if (distance(self.origin, players[i].origin) <= 70 && IsAlive(self))
            {
                self StartRagdoll();
                self launchragdoll((0, 0, 400));
                self DoDamage(self.health + 666, self.origin, owner);
                return;
            }
            if (!IsAlive(self)) return;
            wait .1;
        }
    }'''

    def translate(self, text):
        script = gsc.parse(text, 'test')
        count = gscport.bridge_contact_kill_attacks(script.tokens, script)
        return count, gsc.emit(script.tokens)

    def test_contact_kill_keeps_source_behavior_and_cleans_up_all_exits(self):
        count, text = self.translate(self.source)
        self.assertEqual(count, 1)
        self.assertEqual(text.count('::waw_contact_kill_begin()'), 1)
        self.assertEqual(text.count('::waw_contact_kill_end('), 3)
        self.assertIn('self DoDamage(self.health + 666, self.origin, owner);', text)
        self.assertIn('<= 70', text)
        self.assertIn('wait .1;', text)
        self.assertIn('if (!IsAlive(self)) { self', text)
        self.assertIn('return; }', text)
        gsc.parse(text, 'translated')

    def test_ordinary_attacks_and_exploding_enemies_are_unchanged(self):
        for text in (self.source.replace('self.health + 666', '10'),
                     self.source.replace('self StartRagdoll();', ''),
                     self.source.replace('return;', 'return 1;')):
            count, translated = self.translate(text)
            self.assertEqual(count, 0)
            self.assertEqual(translated, text)


if __name__ == '__main__':
    unittest.main()
