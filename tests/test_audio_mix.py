"""The stock class mix: WaW buses name BO2 groups, and each class takes its
routing, falloff, priority and occlusion from the stock tables by rank."""
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import audioremaster, sounds

COS = [[0, 1], [.344482, .794328], [.49353, .707946], [.612793, .562341], [.718018, .398107],
       [.815552, .281838], [.908691, .141254], [1, 0]]
DEFAULT = [[0, 1], [.142857, .630957], [.285714, .398107], [.428571, .251189], [.571429, .177828],
           [.714286, .112202], [.857143, .070795], [1, 0]]
ALL_ON = [[0, 1]] + [[1, 1]] * 6 + [[1, 0]]
RISING = [[0, 0], [.3, 0], [.43, .14], [.5, .28], [.57, .4], [.65, .56], [.76, .71], [1, 1]]
HEAD = ("Name,Bus,VolumeGroup,DuckGroup,PanType,VolMax,DistMin,DistMaxDry,DistMaxWet,DryMinCurve,DryMaxCurve,"
        "WetMinCurve,WetMaxCurve,PriorityMin,PriorityMax,OcclusionLevel,LimitCount,EntityLimitCount,LimitType,"
        "EntityLimitType,Pan,ReverbSend,CenterSend,DistanceLpf,StopOnEntDeath,PriorityThresholdMin,"
        "PriorityThresholdMax\n")


def stock(root: Path, lines: list[str]) -> Path:
    """A BO2 mod tools tree holding one zombie alias table and the driver curves."""
    (root / "raw/soundbank/globals").mkdir(parents=True)
    (root / "raw/soundbank/zmb_x.all.aliases.csv").write_text(HEAD + "".join(lines))
    points = {"default": DEFAULT, "allon": ALL_ON, "cos": COS, "sindelay": RISING,
              "alloff": [[0, 0]] + [[1, 0]] * 7}
    (root / "raw/soundbank/globals/curves.csv").write_text(
        "name," + ",".join(f"x{i},y{i}" for i in range(8)) + ",id\n" + "".join(
            f"{name}," + ",".join(f"{x},{y}" for x, y in curve) + ",1\n" for name, curve in points.items()))
    return root


def voice(name: str, near: int, far: int, low: str = "allon", high: str = "default") -> str:
    return (f"{name},bus_voice,grp_voice,snp_voice,3d,87,{near},{far},{far + 100},{low},{high},allon,default,"
            f"70,90,0.35,1,8,reject,oldest,default,72,0,no,no,0,0\n")


def row(name: str, near: float, far: float, curve: list, low: list = ALL_ON, **columns) -> dict:
    out = {"Name": name, "Bus": "bus_voice", "VolumeGroup": "grp_voice", "DuckGroup": "snp_voice",
           "PanType": "3d", "VolMin": "100", "VolMax": "100", "DistMin": str(near), "DistMaxDry": str(far),
           "DistMaxWet": str(far), "DryMinCurve": "defaultmin", "DryMaxCurve": "cos", "WetMinCurve": "defaultmin",
           "WetMaxCurve": "cos", "PriorityMin": "10", "PriorityMax": "25", "OcclusionLevel": "0.5",
           "_waw": {**sounds.falloff_descriptor(near, far, low, curve), "wet_ratio": 1.0}}
    out.update(columns)
    return out


class TransferTests(unittest.TestCase):
    def test_a_value_takes_the_target_of_its_rank(self):
        source = audioremaster.percentiles([float(v) for v in range(101)])
        target = audioremaster.percentiles([float(v) * 10 for v in range(101)])
        self.assertAlmostEqual(audioremaster.transfer(25, source, target), 250)
        self.assertAlmostEqual(audioremaster.transfer(25.5, source, target), 255)
        # outside WaW's range: BO2's end, never an extrapolation
        self.assertEqual(audioremaster.transfer(-5, source, target), 0)
        self.assertEqual(audioremaster.transfer(900, source, target), 1000)

    def test_waws_one_default_lands_on_bo2s_typical_value(self):
        # every WaW voice line is authored priority 25; BO2's sit around 90
        source = audioremaster.percentiles([25.0] * 500)
        target = audioremaster.percentiles([70.0] * 100 + [90.0] * 300 + [100.0] * 100)
        self.assertEqual(audioremaster.transfer(25, source, target), 90)

    def test_order_is_kept(self):
        source = audioremaster.percentiles([100.0, 500.0, 500.0, 1700.0, 1700.0, 1700.0, 6000.0] * 20)
        target = audioremaster.percentiles([float(v) for v in range(200, 1300, 10)])
        moved = [audioremaster.transfer(v, source, target) for v in (100, 500, 1700, 6000)]
        self.assertEqual(moved, sorted(moved))
        self.assertLess(moved[0], moved[-1])


