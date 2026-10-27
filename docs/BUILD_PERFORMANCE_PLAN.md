# Plan: complete map rebuilds in under 60 seconds

Investigation: 2026-10-25. Target machine: Ryzen 7 7800X3D (8 cores/16 threads),
RTX 3080 (10 GiB VRAM), 31.1 GiB usable system RAM, WD Black SN850X NVMe SSD.

## Target and measurement contract

Target a complete, validated, locally installable rebuild in **less than 60 seconds**
on this machine. Preserve geometry, collision, materials, models, scripts, effects,
sound, and required audits. Do not reach the target by dropping content, reducing
quality, skipping validation, or rebuilding a special reduced version of a map.

Measure these separately:

1. **Full rebuild:** stock/source extraction caches are already prepared, but all
   map conversion, compilation, linking and validation outputs are regenerated.
   Disable converted-asset caches for this measurement. This is the main target.
2. **Incremental rebuild:** unchanged converted assets may be reused; exercise
   no changes, a script edit, one texture edit, one sound edit, and converter changes.
3. **Cold conversion:** extraction and all cache population included. Report this
   independently; a warm incremental result is not evidence for a full rebuild.

Time from build request to validated final output, including subprocess startup,
dependency preparation, copying and local packaging/install when requested.
Report ZIP export separately and also include it when an exported ZIP is the
requested deliverable. Game startup/playtesting is a separate measurement.
Record median and p95 over repeated builds, with machine load and cache state.

An arbitrary map can contain arbitrarily much content: a universal 60-second
guarantee is not supportable. Establish the target first on Der Riese, Shi No Numa,
Verruckt, McDonald's and a large custom map such as Project X, with asset/output
sizes reported. No sub-minute full-build result has been established yet.

## Evidence collected

Recent successful build logs on this workspace show:

| Timed operation | Der Riese | Verruckt | Shi No Numa |
|---|---:|---:|---:|
| Main material conversion pass | 75.1 s | 85.7 s | 93.4 s |
| Additional material conversion pass | 13.6 s | 10.2 s | 11.6 s |
| Image conversion | 0.9 s | 1.1 s | 1.2 s |
| Lightmap conversion | 1.1 s | 1.1 s | 1.2 s |
| Standalone shader translation stage | 7.6 s | 7.0 s | 7.5 s |
| Audio decoding | 7.5 s | 6.7 s | 7.6 s |
| Gameplay mod zone writing | 13.186 s | 11.999 s | 13.084 s |
| Shader-bound mod zone writing | 20.239 s | 18.021 s | 19.840 s |
| Map zone writing | 14.585 s | 15.478 s | 15.758 s |

Sources: `work/der_riese/build_source_dogs.log`,
`work/verruckt/build_waw_drives.log`,
`work/shi_no_numa_dlc2/build_waw_drives.log`; their corresponding
`build/mod_build/{linker,shader_linker}.log` and `build/stage/*_bridge.log`.

The phase timers are incomplete and sometimes nested. For example the small
effects-material pass is included in the effects timer; do not sum it twice.
After removing that obvious nesting, recorded staging intervals total about
149.1 / 149.1 / 169.0 seconds respectively. Adding just the three recorded zone
writing intervals gives about 197 / 195 / 218 seconds. These are partial historical
measurements, not newly measured end-to-end baselines. Asset loading, other native
work, audits and packaging also need measurement.

A new isolated cProfile probe regenerated 48 Der Riese materials into a scratch
directory, with zero reported material errors. It took 4.015 seconds under profiling:

- `bind_material`: 2.825 seconds cumulative.
- `shaders.translate`: 1,062 calls, 1.461 seconds cumulative.
- `index_donors`: 0.952 seconds, scanning the stock material tree.
- 2,178 text reads and 2,808 file opens across the probe.
- Native assembly disassembly: 210 cache misses, about 0.109 seconds cumulative.

This sample is the first 48 resolvable non-FX report materials, not a random or
complete material benchmark. Its timings must not be extrapolated as a measured
full-map improvement. Evidence: `work/performance_investigation_20261025/materials.prof`
and `materials_profile.txt`.

Source inspection confirms:

- `t6bridge.stage_materials` processes materials serially and indexes stock donors
  anew per call. Binding repeatedly translates and reflects shader programs.
- `shaders.compile_hlsl` has a 128-entry, process-local LRU; this is not a durable
  build cache. D3DCompile compiles shader source; running those shaders on a GPU is
  a different operation.
