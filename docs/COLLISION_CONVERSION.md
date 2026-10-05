# Collision conversion: invariants that must never regress

WaW (T4) and BO2 (T6) store collision in almost the same structures, but the
engines *consume* them differently. Every rule below was measured in the
mapped Plutonium T6 binary (`work/T6_mapped_vision.exe`, IDA) or in WaW
(`work/CoDWaW_mapped.exe`), and each one has caused a visible in-game bug when
it was violated. Read this before touching `ClipMapLinker.cpp`, `hulls.py`,
`fbx.write_collision_fbx` or the T4 clip dumper.

## 1. Terrain partitions MUST be convex (the invisible-wall bug)

T6 builds a **GJK convex shape** from every terrain partition's unique-vertex
list (`gjk_partition_t`, created by sub_47AE30, which is the only reader of
`clipMap.info.uinds`; gathered by sub_6F3F30 for player/physics collision).
The player therefore collides with the **convex hull** of each partition.
Rays (bullets, grenades, `bullettrace`, `playerphysicstrace`) still test every
triangle individually (sub_4F6DB0 -> sub_5D1230 / sub_881940).

A non-convex partition = invisible, player-only solid: hulls that fill door
arches, smooth mounds over rubble ("climbing"/"floating"), wedges blocking
stairs. Bullets and grenades pass because they never use the hull.

Stock zm_nuked: 91% of 6,866 partitions have no vertex in front of any face,
only 16 dent by more than 8 units. Our Session 21 "16 consecutive Morton
triangles" partitions had 3,272 partitions denting > 8 units (worst 318).

**Rule (ClipMapLinker::LoadPartitions):** a partition grows from a seed over
shared edges of one collision surface (one clip material), and a triangle may
join only while the partition stays convex:
- no partition vertex lies in front of any partition face (epsilon 0.1), front
  normal = T6's `(v2 - v0) x (v1 - v0)`;
- no partition vertex lies beyond any boundary edge, measured in the face's
  plane (rejects flat L shapes and disjoint pieces);
- at most 16 triangles and 256 units of extent (stock limits).

Nuketown result: 31,540 partitions (mean 2.5 triangles), standing player
passes the arch that blocked it, all 78,035 source triangles kept.

## 2. ...but partitions must not be one triangle each (the grenade bug)

Movement/missile queries gather at most **512** partitions and 512 brushes per
query (sub_5C7760 / sub_6A5CE0; leaves capped at 256 by sub_6F6C70) and
silently drop the rest. One triangle per partition overflowed next to dense
model collision (houses, the bus): grenades flew through walls. Convex groups
keep a 48-unit query centred on geometry at <= 385 partitions on Nuketown.
Seeds are taken in Morton order of triangle centroids to keep partitions
compact. Do not trade convexity for count, or count for convexity.

## 3. Per-triangle clip materials

WaW stores a clip material per collision triangle (leaf aabb node). Only half
of Nuketown's triangles are solid (0x1); most model collision is missile/shot
clip 0x2080 (stops bullets/grenades, not players), plus player/monster clip and
glass. One FBX material per WaW clip material (`clip_<n>`) and
`BSP/clipmaterials.json`; parents in the aabb tree are material-homogeneous
because the engine tests the parent material before its children
(sub_6B9730 / sub_885460). Contents bits are compatible: WaW player mask
0x281C011, T6 0x2818011; WaW grenade 0x280E091, T6 0x280E893.

## 4. Walkable edge bits come from WaW

`triEdgeIsWalkable` (bit 3t+e): sub_881940 copies an edge's bit into the trace
when a capsule touches that edge; sub_6D8770 only derives walkability from
normal.z >= 0.7 when the bit is clear. All-ones (the OAT placeholder) makes
every edge standable. T4 clip dump v6 carries WaW's bits;
`BSP/collisionedges.bin` matches them to linked triangles by corner positions
(cyclic rotation aware, because the linker reorders triangles). Nuketown:
84,591 of 234,105 edges walkable (36%; stock BO2 ~50%). Linker log must say
`78035 of 78035 triangles matched`.

