import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import remaster, t6bridge, techsets
from waw2bo2.launcher import Settings
from waw2bo2.t6bridge import StageReport


class MatchTests(unittest.TestCase):
    INDEX = {"mc/mtl_p6_zm_nuked_bed": ("mc/mtl_p6_zm_nuked_bed", "zm_nuked.ff"),
             "mc/mtl_lights_glass_v2_on": ("mc/mtl_lights_glass_v2_on", "zm_buried.ff"),
             "mlv/mtl_p6_zm_nuked_couch_01": ("mlv/mtl_p6_zm_nuked_couch_01", "zm_nuked.ff"),
             "wc/zm_nuked_metal_ribbed": ("wc/zm_nuked_metal_ribbed", "zm_nuked.ff"),
             "mc/mtl_exact": ("mc/mtl_exact", "common_mp.ff"),
             "mc/bo2_mtl_exact": ("mc/bo2_mtl_exact", "mp_la.ff")}

    def test_remaster_prefix_is_stripped_after_an_exact_name(self):
        self.assertEqual(remaster.bo2_candidates("mc/bo2_mtl_x"), ["mc/bo2_mtl_x", "mc/mtl_x"])
        self.assertEqual(remaster.bo2_candidates("mc/mtl_x"), ["mc/mtl_x"])
        found = {m.waw: m.bo2 for m in remaster.find_matches(["mc/bo2_mtl_exact"], self.INDEX)}
        self.assertEqual(found, {"mc/bo2_mtl_exact": "mc/bo2_mtl_exact"})

    def test_only_model_materials_of_the_same_class(self):
        names = ["mc/bo2_mtl_p6_zm_nuked_bed", "mc/bo1_mtl_lights_glass_v2_on",
                 "mc/bo2_mtl_p6_zm_nuked_couch_01",      # BO2 only has it as mlv/
                 "wc/bo2_zm_nuked_metal_ribbed",          # world: WaW lighting path
                 "mc/unrelated"]
        found = {m.waw: (m.bo2, m.zone.name) for m in remaster.find_matches(names, self.INDEX)}
        self.assertEqual(found, {"mc/bo2_mtl_p6_zm_nuked_bed": ("mc/mtl_p6_zm_nuked_bed", "zm_nuked.ff"),
                                 "mc/bo1_mtl_lights_glass_v2_on": ("mc/mtl_lights_glass_v2_on", "zm_buried.ff")})

    def test_option_round_trips_and_defaults_off(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "settings.json"
            self.assertFalse(Settings().remaster)
            Settings(remaster=True).save(path)
            self.assertTrue(Settings.load(path).remaster)


class StageTests(unittest.TestCase):
    def test_remastered_material_replaces_a_defined_waw_material(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            native = root / "set"
            mat = native / techsets.oat_material_path("mc/mtl_bed")
            mat.parent.mkdir(parents=True)
            mat.write_text(json.dumps({"techniqueSet": "mc_lit_x", "thermalMaterial": "mc/mtl_bed_thermal", "textures": [
                {"name": "colorMap", "image": "bed_c"}, {"name": "specularMap", "image": "bed_s"},
                {"name": "x", "image": ",$white"}]}))
            for image in ("bed_c", "bed_s"):
                path = native / techsets.oat_image_path(image)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"IWi\x1b" + bytes(28))
            stock = root / "stock" / "materials"
            donor = stock / "mc" / "donor.json"
            donor.parent.mkdir(parents=True)
            donor.write_text(json.dumps({"techniqueSet": "mc_lit_donor", "textures": []}))
            report = StageReport("p")
            used = t6bridge.stage_materials(report, {"mc/bo2_mtl_bed"}, [root / "waw"], stock, root / "out",
                                            remaster={"mc/bo2_mtl_bed": mat})
            self.assertEqual(used, {"mc_lit_x"})
            out = json.loads((root / "out" / techsets.oat_material_path("mc/bo2_mtl_bed")).read_text())
            self.assertEqual([t["image"] for t in out["textures"]],
                             ["waw_world/bo2_remaster/bed_c", "waw_world/bo2_remaster/bed_s", ",$white"])
            self.assertTrue(report.materials[0]["remastered"])
            self.assertNotIn("thermalMaterial", out)     # WaW materials have no thermal view
            self.assertTrue(any(w.startswith("REMASTER material mc/bo2_mtl_bed") for w in report.warnings))
            for image in ("bed_c", "bed_s"):
                self.assertTrue((root / "out" / techsets.oat_image_path("waw_world/bo2_remaster/" + image)).is_file())

    def test_technique_sets_come_from_the_remaster_set_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            native = root / "set"
            (native / "remaster.json").parent.mkdir(parents=True)
            (native / "remaster.json").write_text("{}")
            (native / "techniquesets").mkdir()
            (native / "techniquesets" / "mc_lit_x.json").write_text(json.dumps({"techniques": [
                {"passArray": [{"pixelShader": {"name": "p"}, "vertexShader": {"name": "v"}}]}]}))
            (native / "shader_bin").mkdir()
            for shader in ("ps_p.cso", "vs_v.cso"):
                (native / "shader_bin" / shader).write_bytes(b"x")
            (root / "dump").mkdir()
            report = StageReport("p")
            t6bridge.verify_techsets(report, {"mc_lit_x"}, root / "dump", root / "out", [native])
            self.assertEqual(report.errors, [])
            self.assertTrue((root / "out" / "techniquesets" / "mc_lit_x.json").is_file())
            self.assertTrue((root / "out" / "shader_bin" / "ps_p.cso").is_file())


if __name__ == "__main__":
    unittest.main()
