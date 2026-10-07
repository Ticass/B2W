# Prepare conversion assets

The Setup **Extract All** button prepares a reusable conversion cache. It does
not export every asset from every installed game fastfile.

WaW caches scripts and definition indexes across installed maps, plus shared
assets from common/code_post_gfx. Other stock-zone payloads are extracted once
only when the selected custom map references them. Campaign geometry is not
exported into the stock cache. The selected source map and its companion FFs
retain the world data needed for conversion.

BO2 caches material and technique/shader metadata across installed maps, the
stock sound-driver curves and barrier definitions. Images are limited to the
shared Zombies runtime and the existing Nuketown/prison donor zones; prison
models supply barrier render dependencies. Campaign/multiplayer geometry,
animation libraries, sound banks, and gameplay exports are excluded. BO2 raw
assets remain in the installed mod tools rather than being copied wholesale.

Caches live under `<Build files folder>/asset_cache/`. Unchanged data is reused.
Existing full exports migrate to compact subsets when possible without native
re-extraction. Once the new stock view validates, obsolete stock generations
and failed partial views are removed. Custom source caches and built packages
are preserved. Failed native exports retain their logs and remove partial files.

Independent fastfiles run concurrently using every logical CPU, up to the
number of zones. Set `WAW2BO2_EXTRACT_WORKERS` to a positive worker count to limit
CPU or RAM usage. Duplicate-asset priority stays deterministic.

`All2Raw.exe` (Windows) or `All2Raw` (Linux) prepares the same cache using saved
paths. Keep it beside the launcher and `_internal` folder. Source command:
`python -m waw2bo2.all2raw_cli`. Enable **Verbose Console**, or `--verbose`, for
native tool output. Per-zone logs and completed counts remain available.

```text
All2Raw.exe --waw "C:\Games\WaW" --bo2 "C:\Games\BO2" --work "D:\WawConverter\builds"
```

On Linux use the launcher's `WINEPREFIX` and `WAWCONVERTER_WINE` variables.
Extracted game assets are never included in the converter download.
