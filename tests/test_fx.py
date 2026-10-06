"""WaW -> BO2 effect translation rules (waw2bo2.fx, waw2bo2.fxmaterials)."""
import copy
import unittest
import tempfile
import json
from pathlib import Path

from waw2bo2 import fx, fxmaterials, techsets, t6bridge


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
    def overlay_effect(self):
        model = t4_elem(6, visuals=[{'model': 'machine_on'}])
        model['spawnOneShot'] = [1, 0]
        model['spawnDelayMsec'] = [0, 0]
        for sample in model['visSamples']:
            sample['base']['scale'] = 1
            sample['amplitude']['scale'] = 0
        return fx.convert_effect(t4_effect([t4_elem(), model, t4_elem(7)], looping=1))[0]

    def test_overlay_variant_preserves_original_and_other_effects(self):
        effect = self.overlay_effect()
        before = copy.deepcopy(effect)
        [(model, variant)] = fx.model_overlay_variants(effect)
        self.assertEqual(model, 'machine_on')
        self.assertEqual(effect, before)
        self.assertEqual(variant['elemDefs'], [effect['elemDefs'][0], effect['elemDefs'][2]])
        self.assertEqual(variant['elemDefCountLooping'], 1)
        self.assertEqual(variant['elemDefCountOneShot'], 1)
        self.assertEqual(variant['msecNonLoopingLife'], effect['msecNonLoopingLife'])
        self.assertFalse(variant['flags'] & fx.EF_MODEL)
        self.assertTrue(variant['flags'] & fx.EF_OMNI)
        self.assertEqual(variant['totalSize'], fx.t6_total_size(variant))

    def test_overlay_never_reuses_moving_physical_random_or_child_models(self):
        for field, value in (('flags', 0x08000000), ('flags', 0x200),
                             ('spawnOneShot', [2, 0]), ('spawnDelayMsec', [1, 0]),
                             ('effectOnDeath', 'another/fx'), ('spawnSound', 'sound')):
            effect = self.overlay_effect()
            effect['elemDefs'][1][field] = value
            self.assertFalse(fx.model_overlay_variants(effect), (field, value))
        effect = self.overlay_effect()
        effect['elemDefs'][1]['velSamples'][0]['world']['velocity']['base'][0] = 1
        self.assertFalse(fx.model_overlay_variants(effect))
        effect = self.overlay_effect()
        effect['elemDefs'][1]['visSamples'][0]['base']['scale'] = 2
        self.assertFalse(fx.model_overlay_variants(effect))

    def test_overlay_metadata_survives_asset_table_and_repeat_staging(self):
        effect = self.overlay_effect()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = fx.fx_file(root, effect['name']); path.parent.mkdir(parents=True)
            path.write_text(json.dumps(effect))
            assets = root / 'maps/mp/waw/_waw2bo2_assets.gsc'
            assets.parent.mkdir(parents=True)
            assets.write_text('init()\n{\n    level.waw2bo2_fx["env/test"] = "waw/env/test";\n'
                              '    level.waw2bo2_weapons["gun"] = "waw_gun";\n}\n')
            report = t6bridge.StageReport(project='test', fx_table={'env/test': effect['name']})
            names = t6bridge.stage_model_overlay_fx(report, root)
            text = assets.read_text()
            self.assertEqual(t6bridge.stage_model_overlay_fx(report, root), names)
            self.assertEqual(assets.read_text(), text)
            self.assertIn('level.waw2bo2_fx["env/test"] = "waw/env/test";', text)
            self.assertIn('level.waw2bo2_weapons["gun"] = "waw_gun";', text)
            self.assertEqual(report.content['model_overlay_fx'][0]['model'], 'machine_on')

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


