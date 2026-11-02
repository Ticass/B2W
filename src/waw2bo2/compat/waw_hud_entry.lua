-- Keep T6's common HUD (menus, damage effects, spectating and engine refresh).
-- Converted maps assemble the source HUD instead of the stock zombie widgets.
require("T6.Waw_Hud_Base")
require("T6.Waw_Hud_Runtime")
require("T6.Waw_Menu")
CoD.WawHUD.hookScoreboard()
CoD.WawMenu.hook()
local original = HUD_FirstSnapshot_Zombie
HUD_FirstSnapshot_Zombie = function(hud, event)
    if not CoD.WawHUD.active() then return original(hud, event) end
    CoD.WawHUD.hookScoreboard()
    CoD.WawMenu.hook()
    local controller = event.controller
    local ammo = CoD.WawHUD.ammo(controller)
    local scores = CoD.WawHUD.scores(controller)
    ammo:setUseGameTime(true)
    scores:setUseGameTime(true)
    hud:addElement(ammo)
    hud:addElement(scores)
    if UIExpression.DvarInt(nil, "waw_hud_debug") == 1 then
        print("WAW HUD snapshot: source ammo + source scores; stock zombie widgets skipped")
    end
end
