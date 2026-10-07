# waw2bo2 — World at War to Black Ops II map converter

`waw2bo2` extracts compiled World at War custom maps and translates geometry,
collision, materials, images, scripts, models, weapons, effects, and audio into
Black Ops II Zombies zones for Plutonium T6.

**Development preview:** coverage depends on the map; a successful link does
not establish complete gameplay or visual fidelity. Game content and proprietary
Mod Tools are not bundled.

## How to use it

The **[desktop launcher](docs/DESKTOP.md)** provides a familiar Windows mod-tools
interface: select a WaW map, click **Build Map**, then **Install to Plutonium**
and **Launch Map**. It remembers game paths, manages intermediate files, checks
requirements, displays progress, and collects conversion reports.

**[Download the portable Windows GUI](https://github.com/Ticass/B2W/releases/download/v0.2.0/WawConverter-Windows.zip)**
or view the [v0.2.0 release](https://github.com/Ticass/B2W/releases/tag/v0.2.0).

Portable Windows builds include Python and the native conversion tools. Extract
the whole ZIP and open `WawConverter.exe`. Game installations and BO2 Mod Tools
are still required. In a source checkout with Python installed, double-click
`Launch Mod Tools.bat`.

See the **[desktop quick-start guide](docs/DESKTOP.md)** for normal use and the
**[advanced installation and usage guide](docs/USAGE.md)** for native setup and
individual CLI stages.

To view the Python command interface in PowerShell:

```powershell
git clone https://github.com/Ticass/B2W.git C:\WawConverter
Set-Location C:\WawConverter
$env:PYTHONPATH = Join-Path (Get-Location) 'src'
python -m waw2bo2.cli --help
```

Python 3.11 or newer is required. Full conversion additionally requires Windows,
patched OpenAssetTools builds, WaW and BO2 game assets, BO2 Mod Tools, and
Plutonium. Read the guide before running the pipeline.

## Conversion behavior

- Reads typed assets from original T4 fastfiles and companion zones.
- Preserves geometry, UVs, vertex colors, lightmap data, static models, and
  moving brush ownership through the T6 bridge.
- Translates collision, scripts, asset dependencies, materials/shaders,
  weapons/animations, FX, localization, and sound banks, with reports of
  unsupported features and compatibility decisions.
- Reduces network entity load by translating a narrowly matched stationary FX
  carrier idiom into effects at the original position and orientation.
- Supports optional authored titles, descriptions, icons, and map menu artwork.

WaW source assets take priority. Ordinary WaW primary frags currently map to
BO2's native frag; sound curve approximation is enabled by the convenience
driver, and stock FX fallback is opt-in. Inspect the reports to assess fidelity.

## Documentation

- [Setup, conversion, installation, and troubleshooting](docs/USAGE.md)
- [Desktop launcher and portable Windows builds](docs/DESKTOP.md)
- [Release history and limitations](CHANGELOG.md)
- [Collision conversion](docs/COLLISION_CONVERSION.md)
- [Authored map menus](docs/MAP_MENU_ASSETS.md)
- [Sound conversion](docs/SOUND_CONVERSION.md)
- [Shader translation](docs/SHADER_TRANSLATION.md)
- [Perk conversion](docs/PERK_CONVERSION.md)
- [Source model recovery](docs/SCRIPT_MODEL_SOURCE_RECOVERY.md)
- [Development guidelines](GUIDELINES.MD)

## Repository layout

- `src/waw2bo2/`: Python converter and compatibility scripts/data.
- `tests/`: regression tests; run with `PYTHONPATH=src` as shown in the guide.
- `tools/`: `run_bridge.ps1`, native audio helper, and conversion audit tools.
- `vendor/OpenAssetToolsT6/`: modified T6 OpenAssetTools source with the BSP,
  clipmap, and gfxworld bridge and native asset loaders/dumpers.
- `vendor/OpenAssetTools.patch`: T4 dumper changes against upstream commit
  `7d027e8f89118196713e955b0e11f8404149c54d`.
- `AGENT_HANDOFF.md`: detailed engineering and playtest evidence.

Extracted data (`work/`), game installs, WaW Mod Tools, third-party dependency
checkouts, and native build outputs are excluded from Git. The vendored T6
OpenAssetTools code is GPL-3.0; see its [license](vendor/OpenAssetToolsT6/LICENSE).
