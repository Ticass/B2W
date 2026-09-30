import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import bo2equiv


def glb(document: dict) -> bytes:
    body = json.dumps(document).encode()
    body += b" " * (-len(body) % 4)
    return b"glTF" + (2).to_bytes(4, "little") + (20 + len(body)).to_bytes(4, "little") + \
        len(body).to_bytes(4, "little") + b"JSON" + body


class Bo2EquivalentTests(unittest.TestCase):
    def raw(self, base: Path, image_ext: str | None) -> Path:
        raw = base / "bo2" / "raw"
        for folder in ("xmodel", "model_export", "materials", "images"):
            (raw / folder).mkdir(parents=True)
        (raw / "xmodel/bo2_bottle.json").write_text(json.dumps({"lods": [{"file": "model_export/bo2_bottle.glb"}]}))
        (raw / "model_export/bo2_bottle.glb").write_bytes(glb({"materials": [{"name": "mtl_bottle"}]}))
        (raw / "materials/mtl_bottle.json").write_text(json.dumps({"textures": [{"image": "~-gbottle_col"},
                                                                              {"image": "$white"}]}))
        if image_ext:
            (raw / f"images/~-gbottle_col{image_ext}").write_bytes(b"x")
        table = base / "table.json"
        table.write_text(json.dumps({"xmodel": {"waw_bottle": "bo2_bottle"}}))
        return table

    def test_chain_must_be_buildable_and_dds_images_are_listed(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            table = self.raw(base, ".dds")
            found = bo2equiv.Bo2Equivalents(base / "bo2", None, table).find("xmodel", "waw_bottle")
            self.assertEqual((found.name, found.rule, found.dds_images), ("bo2_bottle", "table", ("~-gbottle_col",)))
            self.assertIsNone(bo2equiv.Bo2Equivalents(base / "bo2", {"image": {"~-gbottle_col"}}, table)
                              .find("xmodel", "waw_bottle"))
            self.assertIsNone(bo2equiv.Bo2Equivalents(base / "bo2", {"xmodel": {"bo2_bottle"}}, table)
                              .find("xmodel", "waw_bottle"))

    def test_image_only_in_stock_ipak_is_not_buildable(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            table = self.raw(base, None)
            self.assertIsNone(bo2equiv.Bo2Equivalents(base / "bo2", None, table).find("xmodel", "waw_bottle"))


if __name__ == "__main__":
    unittest.main()
