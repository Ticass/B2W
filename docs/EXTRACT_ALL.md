# Prepare conversion assets

The Setup **Extract All** button prepares a reusable conversion cache. It does
not export every asset from every installed game fastfile.

WaW caches scripts and definition indexes across installed maps, plus shared lighting images from code_post_gfx. Other stock-zone payloads are extracted once
only when the selected custom map references them. Campaign geometry is not
exported into the stock cache. The selected source map and its companion FFs
retain the world data needed for conversion.

BO2 only scans Nuketown, prison, and the shared Zombies runtime fastfiles.
It caches material and technique/shader metadata, the
stock sound-driver curves and barrier definitions. Prison payloads are trimmed to the basic wooden barrier and its model,
material, shader, and texture dependencies. Other BO2 images are fetched only
when an actual material fallback requires them, and only that requested texture
is retained. Campaign/multiplayer geometry,
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

`waw2bo2n all2raw` prepares the same cache from the command line, with the
launcher's saved settings (Windows: `waw2bo2n.exe`, Linux:
`./WawConverter.CLI`). Add `--refresh` to extract again. Enable **Verbose
Console** in the settings for native tool output. Per-zone logs and completed
counts remain available.

```text
waw2bo2n.exe all2raw --settings-file "%LOCALAPPDATA%\WawConverter\settings.json"
```

On Linux use the launcher's `WINEPREFIX` and `WAWCONVERTER_WINE` variables.
Extracted game assets are never included in the converter download.
