import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import entities, paths

ASD = """zm_traverse : aliased restart notify traverse_anim
{
	jump_down_72					ai_zombie_jump_down_72
//	jump_down_120					ai_zombie_jump_down_120
	jump_down_127					ai_zombie_jump_down_127
	jump_up_96						ai_zombie_jump_up_96
	jump_up_to_climb				ai_zombie_jump_up_2_climb
	traverse_garage_door			ai_zombie_traverse_garage_roll
}

zm_traverse_crawl : aliased restart missing_legs notify traverse_anim
{
	jump_down_72_crawl				ai_zombie_crawl_jump_down_72
	jump_down_127_crawl				ai_zombie_crawl_jump_down_127
	jump_up_96_crawl				ai_zombie_crawl_jump_up_96
	jump_up_to_climb_crawl			ai_zombie_crawl_jump_up_2_climb
}
"""


class TraversalTests(unittest.TestCase):
    def test_substates_need_a_crawl_twin_and_skip_comments(self):
        self.assertEqual(paths.traverse_substates(ASD), {"jump_down_72", "jump_down_127", "jump_up_96", "jump_up_to_climb"})

    def test_same_named_substate_first_then_measured_height(self):
        sub = paths.traverse_substates(ASD)
        self.assertEqual(paths.traverse_alias_measured("zombie_jump_up_to_climb", 95, sub), "jump_up_to_climb")
        self.assertEqual(paths.traverse_alias_measured("zombie_jump_down_127", -103, sub), "jump_down_127")
        self.assertEqual(paths.traverse_alias_measured("zombie_jump_down_184", -183, sub), "jump_down_127")
        self.assertEqual(paths.traverse_alias_measured("some_drop", -70, sub), "jump_down_72")
        self.assertIsNone(paths.traverse_alias_measured("wall_hop", 0, sub))


class SpawnerTests(unittest.TestCase):
    def test_plain_spawners_use_windows_and_risers_rise_at_zone_rise_structs(self):
        ents = "\n".join([
            '{\n"classname" "info_volume"\n"targetname" "zone_a"\n"target" "zone_a_spawners"\n}',
            '{\n"classname" "actor_axis_zombie"\n"targetname" "zone_a_spawners"\n"script_noteworthy" "zombie_spawner"\n'
            '"origin" "1 2 3"\n}',
            '{\n"classname" "actor_axis_zombie"\n"targetname" "zone_a_spawners"\n"script_noteworthy" "zombie_spawner"\n'
            '"script_string" "riser"\n"origin" "900 900 0"\n}',
            '{\n"classname" "script_struct"\n"targetname" "zone_a_spawners_rise"\n"script_noteworthy" "find_flesh"\n'
            '"origin" "10 10 0"\n}',
            '{\n"classname" "script_struct"\n"targetname" "zone_a_spawners_rise"\n"script_noteworthy" "riser_door"\n'
            '"origin" "20 20 0"\n}',
            '{\n"classname" "script_struct"\n"targetname" "unrelated_rise"\n"origin" "30 30 0"\n}',
        ])
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "map.ents"
            path.write_text(ents)
            entities.write_entities(path, "proj", Path(temp) / "BSP")
            out = json.loads((Path(temp) / "BSP/entities.json").read_text())["entities"]
            spots = [(e["script_noteworthy"], e["origin"], e.get("script_string")) for e in out
                     if e.get("script_noteworthy") in ("spawn_location", "riser_location")]
            self.assertEqual(sorted(spots), [("riser_location", "10 10 0", "find_flesh"),
                                             ("riser_location", "20 20 0", None),
                                             ("spawn_location", "1 2 3", None)])
            self.assertTrue(all(e["targetname"] == "zone_a_spawners" for e in out
                                if e.get("script_noteworthy") in ("spawn_location", "riser_location")))


class BarrierTests(unittest.TestCase):
    def test_goal_target_group_gets_the_inward_window_traversal(self):
        with tempfile.TemporaryDirectory() as temp:
            bsp = Path(temp)
            goal = {"classname": "script_struct", "targetname": "exterior_goal", "target": "win1",
                    "origin": "0 0 0", "angles": "0 90 0"}
            (bsp / "entities.json").write_text(json.dumps({"entities": [
                goal,
                {"classname": "node_negotiation_begin", "targetname": "traverse", "target": "in1", "origin": "0 12.4 0"},
                {"classname": "node_negotiation_begin", "targetname": "traverse", "target": "out1", "origin": "5 5 0"},
            ]}))
            (bsp / "paths.json").write_text(json.dumps({"nodes": [
                {"type": 17, "targetname": "traverse", "target": "in1", "origin": [0, 12.40002, 0]},
                {"type": 18, "targetname": "in1", "target": "", "origin": [0, 90, 0]},
                {"type": 17, "targetname": "traverse", "target": "out1", "origin": [5, 5, 0]},
                {"type": 18, "targetname": "out1", "target": "", "origin": [5, -70, 0]},
            ]}))
            self.assertEqual(entities.link_barrier_traversals(bsp), [])
            nodes = json.loads((bsp / "paths.json").read_text())["nodes"]
            self.assertEqual([n["targetname"] for n in nodes], ["win1", "in1", "traverse", "out1"])
            ents = json.loads((bsp / "entities.json").read_text())["entities"]
            self.assertEqual([e["targetname"] for e in ents], ["exterior_goal", "win1", "traverse"])


if __name__ == "__main__":
    unittest.main()
