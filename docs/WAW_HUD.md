# Original WaW HUD on T6

`wawhud.py` stages WaW's engine-owned weapon and competitive-score displays.
`compat/waw_hud.lua` renders them through LUI, using native T6 client HUD events.
The map's ported round, perk, power-up, revive and floating-score scripts remain
the owners of those displays and rules.

Source references:

- WaW `ui/hud.menu`: `weaponinfo`, `competitivemodescores`, foreground alpha,
  offhand icons/counts and the score anchor at (-103, -71).
- T4 `sub_42B710`, `sub_42B850`, `sub_42B990`, `sub_42BB00`, `sub_42BC40`:
  magazine, short magazine, shell/Tesla, rocket and alternating belt graphics.
  Spent rounds are grey. `sub_42B990` selects `ammo_counter_tesla` when the source
  weapon name contains `tesla_gun`.
- T4 `sub_6690B0` / `sub_668A40`: client-number order, original
  `scorebar_zom_1` through `scorebar_zom_4`, 18-unit rows and 21-unit local row.
- Original `fonts/objectiveFont` and `fonts/normalFont`: atlas pixels, bearings,
  advances and character repertoire. Source overrides precede stock zones.

The T6 linker does not import T4 font objects. Each glyph is copied losslessly
from its original atlas into a small padded RGBA sprite; its original metrics
are retained in `zm/waw_hud_glyphs.csv`. LUI pools glyph elements and draws a
shadow beneath the text. It does not use a substitute BO2 font.

`stage_bridge` stages the source images and font metrics. `build-mod` stages a
checked-in, hash-verified 32-bit T6 Lua wrapper and generates the weapon metadata
from `content_source/weapons`. The original BO2 HUD bytecode is preserved under
`ui_mp/t6/waw_hud_base.lua`. `waw_hud_entry.lua` replaces only the zombie
first-snapshot assembly when the current map appears in `zm/waw_hud_maps.csv`.
It creates the source ammo and score displays before T6's `ForceHUDRefresh`.
This skips both native ammo branches (TranZit's `DPadArea` and later DLCs'
`AmmoAreaZombie`) and the native score artwork. Common HUD setup, menus, damage
effects and spectating stay native. Unregistered maps call the preserved stock
assembly. It changes no global GSC installation or gameplay callback.

Bitmap labels consume localization control delimiters and `^digit` colour
codes instead of drawing them as missing-glyph question marks. Actual question
marks, other punctuation and Unicode text remain unchanged.

Ammo, offhand and score changes arrive through native client HUD events, so there
is no server polling or per-frame GSC HUD allocation. Menu, scope, flash and
other native visibility bits hide the display. Unknown weapon metadata keeps a
numeric counter instead of inventing a magazine type. Native last stand and
spectating feed the same client HUD events.

Rebuild bytecode after changing Lua:

```powershell
python tools/compile_menu_layers.py work/menu_layer_fix/hksc/hksc.exe waw_hud
python tools/compile_menu_layers.py work/menu_layer_fix/hksc/hksc.exe waw_hud_entry
```

`tests/waw_hud_runtime.lua` exercises map isolation, firing/reloading, heat-to-gun
switches, menu visibility, co-op row changes and glyph pooling with mocked LUI
objects. `test_wawhud.py` also verifies original alpha/metrics survive staging
and fails explicitly when source assets are missing. In-game visual checking
is required before treating a package as a release. Set `waw_hud_debug` to 1 to
log incoming ammo/weapon events for that check.

## Scoreboard (TAB)

T4 draws the zombies scoreboard in code (`sub_6680B0` -> `sub_667F80`):
`sub_666820` picks the column table (co-op table when online or system link,
solo table otherwise), `sub_666920` draws right-aligned `CGAME_SB_*` headers
at scale 0.2, `sub_667680` draws each row: `scorebar_zom_long_<client+1>`
tinted `cg_ScoresColor_Zombie` at `cg_ScoresColor_Transparency`, text in
`cg_ScoresColor_Gamertag_<client>` (all from `coop_arcademode.cfg`, run by
`default.cfg`). Geometry: width 510 centred, header baseline 103.4, rows
12 high from 107.4, 4 apart. `W.scoreboard` rebuilds it in LUI from BO2's
scoreboard data (`GetMatchScoreboardIndexAndClientNumForTeam`, columns
matched by name). Plutonium ships a loose `hud/scoreboard.lua`, so the HUD
entry wraps `LUI.createMenu.Scoreboard` instead of replacing that module.
The `CGAME_SB_*` labels are staged from WaW's `code_post_gfx` strings.

