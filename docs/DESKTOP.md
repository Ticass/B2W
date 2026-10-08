# WaW → BO2 Mod Tools

## Quick start

[Download WawConverter-Windows.zip](https://github.com/Ticass/B2W/releases/download/v0.2.11/WawConverter-Windows.zip)
from the [v0.2.11 release](https://github.com/Ticass/B2W/releases/tag/v0.2.11).
Choose the portable ZIP asset rather than GitHub's source-code archives.

1. Extract the entire portable Windows ZIP into a writable folder. Keep
   `WawConverter.exe`, `WawConverter.CLI.exe`, and `_internal/` together.
2. Double-click **WawConverter.exe**. Python and Visual Studio are not required
   for the portable download; the converter and native tools are included.
3. Open **Setup**. Check the automatically detected WaW and BO2 folders. Browse
   to your installations if needed. BO2 Mod Tools must be installed with BO2.
4. In **Mod Builder**, browse to your WaW map's original `.ff` file. Leave its
   `mod.ff` and IWD archives beside it. The BO2 map name is filled in for you.
   Use **Map Details & Artwork** to set the title and description and upload
   Blit (512 × 256 with transparency), Large (2048 × 2048), and Blur (2048 × 2048).
   The tab previews map selection, lobby, and loading artwork. Large supplies
   the loading image and the 256 × 256 lobby thumbnail.
5. Click **Build Map**. Missing or stale shared game assets prepare automatically.
   **Extract All** in Setup can prepare them in advance. The tool creates its own working folders, runs each
   conversion stage, shows progress, and records the build logs.
6. Review **Reports**. Close BO2, then click **Install to Plutonium**.
7. Click **Launch Map** and playtest the conversion.

Game content and proprietary Mod Tools are still required. The Setup list
identifies missing files before a build starts. WaW Mod Tools source FX
recovery is optional and disabled by default in the desktop app.

The updated desktop build driver runs in bundled Python and needs no PowerShell.
The original v0.2.0 release used PowerShell: Wine's built-in stub could return
success without executing the build, leaving an empty `build.log` and no map
files. The native Linux package replaces that driver. See `LINUX.md` for launch
instructions and verification limits.

## Familiar mod-tools workflow

- **Mod Builder:** source map, output name, Build Map, build checklist,
  Install to Plutonium, and Launch Map.
- **Map Details & Artwork:** title, description, three image uploads,
  required resolutions, validation, and approximate in-game previews.
- **Setup:** saved game paths, automatic Steam library detection, prerequisite
  checks, and optional advanced paths for custom native tool builds.
- **Reports:** staging errors, unsupported features, compatibility decisions,
  and the complete conversion report.
- **Build Console:** live stage messages and converter output, with Save
  Console and Clear Console controls. Native tool details are saved to log
  files; the newest log tails are shown if a build fails.
- **Stop Build:** stops the launcher's build process and its child tools.

Builds are kept separate from installed maps. The launcher records success
only after native stages, material verification, and packaging complete.
Install and Launch unlock when the corresponding output exists.

Settings and build files default to `%LOCALAPPDATA%/WawConverter/`. Each source
map and game-installation pair has separate intermediate files. Custom-map
source changes invalidate that source's dump. The updated Extract All workflow
checks stock cache freshness at build time and automatically prepares missing
or stale caches. See [Extract All](EXTRACT_ALL.md).
Select **Refresh source files** to force a fresh extraction when troubleshooting.
Installed maps live in `%LOCALAPPDATA%/Plutonium/storage/t6/mods/<map>/`.

## Run from source

The console reports staging phases, asset counts, and phase durations. Enable
**Verbose Console** before starting a build or extraction to show individual
asset names and stream native extractor/linker output. The choice is saved.
Quiet subprocesses report their PID, total elapsed time, and time since the
last output every 15 seconds. These messages confirm the process is still
running; they cannot establish whether it is making progress. Console activity
is also written to the build or extraction log.

Double-click `Launch Mod Tools.bat` with Python 3.11+ installed, including
Tcl/Tk, or run from PowerShell:

```powershell
python -m pip install -e .
$env:PYTHONPATH = Join-Path (Get-Location) 'src'
python -m waw2bo2.gui
```

A source checkout needs built native tools. The portable package removes this
setup requirement. See `USAGE.md` for native build instructions.

## Build the portable package

For maintainers, after building the native extractors/linker and audio decoder:

```powershell
python -m pip install pyinstaller imageio-ffmpeg
python tools/build_desktop.py
```

Outputs: `work/desktop_dist/WawConverter/WawConverter.exe` and
`work/desktop_dist/WawConverter-Windows.zip`. The archive includes the Python
runtime, native tools, shared schemas, compatibility data, licenses, and native
corresponding source archives. It does not include extracted game assets or
proprietary Mod Tools.

This remains a development preview. A successful build is followed by source
comparison and playtesting; inspect Reports for unsupported map features.
## Gameplay options

In Mod Builder, **Use BO2 stock perks (maps with BO2 perks only)** opts into
stock BO2 perk behavior, including solo Quick Revive. It is saved with the
settings and defaults to off. Enable it for Nuketown, then build and install
the resulting map. Maps with custom perks should keep it off.

## Build failures

Failed builds automatically create a diagnostics ZIP containing the console,
native-tool logs and conversion reports. Use **Reports → Save Diagnostics**
to save one file for support. Extract All failures also produce diagnostics
when no map is selected. Game fastfiles and source assets are excluded.

Models whose definitions and mesh geometry come from different WaW zones are
resolved automatically. If required geometry is absent from both the map and
stock WaW assets, the report identifies the model and missing mesh explicitly.
