// Port of gscport.py's generated scripts and BO2 template hooks (see generators.h).
#include "generators.h"

#include "api.h"
#include "core/encoding.h"
#include "core/files.h"
#include "core/pyre.h"
#include "rewrites.h"
#include "statements.h"
#include "translator.h"
#include "support.h"
#include "zombiebehavior.h"

#include <algorithm>
#include <stdexcept>

namespace w2b::gsc::port
{
    namespace fs = std::filesystem;

    namespace
    {
        std::string S(const py::Json& value)
        {
            return value.get<std::string>();
        }

        std::string Dumps(const std::string& text)
        {
            return py::JsonDumps(py::Json(text), 2);
        }

        size_t ByteOffset(const std::string& text, const long long cp)
        {
            size_t i = 0;
            for (long long n = 0; n < cp && i < text.size(); n++)
            {
                const auto c = static_cast<unsigned char>(text[i]);
                i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            }
            return i;
        }

        size_t Count(const std::string& text, const std::string& needle)
        {
            size_t n = 0;
            for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
                n++;
            return n;
        }

        std::string ReplaceFirst(const std::string& text, const std::string& from, const std::string& to)
        {
            const auto at = text.find(from);
            if (at == std::string::npos)
                return text;
            return text.substr(0, at) + to + text.substr(at + from.size());
        }

        std::string Posix(std::string path)
        {
            return Replace(std::move(path), "\\", "/");
        }

        std::string StripText(const Tokens& tokens, const long long a, const long long b)
        {
            return PyStrip(EmitRange(tokens, a, b));
        }

        std::string LowJoin(const Tokens& tokens, const long long a, const long long b)
        {
            std::string text;
            const auto [lo, hi] = Slice(tokens.size(), a, b);
            for (size_t i = lo; i < hi; i++)
                text += tokens[i].Low();
            return text;
        }
    } // namespace

    std::string ReadTextReplace(const fs::path& path)
    {
        std::string bytes;
        if (!files::ReadBytes(path, bytes))
            throw std::runtime_error("cannot read " + files::Utf8(path));
        return encoding::UniversalNewlines(encoding::Utf8Replace(bytes));
    }

    const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>& WeaponRegistration()
    {
        static const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> registration{
            {"include", {{"maps\\dlc3_code", "include_weapons"}}},
            {"add", {{"maps\\_zombiemode_weapons", "init_weapons"}}},
        };
        return registration;
    }

    std::string AssetsSource(const std::map<std::string, std::string>& fxTable, const std::optional<std::string>& wawMap,
                             const std::map<std::string, std::string>& weapons, const std::set<std::string>& boltedLoops,
                             const std::map<std::string, std::string>& zombieVarOverrides, const std::map<std::string, std::string>& tables)
    {
        std::vector<std::string> lines{"// waw2bo2: WaW asset name -> BO2 asset name for converted assets (generated).", "init()", "{"};
        lines.push_back("    level.waw2bo2_tables = [];");
        for (const auto& [waw, staged] : tables)
            lines.push_back("    level.waw2bo2_tables[" + Dumps(waw) + "] = " + Dumps(staged) + ";");
        if (wawMap && !wawMap->empty())
            lines.push_back("    level.waw_script = \"" + *wawMap + "\";");
        lines.push_back("    level.waw_zombie_var_overrides = [];");
        for (const auto& [name, value] : zombieVarOverrides)
            lines.push_back("    level.waw_zombie_var_overrides[" + Dumps(name) + "] = " + Dumps(value) + ";");
        for (const auto& [waw, bo2] : fxTable)
            lines.push_back("    level.waw2bo2_fx[\"" + waw + "\"] = \"" + bo2 + "\";");
        std::set<std::string> effects;
        for (const auto& [_, bo2] : fxTable)
            effects.insert(bo2);
        for (const auto& bo2 : effects)
            lines.push_back("    loadfx( \"" + bo2 + "\" );");
        if (!boltedLoops.empty())
        {
            lines.push_back("    level.waw2bo2_bolted_loops = [];");
            for (const auto& name : boltedLoops)
                lines.push_back("    level.waw2bo2_bolted_loops[loadfx( \"" + name + "\" )] = true;");
        }
        std::set<std::string> reverse;
        for (const auto& [waw, bo2] : weapons)
        {
            lines.push_back("    level.waw2bo2_weapons[\"" + waw + "\"] = \"" + bo2 + "\";");
            if (!bo2.empty() && bo2 != waw && !reverse.count(bo2))
            {
                lines.push_back("    level.waw2bo2_weapon_names[\"" + bo2 + "\"] = \"" + waw + "\";");
                reverse.insert(bo2);
            }
        }
        lines.push_back("}");
        lines.push_back("");
        return Join(lines, "\n");
    }

    std::string Bo2WeaponsSource(const std::set<std::string>& weapons)
    {
        std::vector<std::string> lines{"// waw2bo2: BO2 weapons loaded with the map (generated by build-mod).", "init()", "{"};
        std::set<std::string> lowered;
        for (const auto& w : weapons)
            lowered.insert(Lower(w));
        for (const auto& name : lowered)
            lines.push_back("    level.waw2bo2_bo2_weapons[\"" + name + "\"] = 1;");
        lines.push_back("}");
        lines.push_back("");
        return Join(lines, "\n");
    }

    bool HasWawAppearance(Sources& sources)
    {
        const auto script = sources.Get(LOADOUT);
        return script && script->Find("init_loadout") && script->Find("give_model");
    }

    std::string CharactersSource()
    {
        const auto ported = PortedPath(LOADOUT);
        const std::vector<std::string> lines{
            "// waw2bo2: player bodies and viewmodel arms come from the map's WaW _loadout (generated).",
            "#include common_scripts\\utility;",
            "#include maps\\mp\\_utility;",
            "#include maps\\mp\\zombies\\_zm_utility;",
            "",
            "precache()",
            "{",
            std::string("    ") + COMPAT + "::init();",
            "    level.is_zombie_level = 1;",
            "    " + ported + "::init_loadout();",
            "    replacefunc( " + S(Api()["player_loadout_glue"]["native_start_weapon"]) + ", ::give_start_weapon );",
            "    // WaW's _loadout names the start and last-stand weapons; BO2's",
            "    // _zm::init_levelvars sets its own defaults right after this hook",
            "    level thread waw_loadout_weapons( level.player_switchweapon, level.laststandpistol );",
            "}",
            "",
            "// WaW _loadout::give_loadout grants with GiveWeapon only. T6's",
            "// extra GiveStartAmmo adds a full clip to the source reserve.",
            "give_start_weapon( switch_to_weapon )",
            "{",
            "    self giveweapon( level.start_weapon );",
            "    if ( isdefined( switch_to_weapon ) && switch_to_weapon )",
            "        self switchtoweapon( level.start_weapon );",
            "}",
            "",
            "waw_loadout_weapons( start, pistol )",
            "{",
            "    waittillframeend;",
            std::string("    start = ") + COMPAT + "::waw_weapon( start );",
            "    if ( isdefined( start ) )",
            "        level.start_weapon = start;",
            std::string("    pistol = ") + COMPAT + "::waw_weapon( pistol );",
            "    if ( !isdefined( pistol ) )",
            "        return;",
            "    level.laststandpistol = pistol;",
            "    level.default_laststandpistol = pistol;",
            "    // WaW has no solo last stand with an upgraded pistol; keep the same one",
            "    level.default_solo_laststandpistol = pistol;",
            "    // BO2's solo swap ranks the player's pistols against BO2's list (a WaW",
            "    // pistol ranks 0), marks the player as having no pistol and hands over",
            "    // level.laststandpistol, taken again on revive. That is the WaW start",
            "    // pistol here, so a solo revive took the player's own gun. WaW gives",
            "    // back exactly the weapons held when downed: disable the swap.",
            "    while ( !isdefined( level.pistol_value_solo_replace_below ) )",
            "        wait 0.05;",
            "    level.pistol_value_solo_replace_below = -1;",
            "}",
            "",
            "give()",
            "{",
            "    self detachall();",
            "    self.entity_num = self getentitynumber();",
            "    if ( !isdefined( self.characterindex ) )",
            "        self.characterindex = self.entity_num;",
            "    self.favorite_wall_weapons_list = [];",
            "    self.talks_in_danger = 0;",
            "    if ( !isdefined( self.pers[\"class\"] ) )",
            "        self.pers[\"class\"] = \"closequarters\";",
            "    self " + ported + "::give_model( self.pers[\"class\"] );",
            "    self set_player_is_female( 0 );",
            "    self setmovespeedscale( 1 );",
            "    // BO2 zombies players get no sprint unless the character hook sets it",
            "    // (stock maps: 4 s, no cooldown); WaW's player_sprintTime default is 4 s",
            "    self setsprintduration( 4 );",
            "    self setsprintcooldown( 0 );",
            "    self set_player_tombstone_index();",
            "}",
            "",
        };
        return Join(lines, "\n");
    }

