import unittest
import tempfile
from pathlib import Path

from waw2bo2 import gsc, gscport
from waw2bo2.t6api import T6Api


class PoiRecordTests(unittest.TestCase):
    def test_extracted_source_allocator_receives_the_record_bridge(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode_utility.gsc').write_text(
                '#include common_scripts\\utility;\n'
                'add_poi_attractor(poi){ '
                'if(array_check_for_dupes(poi.attractor_array,self)) '
                'return array_check_for_dupes(poi.claimed_attractor_positions,poi.attractor_positions[i]); }')
            api = T6Api(builtins={}, methods={}, scripts={
                r'common_scripts\utility': {'array_check_for_dupes': 2}})
            report = gscport.PortReport()
            tr = gscport.Translator(gscport.Sources([root], [], None), api, report)
            tr.extract_entry(r'maps\_zombiemode_utility', 'add_poi_attractor')
            result = tr.translate_extracted(r'maps\_zombiemode_utility', 'add_poi_attractor')
            self.assertIn('::waw_poi_record_available(', result)
            self.assertIn('array_check_for_dupes(poi.attractor_array,self)', result)
            self.assertEqual(report.errors, [])

    def translate(self, source):
        tokens = gsc.parse(source, 'test').tokens
        count = gscport.bridge_poi_record_comparisons(tokens)
        return gsc.emit(tokens), count

    def test_claim_checks_location_records_but_entity_membership_is_unchanged(self):
        result, count = self.translate('f(){ '
            'array_check_for_dupes(poi.attractor_array,self); '
            'array_check_for_dupes(poi.claimed_attractor_positions,poi.attractor_positions[i]); }')
        self.assertEqual(count, 1)
        self.assertIn('array_check_for_dupes(poi.attractor_array,self)', result)
        self.assertIn(gscport.COMPAT + '::waw_poi_record_available(', result)
        self.assertIn('poi.claimed_attractor_positions,poi.attractor_positions[i]', result)

    def test_invalidation_and_release_keep_distinct_locations(self):
        result, count = self.translate('f(){ '
            'array_check_for_dupes(self.attractor_positions,attractor_pos); '
            'if(self.attractor_positions[i]==attractor_pos)index=i; '
            r'a=maps\mp\waw\_waw2bo2_compat::waw_array_remove(self.attractor_positions,attractor_pos); '
            r'b=maps\mp\waw\_waw2bo2_compat::waw_array_remove(self.claimed_attractor_positions,claimed); '
            r'c=maps\mp\waw\_waw2bo2_compat::waw_array_remove(self.attractor_array,zombie); }')
        self.assertEqual(count, 4)
        self.assertIn('self.attractor_positions[i][0]==attractor_pos[0]', result)
        self.assertEqual(result.count('::waw_poi_record_remove('), 2)
        self.assertIn('::waw_array_remove(self.attractor_array,zombie)', result)
        self.assertEqual(self.translate(result)[1], 0)

    def test_record_helpers_compare_positions_and_preserve_surviving_records(self):
        script = gsc.parse((Path(gscport.COMPAT_DIR) / '_waw2bo2_compat.gsc').read_text(), 'compat')
        bodies = {}
        for name in ('waw_poi_record_available', 'waw_poi_record_remove'):
            fn = script.functions[name]
            bodies[name] = gsc.emit(script.tokens[fn.body_open:fn.body_close + 1])
        self.assertIn('array[i][0] == value[0]', bodies['waw_poi_record_available'])
        self.assertIn('array[i][0] != value[0]', bodies['waw_poi_record_remove'])
        self.assertIn('result[result.size] = array[i]', bodies['waw_poi_record_remove'])


if __name__ == '__main__':
    unittest.main()
