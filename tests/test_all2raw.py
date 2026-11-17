from dataclasses import replace
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from waw2bo2 import all2raw, cache_share, native, t6api, wawassets, modzone
from waw2bo2.launcher import Settings


class All2RawTests(unittest.TestCase):
    def test_dependency_cache_shares_only_identical_payloads(self):
        if native.tool() is None:
            self.skipTest("native converter required")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "dependencies" / "zone"
            old, staging = root / "old", root / "new"
            old.mkdir(parents=True)
            staging.mkdir()
            for parent, changed in ((old, b"a"), (staging, b"b")):
                (parent / "same.gltf").write_bytes(b"x" * 300_000)
                (parent / "changed.gltf").write_bytes(changed * 300_000)
            (old / "extraction.json").write_text(json.dumps({"inputs": {
                "file": "same.ff", "assets": "xmodel"}}), encoding="utf-8")
            inputs = {"file": "same.ff", "assets": "xmodel,image"}
            cache_share.share_dependencies(root, staging, inputs, ["same.gltf", "changed.gltf"])
            self.assertTrue((old / "same.gltf").samefile(staging / "same.gltf"))
            self.assertFalse((old / "changed.gltf").samefile(staging / "changed.gltf"))
            self.assertEqual((staging / "changed.gltf").read_bytes(), b"b" * 300_000)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'game'
        self.cache = self.root / 'cache'
        self.tool = self.root / 'Unlinker.exe'
        self.tool.write_bytes(b'tool ' + all2raw.WORLD_DUMPER_MARKER)
        self.commands = []
        for folder, name in [('all', 'common_zm'), ('all', 'zm_alpha'), ('all', 'mp_beta'),
                             ('english', 'en_common_zm')]:
            self.ff(folder, name).write_text(name)
        self.addCleanup(patch.stopall)
        patch.object(all2raw, 'run', side_effect=self.native).start()
        patch.object(t6api, 'build', side_effect=self.api).start()
        self.real_trim_barrier = all2raw.trim_barrier_payload
        patch.object(all2raw, 'trim_barrier_payload', side_effect=lambda folder, data: data).start()
        self.real_stock_zones = all2raw.stock_zones
        patch.object(all2raw, 'stock_zones', side_effect=lambda game, engine:
            sorted((game / 'zone').rglob('*.ff'), key=lambda p: all2raw._rank(p.relative_to(game).as_posix()))).start()

    def ff(self, folder, name):
        path = self.game / 'zone' / folder / (name + '.ff')
        path.parent.mkdir(parents=True, exist_ok=True)
        return path

    def native(self, command, log):
        self.commands.append(command)
        ff = Path(command[-1])
        log.parent.mkdir(parents=True, exist_ok=True)
        if '--list' in command:
            log.write_text('material,shared\nscript,maps/common.gsc\nimage,,reference_only\n')
        else:
            output = Path(command[command.index('--output-folder') + 1])
            (output / 'materials').mkdir(parents=True, exist_ok=True)
            (output / 'materials/shared.json').write_bytes(ff.read_bytes())
            log.write_text('done')

    def api(self, game, cache, tool, stock_dump=None):
        cache.write_text(json.dumps({'version': t6api.CACHE_VERSION, 'builtins': {},
                                    'methods': {}, 'scripts': {}, 'stock_assets': {'script': ['maps/common.gsc']}}))

    def prepare(self, **kwargs):
        return all2raw.prepare(self.game, self.tool, self.cache, engine='T6', **kwargs)

    def test_native_extraction_overlaps_with_configured_worker_limit(self):
        barrier = threading.Barrier(2, timeout=10)
        lock = threading.Lock()
        active = 0
        peak = 0
        def concurrent_native(command, log):
            nonlocal active, peak
            if '--list' in command:
                return self.native(command, log)
            with lock:
                active += 1
                peak = max(peak, active)
            try:
                barrier.wait()
                self.native(command, log)
            finally:
                with lock:
                    active -= 1
        with patch.object(all2raw, 'run', side_effect=concurrent_native):
            raw = self.prepare(workers=2)
        self.assertEqual(peak, 2)
        self.assertEqual((raw / 'materials/shared.json').read_text(), 'common_zm')

    def test_all_logical_cpus_are_used_by_default_and_results_keep_input_order(self):
        started = threading.Barrier(4, timeout=10)
        first_finished = threading.Event()
        def extract(index):
            started.wait()
            if index == 0:
                self.assertTrue(first_finished.wait(10))
            else:
                first_finished.set()
            return index
        with patch.dict('os.environ', {'WAW2BO2_EXTRACT_WORKERS': ''}), \
                patch.object(all2raw.os, 'cpu_count', return_value=4):
            self.assertEqual(all2raw.parallel_zones(list(range(4)), extract, label='test'), list(range(4)))

    def test_selecting_mod_ff_extracts_it_once_for_both_roles(self):
        folder = self.root / 'custom'
        folder.mkdir()
        (folder / 'mod.ff').write_text('mod')
        settings = Settings(waw=str(self.game), t4=str(self.root), work=str(self.root),
                            fastfile=str(folder / 'mod.ff'))
        outputs = all2raw.source_dumps(settings, all2raw.CachePaths.for_settings(settings))
        self.assertEqual(outputs['map'], outputs['mod'])
        self.assertEqual(len(self.commands), 2)

    def test_all_installed_zones_are_extracted_and_reused_without_native_calls(self):
        raw = self.prepare()
        self.assertEqual(len(self.commands), 8)
        self.commands.clear()
        self.assertEqual(self.prepare(), raw)
        self.assertEqual(self.commands, [])
        self.assertEqual(all2raw.ready(self.game, self.tool, self.cache, engine='T6'), raw)

    def test_stock_exports_only_conversion_metadata_for_other_maps(self):
        self.prepare()
        dumps = [c for c in self.commands if '--list' not in c]
        self.assertTrue(all(c[c.index('--include-assets') + 1] ==
                            all2raw.stock_assets('T6', Path(c[-1])) for c in dumps))
        self.assertTrue(all('IWI' in c for c in dumps))

    def test_native_search_paths_never_include_missing_optional_folders(self):
        self.prepare()
        for command in self.commands:
            search = command[command.index('--search-path') + 1]
            folders = [Path(p) for p in search.split(';')]
            self.assertTrue(all(p.is_dir() for p in folders))
            self.assertIn(Path(command[-1]).parent, folders)
            self.assertNotIn(self.game / 'main', folders)
            self.assertNotIn(self.game / 'sound', folders)
        main = self.game / 'main'
        main.mkdir()
        self.assertEqual(all2raw.search_paths((main, self.game / 'missing')), str(main))

    def test_changed_zone_invalidates_cache_but_only_that_zone_is_redumped(self):
        self.prepare()
        self.commands.clear()
        self.ff('all', 'zm_alpha').write_text('updated assets')
        with self.assertRaisesRegex(RuntimeError, 'Extract All'):
            all2raw.ready(self.game, self.tool, self.cache, engine='T6')
        raw = self.prepare()
        self.assertEqual(len(self.commands), 2)
        self.assertTrue(all(Path(c[-1]).stem == 'zm_alpha' for c in self.commands))
        self.assertEqual(all2raw.ready(self.game, self.tool, self.cache, engine='T6'), raw)

    def test_duplicates_retain_zone_variants_with_deterministic_lookup_and_provenance(self):
        raw = self.prepare()
        self.assertEqual((raw / 'materials/shared.json').read_text(), 'common_zm')
        catalog = json.loads((raw / 'catalog.json').read_text())
        self.assertEqual(len(catalog['conflicts']), 3)
        for name, entry in catalog['zones'].items():
            self.assertEqual((Path(entry['folder']) / 'materials/shared.json').read_text(), Path(name).stem)

    def test_interrupted_extraction_resumes_and_never_marks_partial_cache_ready(self):
        original = self.native
        def fail_second(command, log):
            if Path(command[-1]).stem == 'mp_beta':
                raise RuntimeError('native failure')
            original(command, log)
        with patch.object(all2raw, 'run', side_effect=fail_second):
            with self.assertRaisesRegex(RuntimeError, 'native failure'):
                self.prepare()
        self.assertFalse((self.cache / 'all2raw.json').exists())
        self.commands.clear()
        self.prepare()
        self.assertNotIn('common_zm.ff', [Path(c[-1]).name for c in self.commands])

    def test_missing_extracted_file_is_repaired_without_reextracting_other_zones(self):
        self.prepare()
        receipt = json.loads((self.cache / 'all2raw.json').read_text())
        entry = receipt['zones']['zone/all/zm_alpha.ff']
        (Path(entry['folder']) / 'materials/shared.json').unlink()
        self.commands.clear()
        repaired = self.prepare()
        self.assertEqual(len(self.commands), 2)
        self.assertEqual(all2raw.ready(self.game, self.tool, self.cache, engine='T6'), repaired)
        self.commands.clear()
        self.assertEqual(self.prepare(), repaired)
        self.assertEqual(self.commands, [])

    def test_deleted_merged_file_is_repaired_using_zone_cache_without_native_calls(self):
        raw = self.prepare()
        (raw / 'materials/shared.json').unlink()
        self.commands.clear()
        repaired = self.prepare()
        self.assertEqual(self.commands, [])
        self.assertEqual((repaired / 'materials/shared.json').read_text(), 'common_zm')

    def test_different_custom_maps_share_the_same_game_cache(self):
        settings = Settings(waw=str(self.game), bo2=str(self.game), work=str(self.root),
                            fastfile='/maps/one.ff', project='zm_one')
        first = all2raw.CachePaths.for_settings(settings)
        second = all2raw.CachePaths.for_settings(replace(settings, fastfile='/maps/two.ff', project='zm_two'))
        self.assertEqual(first, second)

    def test_cached_asset_lists_are_consumed_without_relisting_stock_zones(self):
        raw = self.prepare()
        with patch('subprocess.run', side_effect=AssertionError('stock listing repeated')):
            assets = t6api.list_zone_assets(self.game, self.tool, raw)
            scripts = modzone.stock_scripts(self.game, self.tool, raw)
        self.assertIn('reference_only', assets['image'])
        self.assertIn('maps/common.gsc', scripts)

    def test_reference_assets_are_not_claimed_as_definitions(self):
        data = all2raw.listing('image,,referenced\nimage,defined\n')
        self.assertEqual(data, {'image': ['defined']})

    def test_waw_dependency_resolver_consumes_prepared_zone_index_and_dumps(self):
        all2raw.prepare(self.game, self.tool, self.cache, engine='T4')
        resolver = wawassets.StockWawAssets(self.game, self.tool, self.cache)
        self.commands.clear()
        with patch('subprocess.run', side_effect=AssertionError('uncached native call')):
            resolver.load()
            self.assertEqual(resolver.zones_defining('material', 'shared'), ['en_common_zm'])
            self.assertTrue((resolver.dump('en_common_zm') / 'materials/shared.json').is_file())
            resolver.dump('en_common_zm')
        self.assertEqual(len(self.commands), 2)
        self.commands.clear()
        retry = wawassets.StockWawAssets(self.game, self.tool, self.cache)
        retry.load()
        retry.dump('en_common_zm')
        self.assertEqual(self.commands, [])

    def test_dependency_lookup_extracts_requested_types_and_expands_without_losing_them(self):
        all2raw.prepare(self.game, self.tool, self.cache, engine='T4')
        resolver = wawassets.StockWawAssets(self.game, self.tool, self.cache)
        resolver.load()
        self.commands.clear()
        resolver.dump('en_common_zm', 'xanim')
        exports = [c for c in self.commands if '--include-assets' in c]
        self.assertEqual(exports[0][exports[0].index('--include-assets') + 1], 'rawfile,xanim')
        self.commands.clear()
        resolver.dump('en_common_zm', 'material')
        exports = [c for c in self.commands if '--include-assets' in c]
        self.assertEqual(exports[0][exports[0].index('--include-assets') + 1], 'material,rawfile,xanim')
        self.commands.clear()
        resolver.dump('en_common_zm', 'xanim')
        resolver.dump('en_common_zm', 'material')
        self.assertEqual(self.commands, [])
        resolver.dump('en_common_zm', 'image')
        exports = [c for c in self.commands if '--include-assets' in c]
        self.assertEqual(exports[0][exports[0].index('--include-assets') + 1], 'image,material,rawfile,xanim')
        self.assertEqual(set(resolver.prepared_sources['en_common_zm']['assets'].split(',')),
                         {'image', 'material', 'rawfile', 'xanim'})

    def test_failed_dependency_expansion_keeps_the_previous_published_cache(self):
        all2raw.prepare(self.game, self.tool, self.cache, engine='T4')
        resolver = wawassets.StockWawAssets(self.game, self.tool, self.cache)
        resolver.load()
        before = resolver.prepared['en_common_zm']
        inputs = dict(resolver.prepared_sources['en_common_zm'])
        with patch.object(all2raw, 'run', side_effect=RuntimeError('native extraction failed')):
            with self.assertRaisesRegex(RuntimeError, 'native extraction failed'):
                resolver.dump('en_common_zm', 'xanim')
        self.assertEqual(resolver.prepared['en_common_zm'], before)
        self.assertEqual(resolver.prepared_sources['en_common_zm'], inputs)
        self.assertTrue((before / 'extraction.json').is_file())

    def test_stock_level_sources_include_its_companions_without_other_levels(self):
        for name in ('nazi_zombie_swamp', 'nazi_zombie_swamp_patch', 'nazi_zombie_swamp_load',
                     'localized_nazi_zombie_swamp', 'nazi_zombie_other', 'localized_nazi_zombie_other'):
            self.ff('english', name).write_text(name)
        settings = Settings(waw=str(self.game), bo2=str(self.game), t4=str(self.root), work=str(self.root),
                            fastfile=str(self.ff('english', 'nazi_zombie_swamp')), project='zm_swamp')
        result = all2raw.source_dumps(settings, all2raw.CachePaths.for_settings(settings))
        self.assertEqual(set(result), {'map', 'patch', 'extra:nazi_zombie_swamp_load',
                                      'extra:localized_nazi_zombie_swamp'})
        names = {Path(c[-1]).stem for c in self.commands}
        self.assertEqual(names, {'nazi_zombie_swamp', 'nazi_zombie_swamp_patch',
                                'nazi_zombie_swamp_load', 'localized_nazi_zombie_swamp'})

    def test_compact_policy_excludes_heavy_game_assets(self):
        self.assertEqual(all2raw.stock_assets('T4', Path('campaign.ff')), 'rawfile')
        self.assertEqual(all2raw.stock_assets('T4', Path('common.ff')), 'rawfile')
        for zone in ('zm_nuked', 'zm_prison', 'campaign', 'mp_test'):
            kinds = set(all2raw.stock_assets('T6', Path(zone + '.ff')).split(','))
            self.assertTrue(kinds.isdisjoint({'clipmap', 'gfxworld', 'xanim', 'sound', 'loadedsound', 'rawfile'}))
        self.assertNotIn('image', all2raw.stock_assets('T6', Path('mp_test.ff')))
        self.assertNotIn('image', all2raw.stock_assets('T6', Path('zm_nuked.ff')))

    def test_prison_keeps_only_barrier_models_and_their_textures(self):
        folder = self.root / 'prison'
        files = {
            'zbarrier/zmcore_basicwoodbarrier': 'ZBARRIER\\boardModel1\\board\\boardModel2\\',
            'zbarrier/unrelated': 'unneeded',
            'xmodel/board.json': json.dumps({'lods': [{'file': 'model_export/board.gltf'}]}),
            'xmodel/unrelated.json': '{}',
            'model_export/board.gltf': json.dumps({'materials': [{'name': 'wood'}]}),
            'model_export/unrelated.gltf': '{}',
            'materials/wood.json': json.dumps({'textures': [{'image': 'wood'}]}),
            'images/wood.iwi': 'pixels', 'images/unrelated.iwi': 'unneeded'}
        for name, content in files.items():
            path = folder / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        data = self.real_trim_barrier(folder, {'files': list(files), 'inputs': {}})
        self.assertIn('images/wood.iwi', data['files'])
        self.assertIn('model_export/board.gltf', data['files'])
        self.assertTrue(all('unrelated' not in name for name in data['files']))

    def test_bo2_image_dependency_keeps_only_requested_texture_and_reuses_it(self):
        stock = self.cache / 'views/test'
        stock.mkdir(parents=True)
        ff = self.ff('all', 'zm_nuked')
        ff.write_text('nuked')
        (stock / 'catalog.json').write_text(json.dumps({'zones': {'nuked': {
            'inputs': {'file': str(ff)}, 'index': {'image': ['needed']}}}}))
        def export(command, log):
            self.commands.append(command)
            output = Path(command[command.index('--output-folder') + 1])
            (output / 'images').mkdir()
            for name in ('needed', 'unrelated'):
                (output / 'images' / (name + '.iwi')).write_bytes(b'image')
            log.write_text('done')
        with patch.object(all2raw, 'run', side_effect=export):
            image = all2raw.required_bo2_image(stock, 'needed', self.tool)
            self.assertTrue(image.is_file())
            self.assertFalse(image.with_name('unrelated.iwi').exists())
            self.commands.clear()
            self.assertEqual(all2raw.required_bo2_image(stock, 'needed', self.tool), image)
            self.assertEqual(self.commands, [])

    def test_bo2_only_prepares_nuketown_shared_runtime_and_barrier_donor(self):
        for name in ('zm_nuked', 'zm_prison', 'zm_transit', 'code_post_gfx_zm', 'patch_zm'):
            self.ff('all', name).write_text(name)
        with patch.object(all2raw, 'stock_zones', side_effect=self.real_stock_zones):
            self.prepare()
            names = {Path(c[-1]).stem for c in self.commands}
            self.assertEqual(names, {'common_zm', 'zm_nuked', 'zm_prison', 'code_post_gfx_zm', 'patch_zm'})
            inputs = all2raw.inventory(self.game, self.tool, 'T6')
            self.assertNotIn('zone/all/zm_transit.ff', inputs['zones'])

    def test_old_full_exports_migrate_without_native_calls_or_heavy_payloads(self):
        ff = self.ff('all', 'mp_beta')
        zone_root = self.cache / 'zone'
        def full_export(command, log):
            self.native(command, log)
            if '--list' not in command:
                output = Path(command[command.index('--output-folder') + 1])
                (output / 'sound').mkdir()
                (output / 'sound/unneeded.wav').write_bytes(b'large audio')
        with patch.object(all2raw, 'SCHEMA', 1), patch.object(all2raw, 'run', side_effect=full_export):
            old, _ = all2raw.extract_zone(ff, self.tool, zone_root, '', assets=None, image_format='IWI')
        self.commands.clear()
        new, data = all2raw.extract_zone(ff, self.tool, zone_root, '',
            assets=all2raw.stock_assets('T6', ff), image_format='IWI')
        self.assertNotEqual(old, new)
        self.assertEqual(self.commands, [])
        self.assertEqual(data['files'], ['materials/shared.json'])
        self.assertFalse((new / 'sound').exists())

    def test_custom_map_sources_and_companions_are_prepared_once(self):
        folder = self.root / 'custom map'
        folder.mkdir()
        for name in ('map_one', 'mod', 'map_one_patch', 'extra', 'map_two'):
            (folder / (name + '.ff')).write_text(name)
        settings = Settings(waw=str(self.game), bo2=str(self.game), t4=str(self.root), work=str(self.root),
                            fastfile=str(folder / 'map_one.ff'))
        paths = all2raw.CachePaths.for_settings(settings)
        first = all2raw.source_dumps(settings, paths)
        self.assertIn('map', first)
        self.assertIn('mod', first)
        self.assertEqual(len(self.commands), 10)
        self.commands.clear()
        self.assertEqual(all2raw.source_dumps(settings, paths), first)
        second = all2raw.source_dumps(replace(settings, fastfile=str(folder / 'map_two.ff')), paths)
        self.assertEqual(self.commands, [])
        self.assertNotEqual(first['map'], second['map'])

    def test_upstream_extractor_is_rejected_before_extracting_a_map(self):
        # Reported 0.2.14 builds: "WaW extractor folder" pointed at an Unlinker
        # without the waw2bo2 world dumper; the map "extracted" with no world.
        folder = self.root / 'waw maps'
        folder.mkdir()
        (folder / 'map.ff').write_text('map')
        upstream = self.root / 'upstream tools'
        upstream.mkdir()
        (upstream / 'Unlinker.exe').write_bytes(b'stock OpenAssetTools')
        settings = Settings(waw=str(self.game), t4=str(upstream), work=str(self.root),
                            fastfile=str(folder / 'map.ff'))
        with self.assertRaisesRegex(RuntimeError, "not the converter's WaW extractor"):
            all2raw.source_dumps(settings, all2raw.CachePaths.for_settings(settings))
        with self.assertRaisesRegex(RuntimeError, "not the converter's WaW extractor"):
            all2raw.prepare(self.game, upstream / 'Unlinker.exe', self.cache, engine='T4')
        self.assertEqual(self.commands, [])
        from waw2bo2.launcher import preflight
        check = next(c for c in preflight(settings, include_map=False) if c.name == 'WaW extractor')
        self.assertFalse(check.ready)
        self.assertIn('Advanced Paths', check.detail)
        check = next(c for c in preflight(replace(settings, t4=str(self.root)), include_map=False)
                     if c.name == 'WaW extractor')
        self.assertTrue(check.ready)

    def test_map_dump_without_its_listed_world_is_an_extraction_error(self):
        folder = self.root / 'waw maps'
        folder.mkdir()
        (folder / 'map.ff').write_text('map')
        settings = Settings(waw=str(self.game), t4=str(self.root), work=str(self.root),
                            fastfile=str(folder / 'map.ff'))
        paths = all2raw.CachePaths.for_settings(settings)

        def native(command, log, world=False):
            self.native(command, log)
            if '--list' in command:
                log.write_text(log.read_text() + 'gfxworld,maps/map.d3dbsp\n')
            elif world:
                output = Path(command[command.index('--output-folder') + 1])
                (output / 'waw2bo2/maps').mkdir(parents=True)
                (output / 'waw2bo2/maps/map.d3dbsp.gfx.bin').write_bytes(b'world')
        with patch.object(all2raw, 'run', side_effect=native):
            with self.assertRaisesRegex(RuntimeError, 'did not export map world maps/map.d3dbsp'):
                all2raw.source_dumps(settings, paths)
        with patch.object(all2raw, 'run', side_effect=lambda c, l: native(c, l, world=True)):
            dumps = all2raw.source_dumps(settings, paths, refresh=True)
        self.assertTrue((dumps['map'] / 'waw2bo2/maps/map.d3dbsp.gfx.bin').is_file())

    def test_old_custom_cache_without_entities_is_reextracted_automatically(self):
        folder = self.root / 'custom map'
        folder.mkdir()
        (folder / 'map.ff').write_text('map')
        settings = Settings(waw=str(self.game), t4=str(self.root), work=str(self.root),
                            fastfile=str(folder / 'map.ff'))
        paths = all2raw.CachePaths.for_settings(settings)
        old_assets = all2raw.WAW_ASSETS.replace('mapents,', '')
        with patch.object(all2raw, 'WAW_ASSETS', old_assets):
            old = all2raw.source_dumps(settings, paths)
        self.commands.clear()
        new = all2raw.source_dumps(settings, paths)
        self.assertNotEqual(old['map'], new['map'])
        native = next(c for c in self.commands if '--include-assets' in c)
        self.assertIn('mapents', native[native.index('--include-assets') + 1].split(','))

    def test_failed_listing_does_not_publish_a_successful_extraction(self):
        def fail_list(command, log):
            if '--list' in command:
                raise RuntimeError('listing failed')
            self.native(command, log)
        with patch.object(all2raw, 'run', side_effect=fail_list):
            with self.assertRaisesRegex(RuntimeError, 'listing failed'):
                self.prepare()
        self.assertFalse((self.cache / 'all2raw.json').is_file())

    def test_explicit_refresh_generations_are_reused_after_completion(self):
        original = self.prepare()
        refreshed = self.prepare(refresh=True)
        self.assertNotEqual(original, refreshed)
        self.assertFalse(original.exists())
        self.commands.clear()
        self.assertEqual(self.prepare(), refreshed)
        self.assertEqual(self.commands, [])

    def test_failed_native_dump_removes_partial_files_but_keeps_log(self):
        def fail(command, log):
            self.native(command, log)
            raise RuntimeError('failed export')
        with patch.object(all2raw, 'run', side_effect=fail):
            with self.assertRaisesRegex(RuntimeError, 'failed export'):
                self.prepare()
        self.assertEqual([p for p in self.cache.rglob('*') if p.parent.name == 'x'], [])
        self.assertTrue(list((self.cache / 'zones').rglob('*.log')))

    def test_unwritten_model_export_is_not_published(self):
        def long_path(command, log):
            self.native(command, log)
            with log.open('a') as handle:
                handle.write("\nERROR: Failed to open file 'model_export/very_long_model_lod0.gltf'\n")
        with patch.object(all2raw, 'run', side_effect=long_path):
            with self.assertRaisesRegex(RuntimeError, 'could not write 1 model/image'):
                self.prepare()
        self.assertFalse((self.cache / 'all2raw.json').is_file())

    def test_exports_are_staged_near_the_cache_top(self):
        top = Path('C:/w/asset_cache/waw_abc')
        staging = all2raw.staging_folder(top / 'custom_maps/aaaaaaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbbbbbb')
        self.assertEqual(staging.parent, top / 'x')
        self.assertEqual(len(staging.name), 12)

    def test_removed_zones_disappear_from_new_lookup_without_other_redumps(self):
        self.prepare()
        self.ff('all', 'mp_beta').unlink()
        self.commands.clear()
        raw = self.prepare()
        self.assertEqual(self.commands, [])
        catalog = json.loads((raw / 'catalog.json').read_text())
        self.assertNotIn('zone/all/mp_beta.ff', catalog['zones'])


