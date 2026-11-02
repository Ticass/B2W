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
        local cursor = right and -width or 0
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
-- Source T4 sub_42B710/850/990/BB00/BC40. Coordinates are 480-high units.
local clips = {
    Magazine = {"ammo_counter_bullet", 4, 8, 4},
    ShortMagazine = {"ammo_counter_riflebullet", 32, 8, 40},
    Shotgun = {"ammo_counter_shotgunshell", 16, 8, 20},
    Rocket = {"ammo_counter_rocket", 12, 64, 72},
    Beltfed = {"ammo_counter_beltbullet", 12, 8, 4}
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
            local row = math.floor((i - 1) / 20)
            local col = (i - 1) % 20
            if row % 2 == 1 then col = 19 - col end
            x = -118.5 - (col + 1) * 6
            y = -6 + math.floor(total/20)*3 - (row + 1)*3
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
    if event.ammoStock ~= nil then e.stockText:setText(event.ammoStock) end
    W.drawAmmo(e)
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
        e.weaponData = {style = lookup(weapons, name, 1), size = tonumber(lookup(weapons, name, 2)) or 0,
            sprite = lookup(weapons, name, 6)}
        e.overheat = lookup(weapons, name, 3) == "1"
        e.heatIcon = lookup(weapons, name, 7)
        if e.heatIcon ~= "" then
            local material = RegisterMaterial(e.heatIcon)
            for _, part in ipairs({e.heat.icon, e.heat.fillIcon, e.heat.flash}) do part:setImage(material) end
        end
        e.stockText:setRGB(1, 1, 1)
    end
    e.hideAmmo = event.inventorytype == 1 or event.inventorytype == 2 or
        (e.weapon and (Engine.IsWeaponType(e.weapon, "melee") or Engine.IsWeaponType(e.weapon, "grenade")))
    W.drawAmmo(e)
    updateVisibility(e, event)
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
            parts.icon:setAlpha(data and 0.65 or 0)
            parts.count:setText(data and data.ammo or "")
            if data and data.material then parts.icon:setImage(data.material) end
        end
    end)
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
