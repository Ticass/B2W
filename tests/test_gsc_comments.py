import unittest

from waw2bo2 import gsc, gscport


class AuthoredCommentTests(unittest.TestCase):
    def test_malformed_block_ending_keeps_disabled_functions_out_of_the_graph(self):
        for newline in ('\n', '\r\n'):
            with self.subTest(newline=newline):
                source = newline.join(['main() {}', '/*', 'disabled() { missing_function(); }',
                                       '*\\', '//radio_location = getent("radio", "targetname");',
                                       'active() { main(); }'])
                script = gsc.parse(source, 'maps/ambient')
                self.assertEqual(set(script.functions), {'main', 'active'})
                self.assertEqual(gsc.emit(script.tokens), source)
                self.assertEqual(gscport.fix_syntax(script.tokens), 1)
                corrected = source.replace('*\\' + newline, '*/' + newline)
                self.assertEqual(gsc.emit(script.tokens), corrected)
                self.assertEqual(set(gsc.parse(corrected, 'maps/ambient').functions), {'main', 'active'})
                self.assertEqual(gscport.fix_syntax(script.tokens), 0)
                self.assertEqual(script.tokens[script.functions['active'].start].line, 6)

    def test_valid_block_comments_and_strings_are_unchanged(self):
        source = '/* normal\n*\\\nstill commented\n*/\nmain() { s = "*\\\\"; }'
        script = gsc.parse(source, 'maps/ambient')
        self.assertEqual(set(script.functions), {'main'})
        self.assertEqual(gscport.fix_syntax(script.tokens), 0)
        self.assertEqual(gsc.emit(script.tokens), source)

    def test_unknown_backslash_reports_the_source_script(self):
        with self.assertRaisesRegex(gsc.GscSyntaxError, r'maps/broken: line 2: cannot tokenize'):
            gsc.parse('main() {}\n\\\n', 'maps/broken')
