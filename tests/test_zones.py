import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from waw2bo2 import entities, zones


class ZoneGraphTests(unittest.TestCase):
    def test_zombie_spawners_without_waw_force_flag_can_spawn_in_t6(self):
        source = [
            {"classname": "actor_zombie_ger_ber_sshonor", "script_noteworthy": "zombie_spawner",
             "targetname": "zone_spawners", "origin": "1 2 3"},
            {"classname": "actor_zombie_ger_ber_sshonor", "script_noteworthy": "zombie_spawner",
             "targetname": "zone_spawners", "script_forcespawn": "0", "script_string": "riser"},
            {"classname": "actor_zombie_ger_ber_sshonor", "script_noteworthy": "zombie_spawner",
             "targetname": "zone_spawners", "script_forcespawn": "1"},
            {"classname": "actor_ally", "script_forcespawn": "0"},
        ]
        out, _, summary = entities.convert_entities(source, "test", {})
        actors = [e for e in out if e['classname'] == entities.ZOMBIE_ACTOR_CLASS]
        self.assertEqual([e['script_forcespawn'] for e in actors], ['1', '1', '1'])
        self.assertEqual(actors[1]['script_string'], 'riser')
        self.assertEqual(next(e for e in out if e['classname'] == 'actor_ally')['script_forcespawn'], '0')
        self.assertEqual(summary['renamed']['WaW DoSpawn -> T6 spawnactor spawner flag'], 2)
    def test_qualified_calls_are_read(self):
        script = ('init_zones[0] = "start_zone";\n'
                  '    maps\\_zombiemode_zone_manager::add_adjacent_zone( "start_zone", "yard", "enter_yard" );\n')
        self.assertEqual(zones.read_waw_zones(script), (["start_zone"], ['"start_zone", "yard", "enter_yard"']))

    def test_map_without_zone_graph_is_not_an_error(self):
        self.assertEqual(zones.read_waw_zones("main() { maps\\_zombiemode::main(); }"), ([], []))


class SynthesizedZoneTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.bsp = Path(temp.name)
        (self.bsp / "submodels.json").write_text(json.dumps({"submodels": [
            {"mins": [0, 0, 0], "maxs": [8, 8, 8], "brushes": [{"contents": 7, "axial": [[7, 9]] * 6}]}]}))
        self.clip = SimpleNamespace(submodels=[SimpleNamespace(mins=(-100, -100, -10), maxs=(100, 100, 50))])

    def test_spawner_groups_become_zones_opened_by_their_doors(self):
        out = [
            {"classname": entities.ZOMBIE_ACTOR_CLASS, "targetname": "zombie_spawner_init", "origin": "1 2 3"},
            {"classname": entities.ZOMBIE_ACTOR_CLASS, "targetname": "yard_spawners", "origin": "4 5 6"},
            {"classname": entities.ZOMBIE_ACTOR_CLASS, "targetname": "attic_spawners", "origin": "7 8 9"},
            {"classname": "trigger_use", "targetname": "zombie_door", "target": "yard_door", "model": "*1",
             "script_flag": "enter_yard"},
            {"classname": "script_brushmodel", "targetname": "yard_door", "target": "yard_spawners"},
        ]
        summary = {}
        initial, adjacency = entities.synthesize_zones(out, self.clip, self.bsp, summary)
        self.assertEqual(initial, ["waw2bo2_zone"])
        self.assertEqual(adjacency, ['"waw2bo2_zone", "waw2bo2_zone_2", "waw2bo2_zone_2_open"'])
        self.assertEqual(summary["synthesized_zones"]["never_unlocked"], ["attic_spawners"])
        self.assertEqual(out[3]["script_flag"], "enter_yard,waw2bo2_zone_2_open")
        volumes = [e for e in out if e.get("classname") == "info_volume"]
        self.assertEqual([(v["targetname"], v["target"], v["model"]) for v in volumes],
                         [("waw2bo2_zone", "zombie_spawner_init", "*2"), ("waw2bo2_zone_2", "yard_spawners", "*3")])
        structs = {(e["targetname"], e["origin"]) for e in out if e.get("script_noteworthy") == "spawn_location"}
        self.assertEqual(structs, {("zombie_spawner_init", "1 2 3"), ("yard_spawners", "4 5 6")})
        subs = json.loads((self.bsp / "submodels.json").read_text())["submodels"]
        self.assertEqual(len(subs), 3)
        self.assertEqual(subs[1]["brushes"][0]["contents"], 7)  # copied from the map's own trigger brush
        self.assertEqual(subs[1]["mins"], [-612, -612, -522])

    def test_missing_entity_models_are_stripped_but_entities_kept(self):
        path = self.bsp / "entities.json"
        path.write_text(json.dumps({"entities": [
            {"classname": "script_model", "model": "weapons/sp/bar", "targetname": "auto12"},
            {"classname": "script_model", "model": "zombie_teddybear"},
            {"classname": "script_brushmodel", "model": "*3"}]}))
        self.assertEqual(entities.placed_models(path), {"weapons/sp/bar", "zombie_teddybear"})
        self.assertEqual(entities.strip_models(path, {"weapons/sp/bar"}), 1)
        ents = json.loads(path.read_text())["entities"]
        self.assertEqual(ents[0], {"classname": "script_model", "targetname": "auto12"})
        self.assertEqual(ents[1]["model"], "zombie_teddybear")

    def test_trigger_zone_volumes_become_info_volumes(self):
        ents = [{"classname": "worldspawn"},
                {"classname": "trigger_multiple", "targetname": "lobby_area", "target": "lobby_area_spawners", "model": "*6"},
                {"classname": "trigger_multiple", "targetname": "playable_area", "model": "*9"}]
        zone_set = zones.zone_names(["spawn_area"], ['"spawn_area", "lobby_area", "enter_lobby"'])
        self.assertEqual(zone_set, {"spawn_area", "lobby_area"})
        out, _, summary = entities.convert_entities(ents, "zm_x", zone_names=zone_set)
        lobby = next(e for e in out if e.get("targetname") == "lobby_area")
        self.assertEqual((lobby["classname"], lobby["model"], lobby["target"], lobby["script_noteworthy"]),
                         ("info_volume", "*6", "lobby_area_spawners", "player_volume"))
        self.assertEqual(next(e for e in out if e.get("targetname") == "playable_area")["classname"], "trigger_multiple")

    def test_bo2_script_gets_synthesized_graph(self):
        script = ('main()\n{\n    init_zones[0] = "start_zone";\n}\n'
                  'zm_x_zone_init()\n{\n    add_adjacent_zone( "a", "b", "c" );\n}\n')
        patched = zones.patch_bo2_script(script, "zm_x", ["waw2bo2_zone"],
                                         ['"waw2bo2_zone", "waw2bo2_zone_1", "waw2bo2_zone_1_open"'])
        self.assertIn('init_zones[0] = "waw2bo2_zone";', patched)
        self.assertIn('add_adjacent_zone( "waw2bo2_zone", "waw2bo2_zone_1", "waw2bo2_zone_1_open" );', patched)
        self.assertNotIn('"a", "b"', patched)


if __name__ == "__main__":
    unittest.main()
