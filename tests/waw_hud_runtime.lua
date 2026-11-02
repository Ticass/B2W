-- Live event, map isolation, glyph reuse, spectator/co-op and visibility tests.
local map = "converted"
local function element()
    local e = {children = {}, handlers = {}}
    function e:setLeftRight(a,b,l,r) self.left=l; self.right=r end
    function e:setTopBottom(a,b,t,v) self.top=t; self.bottom=v end
    function e:setAlpha(a) self.alpha=a end
    function e:setRGB(r,g,b) self.rgb={r,g,b} end
    function e:setImage(i) self.image=i end
    function e:setOwner(c) self.owner=c end
    function e:setUseGameTime(value) self.gameTime=value end
    function e:setUseStencil(value) self.stencil=value end
    function e:beginAnimation(n,t) self.animation=n end
    function e:addElement(c) self.children[#self.children+1]=c end
    function e:registerEventHandler(n,f) self.handlers[n]=f end
    function e:dispatchEventToChildren(_) end
    return e
end
local calls = {ammo=0,score=0,heat=0}
CoD = {Menu={NewSafeAreaFromState=function(name) local e=element();e.menuName=name;return e end,
        NewFromState=function(name) local e=element();e.menuName=name;return e end,close=function(e) e.closed=true end},
    SCOREBOARD_SORT_CLIENTNUM=1,
    OtherAmmoCounters={new=function() calls.heat=calls.heat+1; return 'stock heat' end},
    BIT_HUD_VISIBLE=1, BIT_UI_ACTIVE=2, BIT_IS_SCOPED=3}
LUI = {UIElement={new=element},UIImage={new=element},createMenu={
    AmmoAreaZombie=function() calls.ammo=calls.ammo+1;return 'stock ammo' end,
    CompetitiveScoreboard=function() calls.score=calls.score+1;return 'stock score' end,
    Scoreboard=function() calls.board=(calls.board or 0)+1;return 'stock board' end}}
require=function(name)
    assert(name~='T6.Zombie.AmmoAreaZombie', 'map-owned ammo module must not load in the frontend')
    if name=='T6.Waw_Menu' then dofile('src/waw2bo2/compat/waw_menu.lua') end
end
RegisterMaterial=function(m) return m end
Engine={Localize=function(k)return string.char(21)..k..string.char(21) end,IsWeaponType=function(w,t) return w==t end,
    GetScoreBoardColumnName=function(_,i) return ({'ZMUI_SCORE','MPUI_KILLS','ZMUI_DOWNS','ZMUI_REVIVES','ZMUI_HEADSHOTS'})[i+1] end,
    GetTeamPositions=function() return {{team=1}} end,
    GetMatchScoreboardClientCount=function() return #boardClients end,
    GetMatchScoreboardIndexAndClientNumForTeam=function(i) return i+10, boardClients[i+1] end,
    GetFullGamertagForScoreboardIndex=function(i) return 'player'..i end,
    GetPingForScoreboardIndex=function() return 48 end,
    GetStatusIconForClient=function() return nil end,
    GetScoreboardColumnForScoreboardIndex=function(i,c) return i*100+c end}
boardClients={2,0}
local bits = {[1]=1,[2]=0,[3]=0}
UIExpression={DvarString=function() return map end,DvarInt=function()return 0 end,IsVisibilityBitSet=function(_,b)return bits[b] end,
    TableLookup=function(_,t,k,key,col)
        if t:find('maps') then return key=='converted' and '1' or '' end
        if t:find('glyphs') then
            local row={'glyph',0,-20,10,16,32,28}
            return tostring(row[col])
        end
        if t:find('weapons') then
            if key=='COLT' and k==4 then return 'colt' end
            if key=='FLAME' and k==4 then return 'flame' end
            if key=='colt' then return ({'Magazine','8','0','COLT',''})[col] or '' end
            if key=='flame' then return ({'None','100','1','FLAME','','','hud_icon_flame'})[col] or '' end
        end
        return ''
    end}
dofile('src/waw2bo2/compat/waw_hud.lua')
assert(LUI.createMenu.AmmoAreaZombie(0)=='stock ammo') -- no eager ammo hook
calls.ammo=0
dofile('src/waw2bo2/compat/waw_ammoarea.lua')
dofile('src/waw2bo2/compat/waw_scoreboard.lua')
dofile('src/waw2bo2/compat/waw_otherammo.lua')
CoD.WawHUD.hookScoreboard()
local ammo=LUI.createMenu.AmmoAreaZombie(0)
assert(ammo.menuName=='AmmoAreaZombie') -- native engine routes events by menu name
assert(calls.ammo==0)
local function event(n,e) e.controller=0;ammo.handlers[n](ammo,e) end
event('hud_update_weapon_select',{weapon='colt',weaponDisplayName='COLT'})
assert(ammo.weaponText.text=='COLT')
event('hud_update_ammo',{ammoInClip=8,ammoStock=32})
assert(ammo.alpha==1 and ammo.stockText.text=='32' and #ammo.bullets==8)
event('hud_update_ammo',{ammoInClip=3,ammoStock=32})
assert(ammo.bullets[3].rgb[1]==1 and ammo.bullets[4].rgb[1]==0.3)
event('hud_update_ammo',{ammoInClip=8,ammoStock=27})
assert(#ammo.bullets==8 and ammo.bullets[8].rgb[1]==1 and ammo.stockText.text=='27')
event('hud_update_weapon_select',{weapon='flame',weaponDisplayName='FLAME'})
event('hud_update_overheat',{heatPercent=75,overheat=false})
assert(ammo.stockText.text=='' and ammo.bullets[1].alpha==0 and ammo.heat.alpha==1)
assert(ammo.heat.icon.image=='hud_icon_flame' and ammo.heat.fill.right==90 and ammo.heat.flash.alpha==0)
event('hud_update_overheat',{heatPercent=100,overheat=true})
assert(ammo.heat.flash.animation=='flash' and ammo.heat.fill.right==120)
event('hud_update_overheat',{heatPercent=40,overheat=false})
assert(ammo.heat.flash.alpha==0 and ammo.heat.fill.right==48)
event('hud_update_weapon_select',{weapon='colt',weaponDisplayName='COLT'})
event('hud_update_ammo',{ammoInClip=8,ammoStock=27})
assert(not ammo.overheat and ammo.bullets[1].alpha>0 and ammo.heat.alpha==0)
event('hud_update_offhand',{lethal={material='frag',ammo=2}})
bits[2]=1;event('hud_update_bit_2',{});assert(ammo.alpha==0)
bits[2]=0;event('hud_update_refresh',{});assert(ammo.alpha==1)
local scores=LUI.createMenu.CompetitiveScoreboard(0)
assert(scores.menuName=='CompetitiveScoreboard')
local update=scores.handlers.hud_update_competitive_scoreboard
update(scores,{competitivescores={{clientNum=2,score=750},{clientNum=0,score=500}}})
assert(scores.rows[1].text.text=='500' and scores.rows[2].text.text=='750')
assert(scores.rows[2].bg.image=='waw_hud/scorebar_zom_3')
update(scores,{competitivescores={{clientNum=2,score=900}}})
assert(scores.rows[1].text.text=='900' and scores.rows[2].alpha==0)
local label=CoD.WawHUD.label(element(),'score',20,0,0,false)
label:setText(string.char(20)..'^7Ray Gun'..string.char(21))
assert(label.text=='Ray Gun' and #label.parts==7)
label:setText('?Ray Gun?');assert(label.text=='?Ray Gun?')
label:setText('Panzerschreck');assert(label.text=='Panzerschreck')
label:setText('12345'); local children=#label.children
label:setText('7');label:setText('43210');assert(#label.children==children)
local board=LUI.createMenu.Scoreboard(0)
assert(board.menuName=='Scoreboard' and #board.rows==2)
-- client number order; Points = BO2 score column 0, Headshots column 4
assert(board.rows[1].cells[3].text=='player11' and board.rows[1].cells[5].text=='1100')
assert(board.rows[1].cells[10].text=='48' and board.rows[2].cells[3].text=='player10')
assert(board.rows[2].bg.image=='waw_hud/scorebar_zom_long_3' and board.rows[2].cells[3].rgb[1]==0.965)
boardClients={0};board.handlers.update_scoreboard(board,{});assert(board.rows[2].alpha==0)
board.handlers.close_scoreboard_menu(board,{});assert(board.closed)
map='zm_transit'
assert(LUI.createMenu.Scoreboard(0)=='stock board')
assert(LUI.createMenu.AmmoAreaZombie(0)=='stock ammo')
assert(LUI.createMenu.CompetitiveScoreboard(0)=='stock score')
assert(CoD.OtherAmmoCounters.new()=='stock heat')
assert(calls.ammo==1 and calls.score==1 and calls.heat==1)
-- The actual stock HUD chooses DPadArea on TranZit, not AmmoAreaZombie.
-- Hook the first snapshot so neither native zombie assembly can survive.
local nativeSnapshots=0
HUD_FirstSnapshot_Zombie=function() nativeSnapshots=nativeSnapshots+1 end
dofile('src/waw2bo2/compat/waw_hud_entry.lua')
map='converted'
local root=element()
HUD_FirstSnapshot_Zombie(root,{controller=0})
assert(nativeSnapshots==0 and #root.children==2)
assert(root.children[1].menuName=='AmmoAreaZombie' and root.children[2].menuName=='CompetitiveScoreboard')
map='zm_transit'
HUD_FirstSnapshot_Zombie(element(),{controller=0})
assert(nativeSnapshots==1)
