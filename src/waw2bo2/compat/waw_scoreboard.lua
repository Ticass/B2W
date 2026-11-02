require("T6.Zombie.Waw_Scoreboard_Base")
require("T6.Waw_Hud_Runtime")
local original = LUI.createMenu.CompetitiveScoreboard
LUI.createMenu.CompetitiveScoreboard = function(controller)
    if CoD.WawHUD.active() then return CoD.WawHUD.scores(controller) end
    return original(controller)
end
