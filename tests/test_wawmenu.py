import csv
import io
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import wawmenu

HEADER = '''#define PC_ONLY_Y 40
#define BUTTON( idx, label, act, vis ) \\
	itemDef \\
	{ \\
		name			button \\
		type			1 \\
		rect			-267 (35 + 20 * (idx - 1)) 224 22 3 1 \\
		exp				text( label ); \\
		textfont		1 \\
		textscale		0.4 \\
		textalign		10 \\
		textalignx		-25 \\
		visible			when( vis ); \\
		onFocus { setLocalVarInt ui_highlight idx; } \\
		action { play "mouse_click"; close pausedmenu; act; } \\
	}
'''
PAUSE = '''#include "ui/header.h"
{
	menuDef
	{
		name			"pausedmenu"
		rect			0 0 640 480 4
		focusColor		1 0.5 0 1
		blurWorld		4.8
		onOpen { showMenu "overheadmap" focusfirst }
		onClose { hideMenu "overheadmap" }
		onEsc { close pausedmenu; }
#ifdef PC
		itemDef { style 3 rect -107 0 854 64 0 0 background "menu_black_line" visible 1 decoration }
#else
		itemDef { style 3 rect 0 0 1 1 background "console_only" visible 1 decoration }
#endif
		BUTTON( 1, "@MENU_ZOMBIE_RESUMEGAME", ;, dvarbool( zombiemode ) )
		BUTTON( 2, "@PLATFORM_QUIT_CAP", open "popup_nosave_quit", !dvarBool( onlinegame ) && dvarbool( sv_running ) )
		BUTTON( 3, "@MENU_OPTIONS_UPPER_CASE", open "options_new_pc", 1 )
	}
	menuDef
	{
		name			"overheadmap"
		visible			when( dvarString( mapname ) != "nazi_zombie_prototype" );
		itemDef { style 3 rect 0 120 464 265 1 1 exp material( "menu_map_" + dvarString( mapname ) ); visible 1 decoration }
	}
}
'''
POPUP = '''{
	menuDef
	{
		name			popup_nosave_quit
		onEsc { close self; open "pausedmenu"; }
		itemDef { type 1 rect 100 135 200 24 1 1 exp text( "@MENU_QUIT_WARNING" ); textfont 1 textscale 0.5476 visible 1 decoration }
		itemDef { name resume_game type 1 rect 78 254 180 22 1 1 exp text( "@MENU_RESUMEGAME_CAP" ); textfont 1 textscale 0.4 visible 1
			action { close self; close "pausedmenu"; } }
		itemDef { type 1 rect 78 278 180 22 1 1 exp text( "@PLATFORM_QUIT_CAP" ); textfont 1 textscale 0.4 visible 1
			action { close self; setdvar ui_skipMainLockout "1"; exec "disconnect"; } }
	}
	menuDef { name "unreferenced" itemDef { visible 1 } }
	menuDef { name "options_new_pc" itemDef { visible 1 } }
}
'''


def fixture(root: Path) -> Path:
    (root / 'ui').mkdir(parents=True)
    (root / 'ui/header.h').write_text(HEADER)
    (root / 'ui/pause.menu').write_text(PAUSE)
    (root / 'ui/popup.menu').write_text(POPUP)
    (root / 'ui/ingame.txt').write_text('{\n\tloadMenu { "ui/popup.menu" }\n\tloadMenu { "ui/absent.menu" }\n}\n')
    (root / 'ui/hud.txt').write_text('// hud\n{\n\tloadMenu { "ui/pause.menu" }\n}\n')
    return root


def rows(project: Path) -> dict[str, list[str]]:
    data = (project / wawmenu.TABLE).read_text(encoding='utf-8')
    return {row[0]: row for row in csv.reader(io.StringIO(data))}


