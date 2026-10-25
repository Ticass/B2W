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


if __name__ == "__main__":
    unittest.main()