class NativeRunTests(unittest.TestCase):
    def test_native_crash_is_retried(self):
        import subprocess
        results = [subprocess.CompletedProcess([], 0xC0000409), subprocess.CompletedProcess([], 0)]
        with tempfile.TemporaryDirectory() as tmp:
            with patch('waw2bo2.progress.native', side_effect=lambda c, l: results.pop(0)):
                all2raw.run(['Unlinker.exe', 'zone.ff'], Path(tmp) / 'x.log')
        self.assertEqual(results, [])


class PublishRetryTests(unittest.TestCase):
    """Antivirus/indexers briefly hold freshly staged folders on Windows."""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.source = self.root / 'staging'
        self.source.mkdir()
        (self.source / 'a.txt').write_text('a')
        self.target = self.root / 'published'
        # A fake clock advanced by sleep keeps the timeout test instant.
        self.now = 0.0
        self.sleeps = []
        def sleep(seconds):
            self.sleeps.append(seconds)
            self.now += seconds
        self.addCleanup(patch.stopall)
        patch.object(all2raw.time, 'monotonic', side_effect=lambda: self.now).start()
        patch.object(all2raw.time, 'sleep', side_effect=sleep).start()

    def flaky(self, method, failures, winerror=5):
        real = getattr(Path, method)
        calls = []
        def attempt(path, target):
            calls.append(path)
            if failures is None or len(calls) <= failures:
                error = PermissionError(13, 'Access is denied')
                error.winerror = winerror
                raise error
            return real(path, target)
        patch.object(Path, method, attempt).start()
        return calls

    def test_rename_retries_until_the_holder_lets_go(self):
        calls = self.flaky('rename', 3)
        all2raw.publish(self.source, self.target)
        self.assertEqual(len(calls), 4)
        self.assertEqual(len(self.sleeps), 3)
        self.assertEqual((self.target / 'a.txt').read_text(), 'a')
        self.assertFalse(self.source.exists())

    def test_sharing_violation_is_retried_for_json_replace(self):
        calls = self.flaky('replace', 2, winerror=32)
        all2raw.write_json(self.root / 'pointer.json', {'folder': 'x'})
        self.assertEqual(len(calls), 3)
        self.assertEqual(json.loads((self.root / 'pointer.json').read_text()), {'folder': 'x'})

    def test_persistent_lock_raises_naming_both_paths(self):
        calls = self.flaky('rename', None)
        with self.assertRaises(PermissionError) as raised:
            all2raw.publish(self.source, self.target)
        self.assertIn(str(self.source), str(raised.exception))
        self.assertIn(str(self.target), str(raised.exception))
        self.assertGreater(len(calls), 3)
        self.assertLessEqual(sum(self.sleeps), all2raw.PUBLISH_TIMEOUT)
        self.assertTrue(self.source.is_dir())

    def test_other_windows_errors_are_not_retried(self):
        calls = self.flaky('rename', None, winerror=1920)
        with self.assertRaises(PermissionError):
            all2raw.publish(self.source, self.target)
        self.assertEqual(len(calls), 1)

    def test_extract_zone_survives_a_held_staging_folder(self):
        calls = self.flaky('rename', 2)
        tool = self.root / 'Unlinker.exe'
        tool.write_bytes(b'tool')
        ff = self.root / 'zone.ff'
        ff.write_bytes(b'ff')
        def native(command, log):
            if '--list' in command:
                log.write_text('')
                return
            output = Path(command[command.index('--output-folder') + 1])
            (output / 'asset.txt').write_text('x')
            log.write_text('')
        with patch.object(all2raw, 'run', side_effect=native):
            folder, data = all2raw.extract_zone(ff, tool, self.root / 'cache', '', assets=None,
                                                image_format='DDS')
        self.assertEqual(len(calls), 3)
        self.assertEqual(data['files'], ['asset.txt'])
        self.assertTrue((folder / 'extraction.json').is_file())


if __name__ == '__main__':
    unittest.main()
