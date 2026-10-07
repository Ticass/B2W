import copy
import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import bo2equiv, weapons


def write_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data))


class RuntimeWeaponTests(unittest.TestCase):
    def test_primary_frags_use_stock_weapon_without_carrying_ghost_assets(self):
        frag = {"weaponType": "grenade", "offhandClass": "Frag Grenade",
                "offhandSlot": "Lethal grenade", "projectileModel": "waw_xmodel/absent"}
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            project = self.project(base, {
                "fraggrenade": frag, "stielhandgranate": frag,
                "zombie_cymbal_monkey": {"weaponType": "grenade", "offhandClass": "Frag Grenade"},
                "custom_special_grenade": {"weaponType": "grenade", "offhandClass": "Frag Grenade"},
                "gun": {"altWeapon": "fraggrenade"}}, missing=["absent"])
            result = weapons.stage_runtime(project, "map_mod")
            self.assertEqual(result["replacements"], {
                "fraggrenade": "frag_grenade_zm", "stielhandgranate": "frag_grenade_zm"})
            self.assertEqual(result["weapons"], ["custom_special_grenade", "gun", "zombie_cymbal_monkey"])
            self.assertEqual(result["excluded"], {})
            self.assertEqual(result["zone_lines"].count("weapon,frag_grenade_zm"), 1)
            for name in ("fraggrenade", "stielhandgranate"):
                self.assertFalse((project / "weapons" / name).exists())
                self.assertTrue((project / "weapons_not_carried" / name).exists())
                self.assertNotIn(f"weapon,{name}", result["zone_lines"])
            self.assertEqual(weapons.read_info((project / "weapons/gun").read_text())["altWeapon"],
                             "frag_grenade_zm")

    def test_tactical_grenade_is_not_replaced_even_with_stock_frag_name(self):
        self.assertIsNone(weapons.primary_frag_replacement("fraggrenade", {
            "weaponType": "grenade", "offhandClass": "Frag Grenade", "offhandSlot": "Tactical grenade"}))

    def project(self, base: Path, weapon_fields: dict[str, dict], models=(), missing=(), unsupported=None,
                failed_materials=None):
        project = base / "project"
        for name, fields in weapon_fields.items():
            (project / "weapons").mkdir(parents=True, exist_ok=True)
            (project / "weapons" / name).write_text(weapons.write_info(fields))
        write_json(project / "weapons.stage.json", {
            "weapons": [{"name": n} for n in weapon_fields],
            "models": {"models": [{"waw": m, "name": f"waw_xmodel/{m}", "materials": ["gun_mtl"]} for m in models],
                       "missing": list(missing), "unsupported": unsupported or {}},
            "missing_animations": [], "missing_accuracy_graphs": []})
        write_json(project / "weapons.visuals.json", {
            "failed_materials": failed_materials or {},
            "material_images": {"waw_material/gun_mtl": ["waw_image/gun_col"]}})
        return project

    def equivalents(self, base: Path, bo2_models=(), table=None, loaded=None):
        raw = base / "bo2" / "raw" / "xmodel"
        raw.mkdir(parents=True)
        for m in bo2_models:
            (raw / f"{m}.json").write_text("{}")
        table_file = base / "table.json"
        table_file.write_text(json.dumps({"xmodel": table or {}}))
        return bo2equiv.Bo2Equivalents(base / "bo2", loaded, table_file)

    def test_absent_models_use_bo2_equivalent_or_clear_variant_slot(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            project = self.project(base, {"gun": {
                "gunModel": "waw_xmodel/gun_view", "gunModel2": "waw_xmodel/t6_same_name",
                "gunModel3": "waw_xmodel/community_bottle", "gunModel4": "waw_xmodel/nowhere",
                "idleAnim": "waw_xanim/gun_idle", "viewFlashEffect": "waw/muzzle",
                "worldFlashEffect": "waw/not_converted"}},
                models=["gun_view"], missing=["t6_same_name", "community_bottle", "nowhere"])
            eq = self.equivalents(base, ["t6_same_name", "t6_bottle"], {"community_bottle": "t6_bottle"})
            result = weapons.stage_runtime(project, "map_mod", eq, {}, {"muzzle"})
            self.assertEqual(result["weapons"], ["gun"])
            fields = weapons.read_info((project / "weapons/gun").read_text())
            self.assertEqual(fields["gunModel2"], "t6_same_name")
            self.assertEqual(fields["gunModel3"], "t6_bottle")
            self.assertEqual(fields["gunModel4"], "")
            self.assertEqual(fields["worldFlashEffect"], "")
            self.assertEqual(fields["fireIntroAnim"], "")   # T6-only char* field: never left NULL
            self.assertTrue(any(n.startswith("gun: BO2_FALLBACK xmodel t6_same_name") for n in result["notes"]))
            self.assertEqual(result["zone_lines"][:3], [">level.ipak_read,map_mod", ">ipak,map_mod",
                                                        "image,waw_image/gun_col"])
            self.assertIn("xanim,waw_xanim/gun_idle", result["zone_lines"])
            self.assertIn("fx,,waw/muzzle", result["zone_lines"])
            self.assertEqual(result["zone_lines"][-1], "weapon,gun")

    def test_missing_primary_model_excludes_weapon_and_its_alternate_owner(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            project = self.project(base, {
                "launcher": {"gunModel": "waw_xmodel/gun_view", "altWeapon": "grenade"},
                "grenade": {"gunModel": "waw_xmodel/absent_view"}},
                models=["gun_view"], missing=["absent_view"])
            result = weapons.stage_runtime(project, "map_mod", self.equivalents(base), {})
            self.assertEqual(result["weapons"], [])
            self.assertIn("absent_view", result["excluded"]["grenade"][0])
            self.assertIn("alternate weapon grenade", result["excluded"]["launcher"][0])
            self.assertEqual(result["zone_lines"], [])

    def test_unconverted_model_material_excludes_but_loaded_bo2_asset_is_no_equivalent(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            project = self.project(base, {"gun": {"gunModel": "waw_xmodel/gun_view"},
                                          "other": {"gunModel": "waw_xmodel/loaded_one"}},
                                   models=["gun_view"], missing=["loaded_one"],
                                   failed_materials={"gun_mtl": ["no donor"]})
            eq = self.equivalents(base, ["loaded_one"], loaded={"xmodel": {"loaded_one"}})
            result = weapons.stage_runtime(project, "map_mod", eq, {})
            self.assertIn("gun", result["excluded"])
            self.assertIn("other", result["excluded"])

    def test_bo2_name_collision_is_renamed_and_null_weapon_maps_to_bo2(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            project = self.project(base, {
                "zombie_knuckle_crack": {"gunModel": "waw_xmodel/gun_view"},
                "gl": {"gunModel": "waw_xmodel/gun_view", "altWeapon": "zombie_knuckle_crack"},
                "none": {"displayName": ""}}, models=["gun_view"])
            result = weapons.stage_runtime(project, "map_mod", self.equivalents(base), {"weapon": {"none"}},
                                           reserved_weapons={"zombie_knuckle_crack"})
            self.assertEqual(result["table"], {"zombie_knuckle_crack": "waw_zombie_knuckle_crack", "gl": "gl",
                                               "none": "none"})
            self.assertIn("weapon,waw_zombie_knuckle_crack", result["zone_lines"])
            self.assertNotIn("weapon,none", result["zone_lines"])
            self.assertFalse((project / "weapons/zombie_knuckle_crack").exists())
            fields = weapons.read_info((project / "weapons/gl").read_text())
            self.assertEqual(fields["altWeapon"], "waw_zombie_knuckle_crack")

    def test_class_named_anim_types_become_default_and_unknown_ones_are_reported(self):
        t4 = {"playerAnimType": "WFT_ANIMTYPE"}
        t6 = {"playerAnimType": "WFT_ANIMTYPE"}
        self.assertEqual(weapons.convert("a", {"playerAnimType": "mg"}, t4, t6).fields["playerAnimType"], "default")
        self.assertEqual(weapons.convert("a", {"playerAnimType": "hold"}, t4, t6).fields["playerAnimType"], "hold")
        odd = weapons.convert("a", {"playerAnimType": "banzai"}, t4, t6)
        self.assertNotIn("playerAnimType", odd.fields)
        self.assertIn("playerAnimType", odd.unsupported)

    def test_rigid_roots_with_one_bind_transform_merge_under_tag_origin(self):
        rot = [-0.7071068, 0.0, 0.0, 0.7071068]
        nodes = [{"name": "tag_fx", "rotation": rot}, {"name": "tag_origin", "rotation": rot},
                 {"name": "surf0", "skin": 0}, {"name": "skel", "children": [2, 0, 1]}]
        gltf = {"nodes": copy.deepcopy(nodes), "skins": [{"joints": [0, 1]}]}
        self.assertTrue(weapons._merge_equal_roots(gltf, [0, 1]))
        self.assertEqual(gltf["nodes"][1]["children"], [0])
        self.assertNotIn("rotation", gltf["nodes"][0])
        self.assertEqual(gltf["nodes"][3]["children"], [2, 1])
        different = {"nodes": copy.deepcopy(nodes), "skins": [{"joints": [0, 1]}]}
        different["nodes"][0]["translation"] = [1.0, 0.0, 0.0]
        self.assertFalse(weapons._merge_equal_roots(different, [0, 1]))
        self.assertEqual(different["nodes"][3]["children"], [2, 0, 1])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "src"
            (root / "xmodel").mkdir(parents=True)
            (root / "model_export").mkdir()
            (root / "model_export/p.gltf").write_text(json.dumps({"nodes": nodes, "skins": [{"joints": [0, 1]}]}))
            for kind, staged in (("rigid", True), ("animated", False)):
                (root / "xmodel/p.json").write_text(json.dumps({"type": kind, "lods": [{"file": "model_export/p.gltf"}]}))
                report = weapons.stage_models([root], Path(temp) / kind, {"p"})
                self.assertEqual(bool(report["models"]), staged)
                self.assertEqual("p" in report["reparented_roots"], staged)

    def test_physpreset_carries_original_values(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            root = base / "dump"
            (root / "physic").mkdir(parents=True)
            (root / "physic/default").write_text(
                "PHYSIC\\mass\\10\\bounce\\0.5\\sndAliasPrefix\\phys_crate\\gravityScale\\1")
            staged = {}
            name = weapons.stage_physpreset([root], base / "out", "default", staged)
            self.assertEqual(name, "waw_physpreset/default")
            text = (base / "out/physic/waw_physpreset/default").read_text()
            self.assertEqual(text, "PHYSIC\\mass\\10\\bounce\\0.5\\sndAliasPrefix\\waw/phys_crate\\gravityScale\\1")
            with self.assertRaises(weapons.WeaponError):
                weapons.stage_physpreset([root], base / "out", "absent", staged)


if __name__ == "__main__":
    unittest.main()