    namespace
    {
        bool PowerRelay(const Script& script, const Function& fn)
        {
            bool notifies = false, onlyWaits = true;
            for (const auto& [a, b] : TopLevelStatements(script.tokens, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose)))
            {
                const auto text = LowJoin(script.tokens, a, b);
                notifies = notifies || StartsWith(text, "levelnotify(");
                onlyWaits = onlyWaits
                            && (StartsWith(text, "flag_wait(") || StartsWith(text, "wait") || StartsWith(text, "levelwaittill(") || StartsWith(text, "levelnotify("));
            }
            return notifies && onlyWaits;
        }
    } // namespace

    std::optional<std::pair<std::string, std::set<std::string>>> WawPackAPunchSource(const std::string& source, const std::string& path)
    {
        const auto script = Parse(source, path);
        const Function* init = script.Find("init");
        if (!init)
            return std::nullopt;
        const auto& toks = script.tokens;
        const auto own = Norm(path);
        const auto callees = [&](const long long start, const long long end)
        {
            std::set<std::string> names;
            for (const auto& r : References(toks, static_cast<size_t>(start), static_cast<size_t>(end)))
                if ((!r.qualifier || Norm(*r.qualifier) == own) && script.Find(Lower(r.name)))
                    names.insert(Lower(r.name));
            return names;
        };
        std::vector<Span> kept;
        for (const auto& [start, end] : TopLevelStatements(toks, static_cast<long long>(init->bodyOpen), static_cast<long long>(init->bodyClose)))
        {
            const auto text = LowJoin(toks, start, end);
            std::vector<const Function*> relays;
            for (const auto& n : callees(start, end))
                if (n != "init")
                    relays.push_back(script.Find(n));
            bool marker = false;
            for (const char* m : {"upgrade", "packapunch", "pack_a_punch", "knuckle"})
                marker = marker || Contains(text, m);
            bool allRelays = !relays.empty();
            for (const auto* fn : relays)
                allRelays = allRelays && PowerRelay(script, *fn);
            if (marker || (allRelays && Contains(text, "thread")))
                kept.emplace_back(start, end);
        }
        if (kept.empty())
            return std::nullopt;
        std::set<std::string> reached;
        std::vector<std::string> queue;
        for (const auto& [a, b] : kept)
            for (const auto& n : callees(a, b))
                queue.push_back(n);
        while (!queue.empty())
        {
            const auto name = queue.back();
            queue.pop_back();
            if (reached.count(name) || name == "init")
                continue;
            reached.insert(name);
            const Function* fn = script.Find(name);
            for (const auto& n : callees(static_cast<long long>(fn->bodyOpen), static_cast<long long>(fn->bodyClose)))
                queue.push_back(n);
        }
        std::vector<std::string> directives;
        for (const auto& t : toks)
            if (t.kind == Kind::Directive)
                directives.push_back(t.text);
        std::string initBody;
        for (const auto& [a, b] : kept)
            initBody += EmitRange(toks, a, b);
        std::vector<std::string> parts{Join(directives, "\n"), "init()\n{" + initBody + "\n}"};
        std::vector<const Function*> functions;
        for (const auto& n : reached)
            functions.push_back(script.Find(n));
        std::stable_sort(functions.begin(), functions.end(), [](const Function* a, const Function* b) { return a->start < b->start; });
        for (const auto* fn : functions)
            parts.push_back(StripText(toks, static_cast<long long>(fn->start), static_cast<long long>(fn->bodyClose) + 1));
        const auto header = "// waw2bo2: BO2 stock perks; the WaW Pack-a-Punch and power relays of " + path + " stay.\n";
        reached.insert("init");
        return std::make_pair(header + Join(parts, "\n\n") + "\n", reached);
    }

    std::string TeslaCallbackSource(const std::map<std::string, std::string>& entries)
    {
        const auto& definition = Api()["tesla_runtime"];
        static constexpr std::string_view TEMPLATE = R"GSC(
// WaW's source chain owns Tesla kills; the projectile only starts the chain.
waw_tesla_init()
{{
    {initialize}();
    // Wunderwaffe limit requested by the user (WaW's script says 20).
    if ( isdefined( level.zombie_vars["tesla_max_enemies_killed"] ) )
        level.zombie_vars["tesla_max_enemies_killed"] = min( {max_targets}, level.zombie_vars["tesla_max_enemies_killed"] );
    // A Tesla kill never drops through the ordinary path; WaW's only Waffe
    // drop needs tesla_kills_for_powerup hits in one chain. At the user's
    // request it drops when a shot reaches the cap (WaW's 15 is unreachable).
    if ( isdefined( level.zombie_vars["tesla_kills_for_powerup"] ) && isdefined( level.zombie_vars["tesla_max_enemies_killed"] ) )
        level.zombie_vars["tesla_kills_for_powerup"] = min( level.zombie_vars["tesla_kills_for_powerup"], level.zombie_vars["tesla_max_enemies_killed"] );
    {native_register}( ::waw_tesla_damage );
    // The source actor dispatcher must also use this engine adapter: its
    // listener ends on the target's death, while the shooter's chain must live.
    level.waw_actor_damage_filter = ::waw_tesla_damage;
}}

waw_tesla_damage( mod, hit_location, hit_origin, player, amount )
{{
    if ( !isdefined( player ) || !isplayer( player ) || !self {damage_predicate}( mod ) )
        return false;
    // Reserve synchronously: T6 can dispatch direct and splash callbacks in
    // the same frame before a newly threaded source chain starts running.
    if ( isdefined( player.waw_tesla_chain_active ) && player.waw_tesla_chain_active )
        return true;
    if ( isdefined( self.zombie_tesla_hit ) && self.zombie_tesla_hit )
        return true;
    player.waw_tesla_chain_active = true;
    if ( !isdefined( player.tesla_network_death_choke ) )
        player.tesla_network_death_choke = 0;
    if ( !isdefined( player.waw_tesla_choke_bound ) )
    {{
        player.waw_tesla_choke_bound = true;
        player thread waw_tesla_network_choke();
    }}
    // The shooter owns the thread. The target may die during the source
    // traversal; its damage-listener lifetime must not terminate the chain.
    player thread waw_tesla_run_chain( self, hit_location, hit_origin );
    return true;
}}

waw_tesla_run_chain( target, hit_location, hit_origin )
{{
    self endon( "disconnect" );
    if ( isdefined( target ) && isalive( target ) )
        target {damage_entry}( hit_location, hit_origin, self );
    self.waw_tesla_chain_active = false;
}}

waw_tesla_network_choke()
{{
    self endon( "disconnect" );
    for ( ;; )
    {{
        wait_network_frame();
        wait_network_frame();
        self.tesla_network_death_choke = 0;
    }}
}}
)GSC";
        const auto& maxTargets = definition["max_targets"];
        const auto targets = maxTargets.is_string() ? maxTargets.get<std::string>()
                                                    : (maxTargets.is_number_float() ? py::FloatRepr(maxTargets.get<double>()) : maxTargets.dump());
        return zombiebehavior::Format(TEMPLATE, {{"initialize", entries.at("initialize")},
                                                 {"max_targets", targets},
                                                 {"native_register", S(definition["native_register"])},
                                                 {"damage_predicate", entries.at("damage_predicate")},
                                                 {"damage_entry", entries.at("damage_entry")}});
    }

    std::string WeaponsSource(const Registration& registration, const std::vector<std::string>& melee)
    {
        const auto role = [&](const std::string& name)
        {
            std::vector<std::string> functions;
            for (const auto& [r, fns] : registration)
                if (r == name)
                    for (const auto& fn : fns)
                        if (fn && !fn->empty())
                            functions.push_back(*fn);
            return functions;
        };
        std::vector<std::string> granted(std::begin(BO2_FRAMEWORK_WEAPONS), std::end(BO2_FRAMEWORK_WEAPONS));
        granted.insert(granted.end(), melee.begin(), melee.end());
        std::vector<std::string> lines{"// waw2bo2: the map's WaW weapon registration on BO2 _zm_weapons (generated).",
                                       "// include/add_zombie_weapon calls go through _waw2bo2_compat (WaW -> BO2 arguments).",
                                       "",
                                       "include_weapons()",
                                       "{",
                                       "    // WaW hints are plain text: prompt = display name + cost (see waw_add_zombie_weapon)",
                                       "    level.monolingustic_prompt_format = 1;"};
        for (const auto& w : granted)
            lines.push_back("    maps\\mp\\zombies\\_zm_weapons::include_zombie_weapon( \"" + w + "\", 0 );");
        for (const auto& fn : role("include"))
            lines.push_back(std::string("    ") + CORE + "::" + fn + "();");
        for (const char* l : {"}", "", "add_weapons()", "{"})
            lines.push_back(l);
        lines.push_back("    maps\\mp\\zombies\\_zm_weapons::add_zombie_weapon( \"m1911_zm\", \"m1911_upgraded_zm\", &\"ZOMBIE_WEAPON_M1911\", 50, \"\", \"\", undefined );");
        for (const auto& w : granted)
            lines.push_back(std::string("    ") + COMPAT + "::waw_native_only_weapon( \"" + w + "\" );");
        for (const auto& fn : role("add"))
            lines.push_back(std::string("    ") + CORE + "::" + fn + "();");
        for (const char* l : {"}", "", "// The WaW mystery box, after BO2 _zm_magicbox::init, with independent chest flags.", "start_box()", "{"})
            lines.push_back(l);
        for (const auto& fn : role("box_state"))
            lines.push_back(std::string("    ") + CORE + "::" + fn + "();");
        for (const auto& fn : role("box"))
            lines.push_back(std::string("    ") + CORE + "::" + fn + "();");
        lines.push_back("}");
        lines.push_back("");
        return Join(lines, "\n");
    }

    bool HookBo2Box(const fs::path& mainGsc)
    {
        const auto source = ReadTextReplace(mainGsc);
        const auto call = "    level thread " + std::string(WEAPONS) + "::start_box();\n";
        if (Contains(source, call))
            return false;
        static const pyre::Regex post(R"(^    level thread maps\\mp\\waw\\\w+::waw_main_post\(\);\n)", pyre::MULTILINE);
        const auto m = post.Search(source);
        if (!m)
            throw std::runtime_error(files::Utf8(mainGsc) + ": no waw_main_post() call to start the WaW box after");
        const auto at = ByteOffset(source, m->End());
        files::WriteText(mainGsc, source.substr(0, at) + call + source.substr(at));
        return true;
    }

    bool HookBo2Weapons(const fs::path& mainGsc)
    {
        const auto source = ReadTextReplace(mainGsc);
        static const pyre::Regex include(R"(^(\s*)include_weapons\(\s*\);)", pyre::MULTILINE);
        static const pyre::Regex add(R"((level\._zombie_custom_add_weapons\s*=\s*)[^;]+;)");
        auto changed = include.Sub([](const pyre::Match& m) { return m[1] + WEAPONS + "::include_weapons();"; }, source);
        changed = add.Sub([](const pyre::Match& m) { return m[1] + WEAPONS + "::add_weapons;"; }, changed);
        if (!Contains(changed, std::string(WEAPONS) + "::include_weapons();") || !Contains(changed, std::string(WEAPONS) + "::add_weapons;"))
            throw std::runtime_error(files::Utf8(mainGsc) + ": no include_weapons() call / level._zombie_custom_add_weapons to hook");
        if (changed != source)
            files::WriteText(mainGsc, changed);
        return changed != source;
    }

    bool HookBo2ClientCharacters(const fs::path& mainCsc)
    {
        const auto source = files::ReadText(mainCsc);
        const std::string marker = "    start_zombie_stuff();";
        const std::string hook = "    // WaW player face bones must not receive T6 facial poses.\n    level._faceanimcbfunc = undefined;\n    level.face_anim_tree = undefined;\n";
        if (Contains(source, hook))
            return false;
        if (!Contains(source, marker))
            throw std::runtime_error(files::Utf8(mainCsc) + ": no start_zombie_stuff() to bind source player faces after");
        files::WriteText(mainCsc, ReplaceFirst(source, marker, marker + "\n" + hook));
        return true;
    }

    bool HookBo2Characters(const fs::path& mainGsc)
    {
        const auto source = ReadTextReplace(mainGsc);
        auto changed = source;
        for (const auto& [hook, fn] : std::vector<std::pair<std::string, std::string>>{{"precachecustomcharacters", "precache"}, {"givecustomcharacters", "give"}})
        {
            const pyre::Regex pattern(R"((level\.)" + hook + R"(\s*=\s*)[^;]+;)");
            const auto target = fn;
            changed = pattern.Sub([&](const pyre::Match& m) { return m[1] + CHARACTERS + "::" + target + ";"; }, changed);
        }
        if (!Contains(changed, CHARACTERS))
            throw std::runtime_error(files::Utf8(mainGsc) + ": no level.precachecustomcharacters / level.givecustomcharacters to hook");
        if (changed != source)
            files::WriteText(mainGsc, changed);
        return changed != source;
    }

    std::string AnimtreesSource(const std::vector<std::string>& trees)
    {
        std::vector<std::string> lines{"// waw2bo2: register the WaW animtrees the ported scripts play on script models (generated).", ""};
        for (size_t i = 0; i < trees.size(); i++)
            for (const auto& l : {"#using_animtree( \"" + trees[i] + "\" );", std::string(), "use_tree_" + std::to_string(i) + "()", std::string("{"),
                                  std::string("    scriptmodelsuseanimtree( #animtree );"), std::string("}"), std::string()})
                lines.push_back(l);
        lines.push_back("init()");
        lines.push_back("{");
        for (size_t i = 0; i < trees.size(); i++)
            lines.push_back("    use_tree_" + std::to_string(i) + "();");
        lines.push_back("}");
        lines.push_back("");
        return Join(lines, "\n");
    }

    bool HookBo2Animtrees(const fs::path& mainGsc, const fs::path& mainCsc, const fs::path& outRoot, const std::vector<std::string>& trees)
    {
        bool changed = false;
        struct Side
        {
            std::string base, anchor, ext;
            fs::path main;
        };
        const Side sides[] = {{"maps", "    maps\\mp\\zombies\\_zm::init();", "gsc", mainGsc}, {"clientscripts", "    clientscripts\\mp\\zombies\\_zm::init();", "csc", mainCsc}};
        for (const auto& side : sides)
        {
            const auto path = outRoot / side.base / "mp" / "waw" / ("_waw2bo2_animtrees." + side.ext);
            const auto call = "    " + side.base + "\\mp\\waw\\_waw2bo2_animtrees::init();\n";
            auto source = ReadTextReplace(side.main);
            if (trees.empty())
            {
                const auto updated = Replace(source, call, "");
                if (updated != source)
                {
                    files::WriteText(side.main, updated);
                    changed = true;
                }
                std::error_code error;
                fs::remove(path, error);
                continue;
            }
            files::WriteText(path, AnimtreesSource(trees));
            auto anchor = side.anchor;
            if (!Contains(source, anchor + "\n"))
                anchor = Replace(anchor, side.base + "\\mp\\zombies\\_zm", side.base + "\\mp\\waw\\_waw2bo2_zm");
            if (Contains(source, anchor + "\n" + call))
                continue;
            if (!Contains(source, anchor + "\n"))
                throw std::runtime_error(files::Utf8(side.main) + ": no '" + PyStrip(anchor) + "' line to register the WaW animtrees after");
            source = Replace(source, call, "");
            files::WriteText(side.main, ReplaceFirst(source, anchor + "\n", anchor + "\n" + call));
            changed = true;
        }
        return changed;
    }

    std::vector<std::string> DropPreservedTemplateInits(const fs::path& mainGsc, const fs::path& outRoot, const std::optional<fs::path>& mainCsc)
    {
        std::map<bool, fs::path> mains{{false, mainGsc}};
        if (mainCsc)
            mains[true] = *mainCsc;
        std::map<bool, std::string> sources;
        std::error_code error;
        for (const auto& [client, path] : mains)
            if (fs::is_regular_file(path, error))
                sources[client] = ReadTextReplace(path);
        std::vector<std::string> removed;
        const auto inits = ApiSection("preserved_template_inits");
        for (const auto& [module, lines] : inits.items())
        {
            if (module == "doc" || !fs::is_regular_file(outRoot / files::FromUtf8(Posix(PortedPath(module)) + ".gsc"), error))
                continue;
            for (const auto& entry : lines)
            {
                const auto* flag = entry.contains("client") ? &entry["client"] : nullptr;
                const bool client = flag && !flag->is_null() && (flag->is_boolean() ? flag->get<bool>() : flag->is_number() ? flag->get<double>() != 0 : !flag->empty());
                const auto line = S(entry["line"]);
                const auto source = sources.find(client);
                if (source != sources.end() && Contains(source->second, line + "\n"))
                {
                    source->second = ReplaceFirst(source->second, line + "\n", "");
                    removed.push_back(PyStrip(line));
                }
            }
        }
        for (const auto& [client, source] : sources)
            if (source != ReadTextReplace(mains[client]))
                files::WriteText(mains[client], source);
        return removed;
    }

    bool HookBo2Main(const fs::path& mainGsc, const std::string& mapName)
    {
        auto source = ReadTextReplace(mainGsc);
        const auto ported = PortedPath("maps\\" + mapName);
        const auto pre = "    " + ported + "::waw_main_pre();\n";
        const auto post = "    level thread " + ported + "::waw_main_post();\n";
        if (Contains(source, pre))
            return false;
        if (!Contains(source, PRE_ANCHOR))
            throw std::runtime_error(files::Utf8(mainGsc) + ": no '" + PyStrip(PRE_ANCHOR) + "' line to hook the WaW main around");
        source = ReplaceFirst(source, PRE_ANCHOR, pre + PRE_ANCHOR);
        static const pyre::Regex mainHead(R"(^main\(\)\s*\{)", pyre::MULTILINE);
        const auto m = mainHead.Search(source);
        if (!m)
            throw std::runtime_error("'NoneType' object has no attribute 'end'");
        int depth = 0;
        for (size_t i = ByteOffset(source, m->End()) - 1; i < source.size(); i++)
        {
            if (source[i] == '{')
                depth++;
            else if (source[i] == '}')
            {
                depth--;
                if (depth == 0)
                {
                    source = source.substr(0, i) + post + source.substr(i);
                    break;
                }
            }
        }
        files::WriteText(mainGsc, source);
        return true;
    }

    std::string PrecacheSource(const std::vector<std::string>& models)
    {
        std::vector<std::string> lines{"// waw2bo2: models the map scripts precache, precached in the first frame (generated).", "", "init()", "{"};
        for (const auto& m : std::set<std::string>(models.begin(), models.end()))
            lines.push_back(std::string("    ") + COMPAT + "::waw_precachemodel( \"" + m + "\" );");
        lines.push_back("}");
        lines.push_back("");
        return Join(lines, "\n");
    }

    std::pair<std::string, std::vector<std::string>> GuardMissingModelCalls(const std::string& source, const std::set<std::string>& missing)
    {
        auto script = Parse(source, "<model dependencies>");
        auto& tokens = script.tokens;
        std::vector<std::string> guarded;
        for (const auto& ref : References(tokens))
        {
            const auto low = Lower(ref.name);
            if (ref.pointer || !In(low, {"setmodel", "precachemodel", "waw_precachemodel"}))
                continue;
            if (!ref.qualifier && script.Find(low))
                continue;
            if (ref.qualifier && Lower(*ref.qualifier) != Lower(COMPAT))
                continue;
            if (At(tokens, static_cast<long long>(ref.index) - 1).text == "::" && !ref.qualifier)
                continue;
            const auto [lo, hi] = Slice(tokens.size(), static_cast<long long>(ref.index) + 2, static_cast<long long>(ref.index) + 4);
            if (hi - lo != 2 || tokens[lo].kind != Kind::String || tokens[lo + 1].text != ")")
                continue;
            const auto& text = tokens[lo].text;
            const auto name = text.size() >= 2 ? text.substr(1, text.size() - 2) : std::string();
            if (!missing.count(name))
                continue;
            auto& token = tokens[ref.index];
            token.text = "waw_missing_model";
            if (!ref.qualIndex)
                token.text = std::string(COMPAT) + "::" + token.text;
            guarded.push_back(name);
        }
        return {Emit(tokens), guarded};
    }

    std::vector<std::string> Bo2Scripts(const fs::path& outRoot)
    {
        std::vector<std::string> found;
        for (const auto& p : files::GlobSuffix(outRoot / "maps" / "mp" / "waw", ".gsc"))
            found.push_back(files::Utf8(p.lexically_relative(outRoot).generic_wstring()));
        for (const char* name : BO2_PERK_OVERRIDES)
        {
            const auto path = outRoot / name;
            std::error_code error;
            if (fs::exists(path, error) && StartsWith(files::ReadText(path), PERK_OVERRIDE_HEADER))
                found.push_back(name);
        }
        std::sort(found.begin(), found.end());
        return found;
    }

    fs::path EnsureOwnedClientBootstrap(const fs::path& outRoot, const fs::path& bo2Root)
    {
        const auto destination = outRoot / "clientscripts/mp/waw/_waw2bo2_zm.csc";
        std::error_code error;
        if (fs::is_regular_file(destination, error))
            return destination;
        const auto source = bo2Root / "raw/clientscripts/mp/zombies/_zm.csc";
        if (!fs::is_regular_file(source, error))
            throw std::runtime_error("BO2 client bootstrap missing: " + files::Utf8(source));
        const auto bootstrap = files::ReadText(source);
        const std::string native = "clientscripts\\mp\\zombies\\_zm_perks", owned = "clientscripts\\mp\\waw\\_waw2bo2_perks";
        if (!Contains(bootstrap, native))
            throw std::runtime_error("BO2 client bootstrap has no perk initializer");
        files::WriteText(destination, PERK_OVERRIDE_HEADER + Replace(bootstrap, native, owned));
        return destination;
    }

    namespace
    {
        const std::string WAW_ROUND_COUNTER_FLAG = "isdefined( level.waw2bo2_waw_round_counter ) && level.waw2bo2_waw_round_counter";
        using Patch = std::pair<std::string, std::string>;
        const Patch WAW_DIFFICULTY{"level.gamedifficulty = getgametypesetting( \"zmDifficulty\" );",
                                   "level.gamedifficulty = 1; // waw2bo2: Original runtime, source WaW speed selection"};
        const Patch WAW_SELF_DAMAGE_CAP{"        if ( self.health > 75 && !( isdefined( self.is_zombie ) && self.is_zombie ) )\n            return 75;\n",
                                        "        // waw2bo2: WaW applies the player's own explosive damage in full.\n"};
        const Patch WAW_PLAYER_DAMAGE_SCALE{
            "\n    finaldamage = idamage;\n",
            "\n    // waw2bo2: WaW's player_damageMultiplier on non-melee hits\n"
            "    if ( getdvarfloat( \"player_damageMultiplier\" ) > 0 && idamage > 0 && smeansofdeath != \"MOD_MELEE\"\n"
            "        && smeansofdeath != \"MOD_FALLING\" && !( isdefined( eattacker ) && isdefined( eattacker.is_zombie ) && eattacker.is_zombie ) )\n"
            "        idamage = int( max( 1, idamage * getdvarfloat( \"player_damageMultiplier\" ) ) );\n\n"
            "    finaldamage = idamage;\n"};
        const Patch WAW_START_LOCATION{"    level.scr_zm_map_start_location = getdvar( #\"ui_zm_mapstartlocation\" );\n",
                                       "    if ( getdvar( #\"ui_zm_mapstartlocation\" ) == \"\" )\n"
                                       "        setdvar( \"ui_zm_mapstartlocation\", \"default\" );\n\n"
                                       "    level.scr_zm_map_start_location = getdvar( #\"ui_zm_mapstartlocation\" );\n"};
        const Patch WAW_FAILSAFE_NO_REQUEUE{"level.put_timed_out_zombies_back_in_queue = 1;",
                                            "level.put_timed_out_zombies_back_in_queue = 0; // waw2bo2: WaW never requeues"};
        const Patch WAW_SOURCE_PLAYER_DAMAGE{
            "callback_playerdamage( einflictor, eattacker, idamage, idflags, smeansofdeath, sweapon, vpoint, vdir, shitloc, psoffsettime, boneindex )\n{\n",
            "callback_playerdamage( einflictor, eattacker, idamage, idflags, smeansofdeath, sweapon, vpoint, vdir, shitloc, psoffsettime, boneindex )\n{\n"
            "    // waw2bo2: WaW drives player damage: the map's Callback_PlayerDamage\n"
            "    if ( isdefined( level.waw2bo2_source_player_damage ) )\n    {\n"
            "        if ( smeansofdeath == \"MOD_MELEE\" && isdefined( eattacker ) && isai( eattacker )\n"
            "            && isdefined( eattacker.meleedamage ) )\n"
            "            idamage = eattacker.meleedamage;\n\n"
            "        if ( getdvarfloat( \"player_damageMultiplier\" ) > 0 && idamage > 0 && smeansofdeath != \"MOD_MELEE\"\n"
            "            && smeansofdeath != \"MOD_FALLING\" && !( isdefined( eattacker ) && isdefined( eattacker.is_zombie ) && eattacker.is_zombie ) )\n"
            "            idamage = int( max( 1, idamage * getdvarfloat( \"player_damageMultiplier\" ) ) );\n\n"
            "        self [[ level.waw2bo2_source_player_damage ]]( einflictor, eattacker, idamage, idflags, smeansofdeath, sweapon, "
            "vpoint, vdir, shitloc, undefined, psoffsettime );\n        return;\n    }\n\n"};
        const Patch WAW_SOURCE_PLAYER_LASTSTAND{
            "    [[ maps\\mp\\zombies\\_zm_laststand::playerlaststand ]]( einflictor, eattacker, idamage, smeansofdeath, sweapon, vdir, shitloc, psoffsettime, deathanimduration );\n",
            "    // waw2bo2: WaW drives last stand: the map's Callback_PlayerLastStand\n"
            "    // (a down g_fix_last_stand sent to T6's last stand takes BO2's below)\n"
            "    if ( isdefined( level.waw2bo2_source_player_laststand ) && !self maps\\mp\\waw\\_waw2bo2_fixes::native_laststand() )\n    {\n"
            "        self [[ level.waw2bo2_source_player_laststand ]]( einflictor, eattacker, idamage, smeansofdeath, sweapon, "
            "vdir, shitloc, psoffsettime, deathanimduration );\n        return;\n    }\n\n"
            "    [[ maps\\mp\\zombies\\_zm_laststand::playerlaststand ]]( einflictor, eattacker, idamage, smeansofdeath, sweapon, "
            "vdir, shitloc, psoffsettime, deathanimduration );\n"};
        const Patch WAW_NO_PLAYABLE_AREA_MONITOR{"level.player_out_of_playable_area_monitor = 1;",
                                                 "level.player_out_of_playable_area_monitor = 0; // waw2bo2: WaW never kills outside zones"};
        const Patch WAW_ROUND_HEALTH{"    ai_calculate_health( level.round_number );\n",
                                     "    if ( isdefined( level.waw2bo2_round_health_func ) )\n"
                                     "        waw2bo2_round_health();\n"
                                     "    else\n"
                                     "        ai_calculate_health( level.round_number );\n"};
        const Patch WAW_GAME_OVER_MUSIC{"    if ( isdefined( level.sndgameovermusicoverride ) )\n"
                                        "        level thread maps\\mp\\zombies\\_zm_audio::change_zombie_music( level.sndgameovermusicoverride );\n"
                                        "    else\n"
                                        "        level thread maps\\mp\\zombies\\_zm_audio::change_zombie_music( \"game_over\" );\n",
                                        "    // waw2bo2: the map's _zombiemode::end_game music, not BO2's game_over state\n"
                                        "    if ( isdefined( level.waw2bo2_end_game_music ) )\n"
                                        "        level thread [[ level.waw2bo2_end_game_music ]]();\n"};

        const std::string WAW_ROUND_HEALTH_HELPER = R"GSC(
waw2bo2_round_health()
{
    // waw2bo2: the source health step, called where WaW's round_spawning calls it
    // (an older framework steps it in its own round loop, which runs instead)
    if ( isdefined( level.waw2bo2_health_in_round_loop ) && level.waw2bo2_health_in_round_loop )
        return;

    if ( isdefined( level.waw2bo2_source_zombie_health ) )
        level.zombie_health = level.waw2bo2_source_zombie_health;

    [[ level.waw2bo2_round_health_func ]]();
    level.waw2bo2_source_zombie_health = level.zombie_health;
}
)GSC";

        const std::string WAW_SOURCE_ROUND_GLUE = R"GSC(
