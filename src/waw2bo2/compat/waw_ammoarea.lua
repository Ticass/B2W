-- AmmoAreaZombie is loaded lazily by HUD_IngameGameTypeZombie, after map load.
require("T6.Zombie.Waw_AmmoArea_Base")
require("T6.Waw_Hud_Runtime")
local original = LUI.createMenu.AmmoAreaZombie
LUI.createMenu.AmmoAreaZombie = function(controller)
    if CoD.WawHUD and CoD.WawHUD.active() then return CoD.WawHUD.ammo(controller) end
    return original(controller)
end
