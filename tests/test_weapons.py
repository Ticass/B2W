import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import weapons


class WeaponTests(unittest.TestCase):
    def test_infostring_roundtrip(self):
        fields = {"displayName": "A custom gun", "notetrackSoundMap": "mag_out custom_mag\nbolt custom_bolt"}
        self.assertEqual(weapons.read_info(weapons.write_info(fields)), fields)
        with self.assertRaises(weapons.WeaponError):
            weapons.read_info("WEAPONFILE\\name\\x\\name\\y")

    def test_semantic_sound_and_fov_translation(self):
        src = {"fireSound": "custom/fire", "gunModel": "custom/gun", "adsZoomFov": "65", "knifeModel": "knife"}
        t4 = {"fireSound": "CSPFT_SOUND", "gunModel": "CSPFT_XMODEL", "adsZoomFov": "CSPFT_FLOAT",
              "knifeModel": "CSPFT_XMODEL"}
        t6 = {"fireSound": "CSPFT_STRING", "gunModel": "CSPFT_XMODEL", "adsZoomFov1": "CSPFT_FLOAT"}
        out = weapons.convert("custom", src, t4, t6)
        self.assertEqual(out.fields, {"fireSound": "waw/custom/fire", "gunModel": "waw_xmodel/custom/gun",
                                      "adsZoomFov1": "65", "adsZoomFov2": "65", "adsZoomFov3": "65"})
        self.assertEqual(out.dependencies["sound"], {"custom/fire"})
        self.assertIn("knifeModel", out.unsupported)

    def test_unsafe_names(self):
        for name in ("../outside", "foo/../../bar", "C:/outside", "\\outside", "foo\nweapon,bar", "a,b"):
            with self.subTest(name=name), self.assertRaises(weapons.WeaponError):
                weapons.output_name("xmodel", name)

    def test_rifle_profile_changes_but_original_animation_does_not(self):
        schema = {"playerAnimType": "WFT_ANIMTYPE", "idleAnim": "WFT_ANIM_NAME"}
        out = weapons.convert("rifle", {"playerAnimType": "autorifle", "idleAnim": "custom_idle"}, schema, schema)
        self.assertEqual(out.fields["playerAnimType"], "default")
        self.assertEqual(out.fields["idleAnim"], "waw_xanim/custom_idle")
        self.assertEqual(out.translations["playerAnimType"]["source"], "autorifle")

    def test_skeleton_is_preserved_and_multiple_roots_are_not_stripped(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "source"
            (root / "xmodel").mkdir(parents=True)
            (root / "model_export").mkdir()
            model = {"_game": "t4", "flags": 0, "lods": [{"distance": 10, "file": "model_export/gun.gltf"}]}
            data = {"nodes": [{"name": "tag_view", "children": [1]}, {"name": "tag_weapon"}],
                    "skins": [{"joints": [0, 1]}], "materials": [{"name": "custom_skin"}],
                    "images": [{"uri": "../images/preview.dds"}],
                    "meshes": [{"primitives": [{"attributes": {"JOINTS_0": 1, "WEIGHTS_0": 2}}]}]}
            (root / "xmodel/gun.json").write_text(json.dumps(model))
            (root / "model_export/gun.gltf").write_text(json.dumps(data))
            project = Path(temp) / "valid"
            report = weapons.stage_models([root], project, {"gun"})
            self.assertEqual(len(report["models"]), 1)
            staged = json.loads((project / "model_export/waw_xmodel/gun_lod0.gltf").read_text())
            self.assertEqual(staged["skins"], data["skins"])
            self.assertEqual(staged["meshes"], data["meshes"])
            self.assertEqual(staged["materials"][0]["name"], "waw_material/custom_skin")
            data["nodes"][0].pop("children")
            (root / "model_export/gun.gltf").write_text(json.dumps(data))
            invalid = Path(temp) / "invalid"
            report = weapons.stage_models([root], invalid, {"gun"})
            self.assertIn("gun", report["unsupported"])
            self.assertFalse((invalid / "xmodel/waw_xmodel/gun.json").exists())

    def test_staging_priority_alternate_closure_and_original_animations(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            roots = [base / "map", base / "stock"]
            for root in roots:
                (root / "weapons").mkdir(parents=True)
                (root / "xanim").mkdir()
            (roots[0] / "weapons/custom").write_text(weapons.write_info(
                {"displayName": "Custom", "idleAnim": "custom_idle", "altWeapon": "alternate"}))
            (roots[1] / "weapons/custom").write_text(weapons.write_info({"displayName": "Wrong source"}))
            (roots[1] / "weapons/alternate").write_text(weapons.write_info({"displayName": "Alternate"}))
            original = b"original animation bytes"
            (roots[0] / "xanim/custom_idle").write_bytes(original)
            project = base / "project"
            report = weapons.stage(roots, project, {"custom"})
            self.assertEqual(len(report["weapons"]), 2)
            self.assertEqual((project / "xanim/waw_xanim/custom_idle").read_bytes(), original)
            output = weapons.read_info((project / "weapons/custom").read_text())
            self.assertEqual(output["displayName"], "Custom")
            self.assertEqual(output["altWeapon"], "alternate")
            self.assertEqual(report["status"], "partial_source_stage")
            self.assertEqual(json.loads((project / "weapons.stage.json").read_text())["missing_animations"], [])


if __name__ == "__main__":
    unittest.main()