waw2bo2_round_think_begin( restart )
{
    // waw2bo2: BO2 round_think's start (controls, stats, round display)
    if ( !isdefined( restart ) )
        restart = 0;

    if ( !restart )
    {
        if ( isdefined( level.initial_round_wait_func ) )
            [[ level.initial_round_wait_func ]]();

        if ( !( isdefined( level.host_ended_game ) && level.host_ended_game ) )
        {
            players = get_players();

            foreach ( player in players )
            {
                if ( !( isdefined( player.hostmigrationcontrolsfrozen ) && player.hostmigrationcontrolsfrozen ) )
                    player freezecontrols( 0 );

                player maps\mp\zombies\_zm_stats::set_global_stat( "rounds", level.round_number );
            }
        }
    }

    waw2bo2_setroundsplayed( level.round_number );
}

waw2bo2_round_spawn_ready()
{
    // waw2bo2: the map's own spawners place its zombies (WaW DoSpawn);
    // nothing waits on BO2's zone location lists
}

waw2bo2_round_started()
{
    // waw2bo2: BO2's start-of-round signal and stats
    level notify( "start_of_round" );
    recordzombieroundstart();
    players = getplayers();

    for ( index = 0; index < players.size; index++ )
    {
        zonename = players[index] get_current_zone();

        if ( isdefined( zonename ) )
            players[index] recordzombiezone( "startingZone", zonename );
    }

    if ( isdefined( level.round_start_custom_func ) )
        [[ level.round_start_custom_func ]]();
}

