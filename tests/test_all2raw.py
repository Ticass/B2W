from dataclasses import replace
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from waw2bo2 import all2raw, t6api, wawassets, modzone
from waw2bo2.launcher import Settings


class All2RawTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'game'
        self.cache = self.root / 'cache'
        self.tool = self.root / 'Unlinker.exe'
        self.tool.write_bytes(b'tool')
        self.commands = []
        for folder, name in [('all', 'common_zm'), ('all', 'zm_alpha'), ('all', 'mp_beta'),
                             ('english', 'en_common_zm')]:
            self.ff(folder, name).write_text(name)
        self.addCleanup(patch.stopall)
        patch.object(all2raw, 'run', side_effect=self.native).start()
        patch.object(t6api, 'build', side_effect=self.api).start()

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
        raw = self.prepare()
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

    def test_compact_policy_excludes_heavy_game_assets(self):
        self.assertEqual(all2raw.stock_assets('T4', Path('campaign.ff')), 'rawfile')
        self.assertNotIn('gfxworld', all2raw.stock_assets('T4', Path('common.ff')))
        for zone in ('zm_nuked', 'zm_prison', 'campaign', 'mp_test'):
            kinds = set(all2raw.stock_assets('T6', Path(zone + '.ff')).split(','))
            self.assertTrue(kinds.isdisjoint({'clipmap', 'gfxworld', 'xanim', 'sound', 'loadedsound', 'rawfile'}))
        self.assertNotIn('image', all2raw.stock_assets('T6', Path('mp_test.ff')))

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
        self.assertEqual(list((self.cache / 'zones').rglob('pending-*')), [])
        self.assertTrue(list((self.cache / 'zones').rglob('*.log')))

    def test_removed_zones_disappear_from_new_lookup_without_other_redumps(self):
        self.prepare()
        self.ff('all', 'mp_beta').unlink()
        self.commands.clear()
        raw = self.prepare()
        self.assertEqual(self.commands, [])
        catalog = json.loads((raw / 'catalog.json').read_text())
        self.assertNotIn('zone/all/mp_beta.ff', catalog['zones'])


if __name__ == '__main__':
    unittest.main()
