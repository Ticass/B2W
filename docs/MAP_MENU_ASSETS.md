# Authored BO2 map menus

The desktop launcher has a **Map Details & Artwork** tab. Enter the title and
description and upload all three images. Two layouts are supported:

- **Aligned layers:** all three inputs have identical canvas dimensions. Large
  contains the background and its transparent cutout; Blit fills that cutout at
  the same coordinates; Blur contains the blurred background. Use PNG/TGA to
  preserve alpha. The converter resizes all three together to 2048 square for
  storage; the menu displays them at the screen aspect ratio.
- **Stock layout:** Blit is 512 × 256, Large and Blur are 2048 × 2048. Existing
  Nuketown-style placement, documented below, is retained.

Mixed dimensions produce a non-blocking warning in the launcher and build log.
They use full-canvas placement and are resized to the same export dimensions;
objects may not line up or may appear distorted. Preview and conversion remain
available. Opaque Blits are rejected because they lack the required transparency.
Text can also be customized without uploading artwork.

The preview selector shows map selection, individual canvases, the lobby
thumbnail, and the loading screen. The lobby thumbnail is 256 × 256. The converter
does not generate Blur. Aligned selection overlays the same full canvases; Focused
selection overlays sharp Blit over Blur. Loading art and the thumbnail use the
completed Large + Blit image. Stock selection uses the measured projection below.
These are layout approximations; stock menu
projection and final framing still require an in-game check.

### Launcher section

**Map Details & Artwork** collects every optional menu input on one page: the
in-game text (map name, description, mode line, location), the three map
selection layers and the start-location point, the globe icon, lobby preview
and loading screen images, the globe position and the loading song. Its
preview draws five in-game screens in BO2's 1280 × 720 menu space
(`menupreviews.py`), using the BO2 install's own menu textures when present
(`raw/images`: `globe_map_zm`, `menu_zm_background_main`, `menu_zm_map_frame`,
stock signposts and `raw/zm/mapstable.csv`; nothing from the game ships):

- **Globe:** an orthographic globe textured with `globe_map_zm`, the stock maps'
  signposts and yours. Drag to spin; click to set longitude/latitude. BO2 stores
  longitude with the geographic sign flipped (zm_prison 122 is San Francisco).
- **Map selection / Focused selection:** the composited layers, the 400 × 200
  start-location box and the option list (map name in orange brackets and the
  description). Click to place the start location on aligned artwork.
- **Lobby:** the Custom Games preview frame filled by the lobby picture, with
  the map name and mode line.
- **Loading screen:** the loading image with the map name and location.

The build exports native T6 IWI textures and materials and carries the title,
description, and artwork through both frontend zones and the packaged mod.
Opaque textures use DXT1; textures with alpha use DXT5. Blit and the icon retain
DXT5 for compatibility. Export checks the native IWI format and byte count and
rejects any texture reaching 16 MiB. Each 2048-square DXT5 IWI is 4,194,368 bytes
including its header; DXT1 is 2,097,216 bytes. `menu.json` records each texture's
format, dimensions and byte count in `texture_report`.

Aligned layouts bake both menu states at build time: Map is Large with Blit
composited over it, and Map Blur is the focused view, sharp Blit over Blur. Stock
maps stream one full-screen map image at a time; an earlier design that streamed
the full-canvas Blit as a separate foreground showed it unfocused but lost it on
focus, leaving Blur's cutout black. Aligned layouts also register
`zm/waw_menu_layers.csv` and a compiled T6 wrapper for GameMapZombie whose only
job is to halve the stock selection zoom for full-frame artwork; stock maps keep
their original zoom. The installed game's original bytecode is preserved under
`ui_mp/t6/zombie/waw_gamemap_base.lua`. The stock location icon (gametypestable
column 6) uses a transparent 512 × 256 material named `menu_<project>_loc`: names
read back through those table lookups must fit a 32-byte buffer, and
`menu_<project>_location` was cut short and drew the default checkerboard.
Switching to stock artwork removes these generated overrides. Both frontend zones
register the runtime and layer table.

