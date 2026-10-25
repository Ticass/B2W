import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gsc, paths, traversals

SHARED = '''
#using_animtree ("dog");

dog_jump_down( height, frames )
{
	self endon("killanimscript");
	self traverseMode("noclip");
	anim_name = "jump_down_40";
	if ( height == 190 )
	{
		anim_name = "jump_down_190";
	}
	self thread teleportThreadEx( 40.0 - height, 0.1, frames );
	self clearanim(%root, 0.2);
	self setflaggedanimrestart( "traverse", anim.dogAnims[self.animSet].traverse[anim_name], 1, 0.2, 1);
	self animscripts\\shared::DoNoteTracks( "traverse" );
	self clearanim(anim.dogAnims[self.animSet].traverse[anim_name], 0);
	self traverseMode("gravity");
}

teleportThreadEx( verticalOffset, delay, frames )
{
	wait delay;
	self teleport( self.origin + ( 0, 0, verticalOffset / frames ) );
}

DoTraverse( traverseData )
{
	self setflaggedanimknoballrestart( "traverse", traverseData[ "traverseAnim" ], %body, 1, 0.2, 1 );
}
'''

JUMP_DOWN_96 = '''
main()
{
	if( IsDefined( self.is_zombie ) && self.is_zombie )
	{
		if ( self.type != "dog" )
			jump_down_zombie();
		else
			dog_jump_down(96, 7);
	}
}
jump_down_zombie() { DoTraverse( [] ); }
'''

DOG_INIT = '''
main()
{
	self.animSet = "shepherd";
	if ( self animscripts\\utility::is_zombie() )
	{
		self.animSet = "zombie";
	}
}
init()
{
	anim.dogAnims["shepherd"].traverse["jump_down_40"]	= %german_shepherd_traverse_down_40;
	anim.dogAnims["zombie"].traverse["jump_down_40"]	= %zombie_dog_traverse_down_40;
	anim.dogAnims["zombie"].traverse["jump_down_190"]	= %zombie_dog_traverse_down_190;
}
'''


def _roots(tmp: Path) -> list[Path]:
    (tmp / "animscripts/traverse").mkdir(parents=True)
    (tmp / "animscripts/traverse/shared.gsc").write_text(SHARED)
    (tmp / "animscripts/traverse/zombie_jump_down_96.gsc").write_text(JUMP_DOWN_96)
    (tmp / "animscripts/dog_init.gsc").write_text(DOG_INIT)
    return [tmp]


class TraversalTests(unittest.TestCase):
    def test_dog_branch_comes_from_the_source_traversal(self):
        with tempfile.TemporaryDirectory() as tmp:
            dogs = traversals.source_dog_traversals(_roots(Path(tmp)), {"zombie_jump_down_96"})
            self.assertFalse(dogs.errors)
            self.assertEqual(dogs.calls, {"zombie_jump_down_96": ["dog_jump_down( 96, 7 );"]})
            # the animation set dog_init gives zombie actors
            self.assertEqual(dogs.states, {"waw_dog_jump_down_40": "zombie_dog_traverse_down_40",
                                           "waw_dog_jump_down_190": "zombie_dog_traverse_down_190"})

    def test_dog_library_is_transpiled_for_animation_states(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = traversals.source_dog_traversals(_roots(Path(tmp)), {"zombie_jump_down_96"}).library
            script = gsc.parse(library, "library")
            self.assertEqual(set(script.functions), {"dog_jump_down", "teleportthreadex"})
            self.assertIn('self setanimstatefromasd( "zm_traverse", "waw_dog_" + anim_name );', library)
            self.assertIn('self maps\\mp\\animscripts\\zm_shared::donotetracks( "traverse_anim" );', library)
            self.assertNotIn("clearanim", library)
            self.assertNotIn("dogAnims", library)
            self.assertNotIn("DoTraverse", library)

    def test_unconvertible_dog_call_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            roots = _roots(Path(tmp))
            shared = roots[0] / "animscripts/traverse/shared.gsc"
            shared.write_text(SHARED.replace("self traverseMode(\"gravity\");",
                                             "self setanim( anim.dogAnims[self.animSet].traverse[\"x\"] );"))
            dogs = traversals.source_dog_traversals(roots, {"zombie_jump_down_96"})
            self.assertFalse(dogs.calls)
            self.assertTrue(any("no T6 form" in e for e in dogs.errors))

    def test_generated_script_runs_the_source_dog_branch_first(self):
        node = dict(type=paths.T4_NEGOTIATION_BEGIN, animscript="zombie_jump_down_96", targetname="a", target="b",
                    origin=[0, 0, 96], links=[], spawnflags=0, script_linkname="", script_noteworthy="",
                    animscriptfunc="", angle=0, forward=[1, 0, 0], radius=0, minUseDistSq=0, overlap=0)
        end = dict(node, type=paths.T4_NEGOTIATION_END, animscript="", targetname="b", target="", origin=[0, 0, 0])
        waw = dict(nodeCount=2, nodes=[node, end], pathVis="00", visBytes=1)
        _, renamed, summary = paths.convert_paths(waw, {"jump_down_96"},
                                                  {"zombie_jump_down_96": ["dog_jump_down( 96, 7 );"]})
        name = next(iter(summary.generated))
        self.assertEqual(renamed["#0"], name)
        alias, calls = summary.generated[name]
        text = paths.traverse_script(alias, traversals.dog_branch(calls))
        self.assertLess(text.index("waw2bo2_dog_shared::dog_jump_down( 96, 7 );"),
                        text.index('dosimpletraverse( "jump_down_96" );'))
        self.assertIn("if ( isdefined( self.isdog ) && self.isdog )", text)

    def test_dog_states_join_the_native_traverse_state(self):
        asd = "zm_traverse : aliased restart notify traverse_anim\n{\n\tjump_down_127\t\tzombie_dog_traverse_down_126\n}\n"
        atr = "body\n{\n\tzombie_dog_run\n}\n"
        asd, atr, added = traversals.add_dog_states(asd, atr, {"waw_dog_jump_down_40": "zombie_dog_traverse_down_40"},
                                                    lambda n: "waw_anim_" + n)
        self.assertIn("\twaw_dog_jump_down_40\t\t\twaw_anim_zombie_dog_traverse_down_40\n}", asd)
        self.assertIn("jump_down_127", asd)
        self.assertIn("\twaw_anim_zombie_dog_traverse_down_40\n", atr)
        self.assertEqual(added, ["waw_anim_zombie_dog_traverse_down_40"])


if __name__ == "__main__":
    unittest.main()