class PreprocessorTests(unittest.TestCase):
    def test_macros_ifdef_and_line_continuations(self):
        with tempfile.TemporaryDirectory() as temp:
            raw = fixture(Path(temp))
            menus = wawmenu.load_sources(raw)
            pause = menus['pausedmenu']
            self.assertEqual(pause['items'][0]['background'], ['"menu_black_line"'])  # PC branch
            self.assertEqual(len(pause['items']), 4)
            self.assertEqual(pause['items'][2]['rect'][1], '( 35 + 20 * ( 2 - 1 ) )')
            self.assertEqual(pause['onopen'], [['showMenu', '"overheadmap"'], ['focusfirst']])

    def test_commands_split_without_semicolons(self):
        self.assertEqual(wawmenu.split_commands([['showMenu', 'a', 'showMenu', 'b', 'focusfirst', 'exec', 'x']]),
                         [['showMenu', 'a'], ['showMenu', 'b'], ['focusfirst'], ['exec', 'x']])

    def test_expression_rpn_precedence_and_strings(self):
        tokens = wawmenu.tokenize('( !dvarBool( a ) && dvarString( "b c" ) != "x" || 2 + 3 * 4 > 1 )')
        self.assertEqual(wawmenu.rpn(tokens), 's:a f:dvarbool:1 op:! s:b%20c f:dvarstring:1 s:x op:!= op:&& '
                                              'n:2 n:3 n:4 op:* op:+ n:1 op:> op:||')


class TranslateTests(unittest.TestCase):
    def test_closure_stops_at_engine_menus_and_skips_unreferenced(self):
        with tempfile.TemporaryDirectory() as temp:
            menus, names = wawmenu.translate(fixture(Path(temp)))
            self.assertEqual(names, ['pausedmenu', 'overheadmap', 'popup_nosave_quit'])

    def test_stage_source_table_and_requirements(self):
        with tempfile.TemporaryDirectory() as temp:
            raw = fixture(Path(temp) / 'raw')
            project = Path(temp) / 'project'
            result = wawmenu.stage_source(project, [Path(temp) / 'none', raw], 'nazi_zombie_factory')
            self.assertIn('menu_map_nazi_zombie_factory', result['materials'])
            self.assertIn('menu_black_line', result['materials'])
            self.assertNotIn('console_only', result['materials'])
            self.assertEqual(result['strings']['MENU_QUIT_WARNING'], [1])
            table = rows(project)
            self.assertEqual(table['mapname'][1], 'nazi_zombie_factory')
            self.assertEqual(table['engine:options_new_pc'][1], 'OptionsMenu')
            item = table['item:pausedmenu:3']
            self.assertEqual(item[1], '-267 55 224 22 3 1')
            self.assertEqual(item[14], '-25')
            self.assertEqual(item[22], 'play mouse_click;close pausedmenu;open popup_nosave_quit')
            self.assertEqual(wawmenu.requested_strings(project), set(result['strings']))

    def test_missing_sources_are_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(wawmenu.MenuSourceError, 'Mod Tools'):
                wawmenu.stage_source(Path(temp), [Path(temp)], 'x')

    def test_font_roles_follow_t4_ui_get_font_handle(self):
        chars = wawmenu.glyph_sets({'strings': {'A': [1], 'B': [0]}, 'literal_text': {}}, {'A': 'Hi', 'B': 'Yo'})
        self.assertEqual(chars['normal'], {'H', 'i', 'Y', 'o', '?'})
        self.assertEqual(chars['extrabig'], {'Y', 'o', '?'})
        self.assertNotIn('bold', chars)

    def test_lui_interpreter(self):
        try:
            from lupa import LuaRuntime
        except ImportError:
            self.skipTest('lupa is required for the LUI harness')
        with tempfile.TemporaryDirectory() as temp:
            project = Path(temp) / 'project'
            wawmenu.stage_source(project, [fixture(Path(temp) / 'raw')], 'nazi_zombie_factory')
            lua = LuaRuntime()
            table = lua.table_from({k: lua.table_from(dict(enumerate(v))) for k, v in rows(project).items()})
            lua.globals().MENU_ROWS = table
            lua.execute(Path('tests/waw_menu_runtime.lua').read_text())

    def test_wawd_raw_sources_when_installed(self):
        raw = Path(r'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty World at War\raw')
        if not (raw / 'ui/hud.txt').is_file():
            self.skipTest('WaW Mod Tools raw folder not installed')
        menus, names = wawmenu.translate(raw)
        self.assertEqual(names[:3], ['pausedmenu', 'objectiveinfo', 'overheadmap'])
        self.assertIn('popup_restart_warning', names)
        self.assertNotIn('options_new_pc', names)
        required = wawmenu.requirements(menus, names, 'nazi_zombie_factory')
        self.assertIn('MENU_ZOMBIE_RESUMEGAME', required['strings'])


if __name__ == '__main__':
    unittest.main()
