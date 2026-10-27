import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from waw2bo2 import entities, zones


class ZoneGraphTests(unittest.TestCase):
    def test_dlc3_initial_zone_fields_are_preserved(self):
        script = ('level.DLC3.initialZones[0] = "spawn";\n'
                  'level.dlc3.initialzones[1] = "courtyard";\n'
                  'add_adjacent_zone("spawn", "yard", "door_open");\n')
        initial, adjacency = zones.read_waw_zones(script)
        self.assertEqual(initial, ['spawn', 'courtyard'])
        patched = zones.patch_bo2_script(
            'main()\n{\n    init_zones[0] = "start_zone";\n}\nzm_x_zone_init()\n{\n}\n',
            'zm_x', initial, adjacency)
        self.assertIn('init_zones[0] = "spawn";', patched)
        self.assertIn('init_zones[1] = "courtyard";', patched)

    def test_script_ending_in_a_short_statement_has_no_zone_graph(self):
        # Verruckt's script ends with fewer than six tokens after an identifier
        self.assertEqual(zones.read_waw_zones('main()\n{\n    a = b;\n}\nfoo'), ([], []))
        self.assertEqual(zones.read_waw_zones('x[ 0 ] ='), ([], []))

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
        # WaW human AI has no T6 zombies aitype (it crashes the map): dropped, reported
        self.assertFalse(any(e['classname'] == 'actor_ally' for e in out))
        self.assertIn('actor_ally (WaW human AI; T6 zombies has no aitype for it)', summary['dropped'])
        self.assertEqual(summary['renamed']['WaW DoSpawn -> T6 spawnactor spawner flag'], 2)
    def test_spawn_structs_chase_like_waw(self):
        spawner = {"classname": "actor_zombie_ger_ber_sshonor", "script_noteworthy": "zombie_spawner",
                   "targetname": "zone_spawners", "origin": "1 2 3"}
        chaser = dict(spawner, origin="4 5 6", script_string="zombie_chaser")
        goal = {"classname": "script_struct", "targetname": "exterior_goal", "target": "window", "origin": "0 0 0"}
        out, _, summary = entities.convert_entities([spawner, chaser, goal], "test", {})
        entities.chase_without_barricades(out, summary)
        structs = {e["origin"]: e.get("script_string") for e in out if e.get("script_noteworthy") == "spawn_location"}
        # WaW zombie_chaser hunts players; plain spawners keep WaW's nearest-window path
        self.assertEqual(structs, {"1 2 3": None, "4 5 6": "find_flesh"})
        # without any barricade there is no window to reach: every spawn hunts players
        out, _, summary = entities.convert_entities([spawner], "test", {})
        entities.chase_without_barricades(out, summary)
        self.assertEqual([e.get("script_string") for e in out if e.get("script_noteworthy") == "spawn_location"],
                         ["find_flesh"])
        self.assertEqual(summary["renamed"]["no exterior_goal barricades: spawn -> find_flesh"], 1)

    def test_qualified_calls_are_read(self):
        script = ('init_zones[0] = "start_zone";\n'
                  '    maps\\_zombiemode_zone_manager::add_adjacent_zone( "start_zone", "yard", "enter_yard" );\n')
        self.assertEqual(zones.read_waw_zones(script), (["start_zone"], ['"start_zone", "yard", "enter_yard"']))

    def test_local_flag_arrays_resolve_and_dead_code_is_skipped(self):
        script = ('zone_init()\n{\n'
                  '    add_adjacent_zone( "a", "b", "door_ab" );\n'
                  '    flag_array[0] = "door_ab";\n    flag_array[1] = "door_bc";\n'
                  '    add_adjacent_zone( "a", "c", flag_array, true );\n'
                  '    /*\n    add_adjacent_zone( "a", "ghost", flag_array, true );\n    */\n'
                  '    // add_adjacent_zone( "a", "ghost2", "x" );\n'
                  '    /# add_adjacent_zone( "a", "dev", "x" ); #/\n}\n')
        initial, adjacency = zones.read_waw_zones(script)
        self.assertEqual(adjacency, ['"a", "b", "door_ab"', '"a", "c", [ "door_ab", "door_bc" ], true'])
        self.assertEqual(zones.zone_names(initial, adjacency), {"a", "b", "c"})
        call = zones.adjacent_zone_call(adjacency[1])
        # BO2's add_adjacent_zone indexes level.flag with its flag: arrays use make_zone_adjacent
        self.assertNotIn('add_adjacent_zone', call)
        self.assertIn('waw2bo2_flags[1] = "door_bc";', call)
        self.assertIn('make_zone_adjacent( "a", "c", waw2bo2_flags );', call)
        self.assertIn('if ( !( true ) )', call)

    def test_map_without_zone_graph_is_not_an_error(self):
        self.assertEqual(zones.read_waw_zones("main() { maps\\_zombiemode::main(); }"), ([], []))


class SynthesizedZoneTests(unittest.TestCase):
    def test_barricade_root_keeps_attack_origin_and_entry_keeps_repair_trigger(self):
        goal = {'classname': 'script_struct', 'targetname': 'exterior_goal', 'target': 'window',
                'origin': '0 0 0', 'angles': '0 90 0'}
        entry = {'classname': 'script_struct', 'targetname': 'window',
                 'origin': '0 48 0', 'angles': '0 90 0'}
        board = {'classname': 'script_brushmodel', 'targetname': 'window', 'origin': '0 48 32',
                 'model': '*1', 'script_noteworthy': '1'}
        out, _, _ = entities.convert_entities([goal, entry, board], 'test', {})
        barrier = next(e for e in out if e.get('classname', '').startswith('zbarrier_'))
        self.assertEqual(barrier['origin'], goal['origin'])
        self.assertEqual(barrier['angles'], goal['angles'])
        self.assertEqual(next(e for e in out if e.get('targetname') == 'exterior_goal')['origin'], goal['origin'])

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
        # a door zone activates by adjacency; a map-wide box would claim the
        # start zone's static unitriggers (Verruckt barricade repair)
        self.assertEqual((subs[2]["mins"], subs[2]["maxs"]), ([-100, -100, -10], [-84, -84, 6]))

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
        # the map's volume as the map has it, then BO2's player_volume twin
        lobby, twin = [e for e in out if e.get("targetname") == "lobby_area"]
        self.assertEqual((lobby["classname"], lobby["model"], lobby["target"], lobby.get("script_noteworthy")),
                         ("info_volume", "*6", "lobby_area_spawners", None))
        self.assertEqual((twin["classname"], twin["model"], twin["target"], twin["script_noteworthy"]),
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
