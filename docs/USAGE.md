# Installation and usage

For the simple workflow, use the [desktop launcher](DESKTOP.md): select a map,
click Build Map, then Install and Launch. The portable download includes the
converter and its native tools. The instructions below cover building from
source and the command line.

These examples use Windows PowerShell and `C:\WawConverter`; other checkout
locations work too.

## 1. Requirements

Building from source needs 64-bit Windows, Git, and Visual Studio 2022 or newer
with Desktop development with C++ (MSVC x64 and x86 tools, the Windows SDK,
CMake and Ninja). Converting and playtesting additionally need:

- Your WaW installation with `main/` IWD archives and `zone/english/` fastfiles.
- The custom map folder: `<map>.ff`, `mod.ff`, its IWD archives, and
  `<map>_patch.ff` if provided. Keep these files together.
- Your BO2 installation with BO2 Mod Tools: `bin/Linker.exe`, raw GSC/CSC
  sources, `raw/animtrees/fxanim_props.atr`, frontend CSVs under `raw/zm/`, and
  the `mods/zm_test` source template.
- BO2 donor zones: `zone/all/zm_nuked.ff`, `zm_prison.ff` and
  `code_post_gfx_zm.ff`.
- Plutonium T6 configured to use your BO2 directory.
- WaW Mod Tools: they decode wavelet images and compile raw sources (source FX
  recovery is optional).

Game content and proprietary tools are not included.

## 2. Build the converter

```powershell
git clone https://github.com/Ticass/B2W.git C:\WawConverter
Set-Location C:\WawConverter
powershell -ExecutionPolicy Bypass -File tools\restore_native.ps1
native\build\w2b_tests.exe
```

`restore_native.ps1` restores and builds everything the converter needs, and
skips whatever is already built, so it continues after a partial failure:

- **T4 (WaW) OpenAssetTools:** upstream commit `7d027e8` with
  `vendor/OpenAssetTools.patch` applied, built Release x64. The converter links
  it in process to read WaW zones; do not reapply the patch to a patched checkout.
- **T6 (BO2) OpenAssetTools:** the vendored fork in `vendor/OpenAssetToolsT6`
  with the dependency checkouts of upstream snapshot `95b8c68`, built Release
  x86 (`Linker.exe`, `Unlinker.exe`). The converter marks the linker
  large-address aware.
- **Audio decoder:** `tools/bin/xaudio_wma_decoder.exe` from
  `tools/xaudio_wma_decoder.cpp`.
- **The converter:** `native/build/waw2bo2n.exe` (command line) and
  `native/build/B2W.exe` (the launcher), with CMake and Ninja.

Shader translation uses Windows `d3dcompiler_47.dll`. Audio resampling uses
FFmpeg: `ffmpeg.exe` on PATH, `W2B_FFMPEG`, or `tools/bin/ffmpeg.exe`
(`tools/fetch_wheel_binary.ps1 -Package imageio-ffmpeg -Version 0.6.0 -Member
'imageio_ffmpeg/binaries/ffmpeg-*.exe' -Destination tools/bin/ffmpeg.exe`
fetches the build the release ships).

Keep the whole checkout: the converter reads its data from
`src/waw2bo2/compat/` and native schemas from `vendor/`.

## 3. Convert a map

Start `native\build\B2W.exe` (or `Launch Mod Tools.bat`) and follow the
[desktop guide](DESKTOP.md), or build from the command line with a settings
file: the launcher's own, `%LOCALAPPDATA%\WawConverter\settings.json`, or one
written for the map.

```powershell
native\build\waw2bo2n.exe build-map --settings-file "$env:LOCALAPPDATA\WawConverter\settings.json"
```

The settings name the game folders (`waw`, `bo2`), the map (`fastfile`), the
BO2 map name (`project`, a simple `zm_...` name without spaces) and the build
folder (`work`); blank tool paths are filled with the bundled tools, as the
launcher does. `--build-root <folder>` builds somewhere else, `--redump`
extracts the map again.

The build extracts the map and its companion zones, prepares the stock WaW and
BO2 caches, stages geometry, assets and scripts, builds the gameplay and
frontend zones, compiles the map scripts with BO2 Mod Tools, links the map with
the T6 bridge and audits the result. It stops on failing native tool results.
`waw2bo2n package` (below) installs it.

Important behavior:

- WaW's `main` search path supplies stock textures and sounds. Using only the
  map folder produces missing-asset reports.
- Ordinary WaW primary frags currently map to BO2's `frag_grenade_zm`;
  special/custom grenade equipment is handled separately.
- Installing writes into Plutonium's mod folder. Close the game before
  replacing changed files; packaging skips identical files.

