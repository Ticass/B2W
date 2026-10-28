import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import weapons


class WeaponTests(unittest.TestCase):
    def test_tesla_bolt_retains_electric_trail_without_t6_shell_impact(self):
        schema = {'weaponType': 'WFT_WEAPONTYPE', 'impactType': 'WFT_IMPACT_TYPE',
                  'projTrailEffect': 'CSPFT_FX', 'projExplosionEffect': 'CSPFT_FX',
                  'explosionInnerDamage': 'CSPFT_INT'}
        source = {'weaponType': 'projectile', 'impactType': 'tank_shell',
                  'projTrailEffect': 'maps/zombie/fx_zombie_tesla_electric_bolt',
                  'projExplosionEffect': '', 'explosionInnerDamage': '1'}
        result = weapons.convert('custom_tesla', source, schema, schema)
        self.assertEqual(result.fields['impactType'], 'none')
        self.assertEqual(result.fields['projExplosionEffect'], '')
        self.assertEqual(result.fields['explosionInnerDamage'], '1')
        self.assertEqual(result.fields['projTrailEffect'], 'waw/' + source['projTrailEffect'])
        # the upgraded Wunderwaffe's own trail gets the same rule
        result = weapons.convert('custom_tesla_upgraded', {
            **source, 'projTrailEffect': 'maps/zombie/fx_zombie_tesla_ug_elec_bolt'}, schema, schema)
        self.assertEqual(result.fields['impactType'], 'none')
        # An authored explosion, or a bullet weapon, is not suppressed.
        for changes in ({'projExplosionEffect': 'custom/explosion'}, {'weaponType': 'bullet'}):
            result = weapons.convert('custom', {**source, **changes}, schema, schema)
            self.assertEqual(result.fields['impactType'], 'tank_shell')

    def test_shared_ammo_groups_preserve_limits_without_engine_collisions(self):
        schema = {"sharedAmmoCapName": "CSPFT_STRING", "sharedAmmoCap": "CSPFT_INT",
                  "ammoName": "CSPFT_STRING", "clipName": "CSPFT_STRING"}
        def convert(name, capacity, group="WEAPON_GROUP"):
            return weapons.convert(name, {"sharedAmmoCapName": group, "sharedAmmoCap": capacity,
                                          "ammoName": "ammo", "clipName": "clip"}, schema, schema)
        custom, stock, peer = convert("custom", "4"), convert("stock", "2"), convert("peer", "4")
        self.assertNotEqual(custom.fields["sharedAmmoCapName"], "WEAPON_GROUP")
        self.assertNotEqual(custom.fields["sharedAmmoCapName"], stock.fields["sharedAmmoCapName"])
        self.assertEqual(custom.fields["sharedAmmoCapName"], peer.fields["sharedAmmoCapName"])
        self.assertEqual(custom.fields["sharedAmmoCap"], "4")
        self.assertEqual(stock.fields["sharedAmmoCap"], "2")
        self.assertEqual(custom.fields["ammoName"], "waw_ammo")
        self.assertEqual(custom.fields["clipName"], "waw_clip")
        self.assertEqual(convert("empty", "0", "").fields["sharedAmmoCapName"], "")
        self.assertIn("sharedAmmoCapName", custom.translations)

    def test_waw_scope_overlay_reaches_t6_streamed_overlay(self):
        # WaW ptrs41_zombie: adsOverlayShader scope_overlay_mp (CSPFT_MATERIAL);
        # T6 streams it and scales it by adsOverlayAlphaScale (stock snipers: 1)
        t4 = {"adsOverlayShader": "CSPFT_MATERIAL", "adsOverlayReticle": "WFT_OVERLAYRETICLE"}
        t6 = {"adsOverlayShader": "CSPFT_MATERIAL_STREAM", "adsOverlayReticle": "WFT_OVERLAYRETICLE",
              "adsOverlayAlphaScale": "CSPFT_FLOAT"}
        out = weapons.convert("ptrs41_zombie", {"adsOverlayShader": "scope_overlay_mp",
                                                "adsOverlayReticle": "crosshair"}, t4, t6)
        self.assertEqual(out.fields["adsOverlayShader"], "waw_material/scope_overlay_mp")
        self.assertEqual(out.fields["adsOverlayAlphaScale"], "1")
        self.assertIn("scope_overlay_mp", out.dependencies["material"])
        self.assertNotIn("adsOverlayShader", out.unsupported)
        plain = weapons.convert("gun", {"adsOverlayReticle": "none"}, t4, t6)
        self.assertNotIn("adsOverlayAlphaScale", plain.fields)

    def test_view_kick_recentres_while_crouched_and_prone(self):
        # Der Riese: crouched, the camera kept the kick (T6 scale omitted = 0)
        t4 = {"hipViewKickPitchMin": "CSPFT_FLOAT", "adsViewKickPitchMin": "CSPFT_FLOAT"}
        t6 = dict(t4, adsViewKickCenterDuckedScale="CSPFT_FLOAT", adsViewKickCenterProneScale="CSPFT_FLOAT")
        gun = weapons.convert("zombie_thompson", {"hipViewKickPitchMin": "35", "adsViewKickPitchMin": "60"}, t4, t6)
        self.assertEqual(gun.fields["adsViewKickCenterDuckedScale"], "1")
        self.assertEqual(gun.fields["adsViewKickCenterProneScale"], "1")
        grenade = weapons.convert("stielhandgranate", {"hipViewKickPitchMin": "0"}, t4, t6)
        self.assertNotIn("adsViewKickCenterDuckedScale", grenade.fields)

    def test_waw_offhand_class_gets_t6_inventory_slot(self):
        schema = {"offhandClass": "WFT_OFFHAND_CLASS"}
        for cls, slot in (("Frag Grenade", "Lethal grenade"),
                          ("Smoke Grenade", "Tactical grenade"),
                          ("Flash Grenade", "Tactical grenade")):
            converted = weapons.convert("custom", {"offhandClass": cls}, schema, schema)
            self.assertEqual(converted.fields["offhandSlot"], slot)
        self.assertNotIn("offhandSlot", weapons.convert("gun", {}, {}, {}).fields)

    def test_unscaled_waw_projectile_damage_gets_neutral_t6_hit_locations(self):
        schema = {"weaponType": "WFT_WEAPONTYPE", "locNone": "CSPFT_FLOAT", "locHead": "CSPFT_FLOAT"}
        zero = {"locNone": "0", "locHead": "0"}
        for kind in ("projectile", "grenade"):
            out = weapons.convert("raygun", {"weaponType": kind, **zero}, schema, schema)
            self.assertEqual((out.fields["locNone"], out.fields["locHead"]), ("1", "1"))
        # bullets keep their multipliers; real projectile multipliers are kept too
        out = weapons.convert("gun", {"weaponType": "bullet", **zero}, schema, schema)
        self.assertEqual(out.fields["locHead"], "0")
        out = weapons.convert("rpg", {"weaponType": "projectile", "locNone": "1", "locHead": "4"}, schema, schema)
        self.assertEqual(out.fields["locHead"], "4")

    def test_bounce_sound_prefix_reaches_namespaced_surface_aliases(self):
        schema = {"bounceSound": "WFT_BOUNCE_SOUND"}
        converted = weapons.convert("custom", {"bounceSound": "custom/bounce"}, schema, schema)
        self.assertEqual(converted.fields["bounceSound"], "waw/custom/bounce")
        self.assertEqual(converted.dependencies['sound'], {'custom/bounce_default'})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'soundaliases/custom').mkdir(parents=True)
            (root/'soundaliases/custom/bounce_wood.w2bsnd.json').write_text('{}')
            self.assertEqual(weapons.bounce_aliases('custom/bounce', [root]),
                             {'custom/bounce_default', 'custom/bounce_wood'})

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

    def test_unset_player_anim_type_is_left_to_the_t6_default(self):
        schema = {"playerAnimType": "WFT_ANIMTYPE"}
        out = weapons.convert("crossbow", {"playerAnimType": ""}, schema, schema)
        self.assertNotIn("playerAnimType", out.fields)

    def test_skeleton_is_preserved_and_multiple_roots_are_not_stripped(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "source"
            (root / "xmodel").mkdir(parents=True)
            (root / "model_export").mkdir()
            model = {"_game": "t4", "flags": 0, "lods": [{"distance": 10, "file": "model_export/gun.gltf"}]}
            model['boneInfo'] = [{'bone': 'tag_view', 'mins': [0, 0, 0], 'maxs': [1, 1, 1],
                                  'offset': [0.5, 0.5, 0.5], 'radiusSquared': 0.75}]
            data = {"nodes": [{"name": "tag_view", "children": [1]}, {"name": "tag_weapon"}],
                    "skins": [{"joints": [0, 1]}], "materials": [{"name": "custom_skin"}],
                    "images": [{"uri": "../images/preview.dds"}],
                    "meshes": [{"primitives": [{"attributes": {"JOINTS_0": 1, "WEIGHTS_0": 2}}]}]}
            (root / "xmodel/gun.json").write_text(json.dumps(model))
            (root / "model_export/gun.gltf").write_text(json.dumps(data))
            project = Path(temp) / "valid"
            report = weapons.stage_models([root], project, {"gun"})
            self.assertEqual(len(report["models"]), 1)
            definition = json.loads((project / 'xmodel/waw_xmodel/gun.json').read_text())
            self.assertNotIn('boneInfo', definition)
            self.assertEqual(definition['wawBoneInfo'], model['boneInfo'])
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




class OutputNameTests(unittest.TestCase):
    def test_control_characters_become_file_safe_names(self):
        # ORBiT: model material "mc/g\x1a" crashed writing its JSON (Errno 22)
        self.assertEqual(weapons.output_name("material", "mc/g\x1a"), "waw_material/mc/g_x1a")
        self.assertEqual(weapons.output_name("material", 'a?b*"'), "waw_material/a_x3fb_x2a_x22")
        self.assertEqual(weapons.output_name("material", "mc/gun_steel"), "waw_material/mc/gun_steel")
        with self.assertRaises(weapons.WeaponError):
            weapons.output_name("material", "a:b")

if __name__ == "__main__":
    unittest.main()
