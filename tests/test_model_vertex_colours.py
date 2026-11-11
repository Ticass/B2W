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

    def test_waw_dump_reads_vertex_colour_as_d3dcolor(self):
        patch = (ROOT / "vendor/OpenAssetTools.patch").read_text(encoding="utf-8")
        hunk = patch[patch.index("XModelToCommonConverter.cpp.template"):]
        self.assertIn("+#ifdef FEATURE_T4", hunk)
        self.assertIn("+                std::swap(vertex.color[0], vertex.color[2]);", hunk)


if __name__ == "__main__":
    unittest.main()