- `parallel.ordered_map` already provides bounded CPU thread/process workers.
  Images, audio and standalone shader batches already use parallel processing.
- The driver serializes gameplay build, script compilation, map link and audits.
- Gameplay shader activation copies, extracts and relinks a complete baseline mod.
- Native chunk compression uses `Z_BEST_COMPRESSION` (level 9), with sequential
  chunk processing. T6 chunks are 0x8000 bytes, with 0x7fc0 bytes maximum input.
- Script compilation deletes its work directory each time; packaging hashes both
  copies of equal-size files before deciding whether copying is necessary.

A new raw-DEFLATE probe used 8,426,469 bytes sampled from staged shader/model
files, split into 258 chunks matching the native maximum input size:

| zlib level | Compression time | Compressed bytes |
|---|---:|---:|
| 1 | 0.0193 s | 2,407,204 |
| 3 | 0.0491 s | 1,650,712 |
| 6 | 0.0882 s | 1,420,781 |
| 9 | 0.2457 s | 1,415,713 |

Every chunk decompressed back to identical bytes. Level 6 was about 2.8x faster
than level 9 in this single probe, for about 0.36% more compressed bytes.
This is not a benchmark of complete serialized zones, encryption, the bundled
native zlib build, or in-game loading. Evidence: `compression_probe.json` in the
same investigation directory. Benchmark actual zone chunks before choosing a default.

## Implementation order

### 1. Measure the whole pipeline

Add a machine-readable build timeline around every Python phase and native
subprocess, including failed retries, filesystem scanning, extraction, IPAK work,
serialization, compression, encryption, audits and packaging. Capture process-tree
CPU time, peak memory, bytes read/written, asset counts and cache hits/misses.

Run isolated output builds; do not overwrite the user's installed maps. Establish
full and incremental baselines on the benchmark maps. Use perf_counter, cProfile
for Python, and native timers/profiling for OAT and the proprietary tools. Account
for profiler overhead and perform final timing runs without profiling.

Exit criterion: every substantial portion of elapsed build time is attributable;
the timeline identifies the critical path and worker oversubscription.

### 2. Eliminate repeated material/shader work

Create a per-build immutable asset context: donor index, parsed source/donor JSON,
shader assembly/IR, reflection signatures and technique matching candidates.
Build the donor index once. Index paths instead of probing many roots repeatedly,
while retaining the exact current source precedence.

Deduplicate shader translation and HLSL compilation by their actual semantic
inputs. A binding key must include source bytecode/pass data, donor interfaces,
embedded material constants, geometry/lightmap ABI, light falloff/shadow options,
compiler flags/version and converter schema/version. A filename alone is unsafe.
Keep cached IR immutable or clone before mutation.

Split material work into immutable analysis, unique shader jobs and deterministic
output assembly. Dispatch independent Python-heavy jobs to a reusable process pool;
benchmark 4/8/12 workers on this CPU. Use threads only for suitable native calls or
I/O. Merge reports on the main process; avoid concurrent writes to shared shader
and technique files. Preserve rejection rules, naming and stable output ordering.

Unbound shader diagnostics currently scan thousands of programs in source roots.
Share IR with bound conversion and cache diagnostic results. Investigate whether
unreachable diagnostics belong in an explicit full-source audit, without losing
validation for any reachable map shader or hiding unsupported assets.

Provisional engineering target: material conversion plus associated shader analysis
in 10–15 seconds on the representative maps. This is a budget, not a measured result.

### 3. Reduce native zone-writing cost

Instrument serialization, compression and encryption separately. Compare raw
DEFLATE levels 1/3/6/9 on actual zone chunks, including final output sizes and
roundtrip/in-game compatibility. Level 6 is the first candidate from the probe.
Keep the existing T6 chunk structure, flags, buffer limits and encryption protocol.

Parallelize independent chunk compression in a bounded native pool. Reassemble
chunks in their original order. Preserve logical stream assignment and sequential
per-stream Salsa20 state; do not parallelize stateful encryption without a proof
that its state dependencies are preserved. Bound memory and reject overflow.

Investigate avoiding the compressed baseline mod write followed by decompression
and a second full write. Long-term options are an in-memory baseline asset bridge
or extending the native loaders to consume the required raw formats directly.
This is the highest-risk structural change: the current two-pass path preserves
asset pools and texture ABI and repairs nested flame tables. Keep it until the
replacement passes the same content audits and runtime tests.

Exit criterion: materially faster native writes with equivalent decoded assets
and successful Plutonium loading; no material, texture or weapon ABI regressions.

### 4. Overlap independent work safely

