import unittest
from waw2bo2 import gsc, gscport, hulls, world
from waw2bo2.t6api import T6Api


class LaunchTests(unittest.TestCase):
    def test_chained_velocity_volume_includes_activation_pad(self):
        text = '''launch(pad) {
            volume = getEnt(pad.target, "targetname");
            if (self isTouching(volume)) { self setVelocity((0,0,1000)); }
            if (self isTouching(volume)) { self notify("other"); }
        }'''
        tokens = gsc.tokenize(text)
        self.assertEqual(gscport.bridge_launch_triggers(tokens), 1)
        result = gsc.emit(tokens)
        self.assertIn('if (self isTouching(pad) || self isTouching(volume))', result)
        self.assertIn('if (self isTouching(volume)) { self notify', result)
        self.assertEqual(gscport.bridge_launch_triggers(gsc.tokenize(result)), 0)

    def test_native_velocity_call_is_routed_through_compat(self):
        api = T6Api({}, {}, methods={"setvelocity": (1, 1)})
        translator = gscport.Translator(gscport.Sources([], [], None), api, gscport.PortReport())
        script = gsc.parse('f() { self setVelocity((0,0,1000)); }', 'test')
        self.assertEqual(translator.resolve_builtin('setvelocity', 1, 'test', script, set(), True),
                         gscport.COMPAT + '::waw_setvelocity')


class BrushVertexTests(unittest.TestCase):
    def test_plane_only_slanted_brush_keeps_its_shape(self):
        planes = [((-1,0,0),0,0), ((1,0,0),10,0), ((0,-1,0),0,0),
                  ((0,1,0),10,0), ((0,0,-1),0,0), ((0,0,1),10,0),
                  ((1,1,0),10,0)]
        brush = world.Brush((0,0,0), (10,10,10), 1, planes, [])
        vertices = hulls.world_brush(brush, [])['verts']
        self.assertEqual(len(vertices), 6)
        self.assertTrue(all(v[0] + v[1] <= 10.01 for v in vertices))
        self.assertNotIn([10,10,10], vertices)

    def test_invalid_hull_fails_instead_of_emitting_null_vertices(self):
        with self.assertRaises(ValueError):
            hulls.brush_vertices([((1,0,0),0), ((-1,0,0),-1)])


if __name__ == '__main__':
    unittest.main()
