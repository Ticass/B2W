# Sound gain, attenuation, and voice limits

WaW's stock `DEFAULT` I3DL2 reverb patch is silent (-10000 millibels for
room, reflections, and reverb). BO2's `default` RAD preset has audible
reflections. Without a declared room, a converted map inherited that BO2
effect on all aliases with nonzero sends, particularly gunfire after the
linear gain correction. The converter now reads the staged WaW driver and,
when its default is silent, declares a default ambient room with dry gain 1
and wet gain 0. It also selects that mix at client startup. The normal ambient
room controller owns subsequent room changes; alias sends, source PCM, and
loaded weapon allocation are unchanged. Missing or audible custom source
defaults are reported and are never assumed silent. Explicit WaW room presets
still require I3DL2-to-RAD DSP translation; this fixes the baseline rather
than claiming complete room acoustics conversion.

WaW sound aliases retain their original audio, distance ranges, and four
dry/wet falloff curves in the converter's sound IR. BO2 rejects a second
`snddriverglobals` asset, so the converter binds against curves in the stock
BO2 driver's `.w2bsdg` sidecar.

Exact shape matches take precedence. With `--approximate-sound-curves` (enabled
by `tools/run_bridge.ps1`), unmatched curves use the stock shape with the lowest
RMS loudness error over normalized distance. The comparison uses 256 midpoint
samples and a -60 dB floor. If the source fades smoothly to silence, the
approximation prefers curves that also fade smoothly rather than hold a
nonzero gain until a terminal step. Source minimum and maximum distances are
preserved. Normalizing distance between those endpoints remains an engine
compatibility assumption, rather than a measured guarantee of identical
attenuation. `sounds.curves.json` reports both maximum linear gain error and
RMS dB error; strict mode still rejects unmatched curves.

BO2 alias CSV volume fields use dB SPL, not percentages. The converter writes
`100 + 20 * log10(source_gain)` for volume, reverb, center, and envelope gain,
with zero encoded as CSV zero. Fractional dB values are preserved. For example,
WaW gain 0.5 becomes approximately 93.9794 dB SPL; writing 50 would produce
gain 0.00316. The native loader quantizes gains to 16 bits.

The original WaW bus `volumeMod` multiplies the alias volume before dB
conversion. First-person weapon buses use the first-person BO2 duck group.
Aliases referenced by converted weapons share BO2's weapon volume group,
including custom aliases authored on WaW's `full_vol` bus and reload/notetrack
sounds. This avoids category-dependent gain differences within weapon audio;
BO2's broader driver routing and ducking remain approximations.

Loaded-bank budgeting prioritizes the actual weapon dependency graph before
other loaded sounds. Weapon clips converted to streams can compete for
streaming playback resources during rapid firing. The current map now keeps
all 429 available weapon variants (317 unique files) loaded, compared with
175 loaded / 253 streamed variants before this change. The allocation retains
the existing 392-file budget and uses about 35 MB of its 58 MB byte allowance.

The native XWMA bridge parses only bytes inside the RIFF container. WaW dumps
can carry additional trailing data; treating that trailer as RIFF chunks
incorrectly rejected valid files. The fix recovers 36 decoded files and 104
alias variants. Decoding a real fixture with and without its trailer produces
identical native PCM and matches the source decoded packet counts.

Alias and entity voice limits preserve WaW's counts, including zero, and its
`none`, `oldest`, `reject`, and `priority` policies. WaW's additional `softest`
policy uses BO2's `priority` policy and is recorded per variant in
`sounds.bank.json`. Unknown policies are reported as conversion errors.

Verification for this change:

- 23 sound tests, 5 audio tests, and 3 XWMA tests pass.
- 32 weapon variants compile and extract with their selected curves, loaded
  storage, categories, and voice limits intact; linear gain error is below
  2/65535.
- Evidence is in `work/sound_curve_fix/conversion_summary.json` and
  `work/sound_curve_fix/native_roundtrip.json`.
- Updated playback allocation and native gain verification are recorded in
  `work/sound_playback_fix/conversion_summary.json` and
  `work/sound_playback_fix/native_mix_roundtrip.json`.

The current conversion still reports missing PCM and secondary aliases. Its
full native bank build also warns about two non-48 kHz loaded death-music
clips. This change does not establish that every missing sound or playback
interruption is resolved. The latest 4,014-variant bank was rebuilt and
installed after the game closed, retaining the revive-waypoint and
teleporter boundary fixes. All nine packaged files match the build by
SHA-256; the rebuilt gameplay fastfile passed its 67,090-argument material
audit with zero violations. Installation evidence is in
`work/sound_playback_fix/installed_build.json`. In-game playback remains
unverified.