waw2bo2_round_waited()
{
    // waw2bo2: BO2's end-of-round signal and stats
    level notify( "end_of_round" );
    uploadstats();

    if ( isdefined( level.round_end_custom_logic ) )
        [[ level.round_end_custom_logic ]]();

    players = get_players();
    array_thread( players, maps\mp\zombies\_zm_pers_upgrades_system::round_end );

    for ( player_index = 0; player_index < players.size; player_index++ )
    {
        if ( players[player_index].pers["team"] != "spectator" )
        {
            zonename = players[player_index] get_current_zone();

            if ( isdefined( zonename ) )
                players[player_index] recordzombiezone( "endingZone", zonename );
        }
    }

    recordzombieroundend();
}

waw2bo2_round_advanced()
{
    // waw2bo2: BO2's per-round stats, round display and hot-join Quick Revive
    waw2bo2_setroundsplayed( level.round_number );
    matchutctime = getutc();
    players = get_players();

    foreach ( player in players )
    {
        if ( level.curr_gametype_affects_rank && level.round_number > 3 + level.start_round )
            player maps\mp\zombies\_zm_stats::add_client_stat( "weighted_rounds_played", level.round_number );

        player maps\mp\zombies\_zm_stats::set_global_stat( "rounds", level.round_number );
        player maps\mp\zombies\_zm_stats::update_playing_utc_time( matchutctime );
    }

    check_quickrevive_for_hotjoin();
}
)GSC";

        const std::string WAW_ZOMBIE_VAR_HELPER = R"GSC(
