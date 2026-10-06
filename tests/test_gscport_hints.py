import tempfile
import unittest
from pathlib import Path
from waw2bo2 import gscport
from waw2bo2.t6api import T6Api

class HintRegistryTests(unittest.TestCase):
    def test_source_registry_initializes_and_native_helper_does_not_replace_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); src=root/'src/maps'; src.mkdir(parents=True)
            (src/'test.gsc').write_text('#include maps\\_zombiemode_utility;\nmain()\n{\n maps\\_zombiemode::main();\n self set_hint_string(self,"default_buy_door_750");\n x=level.zombie_hints["custom"];\n}\n')
            (src/'_zombiemode.gsc').write_text('#include maps\\_zombiemode_utility;\nmain(){}\ninit_strings(){ add_zombie_hint("default_buy_door_750", &"ZOMBIE_DOOR_750"); }')
            (src/'_zombiemode_utility.gsc').write_text('add_zombie_hint(ref,text){}\nset_hint_string(ent,ref){}')
            api=T6Api({}, {r'maps\mp\zombies\_zm_utility': {'add_zombie_hint':2,'set_hint_string':3}})
            gscport.port_map(gscport.Sources([root/'src'],[],None),api,'test',root/'out')
            main=(root/'out/maps/mp/waw/test.gsc').read_text()
            core=(root/'out/maps/mp/waw/_waw2bo2_core.gsc').read_text()
            self.assertIn('waw_set_hint_string(self,"default_buy_door_750")',main)
            self.assertIn('level.waw2bo2_hints["custom"]',main)
            self.assertIn('waw_add_zombie_hint("default_buy_door_750", &"ZOMBIE_DOOR_750")',core)
            self.assertIn('framework_level_state()\n{\n\tzombiemode__init_strings();',core)

    def test_local_map_hint_function_is_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); (root/'maps').mkdir()
            (root/'maps/test.gsc').write_text('main(){ set_hint_string(1,2); }\nset_hint_string(a,b){}')
            tr=gscport.Translator(gscport.Sources([root],[],None),T6Api({},{}),gscport.PortReport())
            text=tr.translate(r'maps\test')
            self.assertIn('set_hint_string(1,2)',text)
            self.assertNotIn('waw_set_hint_string',text)
