"""WaW -> BO2 effect translation rules (waw2bo2.fx, waw2bo2.fxmaterials)."""
import copy
import unittest

from waw2bo2 import fx, fxmaterials, techsets


def _state(color):
    return {"color": color, "rotationDelta": 0.0, "rotationTotal": 0.0, "size": [1.0, 1.0], "scale": 0.0}


def _vec():
    return {"base": [0.0, 0.0, 0.0], "amplitude": [0.0, 0.0, 0.0]}


def t4_elem(elem_type=0, flags=0x2 | 0x4 | 0x40, visuals=None, entry_count=4, trail=None):
    rng = [0.0, 0.0]
    return {
        "flags": flags, "spawnLooping": [100, 1], "spawnOneShot": [100, 1], "spawnRange": rng,
        "fadeInRange": rng, "fadeOutRange": rng, "spawnFrustumCullRadius": 10.0, "spawnDelayMsec": [100, 50],
        "lifeSpanMsec": [1000, 200], "spawnOrigin": [rng, rng, rng], "spawnOffsetRadius": rng,
        "spawnOffsetHeight": rng, "spawnAngles": [rng, rng, rng], "angularVelocity": [rng, rng, rng],
        "initialRotation": rng, "gravity": rng, "reflectionFactor": rng,
        "atlas": {"behavior": 1, "index": 0, "fps": 0, "loopCount": 1, "colIndexBits": 1, "rowIndexBits": 1,
                  "entryCount": entry_count},
        "windInfluence": 0.0, "elemType": elem_type, "visualCount": len(visuals or [{"material": "m"}]),
        "velIntervalCount": 1, "visStateIntervalCount": 1,
        "velSamples": [{"local": {"velocity": _vec(), "totalDelta": _vec()},
                        "world": {"velocity": _vec(), "totalDelta": _vec()}}] * 2,
        "visSamples": [{"base": _state([10, 20, 30, 40]), "amplitude": _state([1, 2, 3, 4])}] * 2,
        "visuals": visuals if visuals is not None else [{"material": "m"}],
        "collMins": [0.0, 0.0, 0.0], "collMaxs": [0.0, 0.0, 0.0],
        "effectOnImpact": "", "effectOnDeath": "", "effectEmitted": "",
        "emitDist": rng, "emitDistVariance": rng, "trail": trail, "sortOrder": 5, "lightingFrac": 0,
        "useItemClip": 0,
    }


def t4_effect(elems, looping=None, name="env/test"):
    return {"_type": "waw2bo2_fx", "_game": "T4", "_version": 1, "name": name, "flags": 0, "totalSize": 0,
            "msecLoopingLife": 0, "elemDefCountLooping": len(elems) if looping is None else looping,
            "elemDefCountOneShot": 0 if looping is None else len(elems) - looping, "elemDefCountEmission": 0,
            "efPriority": 0, "elemDefs": elems}


