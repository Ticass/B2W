import unittest
from types import SimpleNamespace

from waw2bo2 import hulls, world


def box(lo, hi, contents=1):
    planes = [((-1.0, 0.0, 0.0), -lo[0], 0), ((1.0, 0.0, 0.0), hi[0], 0), ((0.0, -1.0, 0.0), -lo[1], 0),
              ((0.0, 1.0, 0.0), hi[1], 0), ((0.0, 0.0, -1.0), -lo[2], 0), ((0.0, 0.0, 1.0), hi[2], 0)]
    return world.Brush(tuple(lo), tuple(hi), contents, planes, [])


class UnlistedWorldBrushTests(unittest.TestCase):
    def test_complete_ownership_keeps_entity_and_unreferenced_brushes_out_of_world(self):
        # A brush shared by the world's BSP leaves and a door stays entity-owned.
        # An unused brush outside every entity bound must not become a wall.
        clip = SimpleNamespace(brushes=[box((0, 0, 0), (10, 10, 10)),
                                        box((-4, -4, -4), (4, 4, 4)),
                                        box((500, 500, 0), (600, 600, 50))], materials=[],
                               brush_ownership_complete=True,
                               submodels=[world.SubModel((0, 0, 0), (0, 0, 0), [0, 1]),
                                          world.SubModel((-5, -5, -5), (5, 5, 5), [1])])
        out, summary = hulls.collision_brushes(clip)
        self.assertEqual([b["mins"] for b in out], [[0, 0, 0]])
        self.assertEqual(summary["world_brushes_recovered_unlisted"], 0)
        self.assertEqual(summary["unreferenced_brushes"], 1)
        self.assertEqual(len(hulls.submodel_records(clip)[0]["brushes"]), 1)

    def test_unlisted_brushes_outside_entity_local_bounds_are_world(self):
        brushes = [box((0, 0, 0), (10, 10, 10)),              # listed world brush
                   box((-4, -4, -4), (4, 4, 4)),               # door brush (entity local space)
                   box((500, 500, 0), (600, 600, 50), 0x2080),  # unlisted missile clip in the world
                   box((-2, -2, -2), (2, 2, 2))]               # unlisted, inside a door's local bounds
        clip = SimpleNamespace(brushes=brushes, materials=[], static_models=[],
                               submodels=[world.SubModel((0, 0, 0), (0, 0, 0), [0]),
                                          world.SubModel((-5, -5, -5), (5, 5, 5), [1])])
        out, summary = hulls.collision_brushes(clip)
        self.assertEqual(summary["world_brushes_recovered_unlisted"], 1)
        self.assertEqual(summary["unlisted_brushes_ambiguous_skipped"], 1)
        self.assertEqual(sorted(tuple(b["mins"]) for b in out), [(0, 0, 0), (500, 500, 0)])


    def test_detail_only_world_brushes_collide_with_nothing(self):
        # WaW bunker hatch: a "portal" brush with only CONTENTS_DETAIL (players
        # drop through it in WaW); a detail player clip keeps its clip bits.
        clip = SimpleNamespace(brushes=[box((0, 0, 0), (10, 10, 1), 0x8000000),
                                        box((0, 0, 0), (10, 10, 50), 0x8030200),
                                        box((20, 0, 0), (30, 10, 1))], materials=[],
                               brush_ownership_complete=True,
                               submodels=[world.SubModel((0, 0, 0), (0, 0, 0), [0, 1, 2])])
        out, summary = hulls.collision_brushes(clip)
        self.assertEqual([b["contents"] for b in out], [0x8030200, 1])
        self.assertEqual(summary["flag_only_brushes_dropped"], 1)

class StaticModelCollisionTests(unittest.TestCase):
    @staticmethod
    def tri():
        # XModelCollTri_s for corners (0,0,0), (10,0,0), (0,10,0): plane z=0, s=x/10, t=y/10
        return (0.0, 0.0, 1.0, 0.0, 0.1, 0.0, 0.0, 0.0, 0.0, 0.1, 0.0, 0.0)

    def model(self, contents=1, surfaces=True, origin=(100.0, 0.0, 0.0)):
        surfs = [world.CollSurf(1, 0, (0, 0, 0), (10, 10, 0), -1, [self.tri()])] if surfaces else []
        return world.ClipStaticModel("house", origin, (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0),
                                     (origin[0], 0.0, 0.0), (origin[0] + 10, 10.0, 0.0), contents, surfs)

    def test_static_models_stay_native_not_brushes(self):
        clip = SimpleNamespace(brushes=[], materials=[], submodels=[],
                               static_models=[self.model(), self.model(contents=0), self.model(surfaces=False)])
        out, summary = hulls.collision_brushes(clip)
        self.assertEqual(out, [])
        records, summary = hulls.static_model_records(clip)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["name"], "house")
        self.assertEqual(records[0]["invScaledAxis"], [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0])
        self.assertEqual(summary["static_models_without_collision"], 2)
        self.assertEqual(summary["static_model_triangles_outside_waw_bounds"], 0)

    def test_transform_mismatch_is_counted(self):
        bad = self.model()
        bad.absmin, bad.absmax = (500.0, 500.0, 0.0), (510.0, 510.0, 0.0)
        clip = SimpleNamespace(static_models=[bad])
        self.assertEqual(hulls.static_model_records(clip)[1]["static_model_triangles_outside_waw_bounds"], 1)



class CollisionMaterialSlotTests(unittest.TestCase):
    def test_triangles_keep_their_clip_material_and_noncolliding_ones_are_dropped(self):
        from waw2bo2 import fbx
        mats = [world.ClipMaterial('solid', 0, 1), world.ClipMaterial('decal', 0, 0), world.ClipMaterial('missileclip', 0, 0x2080)]
        clip = SimpleNamespace(indices=[0, 1, 2] * 5, triangle_materials=[2, 0, 1, 0xFFFF, 2], materials=mats)
        per_triangle, slots = fbx.collision_material_slots(clip)
        self.assertEqual(per_triangle, [0, 1, None, None, 0])
        self.assertEqual(slots, [2, 0])

    def test_dumps_without_triangle_materials_stay_solid(self):
        from waw2bo2 import fbx
        clip = SimpleNamespace(indices=[0, 1, 2] * 2, triangle_materials=[], materials=[])
        self.assertEqual(fbx.collision_material_slots(clip), ([0, 0], [-1]))


if __name__ == "__main__":
    unittest.main()
