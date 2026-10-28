import tempfile
import unittest
from pathlib import Path
from waw2bo2 import clientport, gscport


class ClientPortTests(unittest.TestCase):
    def test_source_effects_sounds_and_single_argument_level_events_are_translated(self):
        source = '#include clientscripts\\_utility;\ninit(){loadfx("source/tube");}\n' \
                 'watch(){level waittill("TGH"); level waittill("notetrack", n, note); ' \
                 'realwait(0.1); playsound(0,"source_hum",(0,0,0)); ' \
                 'if(getcurrentweapon(0)=="source_weapon") return;}\n'
        translated = clientport.translate_library(source, {'source/tube':'waw/source/tube'},
                                                  {'source_weapon':'converted_weapon'}, {'source_hum'})
        self.assertIn('#include clientscripts\\mp\\_utility;', translated)
        self.assertIn('loadfx("waw/source/tube")', translated)
        self.assertIn('waitrealtime(0.1)', translated)
        self.assertIn('"waw/source_hum"', translated)
        self.assertIn('"converted_weapon"', translated)
        self.assertIn('registersystem( "TGH", ::waw_notify_0 )', translated)
        self.assertNotIn('registersystem( "notetrack"', translated)
        self.assertIn('level notify( "TGH" )', translated)


class MapClientPortTests(unittest.TestCase):
    def sources(self, files):
        root = Path(tempfile.mkdtemp())
        for name, text in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
        return gscport.Sources([root], [], None), Path(tempfile.mkdtemp())

    def test_map_client_runs_its_own_modules_and_bo2_keeps_the_glue(self):
        sources, out = self.sources({
            'clientscripts/testmap.csc': '#include clientscripts\\_utility;\n'
                'main()\n{\n\tclientscripts\\_load::main();\n\tclientscripts\\testmap_fx::main();\n'
                '\tthread clientscripts\\testmap_amb::main();\n\tthread waitforclient(0);\n}\n',
            'clientscripts/testmap_fx.csc': '#include clientscripts\\_fx;\n'
                'main()\n{\n\tlevel._effect["fire"] = loadfx("env/fire");\n\tlevel thread lamp();\n}\n'
                'lamp()\n{\n\tlevel waittill("pl1");\n\tSetVolFog(1,2,3,4,0.1,0.2,0.3,5);\n}\n',
            'clientscripts/_fx.csc': 'fx_init(clientNum)\n{\n\tclientscripts\\_lights::init_lights(clientNum);\n}\n',
            'clientscripts/_lights.csc': 'init_lights(clientNum)\n{\n\tlights = GetStructArray("light", "classname");\n'
                '\tplaysound(clientNum, "light", (0,0,0));\n\tlight = spawn(clientNum, (0,0,0));\n'
                '\tlight makelight(1);\n\tlight setLightIntensity(light getLightIntensity() * 2);\n}\n',
            'clientscripts/_utility.csc': 'getstruct(name, type)\n{\n}\n',
        })
        report = clientport.port_map_client(sources, 'testmap', out,
                                            clientport.staged_client_modules('testmap', 'zm_testmap_waw'))
        self.assertEqual(report['entry'], 'clientscripts\\mp\\waw\\testmap')
        self.assertEqual(set(report['modules']), {'clientscripts\\mp\\waw\\' + m for m in
                                                  ('testmap', 'testmap_fx', '_fx', '_lights', '_utility')})
        self.assertEqual(len(report['dropped']), 3)   # _load, the staged amb, waitforclient
        self.assertEqual(report['fx'], ['env/fire'])
        self.assertEqual(report['events'], ['pl1'])
        main = (out / 'clientscripts/mp/waw/testmap.csc').read_text()
        self.assertIn('\tclientscripts\\mp\\waw\\testmap_fx::main();', main)
        self.assertNotIn('\tclientscripts\\_load::main();', main)
        lights = (out / 'clientscripts/mp/waw/_lights.csc').read_text()
        self.assertIn('light makelight(1)', lights)
        self.assertIn('light clientscripts\\mp\\waw\\_waw2bo2_environment::waw_setlightintensity(light '
                      'clientscripts\\mp\\waw\\_waw2bo2_environment::waw_getlightintensity()', lights)
        fx = (out / 'clientscripts/mp/waw/testmap_fx.csc').read_text()
        self.assertIn('#include clientscripts\\mp\\waw\\_fx;', fx)
        self.assertIn('_waw2bo2_environment::waw_client_setvolfog(', fx)
        hook = clientport.finish_map_client(out, report, {'env/fire': 'waw/env/fire'}, {}, {'light'})
        lights = (out / 'clientscripts/mp/waw/_lights.csc').read_text()
        self.assertIn('GetStructArray("light", "classname")', lights)   # a lookup key, not the "light" alias
        self.assertIn('playsound(clientNum, "waw/light"', lights)
        self.assertEqual(hook.strip(), 'clientscripts\\mp\\waw\\_waw2bo2_client::init();')
        self.assertIn('loadfx("waw/env/fire")', (out / 'clientscripts/mp/waw/testmap_fx.csc').read_text())
        glue = (out / 'clientscripts/mp/waw/_waw2bo2_client.csc').read_text()
        self.assertIn('forward_notify( "pl1", ::waw_notify_0 );', glue)
        self.assertIn('clientscripts\\mp\\waw\\_fx::fx_init( localclientnum );', glue)
        self.assertEqual(report['unbound_fx'], [])

