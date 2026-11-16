import re
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]


class ModelVertexColourTests(unittest.TestCase):
    """WaW models keep their vertex colours (2026-11-11: Der Riese song jars
    drew white; the liquid's green is its vertex colour)."""

    def test_t6_linker_reads_gltf_vertex_colours(self):
        header = ROOT / "vendor/OpenAssetToolsT6/src/ObjCommon/XModel/Gltf/JsonGltf.h"
        if not header.is_file():
            self.skipTest("native sources not restored")
        binding = re.search(r"NLOHMANN_DEFINE_TYPE_EXTENSION\(JsonMeshPrimitivesAttributes,([^)]*)\)",
                            header.read_text(encoding="utf-8"))
        self.assertIsNotNone(binding)
        self.assertIn("COLOR_0", binding.group(1))

    def test_waw_model_vertex_colour_keeps_its_byte_order(self):
        # Unlike WaW world vertices (D3DCOLOR, world._color), model vertex
        # colours are R G B A as dumped: swapped, the Der Riese jar liquid
        # turned yellow instead of its green (playtest 2026-11-11).
        patch = (ROOT / "vendor/OpenAssetTools.patch").read_text(encoding="utf-8")
        self.assertNotIn("XModelToCommonConverter.cpp.template", patch)


if __name__ == "__main__":
    unittest.main()