class EffectModelIsolation(unittest.TestCase):
    def test_model_reference_is_owned_and_source_is_unchanged(self):
        source = t4_effect([t4_elem(elem_type=6, visuals=[{"model": "shared_machine"}])], looping=0)
        before = copy.deepcopy(source)
        converted, _ = fx.convert_effect(source)
        self.assertEqual(converted["elemDefs"][0]["visuals"], [{"model": "waw_fx_model/shared_machine"}])
        self.assertEqual(converted["elemDefs"][0]["lifeSpanMsec"], source["elemDefs"][0]["lifeSpanMsec"])
        self.assertEqual(source, before)

    def test_owned_model_reuses_exact_source_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / "xmodel").mkdir()
            source = root / "xmodel/shared_machine.json"
            source.write_text(json.dumps({"_game": "t6", "lods": [{"file": "model_export/source.gltf"}], "contents": 1}))
            report = t6bridge.StageReport(project="test")
            staged = t6bridge.stage_fx_models(report, root, {"shared_machine"})
            self.assertEqual(staged, {"waw_fx_model/shared_machine"})
            self.assertEqual(source.read_bytes(), (root / "xmodel/waw_fx_model/shared_machine.json").read_bytes())
            self.assertEqual(report.content["fx_model_assets"][0]["lods"], ["model_export/source.gltf"])
            self.assertEqual(report.errors, [])

    def test_missing_source_model_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = t6bridge.StageReport(project="test")
            self.assertEqual(t6bridge.stage_fx_models(report, Path(tmp), {"shared_machine"}), set())
            self.assertTrue(report.errors)


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


class EffectImageStreaming(unittest.TestCase):
    def test_effect_images_use_stock_effect_streaming_mode(self):
        import json
        import tempfile
        from pathlib import Path
        from waw2bo2 import t6bridge
        with tempfile.TemporaryDirectory() as tmp:
            path = t6bridge.write_image_streaming(Path(tmp), {"fxt_smk_gen", "fxt_fx_raygun_ring"})
            data = json.loads(path.read_text())
        self.assertEqual(path.name, "streaming.json")
        self.assertEqual(data, {"streamingMode": {"fxt_fx_raygun_ring": 2, "fxt_smk_gen": 2}})

    def test_hud_images_merge_into_this_runs_effect_images(self):
        import json
        import tempfile
        from pathlib import Path
        from waw2bo2 import t6bridge
        with tempfile.TemporaryDirectory() as tmp:
            t6bridge.write_image_streaming(Path(tmp), {"fxt_smk_gen"})
            path = t6bridge.write_image_streaming(Path(tmp), {"hud_icon_driver"}, merge=True)
            data = json.loads(path.read_text())
        self.assertEqual(data["streamingMode"], {"fxt_smk_gen": 2, "hud_icon_driver": 2})

    def test_prefixed_effect_image_is_staged_from_the_waw_image(self):
        # BO2 ships images with the same names (e.g. fxt_smk_def_3 in common_zm),
        # so effect materials reference a prefixed copy of the WaW pixels.
        import json
        import struct
        import tempfile
        from pathlib import Path
        from waw2bo2 import t6bridge
        header = bytearray(128)
        header[:4] = b'DDS '
        struct.pack_into('<7I', header, 4, 124, 0x100F, 4, 4, 0, 1, 1)
        struct.pack_into('<2I4s5I', header, 76, 32, 0x41, b'\0\0\0\0', 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000)
        with tempfile.TemporaryDirectory() as tmp:
            root, src = Path(tmp) / "out", Path(tmp) / "src"
            src.mkdir()
            (src / "fxt_smk_def_3.dds").write_bytes(bytes(header) + bytes(64))
            mat = root / "materials" / "waw_fx" / "smoke.json"
            mat.parent.mkdir(parents=True)
            mat.write_text(json.dumps({"textures": [{"image": t6bridge.FX_IMAGE_PREFIX + "fxt_smk_def_3"}]}))
            report = t6bridge.StageReport("p", materials=[{"file": "materials/waw_fx/smoke.json"}])
            written = t6bridge.stage_images(report, [src], root)
            self.assertIn("waw_fx/fxt_smk_def_3", written)
            self.assertTrue((root / "images" / "waw_fx" / "fxt_smk_def_3.iwi").exists())
            self.assertFalse([e for e in report.errors if "fxt_smk_def_3" in e])