Build a dependency graph for geometry, scripts, materials, images, audio and final
zones, with explicit ownership of generated files. Resolve dependencies once.
Queue audio decoding when its source list is ready instead of waiting for all
unrelated visual staging. Share reusable worker pools and a global CPU/memory
budget so several 16-worker stages cannot overwhelm an 8-core machine.

Freeze shared staging inputs before starting independent map/gameplay links.
The current build-mod path can prune unavailable weapon references in staged
scripts, so simply running build-mod and compile-scripts concurrently is unsafe.
Move such decisions into an earlier deterministic preparation phase.

Run independent final audits concurrently or reuse validated immutable asset
records keyed to exact built content. Retain final output coverage. Do not cache
an audit by name alone or skip an audit merely because a build is slow.

### 5. Add correct incremental rebuilding

Store converted assets and native compiler products by content hash, including
transitive dependencies, source precedence, all relevant options and tool versions.
Invalidate only affected nodes; publish outputs atomically and cache only complete
successful results. Record cache-hit explanations for debugging.

Use trustworthy manifests to avoid repeatedly hashing/copying unchanged large
IPAKs and sound banks; verify manifests when sources or outputs change. Avoid
changing timestamps for unchanged generated inputs. Preserve the existing
hand-edited staging/resume behavior explicitly, with a distinct reproducible
full-rebuild mode. Converter changes should invalidate affected transformations,
not silently reuse stale whole-stage receipts.

This should make ordinary script edits much faster, but it does not count as
meeting the full-rebuild target.

### 6. GPU acceleration only after a measured break-even test

The recorded image and lightmap stages together cost about two seconds. Making
them instantaneous would still leave the present build far above one minute.
The current dominant work is string/IR processing, dependency work and native
zone writing. CuPy cannot automatically accelerate these operations or the BO2
linkers.

Prototype optional CuPy/CUDA processing only if future profiles show a sizeable
numeric stage: large lightmap transforms, vertex-array transforms or texture
operations. Include host/device copies, batching, initialization and fallback in
the timing. Retain CPU operation for other hardware and exact encoding behavior.
Require a meaningful end-to-end gain on real maps before adding a GPU dependency.

GPU compression is a separate research option after CPU chunk optimization.
Evaluate a compatible raw-DEFLATE backend, not a different codec: NVIDIA documents
GDeflate as a distinct format, so it is not a drop-in T6 compression replacement.
Check Windows/runtime availability, redistribution, chunk framing, output limits,
encryption interaction and CPU/GPU transfer overhead before integration.

## Provisional end-to-end budget

These are allocation targets to drive engineering; they are not forecasts.

| Component on the critical path | Budget |
|---|---:|
| Source preparation and dependency setup | 4 s |
| Complete staging, with independent jobs overlapping | 20 s |
| Final compilation/linking, with compatible compression and safe overlap | 20 s |
| Required final audits | 8 s |
| Manifest publication/local packaging/install | 5 s |
| Margin | 3 s |

Aim for a measured p95 below 60 seconds; the nominal budget reaches 60 seconds,
so releases need positive margin, not a rounded 60-second success. If the existing
two-pass gameplay path or proprietary linker exceeds its allocation, report the
measured limit and prioritize the structural linker work rather than masking it
with an incremental result.

## Correctness and acceptance

- Compare complete asset inventories and semantic roundtrips: materials/pass
  arguments, shader linkage, scripts, sounds, models/hitboxes, collision and world
  data. For compression-only changes compare identical decompressed bytes.
- Verify determinism where supported. Different compression bytes or compiler
  metadata are not themselves content failures; compare decoded semantics.
- Exercise invalidation with source/patch/options/compiler/converter changes,
  interrupted builds and stale/deleted outputs. Enforce source priority.
- Load and playtest representative maps in Plutonium, including mystery box,
  monkey bombs, Pack-a-Punch, lighting, audio, FX and map-specific source behavior.
- Publish full, incremental and cold timings separately, with output sizes, RAM
  peaks, cache state, median/p95, and the build version.

## External references

- Microsoft D3DCompile API:
  https://learn.microsoft.com/en-us/windows/win32/api/d3dcompiler/nf-d3dcompiler-d3dcompile
- CuPy installation/platform requirements: https://docs.cupy.dev/en/stable/install.html
- NVIDIA nvCOMP formats and APIs: https://docs.nvidia.com/cuda/nvcomp/

Investigation produced only this plan and scratch benchmark artifacts. It did not
change the converter implementation, native tools, or installed maps.

