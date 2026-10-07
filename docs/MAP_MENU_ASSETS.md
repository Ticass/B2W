# Authored BO2 map menus

The desktop launcher has a **Map Details & Artwork** tab. Enter the title and
description and upload Blit (512 × 256, transparent PNG/TGA), Large (2048 × 2048),
and Blur (2048 × 2048). Large and Blur accept PNG, JPEG, or TGA. All three images
are required when using custom artwork; incorrect sizes and opaque Blits are
rejected. Text can also be customized without uploading artwork.

The preview selector shows map selection, individual canvases, the lobby
thumbnail, and the loading screen. The three-upload workflow uses Large for
loading artwork and resamples it to 256 × 256 for the lobby thumbnail. It does
not crop uploaded artwork or generate Blur. Map selection overlays Blit using
the measured projection below. These are layout approximations; stock menu
projection and final framing still require an in-game check.

The build exports native T6 IWI textures and materials and carries the title,
description, and artwork through both frontend zones and the packaged mod.

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
localization. It binds the icon in `mapstable.csv` and the Blit in category 5 of
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
