# Installation and usage

These examples use Windows PowerShell and `C:\WawConverter`. The convenience
driver sets `PYTHONPATH` to `C:\WawConverter\src`; if you use another checkout
location, update that assignment or invoke the CLI stages manually.

## 1. Requirements

The Python interface and regression suite require Python 3.11 or newer and Git.
Full conversion and playtesting additionally require:

- 64-bit Windows, Visual Studio 2022 with Desktop development with C++, MSVC
  x86 tools, and the Windows SDK.
- Premake 5 on PATH. The vendored T6 generator specifies `5.0.0-beta6`.
- Your WaW installation with `main/` IWD archives and `zone/english/` fastfiles.
- The custom map folder: `<map>.ff`, `mod.ff`, its IWD archives, and
  `<map>_patch.ff` if provided. Keep these files together.
- Your BO2 installation with BO2 Mod Tools: `bin/Linker.exe`, raw GSC/CSC
  sources, `raw/animtrees/fxanim_props.atr`, frontend CSVs under `raw/zm/`, and
  the `mods/zm_test` source template.
- BO2 donor zones. The driver uses `zone/all/zm_nuked.ff`, `zm_prison.ff`, and
  `code_post_gfx_zm.ff`. `-StockZone` changes the main donor, but does not remove
  the separate `zm_prison.ff` requirement.
- Plutonium T6 configured to use your BO2 directory.
- WaW Mod Tools for raw source recovery. The driver enables source FX recovery
  unless `-NoWawSourceFx` is supplied.

Game content and proprietary tools are not included. The Python interface does
not replace the native extractors, compilers, or linkers.

## 2. Obtain and check the tool

```powershell
git clone https://github.com/Ticass/B2W.git C:\WawConverter
Set-Location C:\WawConverter

# Optional: use the documented release instead of the latest main branch.
git checkout v0.1.0

$env:PYTHONPATH = Join-Path (Get-Location) 'src'
python -m waw2bo2.cli --help
python -m unittest discover -s tests
```

The converter uses Python's standard library; no pip installation is needed
for this workflow. Keep the whole checkout because stages read native schemas
and raw files from `vendor/`.

## 3. Build the native tools

Use Developer PowerShell for Visual Studio 2022 so `MSBuild.exe` is on PATH.
Build x86 Release configurations: the driver expects `build/bin/Release_x86/`.

### T4 extractor

Use the supplied patch against its pinned upstream commit. In a fresh checkout:

```powershell
Set-Location C:\WawConverter
git clone https://github.com/Laupetin/OpenAssetTools.git vendor/OpenAssetTools
git -C vendor/OpenAssetTools checkout 7d027e8f89118196713e955b0e11f8404149c54d
git -C vendor/OpenAssetTools submodule update --init --recursive
git -C vendor/OpenAssetTools apply --check ../OpenAssetTools.patch
git -C vendor/OpenAssetTools apply ../OpenAssetTools.patch

Push-Location vendor/OpenAssetTools
premake5 vs2022
MSBuild.exe build/OpenAssetTools.sln /m /p:Configuration=Release /p:Platform=x86
Pop-Location
```

Do not reapply the patch to an already patched checkout. Confirm that
`vendor/OpenAssetTools/build/bin/Release_x86/Unlinker.exe` exists.

### T6 bridge

The vendored fork is ordinary source inside B2W. Its external submodule
checkouts are excluded; `git submodule update` at the B2W root cannot fetch
them. Obtain dependencies from the upstream bridge snapshot and copy its
dependency directories into the vendored fork:

```powershell
Set-Location C:\WawConverter
New-Item -ItemType Directory -Force work | Out-Null
git clone https://github.com/Laupetin/OpenAssetTools.git work/oat_t6_dependencies
git -C work/oat_t6_dependencies checkout 95b8c68fbc464109c9289a833035763505ea57da
git -C work/oat_t6_dependencies submodule update --init --recursive

Get-ChildItem work/oat_t6_dependencies/thirdparty -Directory | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination vendor/OpenAssetToolsT6/thirdparty -Recurse -Force
}

Push-Location vendor/OpenAssetToolsT6
premake5 --oat-version=waw2bo2-v0.1.0 vs2022
MSBuild.exe build/OpenAssetTools.sln /m /p:Configuration=Release /p:Platform=x86
Pop-Location
```

This preserves B2W's modified source and Premake files. Use the vendored fork's
`Linker.exe` and `Unlinker.exe`. Copied dependency `.git` files do not form
independent checkouts; update dependencies in the upstream checkout and recopy.

The driver marks the T6 linker large-address-aware with `editbin.exe` before
linking, allowing up to 4 GB of address space on 64-bit Windows.

### Audio decoder

```powershell
Set-Location C:\WawConverter
& ./tools/build_audio_decoder.ps1
```

This builds `tools/bin/xaudio_wma_decoder.exe` from the included C++ source.
The driver builds it automatically when no `-XwmaDecoder` path is supplied.
Shader translation uses Windows `d3dcompiler_47.dll`.

## 4. Convert a map

