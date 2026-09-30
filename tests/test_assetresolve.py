import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import assetresolve, weapons


class StockFixture:
    def __init__(self, definitions):
        self.definitions = definitions
        self.calls = []

    def root_for(self, kind, name):
        self.calls.append((kind, name))
        return self.definitions.get((kind, name))


class ResolverTests(unittest.TestCase):
    def write(self, root, relative, value):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)

    def test_recursive_stock_dependencies_cycles_and_map_priority(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, stock_root = root / "map", root / "stock"
            self.write(source, "weapons/custom", weapons.write_info(
                {"gunModel": "original_model", "fireSound": "fire", "altWeapon": "alternate"}))
            self.write(stock_root, "weapons/alternate", weapons.write_info({"altWeapon": "custom"}))
            self.write(stock_root, "xmodel/original_model.json", json.dumps(
                {"lods": [{"file": "model_export/model.gltf"}]}))
            self.write(stock_root, "model_export/model.gltf", json.dumps({"materials": [{"name": "original_skin"}]}))
            self.write(source, "materials/original_skin.json", json.dumps({"textures": [{"image": "map_color"}]}))
            self.write(stock_root, "materials/original_skin.json", json.dumps({"textures": [{"image": "wrong_stock_color"}]}))
            self.write(source, "images/map_color.dds", "original")
            self.write(source, "soundaliases/fire.w2bsnd.json", json.dumps({"aliases": [{"secondaryAliasName": "echo"}]}))
            self.write(stock_root, "soundaliases/echo.w2bsnd.json", json.dumps({"aliases": [{"chainAliasName": "fire"}]}))
            definitions = {(kind, name): ("nazi_zombie_fixture", stock_root) for kind, name in
                           (("weapon", "alternate"), ("xmodel", "original_model"), ("sound", "echo"))}
            stock = StockFixture(definitions)
            resolver = assetresolve.Resolver([source], stock)
            report = resolver.expand({("weapon", "custom")})
            self.assertEqual(report["missing"], [])
            self.assertEqual(len(report["nodes"]), 7)
            self.assertNotIn(("image", "wrong_stock_color"), resolver.nodes)
            self.assertNotIn(("material", "original_skin"), stock.calls)
            self.assertEqual(resolver.nodes["xmodel", "original_model"]["provenance"], "WAW_STOCK_ASSET")
            self.assertIn(["weapon", "alternate", "weapon", "custom"], report["edges"])

    def test_missing_original_asset_keeps_dependency_chain(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.write(root, "weapons/custom", weapons.write_info({"gunModel": "absent_custom"}))
            resolver = assetresolve.Resolver([root], StockFixture({}))
            report = resolver.expand({("weapon", "custom")})
            self.assertEqual(report["missing"][0]["name"], "absent_custom")
            self.assertIn(["weapon", "custom", "xmodel", "absent_custom"], report["edges"])

    def test_unsafe_names_fail_before_lookup(self):
        provider = StockFixture({})
        with self.assertRaises(weapons.WeaponError):
            assetresolve.Resolver([], provider).expand({("xmodel", "../escape")})
        self.assertEqual(provider.calls, [])

    def test_loaded_audio_dependency_resolves_original_stock_xwma(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, stock_root = root / "map", root / "stock"
            self.write(source, "soundaliases/fire.w2bsnd.json", json.dumps({"aliases": [
                {"soundFile": {"type": 1, "name": "wpn/original.wav"}}]}))
            self.write(stock_root, "sound/wpn/original.xwma", "original encoded payload")
            stock = StockFixture({("loadedsound", "wpn/original.wav"): ("common", stock_root)})
            resolver = assetresolve.Resolver([source], stock)
            result = resolver.expand({("sound", "fire")})
            self.assertEqual(result["missing"], [])
            self.assertTrue(resolver.nodes["loadedsound", "wpn/original.wav"]["source"].endswith(".xwma"))


if __name__ == "__main__":
    unittest.main()


class WawFilePathTests(unittest.TestCase):
    def test_leading_and_repeated_separators_collapse_like_the_waw_file_system(self):
        from waw2bo2 import assetresolve, sounds
        self.assertEqual(assetresolve.asset_path("loadedsound", "/sfx//x.wav").as_posix(), "sound/sfx/x.wav")
        self.assertEqual(sounds.audio_name({"name": "/sfx/x.wav"}), "sfx/x.wav")
        with self.assertRaises(ValueError):
            assetresolve.asset_path("loadedsound", "/../x.wav")