Every authored map also gets a full-frame lobby preview. Stock ZM draws the
Custom Games preview (`CoD.MapInfoImage.mapImage`) 294 × 136 and leaves the
46-unit strip below it to the frame's translucent dark gradient behind the map
name. The build wraps the installed game's `ui/t6/mapinfoimage.lua` (original
bytecode kept as `ui/t6/waw_mapinfoimage_base.lua`): for maps listed in
`zm/waw_menu_preview.csv` the picture extends under that strip, so it fills the
whole 294 × 182 frame with the name drawn over it. Stock maps keep the stock
layout. The preview image (`menu_preview`, else the completed art) is centre
cropped to the frame's 294:182 shape. Rebuild its bytecode with
`powershell -File tools/compile_menu_layers.ps1 /path/to/hksc menu_preview`;
`tests/menu_preview_runtime.lua` checks the layout with a stub UI.

The checked-in `compat/menu_layers.lua` and `.luac` have paired SHA-256 hashes;
staging rejects stale bytecode or a mismatching VM header. Recompile changes with
`powershell -File tools/compile_menu_layers.ps1 /path/to/hksc`, using hksc configured with
`--game=t6 --multi-plat`, targeting Windows 32-bit. The compiler is a development
tool only; users do not need it installed. `tests/menu_layers_runtime.lua` checks
zoom and stock-map return with a stub UI.
The checked-in bytecode was built with
[hksc](https://github.com/Jake-NotTheMuss/hksc) revision
`afa5ace6b2d11891b094ca637ccc772b9ef4ea59`. On MinGW that revision needs the
`lua_str2ui64` macro to accept the unused third length argument and `assert.h`
included when building the optional decompiler. No compiler is bundled.

A staged project can carry `menu.json` with `title`, `description`, `icon`,
and `blit` strings. The last two name authored materials. Additional material
names can be listed in `materials`. This is project data; converter code does
not choose artwork based on map names.

Provide the native material JSON files under `materials/` and their T6 IWI
textures under `images/`. The frontend also requires these conventionally
named materials, where `<project>` is the converted map's technical name:

- `menu_<project>_map`
- `menu_<project>_map_blur`
- `menu_<project>_zclassic_default`
- `loadscreen_<project>_zclassic_default`
- `loadscreen_<project>_zclassic_`

The build generates namespaced title, uppercase location title, and description
localization for every map, branded or not. `menu.json` may also carry `mode`
(the line BO2 fills with "TRANZIT" under the map name; default "Zombies") and
`location` (the loading screen's location line; default "World at War").

T6 composes three keys itself (measured in t6zm.exe): the map name is
`<mapstable col 3>_CAPS` (`ls_mapname`, sub_5D0EF0), the location is
`<col 3>_LOC` (`ls_maplocation`, sub_5E6900), and the mode line is
`<gametype col 7>_<map>_CAPS` in the lobby (sub_5581A0) or
`<gametype col 7>_<col 3>_LOC_CAPS` on the loading screen (sub_694EB0), falling
back to the gametype's own name, `ZMUI_CLASSIC_CAPS` = "TRANZIT". Column 3 is
therefore a short key (`WAW_<PROJECT>`, at most 26 characters to fit the
engine's 32-byte buffer), never display text: a title there showed as
"Title_CAPS" and "Title_LOC". It binds the icon in `mapstable.csv` and the Blit in category 5 of
`gametypestable.csv`. Existing vanilla location projection values are retained.
Both `mod.ff` and `mod_load.ff` include the menu registrations and artwork.
An additional `<project>_menu.ipak` contains the image pixels, using native
streaming mode 2. Its zone declaration precedes the materials so all menu
textures enter the pack. Packaging includes the image pack and uses the
authored title and description in `mod.json`.

An optional `image_pack` field can version the pack name when publishing new
art while an earlier pack is still open in a running game.

Match the stock UI's texture canvases, sampler settings, alpha blending, and
framing. Nuketown's Large/blur canvases are 2048 square, its transparent Blit
is 512 by 256, and its lobby miniature is stored in a 256 square texture.
Canvas dimensions alone do not establish visual alignment: compare the menu
in a fresh game session after installing the files.

The native Nuketown screenshot measurements map Blit pixel coordinates to
Large pixels as `x_large = 0.625*x_blit + 864` and
`y_large = (10/9)*y_blit + 881.7778`. Large is stretched unequally on the two
axes by the menu, while Blit uses equal scaling. Extracting the Blit's color
channels from one master Large through this UV mapping keeps the geometry
registered. Independently generated renders do not guarantee matching scale
or perspective even on the correct canvas sizes.

Projects without `menu.json` retain the original placeholder frontend behavior.
Missing authored materials or images fail the build instead of silently falling
back to that placeholder.
