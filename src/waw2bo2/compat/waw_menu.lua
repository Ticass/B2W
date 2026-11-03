-- WaW's own in-game menus (wawmenu.py) interpreted on T6 LUI.
-- zm/waw_menus.csv holds the translated menuDefs/itemDefs: 640x480 rects with
-- T4 alignment, styles, text, visible/exp expressions (RPN) and item scripts.
-- BO2's "class" menu stays the engine host: it pauses the game, and resuming
-- goes through CoD.InGameMenu.BackButtonPressed (cl_paused 0, ingame_menu_closed).

local M = {}
CoD.WawMenu = M
local TABLE = "zm/waw_menus.csv"
local GLYPHS = "zm/waw_menu_glyphs.csv"
local S = 1.5 -- 480 virtual units -> 720 LUI units
-- wawmenu.MENU_COLUMNS / ITEM_COLUMNS (column 0 is the key).
local MENU = {rect = 1, onopen = 2, onclose = 3, onesc = 4, blurworld = 5, items = 6, visible = 7, focuscolor = 9}
local ITEM = {rect = 1, style = 2, background = 3, forecolor = 4, backcolor = 5, text = 9, textfont = 10,
    textscale = 11, textstyle = 12, textalign = 13, textalignx = 14, textaligny = 15, visible = 16, exp = 17,
    type = 18, name = 19, onfocus = 20, leavefocus = 21, action = 22, decoration = 23, origin = 25, autowrapped = 26}
-- compat/waw_menu_glue.json leave_commands: T6 clears cl_paused before these.
local LEAVE = {disconnect = true, fast_restart = true, map_restart = true, quit = true, checkpoint_restart = true}

local function cell(key, column)
    return UIExpression.TableLookup(nil, TABLE, 0, key, column) or ""
end
local function debug(text)
    if UIExpression.DvarInt(nil, "waw_menu_debug") == 1 then print("WAW MENU " .. text) end
end
local function guarded(f, ...)
    if not f then return nil end
    local ok, result = pcall(f, ...)
    if ok then return result end
    debug("call failed: " .. tostring(result))
