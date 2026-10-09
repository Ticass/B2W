import unittest

from waw2bo2 import t6bridge


class ScriptLinkerLogTests(unittest.TestCase):
    def test_bsp_error_interleaved_into_compiled_line(self):
        # Linux/Wine bankjob build with BO2 stock perks: stderr split the last
        # "Compiled" line, so the owned client bootstrap read as missing.
        log = ('Compiled GSC script "clientscripts/mp/waw/_waw2bo2_perks.csc"\n'
               'Compiled GSC scripERROR: Could not open BSP "maps/mp/zm_bankjob_scripts.d3dbsp" '
               'for map "zm_bankjob_scripts"\n'
               't "clientscripts/mp/waw/_waw2bo2_zm.csc"\n'
               'Loaded script "clientscripts/mp/waw/_waw2bo2_zm.csc" (src: disk)\n'
               'Finished with 0 warnings, 1 errors\n')
        compiled, errors = t6bridge.parse_script_linker_log(log)
        self.assertEqual(compiled, ['clientscripts/mp/waw/_waw2bo2_perks.csc',
                                    'clientscripts/mp/waw/_waw2bo2_zm.csc'])
        self.assertEqual(errors, [])

    def test_other_errors_still_reported(self):
        log = ('ERROR: Could not open BSP "maps/mp/x_scripts.d3dbsp" for map "x_scripts"\r\n'
               'ERROR: unknown function foo in "maps/mp/x.gsc"\r\n')
        compiled, errors = t6bridge.parse_script_linker_log(log)
        self.assertEqual(compiled, [])
        self.assertEqual(errors, ['ERROR: unknown function foo in "maps/mp/x.gsc"'])


if __name__ == '__main__':
    unittest.main()
