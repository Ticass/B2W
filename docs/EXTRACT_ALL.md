# Extract All

In Setup, configure WaW and BO2, then click **Extract All** before the first
build. `All2Raw.exe` (Windows) or `All2Raw` (Linux) performs the same action
without opening the GUI, using your saved paths. Keep it beside the other
executables and `_internal` folder. Source: `python -m waw2bo2.all2raw_cli`.

The extractor scans every installed `.ff` under both games' `zone` directories,
including campaign, multiplayer, Zombies, installed DLC, and language zones.
Independent fastfiles extract concurrently, with one native worker per logical
CPU (up to the number of zones). The console shows the worker count and completed
zones. Asset priority and cache reuse do not depend on completion order.
To limit CPU or memory usage, set `WAW2BO2_EXTRACT_WORKERS` to a positive worker
count before launching. WaW and BO2 run in separate batches using the same limit.
It caches all supported BO2 asset exporters and the WaW assets used by the
converter: materials, shader dependencies, images, models, animations, weapons,
FX, audio assets and sound drivers, scripts, localization, world/collision data,
and dependency indexes. BO2 script/API metadata is prepared here too.

If a custom map is selected, its original FF and the other FFs beside it are
also cached. A new map selected later has its sources extracted on first build;
unchanged subsequent builds reuse those source dumps.

Shared caches live under `<Build files folder>/asset_cache/`. They are keyed by
game installation, independent of the custom map or output name. The extractor
keeps per-zone assets and a combined lookup view. Duplicate names use a fixed
framework-first order; `catalog.json` records the chosen source and conflicting
alternatives instead of silently overwriting them.

Running Extract All again reuses unchanged zones. Interrupted extractions
resume from completed zones. New/changed FFs, tool changes, external archive
changes, or missing cache files make the cache stale. Builds then ask you to
run Extract All; they never refresh stock dumps in the background. Failed
extraction is never marked complete. Explicit CLI `all2raw --refresh` rebuilds
all zones; the GUI's **Refresh source files** affects custom-map sources only.

Build Map still performs conversion, compilation, linking, and checks of its
newly generated outputs. Those depend on the current map and cannot be
pre-extracted from the game installation. Extraction does not overwrite either
game's `raw` folder.

Initial extraction can take time and substantial disk space. Only installed
content is available; missing DLC is not downloaded. Extracted game assets
are never included in the converter download.

Standalone path overrides:

```text
All2Raw.exe --waw "C:\Games\WaW" --bo2 "C:\Games\BO2" --work "D:\WawConverter\builds"
```

On Linux, launch with the same `WINEPREFIX` and `WAWCONVERTER_WINE` variables as
the desktop app. Linux cache paths are passed to the Windows worker so both
frontends use the same extraction folders.

Enable **Verbose Console** in the launcher, or pass `--verbose` to All2Raw,
to stream native-tool output. Zone counts appear by default. While a tool is
quiet, the console reports process status every 15 seconds; all activity is
saved in `asset_cache/extract.log` under your build folder.