end
local function unescape(s)
    return (string.gsub(s, "%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end
local function numbers(s)
    local t = {}
    for v in string.gmatch(s or "", "%S+") do t[#t + 1] = tonumber(v) or 0 end
    return t
end
local function split(s, sep)
    local t = {}
    if s == "" then return t end
    local start = 1
    while true do
        local i = string.find(s, sep, start, true)
        if not i then t[#t + 1] = string.sub(s, start); return t end
        t[#t + 1] = string.sub(s, start, i - 1)
        start = i + 1
    end
end

-- ------------------------------------------------------------ expressions

local function number(v)
    if type(v) == "number" then return v end
    return tonumber(v) or 0
end
local function truth(v) return number(v) ~= 0 end
local function bool(b) if b then return 1 end return 0 end

local function session(mode)
    return CoD[mode] ~= nil and Engine.SessionModeIsMode ~= nil and guarded(Engine.SessionModeIsMode, CoD[mode]) == true
end
-- compat/waw_menu_glue.json "dvars": T4 engine state the menus test.
local glue = {
    zombiemode = function() return "1" end,
    mapname = function() return cell("mapname", 1) end,
    splitscreen = function() return tostring(bool(guarded(Engine.IsSplitscreen) == true)) end,
    systemlink = function() return tostring(bool(session("SESSIONMODE_SYSTEMLINK"))) end,
    systemlinkparty = function() return tostring(bool(session("SESSIONMODE_SYSTEMLINK"))) end,
    onlinegame = function()
        return tostring(bool(not session("SESSIONMODE_OFFLINE") and not session("SESSIONMODE_SYSTEMLINK")))
    end,
    xblive_privatematch = function()
        local private = CoD.GAMEMODE_PRIVATE_MATCH and guarded(Engine.GameModeIsMode, CoD.GAMEMODE_PRIVATE_MATCH) == true
        return tostring(bool(private and not session("SESSIONMODE_OFFLINE") and not session("SESSIONMODE_SYSTEMLINK")))
    end,
    arcademode = function() return "0" end,
    credits_active = function() return "" end,
    ui_shipping_cheats = function() return "0" end,
}
function M.dvar(name)
    local f = glue[string.lower(name)]
    if f then return f() end
    return UIExpression.DvarString(nil, name) or ""
end

local functions = {}
functions.dvarstring = function(host, a) return M.dvar(a[1]) end
functions.dvarbool = function(host, a) return bool(truth(M.dvar(a[1]))) end
functions.dvarint = function(host, a) return math.floor(number(M.dvar(a[1]))) end
functions.dvarfloat = function(host, a) return number(M.dvar(a[1])) end
functions.localvarint = function(host, a) return math.floor(number(host.vars[a[1]])) end
functions.localvarbool = function(host, a) return bool(truth(host.vars[a[1]])) end
functions.localvarfloat = function(host, a) return number(host.vars[a[1]]) end
functions.localvarstring = function(host, a) return tostring(host.vars[a[1]] or "") end
functions.text = function(host, a) return a[1] end
functions.when = functions.text
functions.int = function(host, a) return math.floor(number(a[1])) end
functions.min = function(host, a) return math.min(number(a[1]), number(a[2])) end
functions.max = function(host, a) return math.max(number(a[1]), number(a[2])) end
functions.issplitscreenhost = function(host)
    return bool(guarded(Engine.IsSplitscreen) ~= true or host.controller == 0)
end
functions.inprivateparty = function() return bool(guarded(Engine.InPrivateParty) == true) end
functions.privatepartyhost = function() return bool(guarded(Engine.PrivatePartyHost) == true) end
functions.privatepartyhostinlobby = function() return bool(guarded(Engine.PrivatePartyHostInLobby) == true) end
functions.tablelookup = function(host, a)
    return UIExpression.TableLookup(nil, a[1], number(a[2]), tostring(a[3]), number(a[4])) or ""
end

local function compare(a, b)
    local x, y = tonumber(a), tonumber(b)
    if x and y then return x, y end
    return tostring(a), tostring(b)
end
local binary = {
    ["&&"] = function(a, b) return bool(truth(a) and truth(b)) end,
    ["||"] = function(a, b) return bool(truth(a) or truth(b)) end,
    ["=="] = function(a, b) local x, y = compare(a, b) return bool(x == y) end,
    ["!="] = function(a, b) local x, y = compare(a, b) return bool(x ~= y) end,
    ["<"] = function(a, b) return bool(number(a) < number(b)) end,
    [">"] = function(a, b) return bool(number(a) > number(b)) end,
    ["<="] = function(a, b) return bool(number(a) <= number(b)) end,
    [">="] = function(a, b) return bool(number(a) >= number(b)) end,
    -- T4 concatenates when either side is a string ("menu_map_" + mapname).
    ["+"] = function(a, b)
        if type(a) == "string" and not tonumber(a) or type(b) == "string" and not tonumber(b) then
            return tostring(a) .. tostring(b)
        end
        return number(a) + number(b)
    end,
    ["-"] = function(a, b) return number(a) - number(b) end,
    ["*"] = function(a, b) return number(a) * number(b) end,
    ["/"] = function(a, b) if number(b) == 0 then return 0 end return number(a) / number(b) end,
    ["%"] = function(a, b) if number(b) == 0 then return 0 end return number(a) % number(b) end,
}
local compiled = {}
local function compile(code)
    local tokens = compiled[code]
    if tokens then return tokens end
    tokens = {}
    for tok in string.gmatch(code or "", "%S+") do tokens[#tokens + 1] = tok end
    compiled[code] = tokens
    return tokens
end
function M.eval(host, code)
    if not code or code == "" then return 1 end
    local stack = {}
    for _, tok in ipairs(compile(code)) do
        local kind = string.sub(tok, 1, 2)
        if kind == "n:" then
            stack[#stack + 1] = tonumber(string.sub(tok, 3)) or 0
        elseif kind == "s:" then
            stack[#stack + 1] = unescape(string.sub(tok, 3))
        elseif kind == "f:" then
            local _, _, name, argc = string.find(tok, "^f:([^:]+):(%d+)$")
            argc = tonumber(argc) or 0
            local args = {}
            for i = argc, 1, -1 do args[i] = stack[#stack]; stack[#stack] = nil end
            local f = functions[name]
            if f then stack[#stack + 1] = f(host, args)
            else
                debug("unknown expression function " .. tostring(name))
                stack[#stack + 1] = 0
            end
        else
            local op = string.sub(tok, 4)
            if op == "!" then stack[#stack] = bool(not truth(stack[#stack]))
            elseif op == "neg" then stack[#stack] = -number(stack[#stack])
            else
                local b = stack[#stack]; stack[#stack] = nil
                local a = stack[#stack]
                stack[#stack] = binary[op](a, b)
            end
        end
    end
    return stack[#stack] or 0
end

-- ------------------------------------------------------------ layout

local function screenWidth()
    local aspect = guarded(Engine.GetAspectRatio) or 16 / 9
    return 720 * aspect
end
local function screenHeight()
    local _, _, h = string.find(UIExpression.DvarString(nil, "r_mode") or "", "%d+x(%d+)")
    return tonumber(h) or 720
end
-- T4 ScrPlace alignment of a 640x480 rect, as LUI anchors and offsets.
function M.horizontal(x, w, align)
    if align == 4 then
        local k = screenWidth() / 640
        return true, false, x * k, (x + w) * k
    elseif align == 1 then return true, false, x * S, (x + w) * S
    elseif align == 2 or align == 7 then return false, false, x * S, (x + w) * S
    elseif align == 3 then return false, true, x * S, (x + w) * S
    end
    return false, false, (x - 320) * S, (x + w - 320) * S
end
function M.vertical(y, h, align)
    if align == 2 or align == 7 then return false, false, y * S, (y + h) * S
    elseif align == 3 then return false, true, y * S, (y + h) * S
    end
    return true, false, y * S, (y + h) * S
end
local function place(e, x, y, w, h, ha, va)
    e:setLeftRight(M.horizontal(x, w, ha))
    e:setTopBottom(M.vertical(y, h, va))
end

-- ------------------------------------------------------------ text

local glyphCache = {}
local function glyph(role, code)
    local key = role .. ":" .. code
    local g = glyphCache[key]
    if g then return g end
    local material = UIExpression.TableLookup(nil, GLYPHS, 0, key, 1) or ""
    g = {material = material}
    for i = 2, 7 do g[i] = tonumber(UIExpression.TableLookup(nil, GLYPHS, 0, key, i) or "") or 0 end
    if g[7] == 0 then
        if code ~= 63 then return glyph(role, 63) end
        g[7] = 1
    end
    glyphCache[key] = g
    return g
end
-- T4 UI_GetFontHandle (t4sp 0x5B6400); ui_smallFont 0.25, ui_bigFont 0.4,
-- ui_extraBigFont 0.55 against textscale * screen height / 480.
function M.fontRole(enum, scale, height)
    local size = scale * (height or screenHeight()) / 480
    if enum == 1 then return "normal" elseif enum == 3 then return "small"
    elseif enum == 5 then return "console" elseif enum == 6 then return "objective" end
    if size <= 0.25 then return "small" end
    if enum == 4 then
        if size >= 0.4 then return "bold" end
        return "normal"
    end
    if size >= 0.55 then return "extrabig" end
    if size >= 0.4 then return "big" end
    return "normal"
end
local function codes(text)
    local result, i = {}, 1
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
local function plain(value)
    return (string.gsub(string.gsub(tostring(value or ""), "%^[0-9]", ""), "[%z\1-\9\11-\31\127]", ""))
end
-- Lines of glyph codes and their widths in virtual units (R_NormalizedTextScale:
-- 48 * scale / font pixel height).
function M.measure(text, role, scale, wrap)
    local chars = codes(plain(text))
    local lines, line, width, word = {}, {}, 0, nil
    local k = 48 * scale / math.max(1, glyph(role, 63)[7])
    local function push()
        lines[#lines + 1] = {chars = line, width = width}
        line, width = {}, 0
    end
    for _, c in ipairs(chars) do
        if c == 10 then push()
        else
            local advance = glyph(role, c)[4] * k
            if wrap and wrap > 0 and width + advance > wrap and c ~= 32 then
                -- Break at the last space of the line, as T4's wrapped text does.
                local cut
                for i = #line, 1, -1 do if line[i] == 32 then cut = i; break end end
                if cut then
                    local rest = {}
                    for i = cut + 1, #line do rest[#rest + 1] = line[i] end
                    for i = #line, cut, -1 do line[i] = nil end
                    local w = 0
                    for _, d in ipairs(line) do w = w + glyph(role, d)[4] * k end
                    width = w
                    push()
                    for _, d in ipairs(rest) do line[#line + 1] = d; width = width + glyph(role, d)[4] * k end
                else push() end
            end
            line[#line + 1] = c
            width = width + advance
        end
    end
    push()
    return lines, k
end

local function image(parent)
    local e = LUI.UIImage.new()
    e:setLeftRight(true, false, 0, 0)
    e:setTopBottom(true, false, 0, 0)
    parent:addElement(e)
    return e
end

-- Item text: T4 Item_SetTextExtents alignment, shadow under the glyphs.
function M.drawText(item, text, color)
    local d = item.data
    local holder = item.textHolder
    local rect = item.rect
    local role = M.fontRole(d.textfont, d.textscale)
    local wrap = d.autowrapped and rect[3] or nil
    local lines, k = M.measure(text, role, d.textscale, wrap)
    local height = 48 * d.textscale
    local alignX, alignY = d.textalign % 4, d.textalign - d.textalign % 4
    local shadow = 0
    if d.textstyle == 3 then shadow = 1 elseif d.textstyle == 6 then shadow = 2 end
    shadow = shadow * 480 / screenHeight()
    local used = 0
    for n, line in ipairs(lines) do
        local x = rect[1] + d.textalignx
        if alignX == 1 then x = rect[1] + (rect[3] - line.width) / 2 + d.textalignx
        elseif alignX == 2 then x = rect[1] + rect[3] - line.width + d.textalignx end
        local y = rect[2] + d.textaligny
        if alignY == 4 then y = rect[2] + height + d.textaligny
        elseif alignY == 8 then y = rect[2] + (rect[4] + height) / 2 + d.textaligny
        elseif alignY == 12 then y = rect[2] + rect[4] + d.textaligny end
        y = y + (n - 1) * height
        local cursor = x
        for _, c in ipairs(line.chars) do
            local g = glyph(role, c)
            used = used + 1
            local part = item.glyphs[used]
            if not part then
                part = {shadow = image(holder), ink = image(holder)}
                part.shadow:setRGB(0, 0, 0)
                item.glyphs[used] = part
            end
            if g.material ~= "" then
                local material = RegisterMaterial(g.material)
                local gx, gy, gw, gh = cursor + g[2] * k, y + g[3] * k, g[5] * k, g[6] * k
                part.ink:setImage(material)
                place(part.ink, gx, gy, gw, gh, item.ha, item.va)
                part.ink:setRGB(color[1], color[2], color[3])
                part.ink:setAlpha(color[4])
                if shadow > 0 then
                    part.shadow:setImage(material)
                    place(part.shadow, gx + shadow, gy + shadow, gw, gh, item.ha, item.va)
                    part.shadow:setAlpha(color[4])
                else part.shadow:setAlpha(0) end
            else
                part.ink:setAlpha(0)
                part.shadow:setAlpha(0)
            end
            cursor = cursor + g[4] * k
        end
    end
    for i = used + 1, #item.glyphs do
        item.glyphs[i].ink:setAlpha(0)
        item.glyphs[i].shadow:setAlpha(0)
    end
end

local function localize(text)
    text = tostring(text or "")
    if string.sub(text, 1, 1) == "@" then
        local key = string.sub(text, 2)
        local value = Engine.Localize("WAW2BO2_" .. key)
        if value and value ~= "" and value ~= "WAW2BO2_" .. key then return value end
        return key
    end
    return text
end

-- ------------------------------------------------------------ menus

local function readItem(menuName, index)
    local key = "item:" .. menuName .. ":" .. index
    local function col(name) return UIExpression.TableLookup(nil, TABLE, 0, key, ITEM[name]) or "" end
    local d = {
        rect = numbers(col("rect")), style = tonumber(col("style")) or 0, background = col("background"),
        forecolor = numbers(col("forecolor")), backcolor = numbers(col("backcolor")), text = col("text"),
        textfont = tonumber(col("textfont")) or 0, textscale = tonumber(col("textscale")) or 0.55,
        textstyle = tonumber(col("textstyle")) or 0, textalign = tonumber(col("textalign")) or 0,
        textalignx = tonumber(col("textalignx")) or 0, textaligny = tonumber(col("textaligny")) or 0,
        visible = col("visible"), type = tonumber(col("type")) or 0, name = string.lower(col("name")),
        onfocus = col("onfocus"), leavefocus = col("leavefocus"), action = col("action"),
        decoration = col("decoration") == "1", origin = numbers(col("origin")), autowrapped = col("autowrapped") == "1",
        exp = {},
    }
    for _, entry in ipairs(split(col("exp"), "|")) do
        local i = string.find(entry, "=", 1, true)
        if i then d.exp[string.sub(entry, 1, i - 1)] = string.sub(entry, i + 1) end
    end
    if #d.forecolor < 4 then d.forecolor = {1, 1, 1, 1} end
    if #d.backcolor < 4 then d.backcolor = {0, 0, 0, 0} end
    return d
end
local definitions = {}
function M.definition(name)
    local d = definitions[name]
    if d then return d end
    local key = "menu:" .. name
    local count = tonumber(cell(key, MENU.items))
    if not count then return nil end
    d = {name = name, onopen = cell(key, MENU.onopen), onclose = cell(key, MENU.onclose), onesc = cell(key, MENU.onesc),
        blurworld = tonumber(cell(key, MENU.blurworld)) or 0, visible = cell(key, MENU.visible),
        focuscolor = numbers(cell(key, MENU.focuscolor)), items = {}}
    if #d.focuscolor < 4 then d.focuscolor = {1, 1, 1, 1} end
    for i = 1, count do d.items[i] = readItem(name, i) end
    definitions[name] = d
    return d
end

local function focusable(item)
    return item.data.type == 1 and not item.data.decoration and item.shown
end

function M.refreshItem(host, layer, item)
    local d = item.data
    local shown = truth(M.eval(host, d.visible)) and not host.hidden[d.name]
    item.shown = shown
    local rect = {d.rect[1] or 0, d.rect[2] or 0, d.rect[3] or 0, d.rect[4] or 0}
    if d.origin[1] then rect[1] = rect[1] + d.origin[1]; rect[2] = rect[2] + d.origin[2] end
    for axis, i in pairs({x = 1, y = 2, w = 3, h = 4}) do
        if d.exp["rect " .. axis] then rect[i] = number(M.eval(host, d.exp["rect " .. axis])) end
    end
    item.rect = rect
    item.ha, item.va = d.rect[5] or 0, d.rect[6] or 0
    item.root:setAlpha(shown and 1 or 0)
    if not shown then return end
    if item.fill then
        local material = d.background
        if d.exp.material then material = tostring(M.eval(host, d.exp.material)) end
        local color = d.forecolor
        if d.style == 1 then material, color = "white", d.backcolor end
        if material ~= item.material then
            item.material = material
            -- wawmenu.stage_assets names the source materials waw_menu/<name>.
            if material ~= "" then item.fill:setImage(RegisterMaterial("waw_menu/" .. material)) end
        end
        place(item.fill, rect[1], rect[2], rect[3], rect[4], item.ha, item.va)
        item.fill:setRGB(color[1], color[2], color[3])
        item.fill:setAlpha(material ~= "" and color[4] or 0)
    end
    if item.hit then place(item.hit, rect[1], rect[2], rect[3], rect[4], item.ha, item.va) end
    local text = d.text
    if d.exp.text then text = M.eval(host, d.exp.text) end
    text = localize(text)
    local color = d.forecolor
    if layer.focus == item then color = layer.def.focuscolor end
    if text ~= "" or item.glyphs[1] then M.drawText(item, text, color) end
end

function M.refresh(host)
    for _, layer in ipairs(host.layers) do
        local shown = truth(M.eval(host, layer.def.visible))
        layer.root:setAlpha(shown and 1 or 0)
        for _, item in ipairs(layer.items) do M.refreshItem(host, layer, item) end
    end
end

local function findLayer(host, name)
    for i, layer in ipairs(host.layers) do
        if layer.def.name == name then return layer, i end
    end
end

function M.focus(host, layer, item)
    if layer.focus == item then return end
    local old = layer.focus
    layer.focus = nil
    if old then M.run(host, layer, old.data.leavefocus) end
    layer.focus = item
    if item then M.run(host, layer, item.data.onfocus) end
end

function M.activate(host, layer, item)
    if not item or not focusable(item) then return end
    M.focus(host, layer, item)
    M.run(host, layer, item.data.action)
    M.settle(host)
end

local function newItem(host, layer, d)
    local item = {data = d, glyphs = {}}
    item.root = LUI.UIElement.new()
    item.root:setLeftRight(true, true, 0, 0)
    item.root:setTopBottom(true, true, 0, 0)
    layer.root:addElement(item.root)
    if d.style == 1 or d.style == 3 then item.fill = image(item.root) end
    item.textHolder = LUI.UIElement.new()
    item.textHolder:setLeftRight(true, true, 0, 0)
    item.textHolder:setTopBottom(true, true, 0, 0)
    item.root:addElement(item.textHolder)
    if d.type == 1 and not d.decoration then
        local hit = LUI.UIElement.new()
        hit:setLeftRight(true, false, 0, 0)
        hit:setTopBottom(true, false, 0, 0)
        -- As T6's ButtonPrompt: mouse moves raise mouseenter/leave, buttons leftmouse*.
        hit:setHandleMouse(true)
        hit:setHandleMouseMove(true)
        hit:setHandleMouseButton(true)
        hit:registerEventHandler("mouseenter", function()
            if host.top == layer and item.shown then M.focus(host, layer, item); M.refresh(host) end
        end)
        hit:registerEventHandler("mouseleave", function()
            if layer.focus == item then M.focus(host, layer, nil); M.refresh(host) end
        end)
        hit:registerEventHandler("leftmousedown", function() return true end)
        hit:registerEventHandler("leftmouseup", function()
            if host.top == layer and item.shown then M.activate(host, layer, item) end
            return true
        end)
        item.root:addElement(hit)
        item.hit = hit
    end
    return item
end

function M.openLayer(host, name, modal)
    name = string.lower(name)
    local layer = findLayer(host, name)
    if layer then
        if modal then layer.modal = true; host.top = layer end
        return layer
    end
    local def = M.definition(name)
    if not def then debug("no menu " .. name); return nil end
    layer = {def = def, modal = modal, items = {}}
    layer.root = LUI.UIElement.new()
    layer.root:setLeftRight(true, true, 0, 0)
    layer.root:setTopBottom(true, true, 0, 0)
    host.canvas:addElement(layer.root)
    for _, d in ipairs(def.items) do layer.items[#layer.items + 1] = newItem(host, layer, d) end
    host.layers[#host.layers + 1] = layer
    if modal then host.top = layer end
    if def.blurworld > 0 then guarded(Engine.BlurWorld, host.controller, def.blurworld) end
    M.refresh(host)
    M.run(host, layer, def.onopen)
    debug("open " .. name)
    return layer
end

function M.closeLayer(host, name)
    local layer, index = findLayer(host, string.lower(name))
    if not layer or layer.closing then return end
    layer.closing = true
    M.run(host, layer, layer.def.onclose)
    table.remove(host.layers, index)
    layer.root:close()
    host.top = nil
    for i = #host.layers, 1, -1 do
        if host.layers[i].modal then host.top = host.layers[i]; break end
    end
    debug("close " .. layer.def.name)
end

local function firstFocus(host, layer)
    M.refresh(host)
    for _, item in ipairs(layer.items) do
        if focusable(item) then M.focus(host, layer, item); return end
    end
end

-- T4 item script commands (wawmenu.COMMAND_ARGS).
local commands = {}
commands.open = function(host, layer, a)
    local engine = cell("engine:" .. string.lower(a[1] or ""), 1)
    if engine ~= "" then host.engineMenu = engine else M.openLayer(host, a[1], true) end
end
commands.close = function(host, layer, a)
    local name = a[1] or "self"
    if string.lower(name) == "self" then name = layer.def.name end
    M.closeLayer(host, name)
end
commands.showmenu = function(host, layer, a) M.openLayer(host, a[1], false) end
commands.hidemenu = function(host, layer, a) M.closeLayer(host, a[1]) end
commands.focusfirst = function(host, layer) firstFocus(host, layer) end
commands.setfocus = function(host, layer, a)
    M.refresh(host)
    for _, item in ipairs(layer.items) do
        if item.data.name == string.lower(a[1] or "") and focusable(item) then M.focus(host, layer, item); return end
    end
end
commands.setlocalvarint = function(host, layer, a) host.vars[a[1]] = math.floor(number(a[2])) end
commands.setlocalvarbool = function(host, layer, a) host.vars[a[1]] = bool(truth(a[2])) end
commands.setlocalvarfloat = function(host, layer, a) host.vars[a[1]] = number(a[2]) end
commands.setlocalvarstring = function(host, layer, a) host.vars[a[1]] = a[2] or "" end
commands.setdvar = function(host, layer, a)
    guarded(Engine.Exec, host.controller, "set " .. a[1] .. " \"" .. (a[2] or "") .. "\"")
end
commands.exec = function(host, layer, a)
    local command = a[1] or ""
    local word = string.lower(string.match(command, "^%s*(%S+)") or "")
    if LEAVE[word] then
        host.left = true
        Engine.SetDvar("cl_paused", 0)
    end
    -- T6 RestartGamePopup: fast_restart only offline or in system link;
    -- other sessions ask the server (restart_level_zm, BO2's restart).
    if word == "fast_restart" and not session("SESSIONMODE_OFFLINE") and not session("SESSIONMODE_SYSTEMLINK") then
        guarded(Engine.SendMenuResponse, host.controller, "restartgamepopup", "restart_level_zm")
        return
    end
    guarded(Engine.Exec, host.controller, command)
end
commands.execnow = commands.exec
-- A T6 menu response reaches the server by a precached menu's index; WaW's
-- popup names are not precached, the host "class" menu is (BO2's endround
-- response then ends the game as WaW's _ingamemenus does).
commands.scriptmenuresponse = function(host, layer, a)
    guarded(Engine.SendMenuResponse, host.controller, "class", a[1] or "")
end
-- WaW hides the "save and quit" item when no save device exists (zombies never saves).
commands.nosavehide = function(host, layer, a) host.hidden[string.lower(a[1] or "")] = true end
commands.saveavailablehide = function() end
commands.profilehide = function() end
commands.play = function(host, layer, a)
    if cell("sound:" .. (a[1] or ""), 1) ~= "" then guarded(Engine.PlaySound, a[1]) end
end

function M.run(host, layer, code)
    if not code or code == "" then return end
    for _, command in ipairs(split(code, ";")) do
        local words = {}
        for w in string.gmatch(command, "%S+") do words[#words + 1] = unescape(w) end
        local name = string.lower(table.remove(words, 1) or "")
        local f = commands[name]
        if f then f(host, layer, words)
        elseif name ~= "" then debug("ignored script command " .. name) end
    end
end

-- After an event's scripts: hand off to a T6 engine menu, or resume through
-- BO2's in-game menu when no WaW menu is open any more.
function M.settle(host)
    M.refresh(host)
    if host.engineMenu then
        local menu = host.engineMenu
        host.engineMenu = nil
        guarded(Engine.BlurWorld, host.controller, 0)
        host:saveState()
        host:openMenu(menu, host.controller)
        host:close()
        return
    end
    if host.top == nil and not host.resumed then
        host.resumed = true
        guarded(Engine.BlurWorld, host.controller, 0)
        if host.left then host:close() return end
        CoD.InGameMenu.BackButtonPressed(host, {controller = host.controller, name = "button_prompt_back"})
    end
end

function M.escape(host)
    local layer = host.top
    if layer then M.run(host, layer, layer.def.onesc) end
    M.settle(host)
    return true
end

function M.gamepad(host, event)
    if event.down ~= true then return true end
    local layer = host.top
    if not layer then return true end
    local button = event.button
    if button == "up" or button == "down" then
        local list = {}
        local current = 0
        for _, item in ipairs(layer.items) do
            if focusable(item) then
                list[#list + 1] = item
                if item == layer.focus then current = #list end
            end
        end
        if #list > 0 then
            local step = button == "down" and 1 or -1
            local index = current + step
            if current == 0 then index = step > 0 and 1 or #list end
            if index < 1 then index = #list elseif index > #list then index = 1 end
            M.focus(host, layer, list[index])
            M.refresh(host)
        end
    elseif button == "primary" then
        M.activate(host, layer, layer.focus)
    elseif button == "secondary" or button == "start" or button == "select" then
        M.escape(host)
    end
    return true
end

function M.create(controller)
    local host = CoD.InGameMenu.New("class", controller, nil)
    host.controller = controller
    host.layers, host.vars, host.hidden = {}, {}, {}
    -- CoD.InGameMenu covers the safe area; T4 menus place against the full screen.
    host.canvas = LUI.UIElement.new()
    host.canvas:setLeftRight(false, false, -screenWidth() / 2, screenWidth() / 2)
    host.canvas:setTopBottom(false, false, -360, 360)
    host:addElement(host.canvas)
    host:registerEventHandler("gamepad_button", M.gamepad)
    host:registerEventHandler("button_prompt_back", M.escape)
    host:registerEventHandler("button_prompt_start", M.escape)
    host:registerEventHandler("waw_menu_tick", function(self) M.refresh(self) end)
    local timer = LUI.UITimer and guarded(LUI.UITimer.new, 250, "waw_menu_tick", false)
    if timer then host:addElement(timer) end
    M.openLayer(host, "pausedmenu", true)
    M.settle(host)
    return host
end

function M.available()
    return cell("menu:pausedmenu", MENU.items) ~= ""
end
function M.classMenu(controller)
    if CoD.WawHUD and CoD.WawHUD.active() and M.available() then return M.create(controller) end
    return M.stockClass(controller)
end
-- Wrap whichever "class" menu is defined (BO2's pause menu, T6.HUD.class).
function M.hook()
    local original = LUI.createMenu.class
    if original and original ~= M.classMenu then
        M.stockClass = original
        LUI.createMenu.class = M.classMenu
    end
end

return M