class ElementTranslation(unittest.TestCase):
    def test_flags_shift_after_run_mode(self):
        e = t4_elem(flags=0x2 | 0xC0 | 0x100 | 0x200 | 0x400 | 0x800 | 0x04000000 | 0x80000000)
        out, _ = fx.convert_effect(t4_effect([e]))
        self.assertEqual(out["elemDefs"][0]["flags"],
                         0x2 | 0xC0 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x04000000 | 0x80000000)

    def test_unknown_flag_bits_are_reported(self):
        out, notes = fx.convert_effect(t4_effect([t4_elem(flags=0x2 | 0x20000000)]))
        self.assertEqual(out["elemDefs"][0]["flags"], 0x2)
        self.assertTrue(any("0x20000000" in n for n in notes))

    def test_colors_bgra_to_rgba(self):
        out, _ = fx.convert_effect(t4_effect([t4_elem()]))
        self.assertEqual(out["elemDefs"][0]["visSamples"][0]["base"]["color"], [30, 20, 10, 40])

    def test_atlas_packing(self):
        out, _ = fx.convert_effect(t4_effect([t4_elem(entry_count=64)]))
        self.assertEqual(out["elemDefs"][0]["atlas"]["entryCountAndIndexRange"], 64 | 1 << 9)
        out, _ = fx.convert_effect(t4_effect([t4_elem(entry_count=0)]))
        self.assertEqual(out["elemDefs"][0]["atlas"]["entryCountAndIndexRange"], 0)

    def test_type_map(self):
        # WaW 3 line, 4 trail, 5 cloud, 7 omni light, 11 runner
        self.assertEqual([fx.ELEM_TYPE_MAP[t] for t in (0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11)],
                         [0, 1, 3, 4, 5, 6, 7, 8, 10, 11, 12])

    def test_trail_requires_data(self):
        with self.assertRaises(fx.FxConvertError):
            fx.convert_effect(t4_effect([t4_elem(elem_type=4)]))
        trail = {"scrollTimeMsec": 0, "repeatDist": 0, "splitDist": 0, "verts": [], "inds": []}
        out, _ = fx.convert_effect(t4_effect([t4_elem(elem_type=4, trail=trail)]))
        self.assertEqual(out["elemDefs"][0]["trail"], trail)
        self.assertTrue(out["flags"] & fx.EF_TRAIL)

    def test_references_renamed(self):
        runner = t4_elem(elem_type=11, visuals=[{"effect": "misc/child"}])
        runner["effectOnDeath"] = "misc/death"
        out, _ = fx.convert_effect(t4_effect([runner]), {"m": "waw_fx/m"})
        e = out["elemDefs"][0]
        self.assertEqual(e["visuals"], [{"effect": "waw/misc/child"}])
        self.assertEqual(e["effectOnDeath"], "waw/misc/death")
        self.assertEqual(out["name"], "waw/env/test")
        self.assertTrue(out["flags"] & fx.EF_RUNNER)
        out, _ = fx.convert_effect(t4_effect([t4_elem()]), {"m": "waw_fx/m"})
        self.assertEqual(out["elemDefs"][0]["visuals"], [{"material": "waw_fx/m"}])

    def test_sound_elements_not_substituted(self):
        out, notes = fx.convert_effect(t4_effect([t4_elem(elem_type=9, visuals=[{"sound": "shell_eject"}])]))
        self.assertEqual(out["elemDefs"][0]["visuals"], [{"sound": "waw/shell_eject"}])
        self.assertTrue(any(n.startswith("UNSUPPORTED_SOUND") or "UNSUPPORTED_SOUND" in n for n in notes))

    def test_spot_lights_rejected(self):
        with self.assertRaises(fx.FxConvertError):
            fx.convert_effect(t4_effect([t4_elem(elem_type=8, visuals=[{}])]))

    def test_sizes_and_life(self):
        out, _ = fx.convert_effect(t4_effect([t4_elem(), t4_elem()], looping=1))
        # 76 + 2 * (292 + 2 * 96 + 2 * 48) + strings(name + material-free)
        self.assertEqual(out["totalSize"], 76 + 2 * (292 + 192 + 96) + len("waw/env/test") + 1)
        self.assertEqual(out["msecNonLoopingLife"], 100 + 50 + 1000 + 200)

    def test_input_is_not_modified(self):
        src = t4_effect([t4_elem()])
        before = copy.deepcopy(src)
        fx.convert_effect(src)
        self.assertEqual(src, before)


class EffectMaterials(unittest.TestCase):
    def test_waw_features(self):
        self.assertEqual(fxmaterials.waw_features("effect_zfeather_falloff_add_nofog_eyeoffset"),
                         ({"zfeather", "falloff", "add", "eyeoffset"}, {"nofog"}))
        self.assertEqual(fxmaterials.waw_features("particle_cloud_outdoor_add")[0],
                         {"particlecloud", "outdoor", "add"})
        self.assertEqual(fxmaterials.waw_features("distortion_scale_zfeather")[0], {"distortion", "zfeather"})
        with self.assertRaises(techsets.TechsetError):
            fxmaterials.waw_features("effect_something_new")

    def test_blend_class(self):
        self.assertEqual(fxmaterials.blend_class({"blendOpRgb": "add", "srcBlendRgb": "one", "dstBlendRgb": "one"}),
                         "add")
        self.assertEqual(fxmaterials.blend_class({"blendOpRgb": "add", "srcBlendRgb": "srcalpha",
                                                  "dstBlendRgb": "invsrcalpha"}), "blend")
        self.assertEqual(fxmaterials.blend_class({"blendOpRgb": "disabled"}), "opaque")


if __name__ == "__main__":
    unittest.main()