waw2bo2_zombie_var_divisor( var )
{
    if ( isdefined( level.waw2bo2_zombie_var_divisors ) && isdefined( level.waw2bo2_zombie_var_divisors[var] ) )
        return level.waw2bo2_zombie_var_divisors[var];

    return undefined;
}

waw2bo2_apply_zombie_var_defaults()
{
    // waw2bo2: WaW framework zombie_vars BO2's init_levelvars does not set
    // (zombie_health_increase_percent, read by the source health step)
    if ( !isdefined( level.waw2bo2_zombie_var_defaults ) )
        return;

    foreach ( var, value in level.waw2bo2_zombie_var_defaults )
    {
        if ( !isdefined( level.zombie_vars[var] ) )
            maps\mp\waw\_waw2bo2_compat::waw_set_zombie_var( var, value, waw2bo2_zombie_var_divisor( var ) );
    }
}

waw2bo2_set_zombie_var( var, value, is_float, column, is_team_based )
{
    // waw2bo2: a variable WaW's framework sets keeps WaW's default and only the
    // map's own WaW table may change it (Translator.framework_zombie_vars);
    // BO2's stock mp/zombiemode.csv (zombie_spawn_delay 2.0) does not apply.
    if ( isdefined( level.waw2bo2_zombie_var_defaults ) && isdefined( level.waw2bo2_zombie_var_defaults[var] ) )
    {
        maps\mp\waw\_waw2bo2_compat::waw_set_zombie_var( var, level.waw2bo2_zombie_var_defaults[var], waw2bo2_zombie_var_divisor( var ), is_float );
        return level.zombie_vars[var];
    }

    return set_zombie_var( var, value, is_float, column, is_team_based );
}
)GSC";

        const std::string DOG_ACTOR_GLUE = R"GSC(

