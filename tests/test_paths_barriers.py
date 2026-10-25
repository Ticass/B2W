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
            '{\n"classname" "script_struct"\n"targetname" "exterior_goal"\n"origin" "40 40 0"\n}',
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

    def test_zone_dog_structs_become_dog_locations(self):
        # WaW zone manager: zone.dog_locations = structs named volume.target + "_dog"
        ents = "\n".join([
            '{\n"classname" "info_volume"\n"targetname" "zone_a"\n"target" "zone_a_spawners"\n}',
            '{\n"classname" "script_struct"\n"targetname" "zone_a_spawners_dog"\n"origin" "5 6 7"\n}',
            '{\n"classname" "script_struct"\n"targetname" "unrelated_dog"\n"origin" "8 8 8"\n}',
        ])
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "map.ents"
            path.write_text(ents)
            entities.write_entities(path, "proj", Path(temp) / "BSP")
            out = json.loads((Path(temp) / "BSP/entities.json").read_text())["entities"]
            dogs = [(e["targetname"], e["origin"]) for e in out if e.get("script_noteworthy") == "dog_location"]
            self.assertEqual(dogs, [("zone_a_spawners", "5 6 7")])
            # the source struct stays for the map's own lookups
            self.assertTrue(any(e.get("targetname") == "zone_a_spawners_dog" for e in out))


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
                {"classname": "node_negotiation_end", "targetname": "in1", "origin": "0 90 0"},
            ]}))
            (bsp / "paths.json").write_text(json.dumps({"nodes": [
                {"type": 17, "targetname": "traverse", "target": "in1", "origin": [0, 12.40002, 0]},
                {"type": 18, "targetname": "in1", "target": "", "origin": [0, 90, 0]},
                {"type": 17, "targetname": "traverse", "target": "out1", "origin": [5, 5, 0]},
                {"type": 18, "targetname": "out1", "target": "", "origin": [5, -70, 0]},
            ]}))
            self.assertEqual(entities.link_barrier_traversals(bsp), [])
            nodes = json.loads((bsp / "paths.json").read_text())["nodes"]
            self.assertEqual([n["targetname"] for n in nodes],
                             ["win1", "in1", "traverse", "out1", "in1_waw2bo2_window"])
            self.assertEqual(nodes[0]["target"], "in1_waw2bo2_window")
            ents = json.loads((bsp / "entities.json").read_text())["entities"]
            self.assertEqual([e["targetname"] for e in ents],
                             ["exterior_goal", "win1", "traverse", "in1", "in1_waw2bo2_window"])
            self.assertEqual(ents[1]["target"], "in1_waw2bo2_window")
            self.assertEqual(ents[2]["target"], "out1")
            self.assertEqual((ents[4]["classname"], ents[4]["origin"]), ("node_negotiation_end", "0 90 0"))

    def test_window_traversal_lands_on_its_own_end_below_the_hub(self):
        flags = 40
        def link(node, negotiation=0):
            return {"node": node, "dist": 10.0, "disconnectCount": 0, "negotiationLink": negotiation, "flags": flags}
        nodes = [
            {"type": 17, "targetname": "win", "target": "hub", "origin": [0, 0, 0], "links": [link(1, 1), link(2)]},
            {"type": 18, "targetname": "hub", "target": "", "origin": [0, 70, 0], "links": [link(3)]},
            {"type": 0, "targetname": "", "target": "", "origin": [50, -40, 0], "links": [link(0)]},
            {"type": 0, "targetname": "", "target": "", "origin": [0, 200, 0], "links": [link(1)]},
        ]
        count = len(nodes)
        # arbitrary visibility, hub (1) sees 3 but not 2
        seen = {(0, 1), (0, 2), (1, 3), (2, 3)}
        vis = bytearray((count * (count - 1) + 7) // 8)
        for lo, hi in seen:
            k = lo * count + hi
            vis[k >> 3] |= 1 << (k & 7)
        data = {"nodeCount": count, "visBytes": len(vis), "pathVis": vis.hex(), "nodes": nodes}
        self.assertEqual(paths.split_window_ends(data, [0]), [("hub", "hub_waw2bo2_window")])
        new = data["nodes"][4]
        self.assertEqual((data["nodeCount"], new["type"], new["origin"]), (5, 18, [0, 70, 0]))
        self.assertEqual([(l["node"], l["negotiationLink"]) for l in new["links"]], [(1, 0)])
        self.assertEqual([(l["node"], l["negotiationLink"]) for l in nodes[0]["links"]], [(4, 1), (2, 0)])
        self.assertEqual(nodes[0]["target"], "hub_waw2bo2_window")
        self.assertEqual(nodes[1]["links"], [link(3)])  # the hub keeps its inside links
        out = bytes.fromhex(data["pathVis"])
        self.assertEqual(data["visBytes"], (5 * 4 + 7) // 8)
        for a in range(count):
            for b in range(count):
                self.assertEqual(paths.vis_bit(out, 5, a, b), paths.vis_bit(bytes(vis), count, a, b))
        # the new end sees what its hub sees
        self.assertEqual([paths.vis_bit(out, 5, 4, x) for x in range(4)], [1, 1, 0, 1])


class WaterCollisionTests(unittest.TestCase):
    def test_water_only_triangles_leave_collision_but_water_with_clip_stays(self):
        from waw2bo2 import fbx
        self.assertTrue(fbx.water_only(0x08000020))   # WaW water surface (detail + water)
        self.assertTrue(fbx.water_only(0x20))
        self.assertFalse(fbx.water_only(0x20A0))      # water + missile/shot clip: stock keeps it
        self.assertFalse(fbx.water_only(0x08000001))  # detail solid
        self.assertFalse(fbx.water_only(0))


if __name__ == "__main__":
    unittest.main()