Replace paths and names with your own. `-MapZone` is the source fastfile stem
without `.ff`; `-Project` is the new BO2 map/zone name. Use a simple `zm_...`
name without spaces. Use separate stage, dump, and build folders for each map
to avoid reusing another map's cached data.

```powershell
Set-Location C:\WawConverter
$stage = 'C:\WawConverter\work\my_map_stage'
New-Item -ItemType Directory -Force $stage | Out-Null

& ./tools/run_bridge.ps1 `
  -Waw 'D:\Games\Call of Duty World at War' `
  -Bo2 'D:\Games\Call of Duty Black Ops II' `
  -MapMod 'D:\WaWMaps\My Custom Map' `
  -MapZone 'nazi_zombie_my_map' `
  -Project 'zm_my_map_waw' `
  -Stage $stage `
  -Dump 'C:\WawConverter\work\my_map_stock_t6' `
  -WawDumps 'C:\WawConverter\work\my_map_waw_dumps' `
  -ModBuild 'C:\WawConverter\work\my_map_mod_build' `
  -WawModTools 'D:\Tools\WaWModTools' `
  -Redump
```

Create the stage directory first: initial native dump logs are written there.
Omit `-Redump` on subsequent runs with unchanged companion/donor caches. Back
up custom staged inputs before restaging.

The driver extracts the map and companion zones, dumps BO2 donors, stages
geometry and assets, builds gameplay/frontend zones, compiles map scripts with
BO2 Mod Tools, links the map using the T6 bridge, installs it, and audits
installed material arguments. It stops on failing native tool results.

Important behavior:

- WaW's `main` search path supplies stock textures/sounds. Using only the map
  folder produces missing-asset reports.
- `-NoWawSourceFx` disables raw source FX compilation with WaW Mod Tools.
- `-FxFallback` enables reported stock BO2 FX substitutions for debugging;
  this is off by default.
- The driver enables `--approximate-sound-curves`. To require exact curve
  matches, invoke `stage-bridge` without that option. See [sound conversion](SOUND_CONVERSION.md).
- Ordinary WaW primary frags currently map to BO2's `frag_grenade_zm`;
  special/custom grenade equipment is handled separately.
- The driver installs into Plutonium's mod folder. Close the game before
  replacing changed files; packaging skips identical files.

## 5. Output and packaging

For the example above:

| Location | Contents |
| --- | --- |
| `work/my_map_stage/zone_raw/zm_my_map_waw/` | Staged assets, scripts, BSP inputs, and reports |
| `work/my_map_stage/zone_source/zm_my_map_waw.zone` | Map asset declarations |
| `work/my_map_stage/zone_out/zm_my_map_waw/` | Linked map `.ff` and `.ipak` |
| `work/my_map_mod_build/out/` | `mod.ff`, `mod_load.ff`, mod image packs, and sound banks |
| `%LOCALAPPDATA%/Plutonium/storage/t6/mods/zm_my_map_waw/` | Installed mod and `mod.json` |

After a successful build, assemble another package in a chosen directory:

```powershell
python -m waw2bo2.cli package `
  'C:\WawConverter\work\my_map_stage' zm_my_map_waw `
  --bo2 'D:\Games\Call of Duty Black Ops II' `
  --work 'C:\WawConverter\work\my_map_mod_build' `
  --dest 'D:\ConvertedMaps\zm_my_map_waw'
```

Copy the complete resulting folder into the target machine's Plutonium
`storage/t6/mods/` directory, keeping fastfiles, image packs, banks, and metadata
together.

## 6. Launch and playtest

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

## 7. Optional custom menus

Place `menu.json`, authored material JSON, and T6 IWI textures under the staged
project's `zone_raw/<project>/` before `build-mod`. After an initial driver run,
rerun `build-mod` and `package` with the same paths to include new artwork.
See [authored map menus](MAP_MENU_ASSETS.md) for names, streaming packs, and
alignment requirements. The test map's local artwork and converted game data
are not bundled with the release.

## 8. CLI reference and troubleshooting

Use `python -m waw2bo2.cli <command> --help` for exact arguments.

| Command | Purpose |
| --- | --- |
| `extract` / `inspect` | Extract and inspect typed world dumps |
| `convert` | Basic extraction/FBX staging; not the full gameplay pipeline |
| `stage-bridge` | Translate assets, scripts, and geometry for the T6 bridge |
| `build-mod` | Build gameplay `mod.ff` and frontend `mod_load.ff` |
| `compile-scripts` | Compile map GSC/CSC with BO2 Mod Tools |
| `bridge-link` | Link the map with the modified T6 linker |
| `package` | Assemble/install the complete Plutonium folder |
| `official-build` | Invoke the installed BO2 map compiler/linker path |
| `stage-weapons` / `translate-shader` / `dds2iwi` | Individual asset stages |

- **`No module named waw2bo2`:** set `PYTHONPATH` to the checkout's `src`
  directory in the same PowerShell session.
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
  zone, run `python tools/audit_material_args.py <zone-folder> <stock-dump-folder>`.

Map-dependent unsupported APIs/assets, incomplete visual/audio parity, minor
projectile collision gaps in the test map, and unverified final custom frontend
alignment remain. See [the changelog](../CHANGELOG.md) and subsystem documents.
