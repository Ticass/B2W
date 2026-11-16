import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import modelsortorder


def _material(sort_key, dst="one"):
    return {"sortKey": sort_key, "stateBits": [{"colorWriteRgb": True, "srcBlendRgb": "one", "dstBlendRgb": dst}]}


class ModelSortOrderTests(unittest.TestCase):
    """Der Riese jar: glass (surface 0) and liquid (surface 1) share WaW sort
    key 43; T6 donors gave liquid 40 < glass 41, so the glass covered it."""

    def test_later_surface_is_raised_past_the_earlier(self):
        changed = modelsortorder.raise_keys([["glass", "liquid"]], {"glass": 43, "liquid": 43},
                                            {"glass": 41, "liquid": 40})
        self.assertEqual(changed, {"liquid": (40, 42)})

    def test_different_waw_keys_and_kept_order_are_untouched(self):
        self.assertEqual(modelsortorder.raise_keys([["a", "b"]], {"a": 43, "b": 44}, {"a": 41, "b": 40}), {})
        self.assertEqual(modelsortorder.raise_keys([["a", "b"]], {"a": 43, "b": 43}, {"a": 40, "b": 41}), {})

    def test_stage_rewrites_the_staged_material(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "model_export").mkdir()
            (root / "materials/mc").mkdir(parents=True)
            gltf = {"materials": [{"name": "mc/glass"}, {"name": "mc/liquid"}],
                    "meshes": [{"primitives": [{"material": 0}]}, {"primitives": [{"material": 1}]}]}
            (root / "model_export/jar.gltf").write_text(json.dumps(gltf))
            (root / "materials/mc/glass.json").write_text(json.dumps(_material(41, "inv_src_alpha") | {"cameraRegion": "litTrans"}))
            (root / "materials/mc/liquid.json").write_text(json.dumps(_material(40) | {"cameraRegion": "emissiveTrans"}))
            materials = [{"name": "mc/glass", "file": "materials/mc/glass.json", "t4_techset": "mc_l_sm_b0c0s0", "t4_sort_key": 43},
                         {"name": "mc/liquid", "file": "materials/mc/liquid.json", "t4_techset": "mc_unlit_add", "t4_sort_key": 43}]
            self.assertEqual(modelsortorder.stage(root, materials), {"mc/liquid": [40, 42]})
            self.assertEqual(json.loads((root / "materials/mc/liquid.json").read_text())["sortKey"], 42)


if __name__ == "__main__":
    unittest.main()
