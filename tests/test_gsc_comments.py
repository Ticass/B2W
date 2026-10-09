from pathlib import Path
import tempfile
import unittest
import zipfile

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

    def test_comment_left_open_at_end_of_file_hides_the_rest_like_waw(self):
        source = 'run() { wait 1; }\n/*\nold() { maps\\_vehicle::vehicle_init( x ); }\n\n'
        script = gsc.parse(source, 'maps/car_run')
        self.assertEqual(set(script.functions), {'run'})
        self.assertEqual(gscport.fix_syntax(script.tokens), 1)
        text = gsc.emit(script.tokens)
        self.assertTrue(text.rstrip().endswith('*/'))
        self.assertEqual(set(gsc.parse(text, 'maps/car_run').functions), {'run'})

    def test_unknown_backslash_reports_the_source_script(self):
        with self.assertRaisesRegex(gsc.GscSyntaxError, r'maps/broken: line 2: cannot tokenize'):
            gsc.parse('main() {}\n\\\n', 'maps/broken')


class ParenthesisedCallerTests(unittest.TestCase):
    def test_parenthesised_method_caller_is_unwrapped(self):
        source = ('f( e ) { if ( ok() && (self) IsTouching (e) ) {} (self) thread g(); '
                  'n = (a.b).size; x = foo(a) + (b); }')
        script = gsc.parse(source, 'maps/elevator')
        self.assertEqual(gscport.fix_syntax(script.tokens), 3)
        text = gsc.emit(script.tokens)
        self.assertIn('self IsTouching (e)', text)
        self.assertIn('self thread g()', text)
        self.assertIn('a.b.size', text)
        self.assertIn('foo(a) + (b)', text)     # a call and a plain grouping stay

    def test_statement_conditions_keep_their_parentheses(self):
        source = 'f() { if (x) foo(); while (y) bar(); if (self) thread g(); for (;;) h(); switch (s) {} }'
        script = gsc.parse(source, 'maps/elevator')
        self.assertEqual(gscport.fix_syntax(script.tokens), 0)
        self.assertEqual(gsc.emit(script.tokens), source)


class IndentedDirectiveTests(unittest.TestCase):
    def test_indented_includes_start_their_line_like_t6_requires(self):
        # Project X's IWD _zombiemode_perks.gsc: every line indented, includes too.
        source = ('    #include maps\\_utility;\n\t#include common_scripts\\utility;\n'
                  '     \n    init()\n    {\n    x = 1;\n    }\n')
        script = gsc.parse(source, 'maps/_zombiemode_perks')
        self.assertEqual(script.includes, ['maps\\_utility', 'common_scripts\\utility'])
        self.assertEqual(gscport.fix_syntax(script.tokens), 2)
        text = gsc.emit(script.tokens)
        self.assertTrue(text.startswith('#include maps\\_utility;\n#include common_scripts\\utility;\n'))
        self.assertIn('    init()', text)      # only directives move
        self.assertEqual(gscport.fix_syntax(script.tokens), 0)

    def test_iwd_scripts_use_universal_newlines_like_zone_rawfiles(self):
        with tempfile.TemporaryDirectory() as temp:
            iwd = Path(temp) / 'map.iwd'
            with zipfile.ZipFile(iwd, 'w') as z:
                z.writestr('maps/_zombiemode_perks.gsc', '#include maps\\_utility;\r\ninit()\r\n{\r}\r\n')
            sources = gscport.Sources([], [iwd], None)
            self.assertEqual(sources.text['maps\\_zombiemode_perks.gsc'],
                             '#include maps\\_utility;\ninit()\n{\n}\n')
