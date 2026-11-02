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