## Results: pass 1 (2026-10-26, branch `build-perf`)

Code: worktree `C:\WawConverter-perf`, branch `build-perf` from `der-riese-accuracy`
782b489, uncommitted. Nothing installed. Full rebuilds (resume off, extraction
caches warm, no converted-asset cache), isolated output folders, otherwise idle machine.

**Step 1 done: build timeline.** `waw2bo2.timeline` writes
`stage/build_timeline.jsonl` and a summary `build_timeline.json` on every driver
build: every `progress.timed` phase, driver step, build-mod part and audit as a span;
every native subprocess (OS CPU, peak working set, I/O bytes, return code); every
spawned Python worker. Records nest by parent id across processes.
`python -m waw2bo2.timeline <jsonl> [min s]` prints the tree.

**Baselines vs this pass** (wall seconds; the run was cut short at the user's request,
so n is small and there is no p95):

| | Der Riese base | Der Riese new | Shi No Numa base | Shi No Numa new |
|---|---:|---:|---:|---:|
| Full build | 426.7, 410.8 | 301.7, 300.3 | 386.4 | 295.2 |
| All material passes | 113.3, 107.4 | 14.1, 14.1 | 101.9 | 15.2 |
| Stage-bridge step | 279.2, 276.0 | 165.2, 164.7 | 248.4 | 155.1 |

**Step 2: material/shader work.** WaW materials now convert in a spawn process
pool (batches of 8; files and report lines are applied on the staging process in
name order, as a serial run writes them). Unbound and bound `shaders.translate`,
reflection parsing and taint analysis are memoized per process by their exact
inputs. Each distinct HLSL program (source, profile, flags) compiles once per stage
through a shared temporary folder. Identical rewrites of content-named files are
skipped. Replays of the captured 824-material Der Riese call: 67.2 s serial before,
49.5 s serial memoized, 8.0 s with the pool (12 and 16 workers equal; 4: 18.5 s).
Two hazards found: `techsets.LAYERED_VERTEX_DATA` is process state set by world
staging (now passed to workers), and the donor index is not cached across calls.

**Equality.** Old and new code, `PYTHONHASHSEED=0`, both maps: staged trees
identical (35,070 and 31,481 files) except the report's path to AssetViewer
tables (checkout location). Full unlink of the map zones: all assets identical
(5,717 and 5,436 files) except heap pointers in a debug clipmap text. `mod.ff`
decodes to identical assets; only the asset order in its zone listing differs.
The material/techset audits pass. Two **pre-existing** nondeterminisms, unchanged:
unbound translation output order depends on the hash seed
(`ps_particle_zdownsample` oDepth/oC0), and the shader relink linker writes a
different asset order on every run with identical inputs.

**Step 3: zone compression, measured on real chunks** (decrypted and inflated from
the built Der Riese zones; Python zlib; the stored chunks are within 0.01% of level 9):

| Zone (plain size) | Level 9 serial | Level 9, 8 threads | Level 6 serial | Level 6 size |
|---|---:|---:|---:|---:|
| Map (152 MB, 4,644 chunks) | 4.24 s | 0.67 s | 1.71 s | +3.2% |
| mod.ff (461 MB, 14,082 chunks) | 6.38 s | 1.06 s | 3.94 s | +1.5% |

Salsa20 + SHA1 for every chunk: 0.2 s / 0.5 s. Chunk boundaries do not depend on
compressed sizes, and the encryption state depends only on earlier chunks of the
same stream. So compressing chunks on a pool at **unchanged level 9**, then
encrypting in order, gives byte-identical zones. Estimated saving: about 3.5 s per
map link and about 5 s per mod.ff write (three per build), roughly 19 s. Lowering
the level saves less and grows zones. Not implemented yet: needs an isolated native
build and Plutonium load testing.

**Largest remaining costs (new, Der Riese).**
- build-mod, 82.5 s: two mod links of 45.7 s total, then shader relink and check, 34.2 s.
- Stage localization, 30.8 s: 109 serial Unlinker runs.
- Decode audio, 29.6 s: about 3,500 ffmpeg launches.
- Bridge link, 22.2 s.
- Audits, 18 s.
- Sound sources and dependencies, 24 s.

Next in plan order: implement parallel chunk compression in the native writer, then
step 4 overlap. Localization extraction is independent, serial work and an early
candidate. Tools for repeating this: `work/perf` in the worktree (`bench.py`,
`compare_stage.py`, `unlink_compare.py`, `zone_chunks.py`, `series_summary.py`).
