-- T4 hud.menu and native ammo/competitive-score ownerdraws on T6's LUI.

local W = {}
CoD.WawHUD = W
local maps = "zm/waw_hud_maps.csv"
local weapons = "zm/waw_hud_weapons.csv"
local glyphs = "zm/waw_hud_glyphs.csv"
local function lookup(t, key, column, keyColumn)
    return UIExpression.TableLookup(nil, t, keyColumn or 0, tostring(key), column) or ""
end
function W.active()
    local map = UIExpression.DvarString(nil, "mapname")
    local uiMap = UIExpression.DvarString(nil, "ui_mapname")
    if UIExpression.DvarInt(nil, "waw_hud_debug") == 1 and not W.loggedScope then
        W.loggedScope = true
        print("WAW HUD scope map=" .. tostring(map) .. " ui=" .. tostring(uiMap) .. " registered=" .. lookup(maps, map, 1))
    end
    if map and map ~= "" then return lookup(maps, map, 1) == "1" end
    return lookup(maps, uiMap, 1) == "1"
end
local function rect(e, x, y, width, height)
    e:setLeftRight(true, false, x, x + width)
    e:setTopBottom(true, false, y, y + height)
end
local function image(parent, name, x, y, width, height)
    local e = LUI.UIImage.new()
    rect(e, x, y, width, height)
    if name and name ~= "" then e:setImage(RegisterMaterial(name)) end
    parent:addElement(e)
    return e
end
local metricCache = {}
function W.plainText(value)
    -- Engine.Localize wraps localized spans in control bytes (0x14/0x15).
    -- Native UIText consumes those and ^digit colours; bitmap glyphs must not
    -- interpret them as letters. Keep genuine punctuation and Unicode intact.
    return tostring(value or ""):gsub("%^[0-9]", ""):gsub("[%z\1-\31\127]", "")
end
local function metric(role, code)
    local key = role .. ":" .. code
    if metricCache[key] then return metricCache[key] end
    local m = {material = lookup(glyphs, key, 1)}
    for i = 2, 7 do m[i] = tonumber(lookup(glyphs, key, i)) or 0 end
    if m[7] == 0 and code ~= 63 then return metric(role, 63) end
    metricCache[key] = m
    return m