## 5. Static models, brushes, brush models

- Static-model collision stays per model in `clipMap.staticModelList`
  (`BSP/staticmodels.json`), never as brushes (the 512 brush cap).
- World brushes come from the WaW clipmap verbatim (planes, verts, flags);
  entity-owned brushes stay out of the world tree (complete ownership, clip
  dump v5). The leaf-brush tree never contains empty branches (point traversal
  has no contents early-out and loops forever).
- Brush-model brushes are in entity-local space in both games.
- WaW gives every map-placed `script_model` contents 0x2080 and its model
  bounds (SP_script_model, WaW sub_531C40); without collSurfs it is traced as
  that box. T6 (sub_5485E0) only traces collSurfs, so the bridge adds a box
  collSurf to such xmodels. KNOWN ISSUE: bounds currently come from the LOD0
  GLTF assuming game = (x, -z, y); some skinned exports (vending machines) are
  in other axes. Take the bounds from the loaded model instead.

## 6. Triangle winding

WaW and T6 use the same front face, `(v2 - v0) x (v1 - v0)`. The FBX writer
and BSPCreator each reverse, which cancels; never "fix" one side alone.
`tools/audit_collision_roundtrip.py` compares oriented triangles.

## 7. One-sided traversal collision is removed

WaW triangles collide only from their front, which maps use for one-way
passages (Nuketown: crossing back into the spawn area). T6's GJK partition
shapes are closed solids, so those passages block both ways.

User-directed policy (2026-10-04): omit these traversal sheets entirely from
collision so players can cross between map areas in either direction.
`oneway.py` identifies triangles blocking the WaW player mask 0x281C011,
without solid contents, steep (|n.z| < 0.7), and without an opposing face.
Same-facing duplicates are still one-sided. Ordinary solid geometry, floors,
and opposing-face sheets retain collision.

`fbx.collision_material_slots` applies this global filter to both collision
FBX generation and compiled walkable-edge export. The stage report records
`collision_triangles_one_sided_removed` and `ONE_WAY_COLLISION_REMOVED`.
The old `_waw2bo2_oneway.gsc` entry point is a no-op for compatibility with
existing generated map hooks; no movement watcher or teleport is generated.
Nuketown is the validation case: 484 triangles removed (190 clip, 38
clip_nosight_rock, 256 one-sided glass). This intentionally permits passage
from both sides, relaxing the source restriction as requested.

Ruled out on the way (clip dump v8 + `.cmtree.txt` diagnostic): WaW's leaf
`brushContents` and leaf-brush-node `contents` masks reach every brush with its
full contents; leaf-brush kd culling (child[0] = above dist - range) reaches
every brush at its own points; triangle contents equal the leaf aabb material
(no ancestor mask differs). The blocking brushes at other spots are WaW walls.

## How to verify (do this after any collision change)

1. `tools/audit_collision_roundtrip.py <clip.bin> <collision.json>`: every
   triangle, winding and flag matches, and **no partition is non-convex**.
2. Linker log: `Walkable edges ... N of N triangles matched`, `Collision tree:`
   counts, no `exceed` errors.
3. Unlinker clipmap report: `brush tree position contract violations 0`.
4. In game: `tools/bisect_round.ps1` + the debug GSC in
   `%LOCALAPPDATA%\Plutonium\storage\t6\scripts\zm\waw2bo2_viewpos.gsc`
   (HUD position, melee probe that logs hull/line traces, scripted movement
   tests). A standing player must pass the Nuketown yellow-house kitchen arch
   (x 745 -> 885 at y ~490) both ways.

`playerphysicstrace` and `bullettrace` do NOT reproduce player-only walls;
only real movement (setorigin + setvelocity under pmove) does.

## Debug aid

`WAW2BO2_DEBUG_DROP="tris|brushes:x0,y0,z0,x1,y1,z1"` makes the T6 linker drop
terrain triangles / world brushes meeting that box. Used to bisect the arch
wall to the arch's own triangles. Never set it for a real build.