## 4. Output and packaging

A build's folder is `<work>/<map>_<id>/` (or `--build-root`):

| Location | Contents |
| --- | --- |
| `stage/zone_raw/<project>/` | Staged assets, scripts, BSP inputs, and reports |
| `stage/zone_source/<project>.zone` | Map asset declarations |
| `stage/zone_out/<project>/` | Linked map `.ff` and `.ipak` |
| `mod_build/out/` | `mod.ff`, `mod_load.ff`, mod image packs, and sound banks |
| `%LOCALAPPDATA%/Plutonium/storage/t6/mods/<project>/` | Installed mod and `mod.json` |

Install a successful build, or assemble its package in a chosen directory:

```powershell
$build = "$env:LOCALAPPDATA\WawConverter\builds\nazi_zombie_my_map_1a2b3c4d"
native\build\waw2bo2n.exe package "$build\stage" zm_my_map_waw `
  --bo2 'D:\Games\Call of Duty Black Ops II' `
  --work "$build\mod_build" `
  --dest 'D:\ConvertedMaps\zm_my_map_waw'
```

Without `--dest` it installs into Plutonium. Copy a package folder into the
target machine's Plutonium `storage/t6/mods/` directory, keeping fastfiles,
image packs, banks, and metadata together.

## 5. Launch and playtest

Restart BO2 after installing updates. Select the mod through your normal
Plutonium Zombies launcher flow, or open its lobby directly:

```powershell
$pluto = Join-Path $env:LOCALAPPDATA 'Plutonium'
Start-Process `
  -FilePath (Join-Path $pluto 'bin\plutonium-bootstrapper-win32.exe') `
  -ArgumentList 't6zm "D:\Games\Call of Duty Black Ops II" +set fs_game mods/zm_my_map_waw' `
  -WorkingDirectory $pluto `
  -WindowStyle Hidden
```

For a local direct-map test, append `-lan +devmap zm_my_map_waw` to the argument
string. The Plutonium working directory matters: launching from the repository
directory can produce a "no binary" error.

Compare movement and projectile collision, doors, perks, Pack-a-Punch, weapon
animations, FX, audio, lighting, menus, and co-op behavior with the WaW source.
A linked fastfile is only one part of verification.

## 6. Optional custom menus

Place `menu.json`, authored material JSON, and T6 IWI textures under the staged
project's `zone_raw/<project>/` before `build-mod`. After an initial driver run,
rerun `build-mod` and `package` with the same paths to include new artwork.
See [authored map menus](MAP_MENU_ASSETS.md) for names, streaming packs, and
alignment requirements. The test map's local artwork and converted game data
are not bundled with the release.

## 7. Command line and troubleshooting

`waw2bo2n.exe` without arguments lists every command and its arguments.

| Command | Purpose |
| --- | --- |
| `build-map` | The whole build, as the launcher runs it |
| `all2raw` | Prepare the stock WaW and BO2 caches (Extract All) |
| `package` | Assemble or install the complete Plutonium folder |
| `stage-bridge` / `build-mod` / `compile-scripts` / `bridge-link` | One step of the build |
| `launcher-build` | The launcher's Build Map flow on the console |
| `inspect` / `world-*` / `collision-*` / `static-models` | Inspect and export WaW world dumps |
| `oat-unlink` / `oat-dump-assets` / `oat-check-materials` | The linked-in WaW extractor |
| `audit-zone` / `audit-material-args` | Check a linked zone |
| `knowledge-report` | Rank untranslated features across every map built |
| `ipak-check` / `crash-summary` | Check an image pack; summarize a Plutonium crash dump |

- **Staging failure:** read `<stage>/zone_raw/<project>/bridge_stage.report.json`,
  including errors and warnings. Check companion zones, IWD search paths, and
  Mod Tools sources before changing translation rules.
- **Missing native dependencies:** complete the separate T4/T6 setup above.
  Unmodified OAT binaries do not implement this bridge.
- **Missing scripts/frontend CSVs:** check BO2 Mod Tools and `mods/zm_test` in
  the BO2 directory supplied to the driver.
- **Link failure:** inspect `<stage>/<project>_bridge.log` and native build
  logs. A failing result or missing output is not a finished conversion.
- **File in use or old assets:** close the game, repeat packaging, and start
  a fresh session.
- **Material load crash:** inspect the driver's `audit_*.log`. For an extracted
  zone, run `waw2bo2n audit-material-args <zone-folder> <stock-dump-folder>`.

Map-dependent unsupported APIs/assets, incomplete visual/audio parity, minor
projectile collision gaps in the test map, and unverified final custom frontend
alignment remain. See [the changelog](../CHANGELOG.md) and subsystem documents.