waw2bo2_dog_actor_glue()
{
    self.team = level.zombie_team;
    // BO2's dog animscripts and traversals read these
    self.zombie_move_speed = "sprint";
    self.a.disablepain = 1;
    self disable_react();
    // WaW: dog_MeleeDamage 100 x player_meleeDamageMultiplier 0.4 (_zombiemode_dogs::init)
    self.meleedamage = 40;
    self.thundergun_knockdown_func = ::dog_thundergun_knockdown;
    // T4's actor damage event calls the map's own global callbacks.
    self thread maps\mp\waw\_waw2bo2_zombie_behavior::waw2bo2_actor_damage();
    self thread maps\mp\waw\_waw2bo2_zombie_behavior::waw2bo2_actor_sound_cleanup();
    self thread maps\mp\waw\_waw2bo2_zombie_behavior::waw2bo2_actor_kill_credit();
    self thread waw2bo2_dog_keep_enemy();
    self thread waw2bo2_dog_combat_signal();
}

waw2bo2_dog_combat_signal()
{
    // WaW's dog animscripts notify "dog_combat" on entering combat
    // (dog_combat.gsc main, dog_stop.gsc); T6's zm_dog scripts only send
    // "dog_running". Source dog scripts wait for either (Der Riese's
    // dog_run_think drops the spawn health and lights the eyes then).
    self endon( "death" );
    fighting = 0;

    while ( true )
    {
        state = self getanimstatefromasd();
        now = isdefined( state ) && issubstr( state, "zm_combat" );

        if ( now && !fighting )
            self notify( "dog_combat" );

        fighting = now;
        wait 0.1;
    }
}

