# Source player faces on T6

WaW's `_loadout` selects the original third-person character models. T6's
client `_zm::init` also runs `_load::main`, which installs
`_clientfaceanim_zm::doface` as `level._faceanimcbfunc`. Player-spawn callbacks
then start native `pf_*` facial clips from `zombie_player` on those WaW
skeletons. The face bone names overlap, but their local transforms differ;
native face poses can distort the mouth, lids and eyeballs.

When the script port reports `characters=true` (WaW `_loadout` owns player
appearance), `gscport.hook_bo2_client_characters` clears the native face
callback after `start_zombie_stuff()` and before the source client entry.
It also clears `level.face_anim_tree`, which guards the native corpse-face
helper. Source client facial setup may still install its own controller.
Maps using native player appearance do not receive this hook. No character
model, face bone, skinning weight or first-person viewmodel is replaced.

October 30 validation: the bot joined as a real player with source model
`waw_world_model/char_rus_guard_chernova_zomb`. The candidate fix compiled,
linked with zero missing assets and was installed locally. The full suite
ran 845 tests successfully, one skipped. The order/idempotence regression is
in `tests/test_gscport_appearance.py`. Visual confirmation of the new build
is pending a map reload and user-provided screenshots; computer use was
stopped at the user's request.