Flamethrower heat (`hud.menu` heatinfo, T4 `sub_44F540`): the weapon's
`hudIcon` filled left to right in (0.89, 0.18, 0.01) by heat, with a yellow
1.01-scale copy fading every 800 ms while overheated.

## Pause menu (Escape)

`wawmenu.py` translates WaW's own in-game menus instead of drawing an imitation.
T4 loads the engine menulists `ui/ingame.txt` (stock `common.ff`) and
`ui/hud.txt` (the map's zone, re-shipped in `<map>_patch.ff`). Their sources in
the Mod Tools `raw/ui` are preprocessed as WaW's menu compiler does (`PC`
defined; `#include`, macros, `#ifdef`, line continuations, constant folding),
every `menuDef`/`itemDef` is parsed, and the closure reachable from
`pausedmenu` is kept: `pausedmenu`, `objectiveinfo`, `overheadmap` and the
restart/quit popups. The result is `zm/waw_menus.csv`: rects, styles, colours,
fonts, `visible when()`/`exp` expressions (as RPN) and item scripts. The menus'
materials (`menu_black_line`, `menu_button_backing_*`, `menu_map_<wawmap>`, ...)
are staged from the map's zones then the stock zones, and the glyphs their
resolved text uses go to `zm/waw_menu_glyphs.csv`. The `MENU_*`/`PLATFORM_*`
strings are requested from WaW's localization like any other reference.

`compat/waw_menu.lua` interprets that table inside BO2's `class` menu, which
stays the engine host (it pauses solo games). Rects use T4 ScrPlace alignment
(SUBLEFT = centred 4:3 frame, RIGHT/FULLSCREEN anchors), text uses
`Item_SetTextExtents` alignment with `R_NormalizedTextScale` (48 * scale /
pixelHeight) and T4 `UI_GetFontHandle` (t4sp 0x5B6400: DEFAULT picks
small/extraBig/big/normal by textscale * screen height / 480 against
ui_smallFont 0.25, ui_extraBigFont 0.55, ui_bigFont 0.4). Focus, highlight and
focus colour come from the items' own `onFocus` local variables. Scripts run as
in T4: `open`/`close`/`showMenu`/`hideMenu`, `setfocus`/`focusfirst`,
`setLocalVar*`, `setdvar`, `exec`/`execnow`, `scriptMenuResponse`.

Engine glue (`compat/waw_menu_glue.json`): WaW dvars the menus test map to T6
state (`zombiemode` 1, `mapname` the source map, `sv_running`, session modes);
`options_new_pc` opens T6's `OptionsMenu`, because WaW's options edit T4
engine settings; `cl_paused` is cleared before `disconnect`/`fast_restart`, as
T6's own popups do; when no WaW menu is left open the host resumes through
`CoD.InGameMenu.BackButtonPressed` (unpause, `ingame_menu_closed`).

Not ported: WaW's cursor (T6 draws its own), the `mouse_over`/`mouse_click` UI
sounds (no WaW UI aliases are staged yet; `play` only fires for a `sound:<alias>` table row),
the objective-list ownerdraw (zombies has no objectives), and co-op
`scriptMenuResponse "endround"`, whose WaW handler (`_ingamemenus.gsc`) is not
part of the port; the response reaches T6's menu-response handling unchanged (unverified in game). Sources come from
`raw/ui`; the compiled zone menus were checked to match (strings, materials),
but a custom map that ships modified compiled pause menus without sources gets
the stock ones. Set `waw_menu_debug` 1 to log menu opens and ignored commands.

```powershell
python tools/compile_menu_layers.py work/menu_layer_fix/hksc/hksc.exe waw_menu
```
