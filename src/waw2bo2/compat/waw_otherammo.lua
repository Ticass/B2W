require("T6.Zombie.Waw_OtherAmmo_Base")
require("T6.Waw_Hud_Runtime")
local original = CoD.OtherAmmoCounters.new
CoD.OtherAmmoCounters.new = function(...)
    if CoD.WawHUD.active() then return LUI.UIElement.new() end
    return original(...)
end