end
local function codes(text)
    local result = {}
    local i = 1
    while i <= #text do
        local c = string.byte(text, i)
        local count = 1
        if c >= 240 then count = 4; c = c - 240
        elseif c >= 224 then count = 3; c = c - 224
        elseif c >= 192 then count = 2; c = c - 192 end
        for j = 1, count - 1 do c = c * 64 + (string.byte(text, i + j) or 128) - 128 end
        result[#result + 1] = c
        i = i + count
    end
    return result
end
function W.label(parent, role, height, x, baseline, right)
    local e = LUI.UIElement.new()
    rect(e, x, baseline, 0, 0)
    e.parts = {}
    function e:setText(value)
        local text = W.plainText(value)
        if self.text == text then return end
        self.text = text
        local chars = codes(text)
        local scale = height / math.max(1, metric(role, 63)[7])
        local width = 0
        for _, c in ipairs(chars) do width = width + metric(role, c)[4] * scale end
        local cursor = (right == "center" and -width / 2) or (right and -width) or 0
        for i, c in ipairs(chars) do
            local m = metric(role, c)
            local part = self.parts[i]
            if not part then
                part = LUI.UIElement.new()
                local shadow = image(part, nil, 0, 0, 0, 0)
                shadow:setLeftRight(true, true, 1, 1)
                shadow:setTopBottom(true, true, 1, 1)
                shadow:setRGB(0, 0, 0)
                local ink = image(part, nil, 0, 0, 0, 0)
                ink:setLeftRight(true, true, 0, 0)
                ink:setTopBottom(true, true, 0, 0)
                function part:setImage(material) shadow:setImage(material); ink:setImage(material) end
                self:addElement(part)
                self.parts[i] = part
            end
            if m.material ~= "" then
                part:setImage(RegisterMaterial(m.material))
                rect(part, cursor + m[2] * scale, m[3] * scale, m[5] * scale, m[6] * scale)
                part:setAlpha(1)
            else part:setAlpha(0) end
            cursor = cursor + m[4] * scale
        end
        for i = #chars + 1, #self.parts do self.parts[i]:setAlpha(0) end
    end
    parent:addElement(e)
    return e
end
local visibility = {"BIT_HUD_VISIBLE", "BIT_EMP_ACTIVE", "BIT_DEMO_CAMERA_MODE_MOVIECAM",
    "BIT_DEMO_ALL_GAME_HUD_HIDDEN", "BIT_IN_VEHICLE", "BIT_IN_GUIDED_MISSILE",
    "BIT_IN_REMOTE_KILLSTREAK_STATIC", "BIT_AMMO_COUNTER_HIDE", "BIT_IS_FLASH_BANGED",
    "BIT_UI_ACTIVE", "BIT_SCOREBOARD_OPEN", "BIT_IN_KILLCAM", "BIT_HUD_HARDCORE", "BIT_IS_SCOPED"}
local function updateVisibility(e, event)
    local controller = event.controller or e.controller
    local show = true
    for i, key in ipairs(visibility) do
        if CoD[key] then
            local value = UIExpression.IsVisibilityBitSet(controller, CoD[key])
            if (i == 1 and value ~= 1) or (i ~= 1 and value == 1) then show = false end
        end
    end
    e:setAlpha(show and 1 or 0)
    e:dispatchEventToChildren(event)
end
local function area(name, controller)
    local e = CoD.Menu.NewSafeAreaFromState(name, controller)
    e:setOwner(controller)
    e.controller = controller
    e:registerEventHandler("hud_update_refresh", updateVisibility)
    for _, key in ipairs(visibility) do
        if CoD[key] then e:registerEventHandler("hud_update_bit_" .. CoD[key], updateVisibility) end
    end
    e:setAlpha(0)
    return e
end
-- Source T4 sub_42B710/850/990/BB00/BC40 {sprite, width, height, step}.
-- Coordinates are 480-high units (rocket 0x8E4568.., belt 0x8E4578..).
local clips = {
    Magazine = {"ammo_counter_bullet", 4, 8, 4},
    ShortMagazine = {"ammo_counter_riflebullet", 32, 8, 40},
    Shotgun = {"ammo_counter_shotgunshell", 16, 8, 20},
    Rocket = {"ammo_counter_rocket", 64, 16, 72},
    Beltfed = {"ammo_counter_beltbullet", 8, 4, 8}
}
function W.drawAmmo(e)
    local count = math.max(0, tonumber(e.clip) or 0)
    local data = e.weaponData
    local style = data and clips[data.style]
    local total = data and data.size or count
    -- No invented weapon metadata: unknown/custom counters remain numeric.
    e.clipText:setText(style and "" or count)
    -- T4 heat weapons show only the heat icon (sub_44F540), no counters.
    if e.overheat then e.clipText:setText(""); e.stockText:setText("") end
    e.heat:setAlpha(e.overheat and e.heatIcon ~= "" and not e.hideAmmo and 1 or 0)
    for i, bullet in ipairs(e.bullets) do bullet:setAlpha(0) end
    if not style or e.overheat or e.hideAmmo then
        if e.hideAmmo then e.clipText:setText(""); e.stockText:setText("") end
        return
    end
    local step, width, height = style[4], style[2], style[3]
    local sprite = data.sprite ~= "" and data.sprite or style[1]
    -- T4 shows the whole magazine; spent rounds become grey, rather than vanish.
    for i = 1, math.min(300, total) do
        local x, y = -118.5 - (width + (i-1)*step) * 1.5, -6 - height * 0.75
        if data.style == "Beltfed" then
            -- sub_42BC40: rows of 20 that snake back, 2 units apart, starting
            -- (count / 20) * height / 4 below the anchor; no half-height lift.
            local row = math.floor((i - 1) / 20)
            local col = (i - 1) % 20
            if row % 2 == 1 then col = 19 - col end
            x = -118.5 - (col + 1) * step * 1.5
            y = -6 + (math.floor(total/20) * height * 0.25 - (row + 1) * 2) * 1.5
        end
        local bullet = e.bullets[i]
        if not bullet then bullet = image(e.anchor, "waw_hud/" .. sprite, x, y, width*1.5, height*1.5); e.bullets[i] = bullet end
        bullet:setImage(RegisterMaterial("waw_hud/" .. sprite))
        rect(bullet, x, y, width*1.5, height*1.5)
        local color = i <= count and 1 or 0.3
        bullet:setRGB(color, color, color)
        bullet:setAlpha(0.65)
    end
end
-- T4 sub_42CDF0 (CG_PLAYER_WEAPON_LOW_AMMO_WARNING): the clip at or under
-- clipSize * lowAmmoWarningThreshold (sub_44EE30) shows "Reload" with
-- reserve left (not on one-round clips), "LOW AMMO" without, "No Ammo" when
-- both are empty. Each pulses between its two colours (sub_42B2E0 dvar
-- defaults: freq 1.7, min 0, max 1.5, i.e. it rests on colour 2 at the crest).
local lowAmmo = {
    reload = {"WAW2BO2_PLATFORM_RELOAD", {0.9, 0.9, 0.9, 0.8}, {1, 1, 1, 1}},
    noreload = {"WAW2BO2_PLATFORM_LOW_AMMO_NO_RELOAD", {0.7, 0.7, 0, 0.8}, {1, 1, 0, 1}},
    noammo = {"WAW2BO2_WEAPON_NO_AMMO", {0.8, 0, 0, 0.8}, {1, 0, 0, 1}}
}
local pulse = {freq = 1.7, min = 0, max = 1.5}
function W.lowAmmoState(e)
    local data = e.weaponData
    if not data or e.overheat or e.hideAmmo or not e.weaponName then return nil end
    local clip, stock = tonumber(e.clip), tonumber(e.stock)
    local size, threshold = data.size or 0, data.threshold or 0
    if not clip or not stock or clip < 0 or size <= 0 then return nil end
    if math.min(size, 999) * threshold < math.min(clip, 999) then return nil end
    if stock <= 0 then return clip ~= 0 and "noreload" or "noammo" end
    if size == 1 then return nil end
    return "reload"
end
function W.pulseColor(warning, t)
    local half = (pulse.max - pulse.min) * 0.5
    local f = math.max(0, math.min(1, math.sin(t * pulse.freq * 0.0062831853) * half + pulse.min + half))
    local a, b = warning[2], warning[3]
    return a[1] + (b[1]-a[1])*f, a[2] + (b[2]-a[2])*f, a[3] + (b[3]-a[3])*f, a[4] + (b[4]-a[4])*f
end
function W.drawLowAmmo(e)
    local state = W.lowAmmoState(e)
    local warn = e.lowAmmo
    if not state then
        warn:setAlpha(0)
        warn.state = nil
        return
    end
    local warning = lowAmmo[state]
    if warn.state ~= state then
        warn.state = state
        warn.text:setText(Engine.Localize(warning[1]))
    end
    local r, g, b, a = W.pulseColor(warning, warn.time or 0)
    warn:setRGB(r, g, b)
    warn:setAlpha(a)
end
-- T4 sub_42BF20/sub_44ED70: the reserve turns (1, 0.3, 0.3) at or under a
-- fifth of the weapon's maximum.
function W.drawStock(e)
    local data, stock = e.weaponData, tonumber(e.stock)
    if data and stock and data.maxAmmo and data.maxAmmo >= 0 and
            math.min(data.maxAmmo, 999) * 0.2 >= math.min(stock, 999) then
        e.stockText:setRGB(1, 0.3, 0.3)
    else
        e.stockText:setRGB(1, 1, 1)
    end
end
function W.flash(e)
    e:setAlpha(1)
    e:beginAnimation("flash", 800)
    e:setAlpha(0)
end
function W.updateAmmo(e, event)
    if UIExpression.DvarInt(nil, "waw_hud_debug") == 1 then
        print("WAW HUD ammo " .. tostring(event.ammoInClip) .. " / " .. tostring(event.ammoStock))
    end
    if event.ammoInClip ~= nil then e.clip = event.ammoInClip end
    if event.ammoStock ~= nil then
        e.stock = event.ammoStock
        e.stockText:setText(event.ammoStock)
    end
    W.drawAmmo(e)
    W.drawStock(e)
    W.drawLowAmmo(e)
end
function W.updateWeapon(e, event)
    if UIExpression.DvarInt(nil, "waw_hud_debug") == 1 then
        print("WAW HUD weapon " .. tostring(event.weapon) .. " " .. tostring(event.weaponDisplayName) .. " inventory " .. tostring(event.inventorytype))
    end
    if event.weapon ~= nil then e.weapon = event.weapon end
    if event.weaponDisplayName then
        e.displayName = event.weaponDisplayName
        e.weaponText:setText(Engine.Localize(event.weaponDisplayName))
        if UIExpression.DvarInt(nil, "waw_hud_debug") == 1 then
            print("WAW HUD label: " .. e.weaponText.text)
        end
        local name = lookup(weapons, event.weaponDisplayName, 0, 4)
        if event.inventorytype == 3 then
            local alt = lookup(weapons, name, 5)
            if alt ~= "" then name = alt end
        end
        if lookup(weapons, name, 1) == "AltWeapon" then name = lookup(weapons, name, 5) end
        e.weaponName = name ~= "" and name or nil
        e.weaponData = {style = lookup(weapons, name, 1), size = tonumber(lookup(weapons, name, 2)) or 0,
            sprite = lookup(weapons, name, 6), maxAmmo = tonumber(lookup(weapons, name, 8)),
            threshold = tonumber(lookup(weapons, name, 9)) or 0}
        e.overheat = lookup(weapons, name, 3) == "1"
        e.heatIcon = lookup(weapons, name, 7)
        if e.heatIcon ~= "" then
            local material = RegisterMaterial(e.heatIcon)
            for _, part in ipairs({e.heat.icon, e.heat.fillIcon, e.heat.flash}) do part:setImage(material) end
        end
    end
    e.hideAmmo = event.inventorytype == 1 or event.inventorytype == 2 or
        (e.weapon and (Engine.IsWeaponType(e.weapon, "melee") or Engine.IsWeaponType(e.weapon, "grenade")))
    W.drawAmmo(e)
    W.drawStock(e)
    W.drawLowAmmo(e)
    updateVisibility(e, event)
end
-- hud.menu PC "DPad" (rect 0 -42, centre bottom): slot 1 up, 3 left, 4 right,
-- 28x28 icons (CG_PLAYER_ACTIONSLOT_n, HUD_ALPHA) with their ammo count, and
-- the "[key]" label under each usable slot (objectiveFont 0.25). Source
-- scripts fill them with SetActionSlot (the bouncing betty on slot 4).
local slotRects = {[1] = -14, [3] = -88, [4] = 60}
local function slotKey(controller, index)
    local command = "+actionslot " .. index
    local ok, key = pcall(function()
        if Engine.GetKeyBindingLocalizedString then return Engine.GetKeyBindingLocalizedString(controller, command) end
        return UIExpression.KeyBinding(controller, command)
    end)
    return ok and key or ""
end
function W.actionSlots(e, controller)
    local pad = LUI.UIElement.new()
    pad:setLeftRight(false, false, 0, 0)
    pad:setTopBottom(false, true, -63, -63)
    e:addElement(pad)
    local slots = {}
    for index, x in pairs(slotRects) do
        local slot = {}
        slot.icon = image(pad, nil, x * 1.5, -1.5, 42, 42)
        slot.count = W.label(pad, "score", 18, (x + 28) * 1.5 + 3, 40.5, false)
        slot.key = W.label(pad, "objective", 18, (x + 14) * 1.5, 60, "center")
        slot.icon:setAlpha(0)
        slots[index] = slot
    end
    local function hide(slot)
        slot.icon:setAlpha(0)
        slot.count:setText("")
        slot.key:setText("")
    end
    e:registerEventHandler("hud_update_actionslots", function(self, event)
        for _, slot in pairs(slots) do slot.seen = false end
        for key, data in pairs(event.actionSlots or {}) do
            local index = tonumber(data.slotIndex or data.index or key)
            local slot = index and slots[index]
            local material = data.material or (data.materialName and RegisterMaterial(data.materialName))
            if slot and material then
                slot.seen = true
                slot.icon:setImage(material)
                slot.icon:setAlpha(0.65)
                slot.count:setText(data.ammo ~= nil and data.ammo or "")
                slot.key:setText("[" .. W.plainText(slotKey(controller, index)) .. "]")
            end
        end
        for _, slot in pairs(slots) do if not slot.seen then hide(slot) end end
    end)
end
function W.ammo(controller)
    if UIExpression.DvarInt(nil, "waw_hud_debug") == 1 then print("WAW HUD create ammo") end
    local e = area("AmmoAreaZombie", controller)
    e.anchor = LUI.UIElement.new()
    e.anchor:setLeftRight(false, true, 0, 0)
    e.anchor:setTopBottom(false, true, 0, 0)
    e:addElement(e.anchor)
    e.bullets = {}
    -- hud.menu weaponinfo (WEAPINFO_X -190, Y -60): T4 sub_42BF20 draws the
    -- stock at its rect (-75, 4); sub_44F7F0 right-aligns the name at
    -- rect.x + rect.w - 28 = -43, baseline -40.
    e.stockText = W.label(e.anchor, "objective", 18, -112.5, 6, false)
    e.clipText = W.label(e.anchor, "objective", 18, -125, 15, true)
    e.weaponText = W.label(e.anchor, "objective", 18, -64.5, -60, true)
    -- hud.menu heatinfo heaticon (WEAPINFO_X +116, WEAPINFO_Y +15, 80x40):
    -- T4 sub_44F540 draws the weapon's hudIcon, the left heat% of it in
    -- (0.89, 0.18, 0.01) and, while overheated, a 1.01-scale yellow copy
    -- whose alpha falls from 1 to 0 every 800 ms.
    local heat = LUI.UIElement.new()
    rect(heat, -111, -67.5, 120, 60)
    e.anchor:addElement(heat)
    heat.icon = image(heat, nil, 0, 0, 120, 60)
    heat.fill = LUI.UIElement.new()
    rect(heat.fill, 0, 0, 0, 60)
    heat.fill:setUseStencil(true)
    heat:addElement(heat.fill)
    heat.fillIcon = image(heat.fill, nil, 0, 0, 120, 60)
    heat.fillIcon:setRGB(0.89, 0.18, 0.01)
    heat.flash = image(heat, nil, 0, 0, 121.2, 60.6)
    heat.flash:setRGB(1, 1, 0)
    heat.flash:setAlpha(0)
    heat.flash:registerEventHandler("transition_complete_flash", function(self, event)
        if heat.overheated and not event.interrupted then W.flash(self) end
    end)
    heat:setAlpha(0)
    e.heat = heat
    e.heatIcon = ""
    local offhands = {}
    for i, kind in ipairs({"tactical", "lethal"}) do
        local x = -201 + (i-1)*45
        offhands[kind] = {icon = image(e.anchor, nil, x, -57, 36, 36),
            count = W.label(e.anchor, "objective", 18, x+30, -12, false)}
        offhands[kind].icon:setAlpha(0)
    end
    e:registerEventHandler("hud_update_offhand", function(self, event)
        for kind, parts in pairs(offhands) do
            local data = event[kind]
            -- T6 drops an empty offhand from the event; T4 keeps drawing the
            -- held grenade's icon with a 0 count.
            if data and data.material then
                parts.icon:setImage(data.material)
                parts.material = true
            end
            local shown = data ~= nil or parts.material
            parts.icon:setAlpha(shown and 0.65 or 0)
            parts.count:setText(shown and (data and data.ammo or 0) or "")
        end
    end)
    -- hud.menu lowammowarning: rect -10 15 100 30, centred; UI_FONT_DEFAULT
    -- (normalFont), TEXTSIZE_SMALL, centred text.
    local warn = LUI.UIElement.new()
    warn:setLeftRight(false, false, 0, 0)
    warn:setTopBottom(false, false, 0, 0)
    warn.text = W.label(warn, "score", 18, 60, 45 + 9, "center")
    warn:setAlpha(0)
    e:addElement(warn)
    e.lowAmmo = warn
    e:registerEventHandler("waw_low_ammo_pulse", function(self)
        warn.time = (warn.time or 0) + 50
        if warn.state then W.drawLowAmmo(self) end
    end)
    if LUI.UITimer then e:addElement(LUI.UITimer.new(50, "waw_low_ammo_pulse", false, e)) end
    W.actionSlots(e, controller)
    e:registerEventHandler("hud_update_ammo", W.updateAmmo)
    e:registerEventHandler("hud_update_weapon", W.updateWeapon)
    e:registerEventHandler("hud_update_weapon_select", W.updateWeapon)
    e:registerEventHandler("hud_update_overheat", function(self, event)
        self.overheat = true
        local heat = self.heat
        local value = math.max(0, math.min(100, tonumber(event.heatPercent) or 0))
        rect(heat.fill, 0, 0, 120 * value / 100, 60)
        if event.overheat and not heat.overheated then
            heat.overheated = true
            W.flash(heat.flash)
        elseif not event.overheat and heat.overheated then
            heat.overheated = false
            heat.flash:beginAnimation("flash_off", 1)
            heat.flash:setAlpha(0)
        end
        W.drawAmmo(self)
    end)
    e:registerEventHandler("hud_update_fuel", function(self, event)
        self.stockText:setText(tostring(event.fuelPercent or 0) .. "%")
    end)
    return e
end
-- T4 sub_666820 zombie column tables {type, width, label, align}: online or
-- system link games use the co-op table, others the solo one. Types: 0 name,
-- 2 points, 3 ping, 4 status icon, 5 voice, 6 kills, 7 rank, 9 downs,
-- 10 revives, 11 headshots. align 2 right-aligns 4 units inside the column.
local boardColumns = {
    coop = {{7, 0.08}, {4, 0.04}, {0, 0.28, nil, 0}, {5, 0.04}, {2, 0.093, "CGAME_SB_POINTS", 2},
        {6, 0.093, "CGAME_SB_KILLS", 2}, {9, 0.093, "CGAME_SB_DOWNS", 2}, {10, 0.093, "CGAME_SB_REVIVES", 2},
        {11, 0.093, "CGAME_SB_HEADSHOTS", 2}, {3, 0.093, "CGAME_SB_PING", 2}},
    solo = {{4, 0.07}, {0, 0.606, nil, 0}, {2, 0.108, "CGAME_SB_POINTS", 2},
        {6, 0.108, "CGAME_SB_KILLS", 2}, {11, 0.108, "CGAME_SB_HEADSHOTS", 2}}
}
-- BO2 scoreboard columns by their names; T4 stat types they carry.
local boardStats = {{"HEADSHOT", 11}, {"REVIVE", 10}, {"DOWN", 9}, {"KILL", 6}, {"SCORE", 2}, {"POINT", 2}}
-- default.cfg runs coop_arcademode.cfg: cg_ScoresColor_Gamertag_0..3,
-- cg_ScoresColor_Zombie and cg_ScoresColor_Transparency.
local boardNames = {{1, 1, 1}, {0.486, 0.812, 0.933}, {0.965, 0.792, 0.314}, {0.514, 0.925, 0.533}}
-- T4 sub_667F80 geometry in 480-high units: cg_scoreboardWidth 510, height
-- 435, banner 35; rows 12 high, 4 apart; header scale 0.2, rows 0.22 (em
-- height 48 * scale); row baseline (pixelHeight * 0.22 + 12) * 0.64.
local board = {width = 510, inner = 492, header = 103.4, rows = 107.4, row = 12, gap = 4}
local function boardLayout()
    local offline = Engine.SessionModeIsMode and CoD.SESSIONMODE_OFFLINE and
        Engine.SessionModeIsMode(CoD.SESSIONMODE_OFFLINE)
    return offline and boardColumns.solo or boardColumns.coop
end
local function boardText(parent, scale, x, baseline, right)
    return W.label(parent, "score", 48 * scale * 1.5, x * 1.5, baseline * 1.5, right)
end
function W.boardStats(controller)
    local stats = {}
    for i = 0, 15 do
        local name = Engine.GetScoreBoardColumnName(controller, i)
        if not name or name == "" then break end
        local upper = string.upper(name)
        for _, entry in ipairs(boardStats) do
            if string.find(upper, entry[1], 1, true) and not stats[entry[2]] then stats[entry[2]] = i; break end
        end
        if UIExpression.DvarInt(nil, "waw_hud_debug") == 1 then print("WAW HUD board column " .. i .. " " .. name) end
    end
    return stats
end
function W.boardPlayers(controller)
    local players = {}
    for _, position in ipairs(Engine.GetTeamPositions(controller) or {}) do
        for i = 0, (Engine.GetMatchScoreboardClientCount(position.team) or 0) - 1 do
            local index, clientNum = Engine.GetMatchScoreboardIndexAndClientNumForTeam(i, position.team,
                CoD.SCOREBOARD_SORT_CLIENTNUM)
            if index ~= nil and clientNum ~= nil then players[#players + 1] = {index = index, clientNum = clientNum} end
        end
    end
    -- T4 lists zombie players in client number order.
    table.sort(players, function(a, b) return a.clientNum < b.clientNum end)
    return players
end
function W.updateBoard(e)
    local controller = e.controller
    local stats = e.stats
    local players = W.boardPlayers(controller)
    for i, player in ipairs(players) do
        local row = e.rows[i]
        if not row then
            row = LUI.UIElement.new()
            row:setLeftRight(true, true, 0, 0)
            row.bg = image(row, nil, -246 * 1.5, 0, board.inner * 1.5, board.row * 1.5)
            row.cells = {}
            local x = -246
            local baseline = (metric("score", 63)[7] * 0.22 + 12) * 0.64
            for c, column in ipairs(e.columns) do
                local width = column[2] * board.inner
                if column[1] == 4 then
                    row.cells[c] = image(row, nil, (x + (width - 13) / 2) * 1.5, 0, 19.5, 19.5)
                    row.cells[c]:setAlpha(0)
                elseif column[1] == 0 then
                    row.cells[c] = boardText(row, 0.22, x, baseline, false)
                elseif column[4] == 2 then
                    row.cells[c] = boardText(row, 0.22, x + width - 4, baseline, true)
                end
                x = x + width
            end
            e.panel:addElement(row)
            e.rows[i] = row
        end
        local y = board.rows + (i - 1) * (board.row + board.gap)
        row:setTopBottom(true, false, y * 1.5, (y + board.row) * 1.5)
        row.bg:setImage(RegisterMaterial("waw_hud/scorebar_zom_long_" .. ((player.clientNum % 4) + 1)))
        row.bg:setRGB(0.424, 0.004, 0)
        row.bg:setAlpha(0.35)
        local color = boardNames[(player.clientNum % 4) + 1]
        for c, column in ipairs(e.columns) do
            local cell, kind, text = row.cells[c], column[1], nil
            if kind == 0 then text = Engine.GetFullGamertagForScoreboardIndex(player.index)
            elseif kind == 3 then text = Engine.GetPingForScoreboardIndex(player.index)
            elseif stats[kind] then text = Engine.GetScoreboardColumnForScoreboardIndex(player.index, stats[kind])
            elseif kind == 4 then
                local icon = Engine.GetStatusIconForClient(player.clientNum)
                cell:setAlpha(icon and 1 or 0)
                if icon then cell:setImage(icon) end
            end
            if cell and kind ~= 4 then
                cell:setText(text or "")
                cell:setRGB(color[1], color[2], color[3])
            end
        end
        row:setAlpha(1)
    end
    for i = #players + 1, #e.rows do e.rows[i]:setAlpha(0) end
end
function W.scoreboard(controller)
    local e = CoD.Menu.NewFromState("Scoreboard")
    e:setLeftRight(true, true, 0, 0)
    e:setTopBottom(true, true, 0, 0)
    e:setOwner(controller)
    e.controller = controller
    e.columns = boardLayout()
    e.stats = W.boardStats(controller)
    e.rows = {}
    -- 640x480 placement centred on screen; the scoreboard is a centred column.
    e.panel = LUI.UIElement.new()
    e.panel:setLeftRight(false, false, 0, 0)
    e.panel:setTopBottom(false, false, -360, 360)
    e:addElement(e.panel)
    local x = -246
    for _, column in ipairs(e.columns) do
        local width = column[2] * board.inner
        if column[3] then
            boardText(e.panel, 0.2, x + width, board.header, true):setText(Engine.Localize("WAW2BO2_" .. column[3]))
        end
        x = x + width
    end
    e.close = function(self) CoD.Menu.close(self) end
    e:registerEventHandler("close_all_ingame_menus", e.close)
    e:registerEventHandler("close_scoreboard_menu", e.close)
    e:registerEventHandler("update_scoreboard", function(self) W.updateBoard(self) end)
    W.updateBoard(e)
    return e
end
-- Plutonium replaces hud/scoreboard.lua with a loose file, so the HUD entry
-- wraps whichever Scoreboard menu is defined instead of shipping a module.
function W.scoreboardMenu(controller)
    if W.active() then return W.scoreboard(controller) end
    return W.stockScoreboard(controller)
end
function W.hookScoreboard()
    local original = LUI.createMenu.Scoreboard
    if original and original ~= W.scoreboardMenu then
        W.stockScoreboard = original
        LUI.createMenu.Scoreboard = W.scoreboardMenu
    end
end
function W.scores(controller)
    if UIExpression.DvarInt(nil, "waw_hud_debug") == 1 then print("WAW HUD create scores") end
    local e = area("CompetitiveScoreboard", controller)
    e.rows = {}
    e:registerEventHandler("hud_update_competitive_scoreboard", function(self, event)
        if not event.competitivescores then return end
        if UIExpression.DvarInt(nil, "waw_hud_debug") == 1 then
            print("WAW HUD scores: " .. tostring(#event.competitivescores) .. " players")
        end
        local players = {}
        for _, player in pairs(event.competitivescores) do players[#players + 1] = player end
        table.sort(players, function(a, b) return a.clientNum < b.clientNum end)
        for i, player in ipairs(players) do
            local row = self.rows[i]
            if not row then
                row = LUI.UIElement.new()
                row:setLeftRight(false, true, -154.5, -4.5)
                row.bg = image(row, nil, 0, 0, 150, 31.5)
                row.text = W.label(row, "score", 20, 7.5, 24, false)
                self:addElement(row)
                self.rows[i] = row
            end
            local y = -106.5 - (#players - i)*27
            row:setTopBottom(false, true, y - 31.5, y)
            row.bg:setImage(RegisterMaterial("waw_hud/scorebar_zom_" .. ((player.clientNum % 4) + 1)))
            row.bg:setRGB(0.474, 0.004, 0)
            row.bg:setAlpha(0.8)
            row.text:setText(player.score)
            row:setAlpha(1)
        end
        for i = #players + 1, #self.rows do self.rows[i]:setAlpha(0) end
    end)
    return e
end
