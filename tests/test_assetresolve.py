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

    def test_map_model_metadata_with_stock_geometry_recovers_and_stages(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, stock_root = root / 'map', root / 'stock'
            name = 'viewmodel_usa_double_barrel_sawed_off_grips'
            mesh = f'model_export/{name}_lod0.gltf'
            model = {'_game': 't4', 'type': 'rigid', 'flags': 0,
                     'lods': [{'file': mesh, 'distance': 123}]}
            self.write(source, f'xmodel/{name}.json', json.dumps(model))
            self.write(stock_root, f'xmodel/{name}.json', json.dumps(dict(model, type='wrong_metadata')))
            self.write(stock_root, mesh, json.dumps({'nodes': [{'name': 'tag_origin'}],
                                                     'materials': [{'name': 'original_skin'}]}))
            self.write(stock_root, 'materials/original_skin.json', '{"textures":[]}')
            provider = StockFixture({('xmodel', name): ('nazi_zombie_asylum', stock_root),
                ('material', 'original_skin'): ('nazi_zombie_asylum', stock_root)})
            resolver = assetresolve.Resolver([source], provider)
            report = resolver.expand({('xmodel', name)})
            self.assertEqual(report['missing'], [])
            self.assertEqual(resolver.nodes['xmodel', name]['source'], str(source / f'xmodel/{name}.json'))
            self.assertEqual(resolver.nodes['xmodel', name]['recovered_geometry'], [str(stock_root / mesh)])
            staged = weapons.stage_models(resolver.roots, root / 'out', {name})
            self.assertEqual(staged['unsupported'], {})
            output = json.loads((root / f'out/xmodel/waw_xmodel/{name}.json').read_text())
            self.assertEqual(output['type'], 'rigid')
            self.assertEqual(output['lods'][0]['distance'], 123)
            self.assertFalse((source / mesh).exists())  # cache generations stay immutable

    def test_missing_geometry_keeps_dependency_report_without_file_exception(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.write(root, 'xmodel/gun.json', '{"lods":[{"file":"model_export/gun.gltf"}]}')
            resolver = assetresolve.Resolver([root], StockFixture({}))
            result = resolver.expand({('xmodel', 'gun')})
            self.assertEqual(result['missing'][0]['status'], 'missing_model_geometry')
            self.assertEqual(result['missing'][0]['missing_files'], ['model_export/gun.gltf'])
            staged = weapons.stage_models(resolver.roots, root / 'out', {'gun'})
            self.assertIn('absent from the map', staged['unsupported']['gun'])

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
