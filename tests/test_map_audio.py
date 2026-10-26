import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import audioremaster, sounds


class EntitySoundTests(unittest.TestCase):
    def test_client_emitters_name_the_converted_alias(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "entities.json"
            path.write_text(json.dumps({"entities": [
                {"classname": "script_struct", "script_label": "line_emitter", "script_sound": "amb_swamp_01_L"},
                {"classname": "script_struct", "script_label": "looper", "script_sound": "comp_loop"},
                {"classname": "trigger_multiple", "targetname": "audio_bump_trigger", "script_sound": "perks_rattle"},
                # a perk machine: its script compares the raw name
                {"classname": "script_model", "targetname": "vending_jugg", "script_sound": "mx_jugger_jingle"},
                {"classname": "script_struct", "script_label": "random", "script_sound": "not_converted"}]}))
            count = sounds.bind_entity_sounds(path, {"amb_swamp_01_L", "comp_loop", "perks_rattle", "mx_jugger_jingle"})
            ents = json.loads(path.read_text())["entities"]
            self.assertEqual(count, 3)
            self.assertEqual([e["script_sound"] for e in ents],
                             ["waw/amb_swamp_01_L", "waw/comp_loop", "waw/perks_rattle", "mx_jugger_jingle", "not_converted"])
            self.assertEqual(ents[0]["script_label"], "line_emitter")


class AmbientEmitterTests(unittest.TestCase):
    def test_emitter_sounds_are_ambience_after_binding(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "entities.json"
            path.write_text(json.dumps({"entities": [
                {"classname": "script_struct", "script_label": "line_emitter", "script_sound": "waw/amb_swamp_01_L"},
                {"classname": "script_struct", "script_label": "random", "script_sound": "amb_frog"},
                {"classname": "trigger_multiple", "targetname": "audio_bump_trigger", "script_sound": "waw/perks_rattle"}]}))
            self.assertEqual(sounds.ambient_emitter_aliases(path), {"amb_swamp_01_l", "amb_frog"})

    def test_ambient_emitters_carry_as_far_as_bo2s(self):
        stock = [{"Name": "amb_a", "PanType": "3d", "Looping": "looping", "DistMin": "100", "DistMaxDry": "400",
                  "DistMaxWet": "450", "DryMinCurve": "allon", "DryMaxCurve": "default", "WetMinCurve": "allon",
                  "WetMaxCurve": "default"}]
        emitters = audioremaster.stock_emitters(stock)
        self.assertEqual(emitters["looping"]["columns"]["DistMaxDry"], "400")
        rows = [{"Name": "waw/cicada_drone_loop", "VolumeGroup": "grp_ambience", "PanType": "3d", "Looping": "looping",
                 "VolMin": "80", "VolMax": "80", "DistMin": "3000", "DistMaxDry": "8500", "DistMaxWet": "9000",
                 "DryMinCurve": "defaultmin", "DryMaxCurve": "cos"},
                {"Name": "waw/amb_spooky_2d", "VolumeGroup": "grp_ambience", "PanType": "2d", "Looping": "looping",
                 "VolMin": "80", "VolMax": "80", "DistMin": "50", "DistMaxDry": "1250"}]
        profiles = {"groups": {}, "ambience": {"mix": {"Bus": "bus_fx", "VolumeGroup": "grp_ambience"},
                                               "volume": 78.0, "rows": 500, "emitters": emitters}}
        report = audioremaster.remaster(rows, {"waw/cicada_drone_loop": "ambience", "waw/amb_spooky_2d": "ambience"},
                                        profiles)
        # WaW's drone was full to 3000 and audible to 8500: BO2's loops end at 400
        self.assertEqual((rows[0]["DistMin"], rows[0]["DistMaxDry"], rows[0]["DryMaxCurve"]), ("100", "400", "default"))
        self.assertEqual(rows[1]["DistMaxDry"], "1250")    # a 2D bed is not placed
        self.assertEqual(report["roles"]["ambience"]["stock_emitter_rows"], 1)


class GroupLevelTests(unittest.TestCase):
    def test_each_group_sits_at_bo2s_level(self):
        rows = [{"Name": "waw/attack_vocals", "VolumeGroup": "grp_voice", "PanType": "3d", "VolMin": "98", "VolMax": "100"},
                {"Name": "waw/amb_vocals", "VolumeGroup": "grp_voice", "PanType": "3d", "VolMin": "90", "VolMax": "96"},
                {"Name": "waw/mx_round", "VolumeGroup": "grp_music", "PanType": "2d", "VolMin": "90", "VolMax": "90"}]
        profiles = {"groups": {"grp_voice|3d": {"volume": 82.0, "rows": 1400}}}
        report = audioremaster.remaster(rows, {"waw/mx_round": "music"}, profiles)
        # median of 100 and 96 is 98: both move by -16, keeping their offset
        self.assertEqual([r["VolMax"] for r in rows[:2]], ["84", "80"])
        self.assertEqual(report["groups"]["grp_voice|3d"]["volume_shift"], -16.0)
        self.assertEqual(rows[2]["VolMax"], "90")    # a role row is not a group row

    def test_ambience_sits_at_bo2s_ambience_level(self):
        rows = [{"Name": "waw/gun", "VolumeGroup": "grp_weapon", "PanType": "3d", "VolMin": "95", "VolMax": "95"},
                {"Name": "waw/amb_swamp", "VolumeGroup": "grp_foley", "PanType": "3d", "VolMin": "95", "VolMax": "95"}]
        profiles = {"groups": {"grp_weapon|3d": {"volume": 81.0, "rows": 4000}},
                    "ambience": {"mix": {"Bus": "bus_fx", "VolumeGroup": "grp_ambience", "PanType": "3d"},
                                 "volume": 70.0, "rows": 500}}
        report = audioremaster.remaster(rows, {"waw/amb_swamp": "ambience"}, profiles)
        # at BO2's ambience median (70), under the gun
        self.assertEqual([r["VolMax"] for r in rows], ["81", "70"])
        self.assertEqual(rows[1]["VolumeGroup"], "grp_ambience")
        self.assertEqual(report["roles"]["ambience"]["volume_shift"], -25.0)


class LoadedBudgetTests(unittest.TestCase):
    def test_played_sounds_load_before_unreached_ones(self):
        with tempfile.TemporaryDirectory() as temp:
            pcm = Path(temp)
            for name, size in (("cash.wav", 300), ("step.wav", 10), ("gun.wav", 500)):
                (pcm / name).write_bytes(b"\0" * size)
            rows = [{"Name": "waw/cha_ching", "FileSource": "cash.wav", "Storage": "loaded"},
                    {"Name": "waw/step_run_dirt", "FileSource": "step.wav", "Storage": "loaded"},
                    {"Name": "waw/gun_fire", "FileSource": "gun.wav", "Storage": "loaded"}]
            budget = {"entries": 2, "bytes": 10000, "played": ["cha_ching"]}
            streamed = sounds.fit_loaded_budget(rows, pcm, budget, {"gun_fire"})
            self.assertEqual([s["alias"] for s in streamed], ["waw/step_run_dirt"])
            self.assertEqual(budget["played_files_loaded"], 1)

    def test_template_bytes_come_from_the_stock_entries(self):
        import struct

        def bank(path, entries):
            header = struct.pack("<8I3q", 0x23585532, 14, 20, 0, 0, len(entries), 0, 0, 0, 2048, 0)
            path.write_bytes(header.ljust(2048, b"\0") + b"".join(
                struct.pack("<4I4b", key, size, 0, 0, 0, 1, 0, 0) for key, size in entries))

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "sound").mkdir()
            source = "raw\\sound\\fly\\step_00.LN65.pc.snd"
            bank(root / "sound" / "zmb_map.all.sabl", [(sounds.snd_hash(source), 300), (7, 700)])
            bank(root / "sound" / "zmb_common.all.sabl", [(9, 50)])
            csv_path = root / "template.csv"
            csv_path.write_text("Name,FileSource,Storage\nfly_step,%s,loaded\nmus,mus.snd,streamed\n" % source)
            budget = sounds.loaded_budget(root, csv_path)
            self.assertEqual((budget["reference_bytes"], budget["template_bytes"], budget["stock_headroom_bytes"]), (1000, 300, 700))
            # entries are one pool over every bank, not a map budget
            self.assertEqual(budget["entries"], sounds.T6_SND_ENTRY_POOL - 1 - 2)
            # without the template bank its measured memory goes to the map
            self.assertEqual(sounds.loaded_budget(root, None)["bytes"], 1000)


class EngineNameTests(unittest.TestCase):
    def test_bo2_engine_names_play_waws_sound_for_the_same_event(self):
        rows = [{"Name": "waw/step_run_dirt", "FileSource": "a.wav"},
                {"Name": "waw/step_run_plr_dirt", "FileSource": "b.wav"},
                {"Name": "waw/bodyfall_wood_large", "FileSource": "c.wav"},
                {"Name": "waw/bullet_large_metal", "FileSource": "d.wav"}]
        report = sounds.engine_name_copies(rows, [
            "fly_step_run_npc_dirt", "fly_step_run_plr_dirt", "fly_step_crouch_run_dirt",
            "fly_bodyfall_large_wood", "prj_bullet_impact_xtreme_metal", "prj_brass_impact_small_dirt",
            "wpn_an94_fire"])
        self.assertEqual(report["copies"], {
            "fly_step_run_npc_dirt": "step_run_dirt", "fly_step_run_plr_dirt": "step_run_plr_dirt",
            "fly_step_crouch_run_dirt": "step_run_dirt", "fly_bodyfall_large_wood": "bodyfall_wood_large",
            "prj_bullet_impact_xtreme_metal": "bullet_large_metal"})
        # WaW has no brass impact; BO2's weapon is not WaW's: silent, not BO2's
        self.assertEqual(report["unmatched"], ["prj_brass_impact_small_dirt", "wpn_an94_fire"])
        copy = next(r for r in rows if r["Name"] == "fly_step_run_npc_dirt")
        self.assertEqual(copy["FileSource"], "a.wav")    # WaW's file, no new bank entry

    def test_the_template_bank_is_neither_linked_nor_shipped(self):
        from waw2bo2 import modzone, package
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            zone = root / "zm_test.zone"
            zone.write_text("soundbank,zmb_test.all\nweapon,knife_zm\n")
            dropped = []
            self.assertNotIn("soundbank", modzone.build_mod_zone(zone, dropped))
            self.assertIn("soundbank,zmb_test.all", dropped)
            map_out, mod_out, dest = root / "map", root / "mod", root / "dest"
            for d in (map_out, mod_out / "sound", dest):
                d.mkdir(parents=True)
            for f in (map_out / "zm_x.ff", map_out / "zm_x.ipak", mod_out / "mod.ff", mod_out / "mod_load.ff",
                      mod_out / "sound/waw_zm_x.all.sabl", mod_out / "sound/zmb_test.all.sabl", dest / "zmb_test.all.sabs"):
                f.write_bytes(b"x")
            package.package("zm_x", map_out, mod_out, dest)
            self.assertTrue((dest / "waw_zm_x.all.sabl").is_file())
            self.assertFalse((dest / "zmb_test.all.sabl").exists())
            self.assertFalse((dest / "zmb_test.all.sabs").exists())


if __name__ == "__main__":
    unittest.main()
