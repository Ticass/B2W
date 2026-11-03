-- WaW pause menu interpreter against a translated fixture (MENU_ROWS from test_wawmenu.py).
local log = {exec = {}, responses = {}, back = 0, opened = nil}
local function element()
    local e = {children = {}, handlers = {}, alpha = 1}
    function e:setLeftRight(a, b, l, r) self.h = {a, b, l, r} end
    function e:setTopBottom(a, b, t, v) self.v = {a, b, t, v} end
    function e:setAlpha(a) self.alpha = a end
    function e:setRGB(r, g, b) self.rgb = {r, g, b} end
    function e:setImage(i) self.image = i end
    function e:setHandleMouse(v) self.mouse = v end
    function e:setHandleMouseMove(v) self.mouseMove = v end
    function e:setHandleMouseButton(v) self.mouseButton = v end
    function e:addElement(c) self.children[#self.children + 1] = c; c.parent = self end
    function e:registerEventHandler(n, f) self.handlers[n] = f end
    function e:close() self.closed = true end
    return e
end
CoD = {SESSIONMODE_OFFLINE = 1, SESSIONMODE_SYSTEMLINK = 2,
    WawHUD = {active = function() return true end},
    InGameMenu = {New = function(name, controller)
        local e = element(); e.menuName = name
        function e:saveState() self.saved = true end
        function e:openMenu(menu) log.opened = menu end
        return e
    end, BackButtonPressed = function(host) log.back = log.back + 1; host:close() end}}
LUI = {UIElement = {new = element}, UIImage = {new = element},
    createMenu = {class = function() return "stock class" end}}
RegisterMaterial = function(m) return m end
Engine = {Localize = function(k) return "text " .. k end, Exec = function(_, c) log.exec[#log.exec + 1] = c end,
    SetDvar = function(n, v) log[n] = v end, SessionModeIsMode = function(m) return m == 1 end,
    GetAspectRatio = function() return 16 / 9 end, BlurWorld = function(_, v) log.blur = v end,
    SendMenuResponse = function(_, m, r) log.responses[#log.responses + 1] = m .. ":" .. r end}
local dvars = {sv_running = "1", r_mode = "1920x1080"}
UIExpression = {DvarString = function(_, n) return dvars[n] or "" end, DvarInt = function() return 0 end,
    TableLookup = function(_, t, _, key, col)
        if string.find(t, "glyphs") then return tostring(({"glyph", 0, -20, 10, 16, 32, 30})[col]) end
        local row = MENU_ROWS[key]
        if not row then return "" end
        return row[col] or ""
    end}

dofile("src/waw2bo2/compat/waw_menu.lua")
local M = CoD.WawMenu
M.hook()
-- T4 alignment: SUBLEFT is the centred 4:3 frame, RIGHT anchors the right edge.
local la, ra, l, r = M.horizontal(-107, 854, 0)
assert(la == false and ra == false and l == -640.5 and r == 640.5)
la, ra, l = M.horizontal(-267, 224, 3)
assert(la == false and ra == true and l == -400.5)
assert(M.fontRole(0, 0.5476, 1080) == "extrabig" and M.fontRole(0, 0.5476, 480) == "big")
assert(M.fontRole(1, 0.1, 1080) == "normal" and M.fontRole(4, 0.2, 480) == "small")

local host = LUI.createMenu.class(0)
assert(host ~= "stock class" and host.menuName == "class")
assert(#host.layers == 2 and host.layers[1].def.name == "pausedmenu" and host.layers[2].def.name == "overheadmap")
local pause = host.layers[1]
assert(host.top == pause and log.blur == 4.8)
-- focusfirst ran the first visible button's onFocus
assert(pause.focus == pause.items[2] and host.vars.ui_highlight == 1)
-- the source map name drives the map image
assert(host.layers[2].items[1].material == "menu_map_nazi_zombie_factory")
assert(host.layers[2].items[1].fill.image == "waw_menu/menu_map_nazi_zombie_factory")
-- the canvas spans the full screen, not BO2's safe area
assert(host.canvas.h[3] == -640 and host.canvas.v[4] == 360)
-- focused text takes the menu focusColor, others forecolor
local ink = pause.items[2].glyphs[1].ink
assert(ink.rgb[1] == 1 and ink.rgb[2] == 0.5)
-- buttons take mouse moves and clicks; hovering focuses, clicking activates
local resumeHit = pause.items[2].hit
assert(resumeHit.mouseMove and resumeHit.mouseButton)
pause.items[3].hit.handlers.mouseenter()
assert(pause.focus == pause.items[3])
pause.items[3].hit.handlers.mouseleave()
assert(pause.focus == nil)
pause.items[2].hit.handlers.mouseenter()
-- keyboard navigation wraps over visible buttons
M.gamepad(host, {down = true, button = "up"})
assert(pause.focus == pause.items[4])
M.gamepad(host, {down = true, button = "down"})
assert(pause.focus == pause.items[2])

-- QUIT (solo host) opens the WaW popup; Esc there returns to the pause menu
M.activate(host, pause, pause.items[3])
assert(host.top.def.name == "popup_nosave_quit" and log.back == 0)
assert(#host.layers == 1) -- pausedmenu's onClose hid the map
M.escape(host)
assert(host.top.def.name == "pausedmenu" and #host.layers == 2 and log.back == 0)
-- popup QUIT disconnects after clearing the pause, like T6's own quit
pause = host.top
M.activate(host, pause, pause.items[3])
local popup = host.top
M.activate(host, popup, popup.items[3])
assert(log.exec[#log.exec] == "disconnect" and log.cl_paused == 0)
assert(log.exec[#log.exec - 1] == 'set ui_skipMainLockout "1"')
assert(host.closed and log.back == 0)

-- RESUME goes through BO2's in-game back path (unpause, ingame_menu_closed)
host = LUI.createMenu.class(0)
M.activate(host, host.top, host.top.items[2])
assert(log.back == 1 and host.top == nil and log.blur == 0)

-- OPTIONS opens T6's options menu (engine glue)
host = LUI.createMenu.class(0)
M.activate(host, host.top, host.top.items[4])
assert(log.opened == "OptionsMenu" and host.saved and host.closed)

-- WaW's restart and quit in an online session: T6's restart response, and
-- script responses under the precached host menu
host = LUI.createMenu.class(0)
Engine.SessionModeIsMode = function() return false end
local execs = #log.exec
M.run(host, host.top, "exec fast_restart")
assert(#log.exec == execs and log.responses[#log.responses] == "restartgamepopup:restart_level_zm")
M.run(host, host.top, "scriptMenuResponse endround")
assert(log.responses[#log.responses] == "class:endround")
Engine.SessionModeIsMode = function(m) return m == 1 end
M.run(host, host.top, "exec fast_restart")
assert(log.exec[#log.exec] == "fast_restart")

-- not a converted map: BO2's menu
CoD.WawHUD.active = function() return false end
assert(LUI.createMenu.class(0) == "stock class")
print("waw menu runtime ok")
