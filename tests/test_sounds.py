import json
import hashlib
import math
import tempfile
import unittest
import zipfile
from pathlib import Path

from waw2bo2 import sounds


class DefaultReverbTests(unittest.TestCase):
    def generate(self, patch):
        from waw2bo2 import t6bridge
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        driver = root / "content_source/sound_ir/driver_globals.json"
        driver.parent.mkdir(parents=True)
        driver.write_text(json.dumps({"reverbPatches": [patch]}))
        report = t6bridge.StageReport("example")
        t6bridge.write_amb_csc(report, "example", root)
        text = (root / "clientscripts/mp/example_amb.csc").read_text()
        return root, report, text

    def test_silent_source_default_restored_without_changing_aliases(self):
        root, report, text = self.generate({"name": "DEFAULT", "room": -10000,
                                          "reflections": -10000, "reverb": -10000})
        self.assertEqual(report.content["default_reverb"]["wet"], 0)
        self.assertIn('declareambientroom( "waw_default", 1 )', text)
        self.assertIn('setambientroomreverb( "waw_default", "default", 1, 0, 0 )', text)
        self.assertIn('setreverb( "snd_enveffectsprio_level", "default", 1, 0, 0 )', text)
        self.assertIn('declaremusicstate( "WAVE" )', text)
        self.assertFalse((root / "soundbank").exists())

    def test_custom_audible_default_is_reported_and_not_muted(self):
        _, report, text = self.generate({"name": "default", "room": -1000,
                                        "reflections": -711, "reverb": 83})
        self.assertEqual(report.content["default_reverb"]["status"], "unsupported_source_default")
        self.assertNotIn('setreverb(', text)
        self.assertNotIn('waw_default', text)
        self.assertTrue(report.warnings)

    def test_missing_source_default_is_not_assumed_silent(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertEqual(sounds.default_reverb_mix(Path(temp))["status"], "missing_source_default")


def fixture():
    globals_data = {"_game": "T4", "_type": "waw2bo2_sound_globals", "_version": 1,
                    "buses": [{"index": 8, "name": "rfl_1st", "volumeMod": .68}],
                    "curves": [{"index": 8, "name": "default", "pointCount": 2,
                                "points": [[0, 1], [1, 0]] + [[0, 0]] * 6}],
                    "speakerMaps": [{"index": 14, "name": "wpn_all", "maps": [{"volumes": [1, 1]}]}]}
    alias = {"flags": (8 << 22) | (1 << 13) | (1 << 7) | (1 << 11), "speakerMap": 14,
             "volumeFalloffCurve": 8, "volumeMinFalloffCurve": 8,
             "reverbFalloffCurve": 8, "reverbMinFalloffCurve": 8,
             "secondaryAliasName": "echo", "soundFile": {"type": 1, "name": "wpn/fire.wav"}}
    return {"_game": "T4", "_type": "waw2bo2_sound", "_version": 2,
            "name": "custom_fire", "aliases": [alias]}, globals_data


class SoundTests(unittest.TestCase):
    def test_script_aliases_follow_variables_and_arrays(self):
        with tempfile.TemporaryDirectory() as temp:
            project = Path(temp)
            folder = project/'maps/mp/waw'
            folder.mkdir(parents=True)
            (folder/'weather.gsc').write_text('main(){ distant = "THUNDER_FARL"; close[0] = "thunder_closeL"; name="missing"; /* "unused" */ }')
            self.assertEqual(sounds.script_aliases(project, {'thunder_farL', 'thunder_closeL', 'unused'}),
                             {'thunder_farL', 'thunder_closeL'})

    def test_flags_are_t4_not_t6(self):
        decoded = sounds.decode_flags((63 << 22) | (7 << 28) | (3 << 13) | (1 << 7))
        self.assertTrue(decoded["realDelay"])
        self.assertFalse(decoded["doppler"])
        self.assertEqual(decoded["busIndex"], 63)
        self.assertEqual(decoded["loadType"], 3)
        self.assertEqual(decoded["moveType"], 7)
        with self.assertRaises(sounds.SoundError):
            sounds.decode_flags(-1)

    def test_driver_values_and_source_fields_preserved(self):
        source, driver = fixture()
        result = sounds.translate(source, driver)
        row = result["aliases"][0]
        self.assertEqual(row["source"], source["aliases"][0])
        self.assertEqual(row["bus"]["volumeMod"], .68)
        self.assertEqual(row["curves"]["volumeFalloffCurve"], driver["curves"][0])
        self.assertEqual(row["references"]["secondaryAliasName"], "waw/echo")
        source["_version"] = 1
        with self.assertRaises(sounds.SoundError):
            sounds.translate(source, driver)

    def test_invalid_reference_and_storage_fail(self):
        source, driver = fixture()
        source["aliases"][0]["soundFile"]["type"] = 2
        with self.assertRaises(sounds.SoundError):
            sounds.translate(source, driver)
        with self.assertRaises(ValueError):
            sounds.audio_name({"dir": "../../outside", "name": "bad.wav"})

    def test_loaded_payload_priority_and_streamed_iwd(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            loaded = root / "dump/sound/wpn/fire.xwma"
            loaded.parent.mkdir(parents=True)
            original = b"RIFF\x00\x00\x00\x00XWMAoriginal"
            loaded.write_bytes(original)
            archive = root / "map.iwd"
            with zipfile.ZipFile(archive, "w") as z:
                z.writestr("sound/wpn/fire.wav", b"stock must not replace loaded")
                z.writestr("sound/ambient/custom.wav", b"original streamed")
            files = sounds.AudioSources([root / "dump"], [archive])
            self.assertEqual(files.read("wpn/fire.wav", True)[0], original)
            self.assertEqual(files.read("ambient/custom.wav", False)[0], b"original streamed")
            self.assertIsNone(files.read("absent.wav", True))

    def test_stage_preserves_original_bytes_and_explicit_partial_status(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, driver = fixture()
            alias_path = root / "source/soundaliases/custom_fire.w2bsnd.json"
            alias_path.parent.mkdir(parents=True)
            alias_path.write_text(json.dumps(source))
            globals_path = root / "source/soundglobals/singleton.w2bsndglobals.json"
            globals_path.parent.mkdir(parents=True)
            globals_path.write_text(json.dumps(driver))
            audio = root / "source/sound/wpn/fire.wav"
            audio.parent.mkdir(parents=True)
            payload = b"RIFF\x00\x00\x00\x00WAVEoriginal PCM"
            audio.write_bytes(payload)
            result = sounds.stage([root / "source"], root / "output", [])
            self.assertEqual(result["errors"], [])
            self.assertEqual(len(result["aliases"]), 1)
            self.assertEqual((root / "output/sound/waw/wpn/fire.wav").read_bytes(), payload)
            self.assertEqual(result["status"], "partial_audio_and_semantic_stage")
            # A fresh source revision can replace generated output, not another
            # asset in this run. This rule must work regardless of map name.
            audio.write_bytes(payload + b"revision")
            sounds.stage([root / "source"], root / "output", [])
            self.assertEqual((root / "output/sound/waw/wpn/fire.wav").read_bytes(), payload + b"revision")

    def test_pcm_binding_preserves_pitch_range_and_rejects_stale_files(self):
        from waw2bo2.audio import canonical_pcm
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, driver = fixture()
            source["aliases"][0].update(pitchMin=.9, pitchMax=1.1)
            ir = sounds.translate(source, driver)
            (root / "sound_ir").mkdir()
            (root / "sound_ir/custom_fire.json").write_text(json.dumps(ir))
            (root / "sounds.stage.json").write_text(json.dumps({"aliases": [{"name": "waw/custom_fire"}]}))
            pcm_root = root / "pcm"
            pcm_root.mkdir()
            pcm = b"\x01\x00" * 10
            (pcm_root / "fire.wav").write_bytes(canonical_pcm(pcm, 1, 44100))
            entry = {"name": "wpn/fire.wav", "output": "fire.wav", "loadType": 1,
                     "pcm_sha256": hashlib.sha256(pcm).hexdigest(),
                     "bank_sample_rate": 44100, "alias_pitch_scale": 22050 / 24000}
            (pcm_root / "audio.stage.json").write_text(json.dumps({"audio": [entry]}))
            report = sounds.bind_pcm(root, pcm_root)
            self.assertEqual(report["bound_variants"], 1)
            result = json.loads((root / "sound_bound_ir/custom_fire.json").read_text())
            row = result["aliases"][0]
            self.assertEqual(row["source"], source["aliases"][0])
            binding = row["pcm_binding"]
            self.assertAlmostEqual(binding["pitchMin"], .9 * entry["alias_pitch_scale"])
            self.assertAlmostEqual(binding["pitchMax"], 1.1 * entry["alias_pitch_scale"])
            self.assertAlmostEqual(binding["PitchMaxCents"], 1200 * math.log2(1.1 * entry["alias_pitch_scale"]))
            (pcm_root / "fire.wav").write_bytes(canonical_pcm(b"\x00\x00", 1, 44100))
            report = sounds.bind_pcm(root, pcm_root)
            self.assertEqual(report["bound_variants"], 0)
            self.assertIn("stale PCM", report["errors"][0]["reason"])
            # File existence alone cannot rescue a missing current decode.
            (pcm_root / "audio.stage.json").write_text(json.dumps({"audio": []}))
            report = sounds.bind_pcm(root, pcm_root)
            self.assertIn("current manifest", report["errors"][0]["reason"])

    def test_pcm_binding_never_clamps_unsupported_pitch(self):
        from waw2bo2.audio import canonical_pcm
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, driver = fixture()
            source["aliases"][0].update(pitchMin=1, pitchMax=3)
            (root / "sound_ir").mkdir()
            (root / "sound_ir/custom_fire.json").write_text(json.dumps(sounds.translate(source, driver)))
            (root / "sounds.stage.json").write_text(json.dumps({"aliases": [{"name": "waw/custom_fire"}]}))
            (root / "fire.wav").write_bytes(canonical_pcm(b"\x00\x00", 1, 44100))
            entry = {"name": "wpn/fire.wav", "output": "fire.wav", "pcm_sha256": hashlib.sha256(b"\x00\x00").hexdigest(),
                     "bank_sample_rate": 44100, "alias_pitch_scale": 1}
            (root / "audio.stage.json").write_text(json.dumps({"audio": [entry]}))
            report = sounds.bind_pcm(root, root)
            self.assertEqual(report["bound_variants"], 0)
            self.assertIn("exceeds native", report["errors"][0]["reason"])


def t6_driver(path: Path, curves: list[tuple[str, list]]) -> Path:
    """Native sidecar with only a curve table; other tables empty."""
    import struct
    data = bytearray(sounds.T6_DRIVER_MAGIC)
    for table, stride in enumerate(sounds.T6_DRIVER_STRIDES):
        rows = curves if table == 1 else []
        data += struct.pack("<II", len(rows), stride)
        for index, (name, points) in enumerate(rows):
            flat = [v for p in points for v in p]
            data += name.encode().ljust(32, b"\0") + struct.pack("<I16f", index, *flat)
    path.write_bytes(bytes(data))
    return path


LINEAR = [[i / 7, 1 - i / 7] for i in range(8)]
T6_DEFAULT = [[0, 1], [1 / 7, .630957], [2 / 7, .398107], [3 / 7, .251189], [4 / 7, .177828],
              [5 / 7, .112202], [6 / 7, .070795], [1, 0]]
T6_DEFAULTMIN = [[0, 1]] + [[1, 1]] * 6 + [[1, 0]]


class CurveTests(unittest.TestCase):
    def test_sidecar_is_read_in_engine_order_and_validated(self):
        with tempfile.TemporaryDirectory() as temp:
            path = t6_driver(Path(temp) / "d.w2bsdg", [("default", T6_DEFAULT), ("fade", LINEAR)])
            curves = sounds.read_t6_curves(path)
            self.assertEqual([(c["index"], c["name"]) for c in curves], [(0, "default"), (1, "fade")])
            self.assertAlmostEqual(curves[0]["points"][1][1], .630957, places=6)
            path.write_bytes(path.read_bytes() + b"x")
            with self.assertRaises(sounds.SoundError):
                sounds.read_t6_curves(path)
            path.write_bytes(b"W2BSDG2\0")
            with self.assertRaises(sounds.SoundError):
                sounds.read_t6_curves(path)

    def test_curves_bind_by_shape_never_by_name(self):
        t6 = [{"index": 0, "name": "default", "points": T6_DEFAULT},
              {"index": 1, "name": "fade", "points": LINEAR}]
        # Same name, different data: unsupported. Different name, same data
        # (two-point form of a linear fade): bound.
        source = [{"index": 8, "name": "default", "pointCount": 8, "points": LINEAR},
                  {"index": 2, "name": "curve2", "pointCount": 2, "points": [[0, 1], [1, 0]] + [[0, 0]] * 6},
                  {"index": 5, "name": "curve3", "pointCount": 3, "points": [[0, 1], [.5, .9], [1, 0]] + [[0, 0]] * 5}]
        table = sounds.translate_curves(source, t6)
        self.assertEqual((table["default"]["status"], table["default"]["t6"]), ("exact_shape", "fade"))
        self.assertEqual((table["curve2"]["status"], table["curve2"]["index"]), ("exact_shape", 1))
        self.assertEqual(table["curve3"]["status"], "UNSUPPORTED_SOUND_CURVE")
        self.assertNotIn("index", table["curve3"])
        approximated = sounds.translate_curves(source, t6, approximate=True)["curve3"]
        self.assertEqual(approximated["status"], "APPROXIMATED_SOUND_CURVE")
        self.assertGreater(approximated["maxError"], sounds.CURVE_TOLERANCE)

    def test_vertical_step_endpoint_is_flagged(self):
        t6 = [{"index": 1, "name": "defaultmin", "points": T6_DEFAULTMIN}]
        source = [{"index": 9, "name": "defaultmin", "pointCount": 2, "points": [[0, 1], [1, 1]] + [[0, 0]] * 6}]
        entry = sounds.translate_curves(source, t6)["defaultmin"]
        self.assertEqual(entry["status"], "exact_shape")
        self.assertEqual(entry["endpoint"], "evaluator_dependent")
        with self.assertRaises(sounds.SoundError):
            sounds.translate_curves([{"index": 0, "name": "bad", "pointCount": 2,
                                      "points": [[0, 1], [.5, 0]] + [[0, 0]] * 6}], t6)

    def test_approximation_preserves_audible_tail_and_smooth_cutoff(self):
        source = [{"index": 0, "name": "fade", "pointCount": 3,
                   "points": [[0, 1], [.5, .1], [1, 0]]}]
        targets = [{"index": 0, "name": "quiet", "points": [[0, 1], [.5, .001], [1, 0]]},
                   {"index": 1, "name": "audible", "points": [[0, 1], [.5, .3], [1, 0]]},
                   {"index": 2, "name": "cutoff", "points": [[0, 1], [1, 1], [1, 0]]}]
        self.assertLess(sounds.compare_curves(source[0], targets[0])["maxError"],
                        sounds.compare_curves(source[0], targets[1])["maxError"])
        entry = sounds.translate_curves(source, targets, approximate=True)["fade"]
        self.assertEqual(entry["t6"], "audible")
        self.assertLess(entry["rmsDbError"], sounds._curve_db_error(source[0], targets[0]))
        with self.assertRaises(sounds.SoundError):
            sounds.translate_curves(source, [])

    def test_curve_comparison_measures_both_sides_of_internal_steps(self):
        source = {"name": "step", "points": [[0, 1], [.5, 1], [.5, 0], [1, 0]]}
        target = {"name": "fade", "points": [[0, 1], [.5, 0], [1, 0]]}
        self.assertEqual(sounds.compare_curves(source, target)["maxError"], 1)

    def test_bind_curves_marks_variants_incomplete_without_every_curve(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, driver = fixture()
            driver["curves"].append({"index": 9, "name": "defaultmin", "pointCount": 2,
                                     "points": [[0, 1], [1, 1]] + [[0, 0]] * 6})
            source["aliases"][0].update(volumeMinFalloffCurve=9, reverbMinFalloffCurve=9)
            (root / "sound_bound_ir").mkdir()
            (root / "sound_bound_ir/custom_fire.json").write_text(json.dumps(sounds.translate(source, driver)))
            (root / "sounds.bindings.json").write_text(json.dumps(
                {"aliases": [{"name": "waw/custom_fire", "output": "sound_bound_ir/custom_fire.json"}]}))
            path = t6_driver(root / "d.w2bsdg", [("default", T6_DEFAULT), ("defaultmin", T6_DEFAULTMIN)])
            report = sounds.bind_curves(root, path)
            self.assertEqual((report["complete_variants"], report["incomplete_variants"]), (0, 1))
            row = json.loads((root / "sound_bound_ir/custom_fire.json").read_text())["aliases"][0]
            self.assertEqual(row["t6_curves"]["DryMinCurve"]["index"], 1)
            self.assertEqual(sorted(row["t6_curves_missing"]), ["reverbFalloffCurve", "volumeFalloffCurve"])
            self.assertEqual(report["errors"][0]["curve"], "default")
            path = t6_driver(root / "d.w2bsdg", [("default", T6_DEFAULT), ("defaultmin", T6_DEFAULTMIN),
                                                 ("fade", LINEAR)])
            report = sounds.bind_curves(root, path)
            self.assertEqual((report["complete_variants"], report["errors"]), (1, []))
            self.assertEqual(sounds.bind_curves(root, root / "absent.w2bsdg")["status"], "missing_t6_sound_driver")


class PlaybackBankTests(unittest.TestCase):
    def bound(self):
        source, driver = fixture()
        source["aliases"][0].update(
            aliasName="custom_fire", volMin=.9, volMax=1, distMin=120, distMax=600, distReverbMax=800,
            reverbSend=.5, centerPercentage=0, limitCount=0, entityLimitCount=2, minPriority=90, maxPriority=90,
            minPriorityThreshold=.25, maxPriorityThreshold=1, probability=1, startDelay=0, envelopMin=0,
            envelopMax=0, envelopPercentage=0, occlusionLevel=.5, subtitle="")
        document = sounds.translate(source, driver)
        document["aliases"][0]["pcm_binding"] = {"output": "sound/waw/wpn/fire.wav", "PitchMinCents": -100.0,
                                                 "PitchMaxCents": 0.0}
        document["aliases"][0]["t6_curves"] = {k: {"t6": "default", "index": 0} for k in sounds.T6_CURVE_FIELDS.values()}
        return document

    def test_rows_use_namespaced_names_and_bo2_units(self):
        rows, errors = sounds.t6_alias_rows(self.bound())
        self.assertEqual(errors, [])
        row = rows[0]
        self.assertEqual(sorted(row), sorted(sounds.T6_ALIAS_COLUMNS))
        self.assertEqual((row["Name"], row["Secondary"]), ("waw/custom_fire", "waw/echo"))
        self.assertAlmostEqual(float(row["VolMin"]), 100 + 20 * math.log10(.9 * .68), places=4)
        self.assertAlmostEqual(float(row["VolMax"]), 100 + 20 * math.log10(.68), places=4)
        self.assertEqual(row["DuckGroup"], "snp_wpn_1p")
        self.assertAlmostEqual(float(row["ReverbSend"]), 100 + 20 * math.log10(.5), places=4)
        self.assertEqual((row["DistMaxDry"], row["DistMaxWet"], row["PitchMin"]), ("600", "800", "-100"))
        self.assertEqual((row["Storage"], row["LimitCount"], row["EntityLimitCount"]), ("loaded", "0", "2"))
        self.assertEqual(row["PanType"], "2d")

    def test_linear_gains_survive_native_t6_volume_encoding(self):
        document = self.bound()
        document["aliases"][0]["bus"]["volumeMod"] = 1.0
        for gain in (0, .01, .1, .25, .5, .9, 1):
            for field in ("volMin", "volMax", "reverbSend", "centerPercentage", "envelopPercentage"):
                document["aliases"][0]["source"][field] = gain
            rows, errors = sounds.t6_alias_rows(document)
            self.assertEqual(errors, [])
            sounds.clamp_t6_ranges(rows)
            for column in ("VolMin", "VolMax", "ReverbSend", "CenterSend", "EnvelopPercent"):
                # Same conversion and quantization as the native T6 loader.
                native = int(10 ** ((float(rows[0][column]) - 100) / 20) * 65535) / 65535
                self.assertAlmostEqual(native, gain, delta=2 / 65535)

    def test_voice_limits_preserve_unlimited_reject_and_priority(self):
        document = self.bound()
        source = document["aliases"][0]["source"]
        for value, name in ((0, "none"), (1, "oldest"), (2, "reject"), (3, "priority"), (4, "priority")):
            source.update(limitType=value, entityLimitType=value, limitCount=0, entityLimitCount=0)
            rows, errors = sounds.t6_alias_rows(document)
            self.assertEqual(errors, [])
            self.assertEqual((rows[0]["LimitType"], rows[0]["EntityLimitType"]), (name, name))
            self.assertEqual((rows[0]["LimitCount"], rows[0]["EntityLimitCount"]), ("0", "0"))
        source["limitType"] = 99
        rows, errors = sounds.t6_alias_rows(document)
        self.assertEqual(rows, [])
        self.assertIn("voice limit", errors[0]["reason"])

    def test_unbound_variants_are_errors_not_rows(self):
        document = self.bound()
        del document["aliases"][0]["t6_curves"]["WetMaxCurve"]
        rows, errors = sounds.t6_alias_rows(document)
        self.assertEqual(rows, [])
        self.assertIn("curves", errors[0]["reason"])

    def test_weapon_dependencies_share_a_category_even_with_custom_full_volume_bus(self):
        document = self.bound()
        document["aliases"][0]["bus"].update(name="full_vol", volumeMod=1)
        rows, errors = sounds.t6_alias_rows(document, {"custom_fire"})
        self.assertEqual(errors, [])
        self.assertEqual((rows[0]["VolumeGroup"], rows[0]["DuckGroup"]), ("grp_weapon", "snp_wpn_1p"))

    def test_loaded_audio_is_resampled_to_48k(self):
        from waw2bo2 import audio
        tool = audio.find_decoder(None)
        if tool is None:
            self.skipTest("no FFmpeg for resampling")
        import array
        samples = array.array("h", (int(8000 * math.sin(2 * math.pi * 1000 * i / 44100)) for i in range(4410)))
        out = audio.resample(samples.tobytes(), 1, 44100, 48000, tool)
        self.assertLessEqual(abs(len(out) // 2 - 4800), 2)


if __name__ == "__main__":
    unittest.main()


class T6ColumnFormatTests(unittest.TestCase):
    def test_integer_columns_and_16bit_distances(self):
        from waw2bo2 import sounds
        row = {c: "0" for c in sounds.T6_ALIAS_COLUMNS}
        row.update(Name="a", DistMin="500000", PitchMin="-12.4", VolMin="37.5")
        changed = sounds.clamp_t6_ranges([row])
        self.assertEqual((row["DistMin"], row["PitchMin"], row["VolMin"]), ("65535", "-12", "37.5"))
        self.assertEqual({c["column"]: c["why"] for c in changed}, {"DistMin": "range", "PitchMin": "integer column"})


class LoadedBudgetTests(unittest.TestCase):
    def test_weapon_audio_is_kept_loaded_before_shorter_non_weapon_sounds(self):
        with tempfile.TemporaryDirectory() as temp:
            pcm = Path(temp)
            for name, size in (("tiny.wav", 2), ("shot.wav", 10), ("reload.wav", 8), ("large.wav", 30)):
                (pcm / name).write_bytes(b"x" * size)
            rows = [{"Name": "waw/" + n, "FileSource": f, "Storage": "loaded"} for n, f in
                    (("ambient", "tiny.wav"), ("fire", "shot.wav"), ("reload", "reload.wav"), ("too_big", "large.wav"))]
            budget = {"entries": 2, "bytes": 18}
            sounds.fit_loaded_budget(rows, pcm, budget, {"fire", "reload", "too_big"})
            self.assertEqual([r["Storage"] for r in rows], ["streamed", "loaded", "loaded", "streamed"])
            self.assertEqual((budget["weapon_files_loaded"], budget["weapon_files_streamed"]), (2, 1))

    def test_shortest_loaded_sounds_stay_loaded_within_budget(self):
        import tempfile
        from pathlib import Path
        from waw2bo2 import sounds
        with tempfile.TemporaryDirectory() as temp:
            pcm = Path(temp)
            for name, size in (("a.wav", 10), ("b.wav", 20), ("c.wav", 30), ("d.wav", 5)):
                (pcm / name).write_bytes(b"x" * size)
            rows = [{"Name": n, "FileSource": f, "Storage": s} for n, f, s in (
                ("shot", "a.wav", "loaded"), ("shot2", "a.wav", "loaded"), ("reload", "b.wav", "loaded"),
                ("vox", "c.wav", "loaded"), ("music", "d.wav", "streamed"))]
            budget = {"entries": 5, "bytes": 30}
            streamed = sounds.fit_loaded_budget(rows, pcm, budget)
            self.assertEqual([r["Storage"] for r in rows], ["loaded", "loaded", "loaded", "streamed", "streamed"])
            self.assertEqual([s["alias"] for s in streamed], ["vox"])
            self.assertEqual((budget["loaded_files"], budget["loaded_bytes"], budget["streamed_files"]), (2, 30, 1))