class FalloffTests(unittest.TestCase):
    def test_carry_is_the_curve_not_its_name(self):
        # the same distances carry twice as far on WaW's slow curve
        slow = sounds.falloff_descriptor(75, 1700, ALL_ON, COS)
        fast = sounds.falloff_descriptor(75, 1700, ALL_ON, DEFAULT)
        self.assertTrue(slow["standard"] and fast["standard"])
        self.assertAlmostEqual(slow["falloff"] / fast["falloff"], 1.98, places=1)

    def test_layered_shapes_are_not_ordinary_falloffs(self):
        # a far gunshot layer fades in over DistMin; a near one ends at it
        self.assertFalse(sounds.falloff_descriptor(900, 6000, RISING, DEFAULT)["standard"])
        self.assertFalse(sounds.falloff_descriptor(900, 900, COS, [[0, 0], [1, 0]])["standard"])
        # WaW's two-point defaultmin is BO2's allon
        self.assertTrue(sounds.falloff_descriptor(50, 500, [[0, 1], [1, 1]], DEFAULT)["standard"])


class ClassMixTests(unittest.TestCase):
    def profiles(self, temp: str) -> dict:
        lines = [voice(f"vox_plr_{i}", 125, 1250) for i in range(120)]
        lines += [voice(f"vox_far_{i}", 250, 1600) for i in range(30)]
        # layered rows of the class do not count as its placement
        lines += [voice(f"vox_layer_{i}", 900, 6000, "sindelay") for i in range(20)]
        return audioremaster.stock_profiles(stock(Path(temp), lines))

    def test_stock_class_is_measured_on_ordinary_rows(self):
        with tempfile.TemporaryDirectory() as temp:
            found = self.profiles(temp)["classes"]["grp_voice|3d"]
        self.assertEqual(found["routing"], {"Bus": "bus_voice", "DuckGroup": "snp_voice"})
        self.assertEqual(found["falloff"]["DryMaxCurve"], "default")
        self.assertAlmostEqual(found["falloff"]["area"], sounds.curve_area(DEFAULT))
        self.assertEqual(max(found["quantiles"]["DistMin"]), 250)    # not the layers' 900

    def test_waw_voices_keep_their_reach(self):
        with tempfile.TemporaryDirectory() as temp:
            profiles = self.profiles(temp)
        rows = [row(f"waw/vox_{i}", 75, 1700, COS) for i in range(30)]
        layer = row("waw/fire_dist", 700, 6000, COS, RISING, DryMinCurve="rcurve3")
        flat = row("waw/vox_2d", 120, 600, COS, PanType="2d")
        report = audioremaster.remaster(rows + [layer, flat], {}, profiles)
        placed = rows[0]
        # how far a sound is heard is the map's: distances and curve stay
        self.assertEqual((placed["DistMin"], placed["DistMaxDry"], placed["DryMaxCurve"]), ("75", "1700", "cos"))
        # routing, priority and occlusion move to BO2's scale
        self.assertEqual((placed["Bus"], placed["DuckGroup"]), ("bus_voice", "snp_voice"))
        self.assertEqual((placed["PriorityMin"], placed["PriorityMax"], placed["OcclusionLevel"]),
                         ("70", "90", "0.35"))
        self.assertEqual((layer["DistMin"], layer["DistMaxDry"], layer["DryMinCurve"], layer["DryMaxCurve"]),
                         ("700", "6000", "rcurve3", "cos"))
        self.assertEqual(layer["PriorityMax"], "90")
        self.assertEqual((flat["DistMin"], flat["DistMaxDry"]), ("120", "600"))
        self.assertNotIn("falloff", report["groups"]["grp_voice|3d"])

    def test_ambient_emitters_take_bo2s_reach_by_rank(self):
        with tempfile.TemporaryDirectory() as temp:
            stock_class = self.profiles(temp)["classes"]["grp_voice|3d"]
        rows = [row(f"waw/amb_{i}", 75, 1700, COS) for i in range(30)]
        out = audioremaster._mix_class("grp_voice|3d", rows, {**stock_class, "routing": {}}, None, reach="stock")
        placed = rows[0]
        self.assertEqual((placed["DistMin"], placed["DistMaxDry"], placed["DryMaxCurve"], placed["WetMaxCurve"]),
                         ("125", "1250", "default", "default"))
        self.assertEqual((out["falloff"]["rows"], out["falloff"]["median_dist_max"]), (30, [1700.0, 1250.0]))

    def test_stock_waw_is_the_reference_when_measured(self):
        with tempfile.TemporaryDirectory() as temp:
            profiles = self.profiles(temp)
        # stock WaW: most voices reach 1700, a few 3000. A map holding only
        # the far ones still places them far (its own median would not).
        records = [{"bus": "voice", "spatialized": True, "standard": True, "DistMin": 75.0, "VolMax": 100.0,
                    "falloff": sounds.falloff_descriptor(75, far, ALL_ON, COS)["falloff"], "PriorityMin": 10,
                    "PriorityMax": 25, "OcclusionLevel": .5} for far in [1700] * 90 + [3000] * 10]
        reference = audioremaster.reference_quantiles(sounds.waw_mix_reference(records, {"voice": "grp_voice"}))
        rows = [row(f"waw/far_{i}", 75, 3000, COS) for i in range(3)]
        report = audioremaster.remaster([dict(r) for r in rows], {}, profiles, reference)
        self.assertEqual(report["groups"]["grp_voice|3d"]["level_reference"], "stock_waw")
        stock_class = profiles["classes"]["grp_voice|3d"]
        out = audioremaster._mix_class("grp_voice|3d", rows, {**stock_class, "routing": {}}, reference, reach="stock")
        # BO2's far voices' falloff (1600 - 250) from its typical start (125),
        # not its typical 1250: start and falloff length move separately
        self.assertEqual((rows[0]["DistMin"], rows[0]["DistMaxDry"]), ("125", "1475"))
        self.assertEqual(out["falloff"]["reference"], ["stock_waw", "stock_waw"])

    def test_priority_ends_keep_waws_order(self):
        with tempfile.TemporaryDirectory() as temp:
            profiles = self.profiles(temp)
        profiles["classes"]["grp_voice|3d"]["quantiles"]["PriorityMin"] = [95.0] * 101
        rows = [row(f"waw/vox_{i}", 75, 1700, COS) for i in range(30)]
        audioremaster.remaster(rows, {}, profiles)
        self.assertEqual((rows[0]["PriorityMin"], rows[0]["PriorityMax"]), ("90", "90"))


