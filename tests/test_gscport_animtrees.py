import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gscport


class AnimtreeRegistrationTests(unittest.TestCase):
    def test_staged_animtrees_follow_zm_init_on_server_and_client(self):
        # BO2: "Unrecognized animtree '%s'. You may need to call ScriptModelsUseAnimTree()";
        # server and client must register in the same order (both register zm_ally in _zm::init)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gsc, csc = root / "main.gsc", root / "main.csc"
            gsc.write_text("main()\n{\n    maps\\mp\\zombies\\_zm::init();\n    other();\n}\n", encoding="utf-8")
            csc.write_text("main()\n{\n    clientscripts\\mp\\zombies\\_zm::init();\n    other();\n}\n", encoding="utf-8")
            self.assertTrue(gscport.hook_bo2_animtrees(gsc, csc, root, ["nuketown"]))
            self.assertIn("_zm::init();\n    maps\\mp\\waw\\_waw2bo2_animtrees::init();\n    other();",
                          gsc.read_text(encoding="utf-8"))
            self.assertIn("_zm::init();\n    clientscripts\\mp\\waw\\_waw2bo2_animtrees::init();\n    other();",
                          csc.read_text(encoding="utf-8"))
            for script in ("maps/mp/waw/_waw2bo2_animtrees.gsc", "clientscripts/mp/waw/_waw2bo2_animtrees.csc"):
                text = (root / script).read_text(encoding="utf-8")
                self.assertIn('#using_animtree( "nuketown" );', text)
                self.assertIn("scriptmodelsuseanimtree( #animtree );", text)
            self.assertFalse(gscport.hook_bo2_animtrees(gsc, csc, root, ["nuketown"]))
            self.assertFalse(gscport.hook_bo2_animtrees(gsc, csc, root, []))
            # a call left after _load::main by an earlier run moves after _zm::init
            gsc.write_text("main()\n{\n    maps\\mp\\zombies\\_load::main();\n    maps\\mp\\waw\\_waw2bo2_animtrees::init();\n"
                           "    maps\\mp\\zombies\\_zm::init();\n}\n", encoding="utf-8")
            self.assertTrue(gscport.hook_bo2_animtrees(gsc, csc, root, ["nuketown"]))
            text = gsc.read_text(encoding="utf-8")
            self.assertEqual(text.count("_waw2bo2_animtrees::init();"), 1)
            self.assertIn("_zm::init();\n    maps\\mp\\waw\\_waw2bo2_animtrees::init();", text)


if __name__ == "__main__":
    unittest.main()