waw2bo2_dog_keep_enemy()
{
    // T6 dogs drop a favoriteenemy that went down; WaW's engine retargeted
    self endon( "death" );

    while ( true )
    {
        if ( isdefined( self.favoriteenemy ) && !is_player_valid( self.favoriteenemy ) )
            self.favoriteenemy = get_favorite_enemy();

        wait 0.2;
    }
}
)GSC";

        std::string RoundCounterHelper()
        {
            return "\nwaw2bo2_setroundsplayed( round )\n{\n    // waw2bo2: WaW's script round counter replaces BO2's display on stock maps\n    if ( " + WAW_ROUND_COUNTER_FLAG
                   + " )\n        return;\n\n    setroundsplayed( round );\n}\n";
        }

        std::string ApplyPatch(const std::string& text, const Patch& patch, const std::string& error)
        {
            if (Count(text, patch.first) != 1)
                throw std::runtime_error(error);
            return Replace(text, patch.first, patch.second);
        }
    } // namespace

    std::string WawPvpAttackNotify(const std::string& bootstrap)
    {
        auto script = Parse(bootstrap, "owned zombie bootstrap");
        const Function* fn = script.Find("callback_playerdamage");
        if (!fn || fn->params.size() < 6)
            throw std::runtime_error("BO2 server bootstrap has no Callback_PlayerDamage");
        const auto& attacker = fn->params[1];
        const auto& damage = fn->params[2];
        const auto& mod = fn->params[4];
        const auto& weapon = fn->params[5];
        script.tokens[fn->bodyOpen].text += "\n    // waw2bo2: T4's player weapon hit notify (WaW scripts wait for it)\n    if ( isdefined( " + attacker + " ) && isplayer( " + attacker
                                            + " ) )\n        self notify( \"weapon_pvp_attack\", " + attacker + ", " + COMPAT + "::waw_weapon_name( " + weapon + " ), " + damage
                                            + ", " + mod + " );\n";
        return Emit(script.tokens);
    }

    std::string WawZombieVarDefaults(const std::string& bootstrap)
    {
        auto script = Parse(bootstrap, "owned zombie bootstrap");
        const Function* fn = script.Find("init_levelvars");
        if (!fn)
            throw std::runtime_error("BO2 server bootstrap has no init_levelvars");
        auto& toks = script.tokens;
        int count = 0;
        for (long long i = static_cast<long long>(fn->bodyOpen); i < static_cast<long long>(fn->bodyClose) - 2; i++)
            if (toks[i].Low() == "set_zombie_var" && toks[i + 1].text == "(" && toks[i + 2].kind == Kind::String && !In(At(toks, i - 1).text, {".", "::"}))
            {
                toks[i].text = "waw2bo2_set_zombie_var";
                count++;
            }
        if (!count)
            throw std::runtime_error("BO2 init_levelvars sets no zombie_vars");
        toks[fn->bodyClose].pre = "\n    waw2bo2_apply_zombie_var_defaults();\n" + toks[fn->bodyClose].pre;
        return Emit(toks) + WAW_ZOMBIE_VAR_HELPER;
    }

    std::string OwnedDogsSource(const fs::path& bo2Root)
    {
        const auto& dogs = Api()["zombie_runtime"]["dog_rounds"];
        const auto native = S(dogs["native_script"]), owned = S(dogs["owned_script"]);
        auto text = files::ReadText(bo2Root / "raw" / files::FromUtf8(Posix(native) + ".gsc"));
        for (const auto& [bo2, waw] : dogs["native_sounds"].items())
        {
            if (!Contains(text, "\"" + bo2 + "\""))
                throw std::runtime_error(native + ": dog sound \"" + bo2 + "\" not found");
            text = Replace(text, "\"" + bo2 + "\"", "\"waw/" + S(waw) + "\"");
        }
        for (const auto& [state, waw] : dogs["native_music"].items())
        {
            const auto old = "level thread maps\\mp\\zombies\\_zm_audio::change_zombie_music( \"" + state + "\" );";
            if (!Contains(text, old))
                throw std::runtime_error(native + ": dog music state \"" + state + "\" not found");
            text = Replace(text, old, "level thread maps\\mp\\zombies\\_zm_utility::play_sound_2d( \"waw/" + S(waw) + "\" );");
        }
        static const pyre::Regex loadfx(R"(\bloadfx\s*\()", pyre::IGNORECASE);
        text = loadfx.Sub([](const pyre::Match&) { return std::string(COMPAT) + "::waw_loadfx_native("; }, text);
        return "// waw2bo2: BO2 dog rounds playing the source WaW dog sounds and effects (generated).\n" + Replace(text, native + "::", owned + "::") + DOG_ACTOR_GLUE;
    }

    void StageBo2PerkSupport(const fs::path& outRoot, const fs::path& bo2Root, const bool bo2StockPerks)
    {
        std::error_code error;
        if (!bo2StockPerks)
            for (const auto& [prefix, ext] : std::vector<std::pair<std::string, std::string>>{{"maps", "gsc"}, {"clientscripts", "csc"}})
                for (const char* module : {"electric_cherry", "divetonuke"})
                    fs::remove(outRoot / files::FromUtf8(prefix + "/mp/waw/_waw2bo2_perk_" + module + "." + ext), error);
        for (const std::string name : BO2_PERK_OVERRIDES)
        {
            const auto source = files::ReadText(bo2Root / "raw" / files::FromUtf8(name));
            auto script = Parse(source, name);
            std::vector<std::pair<std::string, std::string>> bodies;
            if (EndsWith(name, ".csc"))
                bodies = {{"init", "{\n    if ( !level.createfx_enabled )\n    {\n        init_custom_perks();\n        perks_register_clientfield();\n    }\n}"},
                          {"perk_init_code_callbacks", "{\n    // setupclientfieldcodecallbacks binds native LUI perk-icon events.\n    // WaW creates and removes its own HUD; retain fields without these bindings.\n}"},
                          {"init_perk_custom_threads", "{\n    // Source WaW scripts own perk behavior.\n}"}};
            else
                bodies = {{"init", "{\n    level.additionalprimaryweapon_limit = 3;\n    level.perk_purchase_limit = 4;\n    level.machine_assets = [];\n"
                                   "    initialize_custom_perk_arrays();\n    if ( !level.createfx_enabled )\n        perks_register_clientfield();\n"
                                   "    if ( !isdefined( level.flag[\"pack_machine_in_use\"] ) )\n        flag_init( \"pack_machine_in_use\" );\n"
                                   "    // WaW initializes its machines from waw_main_post, before player startup.\n}"},
                          {"perk_pause_all_perks", "{\n    // The WaW framework owns machine availability.\n}"},
                          {"perk_unpause_all_perks", "{\n    // The WaW framework owns machine availability.\n}"}};
            for (const auto& [fnName, body] : bodies)
            {
                const Function* fn = script.Find(fnName);
                if (!fn)
                    throw std::runtime_error(name + ": expected BO2 perk entry point " + fnName + " is missing");
                script.tokens[fn->bodyOpen].text = body;
                for (size_t k = fn->bodyOpen + 1; k < fn->bodyClose + 1 && k < script.tokens.size(); k++)
                    script.tokens[k].pre = script.tokens[k].text = "";
            }
            auto text = Emit(script.tokens);
            if (bo2StockPerks)
                text = stockperks::NativeSource(source, name, outRoot, bo2Root);
            files::WriteText(outRoot / files::FromUtf8(name), PERK_OVERRIDE_HEADER + text);
        }
        files::WriteText(outRoot / "clientscripts/mp/waw/_waw2bo2_perks.csc", files::ReadText(outRoot / "clientscripts/mp/zombies/_zm_perks.csc"));
        EnsureOwnedClientBootstrap(outRoot, bo2Root);
        const auto serverDir = outRoot / "maps/mp/waw";
        fs::create_directories(serverDir);
        const std::string native = "maps\\mp\\zombies\\_zm_perks", owned = "maps\\mp\\waw\\_waw2bo2_perks";
        files::WriteText(serverDir / "_waw2bo2_perks.gsc", Replace(files::ReadText(outRoot / "maps/mp/zombies/_zm_perks.gsc"), native, owned));
        auto bootstrap = files::ReadText(bo2Root / "raw/maps/mp/zombies/_zm.gsc");
        if (!Contains(bootstrap, native + "::init"))
            throw std::runtime_error("BO2 server bootstrap has no perk initializer");
        if (!Contains(bootstrap, WAW_DIFFICULTY.first))
            throw std::runtime_error("BO2 server bootstrap does not read zmDifficulty");
        bootstrap = Replace(bootstrap, WAW_DIFFICULTY.first, WAW_DIFFICULTY.second);
        const std::vector<std::tuple<std::string, std::string, size_t>> counter{
            {"setroundsplayed( level.round_number );", "waw2bo2_setroundsplayed( level.round_number );", 2},
            {"if ( 255 < level.round_number )", "if ( 255 < level.round_number && !( " + WAW_ROUND_COUNTER_FLAG + " ) )", 1}};
        for (const auto& [old, replacement, count] : counter)
        {
            if (Count(bootstrap, old) != count)
                throw std::runtime_error("BO2 server bootstrap round counter changed: " + old);
            bootstrap = Replace(bootstrap, old, replacement);
        }
        bootstrap = ApplyPatch(bootstrap, WAW_SELF_DAMAGE_CAP, "BO2 server bootstrap explosive self-damage cap changed");
        bootstrap = ApplyPatch(bootstrap, WAW_PLAYER_DAMAGE_SCALE, "BO2 server bootstrap player damage result changed");
        bootstrap = ApplyPatch(bootstrap, WAW_START_LOCATION, "BO2 server bootstrap start location read changed");
        bootstrap = ApplyPatch(bootstrap, WAW_FAILSAFE_NO_REQUEUE, "BO2 server bootstrap failsafe requeue changed");
        bootstrap = ApplyPatch(bootstrap, WAW_NO_PLAYABLE_AREA_MONITOR, "BO2 server bootstrap playable area monitor changed");
        bootstrap = ApplyPatch(bootstrap, WAW_SOURCE_PLAYER_DAMAGE, "BO2 server bootstrap player damage callback changed");
        bootstrap = ApplyPatch(bootstrap, WAW_SOURCE_PLAYER_LASTSTAND, "BO2 server bootstrap last stand callback changed");
        bootstrap = ApplyPatch(bootstrap, WAW_GAME_OVER_MUSIC, "BO2 server bootstrap game over music changed");
        bootstrap = ApplyPatch(bootstrap, WAW_ROUND_HEALTH, "BO2 server bootstrap round health call changed");
        bootstrap += WAW_ROUND_HEALTH_HELPER;
        bootstrap += WAW_SOURCE_ROUND_GLUE;
        bootstrap = WawZombieVarDefaults(bootstrap);
        bootstrap = WawPvpAttackNotify(bootstrap);
        bootstrap += RoundCounterHelper();
        files::WriteText(serverDir / "_waw2bo2_zm.gsc", PERK_OVERRIDE_HEADER + Replace(bootstrap, native, owned));
        const auto& dogs = Api()["zombie_runtime"]["dog_rounds"];
        if (fs::is_regular_file(bo2Root / "raw" / files::FromUtf8(Posix(S(dogs["native_script"])) + ".gsc"), error))
            files::WriteText(outRoot / files::FromUtf8(Posix(S(dogs["owned_script"])) + ".gsc"), OwnedDogsSource(bo2Root));
    }

    namespace
    {
        int RedirectInit(Tokens& tokens, const std::string& native, const std::string& owned)
        {
            int count = 0;
            for (const auto& ref : References(tokens))
                if (Lower(ref.name) == "init" && ref.qualifier && (*ref.qualifier == native || *ref.qualifier == owned))
                {
                    tokens[*ref.qualIndex].text = owned;
                    count++;
                }
                else if (native == "maps\\mp\\zombies\\_zm" && ref.qualifier && *ref.qualifier == "maps\\mp\\zombies\\_zm_perks")
                    tokens[*ref.qualIndex].text = "maps\\mp\\waw\\_waw2bo2_perks";
            return count;
        }
    } // namespace

    void HookBo2PerkServer(const fs::path& mainGsc)
    {
        auto script = Parse(files::ReadText(mainGsc), files::Utf8(mainGsc.generic_wstring()));
        if (!RedirectInit(script.tokens, "maps\\mp\\zombies\\_zm", "maps\\mp\\waw\\_waw2bo2_zm"))
            throw std::runtime_error(files::Utf8(mainGsc) + ": no BO2 server bootstrap call");
        files::WriteText(mainGsc, Emit(script.tokens));
    }

    void HookBo2PerkClient(const fs::path& mainCsc)
    {
        auto script = Parse(files::ReadText(mainCsc), files::Utf8(mainCsc.generic_wstring()));
        if (!RedirectInit(script.tokens, "clientscripts\\mp\\zombies\\_zm", "clientscripts\\mp\\waw\\_waw2bo2_zm"))
            throw std::runtime_error(files::Utf8(mainCsc) + ": no BO2 client bootstrap call");
        files::WriteText(mainCsc, Emit(script.tokens));
    }
} // namespace w2b::gsc::port