class BusGroupTests(unittest.TestCase):
    TABLE = {"full_vol": "grp_hdrfx", "projectile": "grp_impacts", "explosion": "grp_explosion",
             "reload": "grp_foley", "rfl_1st": "grp_weapon", "rfl_3rd": "grp_weapon"}

    def test_table_names_the_group_and_keywords_cover_the_rest(self):
        category = lambda bus: sounds._category({"bus": {"name": bus}}, self.TABLE)
        self.assertEqual(category("projectile")[1], "grp_impacts")
        self.assertEqual(category("rfl_1st")[1:3], ("grp_weapon", "snp_wpn_1p"))
        self.assertEqual(category("rfl_3rd")[1:3], ("grp_weapon", "snp_wpn_3p"))
        self.assertEqual(category("custom_voice_bus")[1], "grp_voice")     # not in the table: keyword
        self.assertEqual(sounds._category({"bus": {"name": "projectile"}})[1], "grp_foley")   # no table: as before

    def test_stock_twins_vote_on_a_bus(self):
        n = sounds.REVERB_MIN_SAMPLES
        stock_groups = {f"prj_bullet_impact_small_s{chr(97 + i)}": "grp_impacts" for i in range(n)}
        stock_groups["prj_bullet_impact_small_flesh"] = "grp_player_impacts"
        stock_groups.update({f"fly_step_run_npc_s{chr(97 + i)}": "grp_foley" for i in range(n - 1)})
        waw = {f"bullet_small_s{chr(97 + i)}": "projectile" for i in range(n)}
        waw["bullet_small_flesh"] = "projectile"
        waw.update({f"step_run_s{chr(97 + i)}": "character" for i in range(n - 1)})
        learned = sounds.learn_bus_groups(stock_groups, waw)
        self.assertEqual(learned, {"projectile": {"group": "grp_impacts", "twins": n, "of": n + 1}})   # too few steps

    def test_only_the_catch_all_bus_follows_its_weapon(self):
        driver = {"_game": "T4", "_type": "waw2bo2_sound_globals", "_version": 1,
                  "buses": [{"index": 0, "name": "full_vol", "volumeMod": 1}, {"index": 12, "name": "explosion"}],
                  "curves": [{"index": 8, "name": "default", "pointCount": 2, "points": [[0, 1], [1, 0]] + [[0, 0]] * 6}],
                  "speakerMaps": [{"index": 14, "name": "wpn_all", "maps": [{"volumes": [1, 1]}]}]}
        source = {"_game": "T4", "_type": "waw2bo2_sound", "_version": 2, "name": "custom_fire", "aliases": [{
            "flags": 0, "speakerMap": 14, "volumeFalloffCurve": 8, "volumeMinFalloffCurve": 8,
            "reverbFalloffCurve": 8, "reverbMinFalloffCurve": 8, "soundFile": {"type": 1, "name": "wpn/fire.wav"}}]}
        source["aliases"][0].update(
            aliasName="custom_fire", volMin=.9, volMax=1, distMin=120, distMax=600, distReverbMax=800,
            reverbSend=.5, centerPercentage=0, limitCount=0, entityLimitCount=2, minPriority=90, maxPriority=90,
            minPriorityThreshold=.25, maxPriorityThreshold=1, probability=1, startDelay=0, envelopMin=0,
            envelopMax=0, envelopPercentage=0, occlusionLevel=.5, subtitle="")
        classes = {"bus_groups": self.TABLE, "weapon_dependency_group": {"grp_hdrfx": "grp_weapon"}}
        groups = {}
        for bus in (0, 12):
            source["aliases"][0]["flags"] = (bus << 22) | (1 << 13) | (1 << 6)
            document = sounds.translate(source, driver)
            document["aliases"][0]["pcm_binding"] = {"output": "a.wav", "PitchMinCents": 0.0, "PitchMaxCents": 0.0}
            document["aliases"][0]["t6_curves"] = {k: {"t6": "default"} for k in sounds.T6_CURVE_FIELDS.values()}
            for weapon in (set(), {"custom_fire"}):
                rows, errors = sounds.t6_alias_rows(document, weapon, None, classes)
                self.assertEqual(errors, [])
                groups[bus, bool(weapon)] = rows[0]["VolumeGroup"]
        self.assertEqual(groups, {(0, False): "grp_hdrfx", (0, True): "grp_weapon",
                                  (12, False): "grp_explosion", (12, True): "grp_explosion"})


class TwinTests(unittest.TestCase):
    STEP = ("fly_step_run_npc_dirt,bus_fx,grp_foley,snp_foley,3d,68,100,625,700,allon,default,allon,default,"
            "10,50,0.2,3,2,priority,oldest,default,30,0,yes,no,0,0\n")
    VOCAL = ("zmb_vocals_zombie_ambience,bus_voice,grp_voice,snp_voice,3d,79,75,500,750,allon,default,allon,default,"
             "70,90,0.75,5,1,priority,reject,default,72,0,no,yes,0,0\n")

    def test_engine_copy_plays_where_bo2_plays_that_alias(self):
        with tempfile.TemporaryDirectory() as temp:
            twins = audioremaster.stock_twins(stock(Path(temp), [self.STEP] * 3 + [self.VOCAL]),
                                              ["FLY_STEP_RUN_NPC_DIRT", "fly_step_run_npc_snow"])
        self.assertEqual(list(twins), ["fly_step_run_npc_dirt"])     # snow is not in the stock tables
        rows = [{"Name": "waw/step_run_dirt", "FileSource": "a.wav", "VolMax": "66", "DistMin": "50",
                 "DistMaxDry": "500", "LimitType": "reject", "LimitCount": "10", "PitchMin": "-50"},
                {"Name": "waw/step_run_snow", "FileSource": "b.wav", "VolMax": "66", "DistMin": "50",
                 "DistMaxDry": "500", "LimitType": "reject", "LimitCount": "10", "PitchMin": "-50"}]
        report = sounds.engine_name_copies(rows, ["fly_step_run_npc_dirt", "fly_step_run_npc_snow"], twins)
        self.assertEqual(report["stock_mix"], 1)
        copy = next(r for r in rows if r["Name"] == "fly_step_run_npc_dirt")
        # BO2's placement and voice limits; WaW's file, level and pitch
        self.assertEqual((copy["DistMin"], copy["DistMaxDry"], copy["LimitType"], copy["LimitCount"],
                          copy["EntityLimitCount"], copy["PriorityMax"]), ("100", "625", "priority", "3", "2", "50"))
        self.assertEqual((copy["FileSource"], copy["VolMax"], copy["PitchMin"]), ("a.wav", "66", "-50"))
        self.assertEqual(rows[0]["DistMaxDry"], "500")               # WaW's own alias is untouched
        self.assertEqual(next(r for r in rows if r["Name"] == "fly_step_run_npc_snow")["DistMaxDry"], "500")

    def test_vocal_event_alias_takes_the_stock_events_placement(self):
        with tempfile.TemporaryDirectory() as temp:
            profiles = audioremaster.stock_profiles(stock(Path(temp), [self.VOCAL]), (),
                                                    ["zmb_vocals_zombie_ambience", "zmb_vocals_zombie_sprint"])
        rows = [row("waw/amb_vocals", 50, 1850, COS, VolMax="83")]
        report = audioremaster.remaster(rows, {}, profiles, None, {"waw/amb_vocals": "zmb_vocals_zombie_ambience",
                                                                  "waw/sprint_vocals": "zmb_vocals_zombie_sprint"})
        self.assertEqual((rows[0]["DistMaxDry"], rows[0]["EntityLimitCount"], rows[0]["EntityLimitType"],
                          rows[0]["StopOnEntDeath"], rows[0]["VolMax"]), ("500", "1", "reject", "yes", "83"))
        self.assertEqual(report["twins"], {"placed": {"waw/amb_vocals": "zmb_vocals_zombie_ambience"},
                                           "not_in_stock": []})


class ApiTableTests(unittest.TestCase):
    def test_every_stock_waw_bus_has_a_group_the_linker_knows(self):
        from waw2bo2 import gscport
        api = gscport.API["audio_remaster"]
        buses = {"full_vol", "music", "ambience", "voice", "pis_1st", "pis_3rd", "smg_1st", "smg_3rd", "rfl_1st",
                 "rfl_3rd", "decay", "projectile", "explosion", "vehicle", "character", "physics", "hvy_wpn", "ui",
                 "dist_wpn", "dist_hmg", "banzai", "vehicle_mp", "reload"}       # WaW's driver, in index order
        self.assertEqual(set(api["bus_groups"]), buses)
        header = Path(__file__).parents[1] / "vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/SoundConstantsT6.h"
        if header.is_file():
            known = header.read_text(encoding="utf-8", errors="replace")
            for group in {*api["bus_groups"].values(), *api["weapon_dependency_group"].values()}:
                self.assertIn(f'"{group}"', known)
        self.assertEqual(api["vocal_twins"]["alias"].format(actor="zombie", category="attack"),
                         "zmb_vocals_zombie_attack")


if __name__ == "__main__":
    unittest.main()
