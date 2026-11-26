// The menus a map's scripts open (see script_menus.h).
#include "script_menus.h"

#include "core/files.h"
#include "core/regex.h"
#include "hud_menus.h"
#include "modutil.h"
#include "oat/waw_menus.h"
#include "source_zones.h"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;
    using util::Json;

    namespace
    {
        // T4 window flags (ui_shared.h): dynamicFlags WINDOW_VISIBLE, staticFlags DECORATION / AUTOWRAPPED
        constexpr unsigned WINDOW_VISIBLE = 0x4, STATIC_DECORATION = 0x100000, STATIC_AUTOWRAPPED = 0x800000;

        std::string Text(const Json& value)
        {
            return value.is_string() ? value.get<std::string>() : std::string();
        }

        std::string Quoted(const std::string& text)
        {
            return "\"" + text + "\"";
        }

        Json Number(const Json& value)
        {
            return value.is_number_integer() ? std::to_string(value.get<long long>()) : util::FloatRepr(value.get<double>());
        }

        Json Numbers(const Json& values)
        {
            Json out = Json::array();
            for (const auto& v : values)
                out.push_back(Number(v));
            return out;
        }

        // the compiler keeps a script as its tokens: quoted strings, words and ";" between the commands
        Json Commands(const std::string& script)
        {
            Json commands = Json::array(), command = Json::array();
            const auto end = [&]
            {
                if (!command.empty())
                    commands.push_back(command);
                command = Json::array();
            };
            for (std::size_t i = 0; i < script.size();)
            {
                const char c = script[i];
                if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}')
                    i++;
                else if (c == ';')
                {
                    end();
                    i++;
                }
                else if (c == '"')
                {
                    const auto close = script.find('"', i + 1);
                    const auto stop = close == std::string::npos ? script.size() : close + 1;
                    command.push_back(script.substr(i, stop - i));
                    i = stop;
                }
                else
                {
                    auto stop = i;
                    while (stop < script.size() && !std::isspace(static_cast<unsigned char>(script[stop])) && script[stop] != ';' && script[stop] != '"')
                        stop++;
                    command.push_back(script.substr(i, stop - i));
                    i = stop;
                }
            }
            end();
            return commands;
        }

        // the literals of a compiled expression, as source tokens (what Requirements reads)
        Json Literals(const Json& tokens)
        {
            Json out = Json::array();
            if (tokens.is_array())
                for (const auto& token : tokens)
                    if (token.contains("string") && token["string"].is_string())
                        out.push_back(Quoted(token["string"].get<std::string>()));
            return out;
        }

        Json Keys(const Json& handlers)
        {
            Json out = Json::array();
            if (handlers.is_array())
                for (const auto& handler : handlers)
                    out.push_back({{"key", handler["key"]}, {"commands", Commands(Text(handler["action"]))}});
            return out;
        }

        void Window(Json& d, const Json& window)
        {
            const auto& rect = window["rect"];
            d["rect"] = Numbers(Json::array({rect["x"], rect["y"], rect["w"], rect["h"], rect["horzAlign"], rect["vertAlign"]}));
            if (const auto name = Text(window["name"]); !name.empty())
                d["name"] = Json::array({Quoted(name)});
            for (const auto& [key, field] : {std::pair{"style", "style"}, {"border", "border"}, {"bordersize", "borderSize"}, {"ownerdraw", "ownerDraw"}})
                if (window[field].is_number() && window[field] != 0)
                    d[key] = Json::array({Number(window[field])});
            for (const auto& [key, field] : {std::pair{"forecolor", "foreColor"}, {"backcolor", "backColor"}, {"bordercolor", "borderColor"}})
                d[key] = Numbers(window[field]);
            auto background = Text(window["background"]);
            if (background.starts_with(','))
                background = background.substr(1); // a material of another zone
            if (!background.empty())
                d["background"] = Json::array({Quoted(background)});
            const auto flags = window["staticFlags"].get<unsigned>();
            if (flags & STATIC_DECORATION)
                d["decoration"] = true;
            if (flags & STATIC_AUTOWRAPPED)
                d["autowrapped"] = Json::array();
        }

        // visible: its expression, else the compiled flag ("visible 1")
        std::string Visible(const Json& window, const Json& expression)
        {
            if (!expression.is_null())
                return HudExpression(expression);
            return window["dynamicFlags"].get<unsigned>() & WINDOW_VISIBLE ? "n:1" : "n:0";
        }

        Json Item(const Json& item, std::vector<std::string>& problems, const std::string& where)
        {
            Json d = Json::object();
            Window(d, item["window"]);
            d["type"] = Json::array({Number(item["type"])});
            d["textfont"] = Json::array({Number(item["font"])});
            d["textscale"] = Json::array({Number(item["textScale"])});
            d["textstyle"] = Json::array({Number(item["textStyle"])});
            d["textalign"] = Json::array({Number(item["textAlignMode"])});
            d["textalignx"] = Json::array({Number(item["textAlignX"])});
            d["textaligny"] = Json::array({Number(item["textAlignY"])});
            if (const auto text = Text(item["text"]); !text.empty())
                d["text"] = Json::array({Quoted(text)});
            for (const auto& [key, field] : {std::pair{"action", "action"}, {"onfocus", "onFocus"}, {"leavefocus", "leaveFocus"}, {"mouseenter", "mouseEnter"},
                                             {"mouseexit", "mouseExit"}})
                if (const auto script = Text(item[field]); !script.empty())
                    d[key] = Commands(script);
            if (auto keys = Keys(item["onKey"]); !keys.empty())
                d["keys"] = std::move(keys);
            Json compiled = Json::object();
            try
            {
                compiled["visible"] = Visible(item["window"], item["visibleExp"]);
            }
            catch (const std::exception& error)
            {
                problems.push_back(where + " visible: " + error.what());
                compiled["visible"] = "n:1";
            }
            std::vector<std::string> exp;
            Json source = Json::object();
            for (const auto& [field, prop] : std::array<std::pair<const char*, const char*>, 7>{{{"materialExp", "material"},
                                                                                                {"rectHExp", "rect h"},
                                                                                                {"rectWExp", "rect w"},
                                                                                                {"rectXExp", "rect x"},
                                                                                                {"rectYExp", "rect y"},
                                                                                                {"textExp", "text"},
                                                                                                {"forecolorAExp", "forecolor a"}}})
            {
                if (item[field].is_null())
                    continue;
                try
                {
                    exp.push_back(std::string(prop) + "=" + HudExpression(item[field]));
                    source[prop] = Literals(item[field]);
                }
                catch (const std::exception& error)
                {
                    problems.push_back(where + " " + field + ": " + error.what());
                }
            }
            std::ranges::sort(exp);
            compiled["exp"] = mod::Join(exp, "|");
            if (!source.empty())
                d["exp"] = std::move(source);
            d["compiled"] = std::move(compiled);
            return d;
        }
    } // namespace

    std::set<std::string> ScriptOpenedMenus(const fs::path& project)
    {
        static const regex::Regex open("openmenu(?:nomouse)?\\s*\\(\\s*\"([^\"]+)\"", regex::IGNORECASE);
        static const regex::Regex openGame("openmenu(?:nomouse)?\\s*\\(\\s*game\\s*\\[\\s*\"([^\"]+)\"", regex::IGNORECASE);
        static const regex::Regex setGame("game\\s*\\[\\s*\"([^\"]+)\"\\s*\\]\\s*=\\s*\"([^\"]+)\"", regex::IGNORECASE);
        std::set<std::string> menus, keys;
        std::map<std::string, std::string> game;
        for (const auto& script : files::GlobSuffix(project / "maps/mp/waw", ".gsc"))
        {
            const auto text = files::ReadText(script);
            for (const auto& row : open.FindAll(text))
                menus.insert(mod::Lower(row[0]));
            for (const auto& row : openGame.FindAll(text))
                keys.insert(mod::Lower(row[0]));
            for (const auto& row : setGame.FindAll(text))
                game[mod::Lower(row[0])] = mod::Lower(row[1]);
        }
        for (const auto& key : keys)
            if (const auto found = game.find(key); found != game.end())
                menus.insert(found->second);
        return menus;
    }

    namespace
    {
    Json MenuSource(const std::string& name, const Json& menu, std::vector<std::string>& problems)
    {
        Json d{{"items", Json::array()}};
        Window(d, menu["window"]);
        d["name"] = Json::array({Quoted(name)});
        for (const auto& [key, field] : {std::pair{"onopen", "onOpen"}, {"onclose", "onClose"}, {"onesc", "onESC"}})
            if (const auto script = Text(menu[field]); !script.empty())
                d[key] = Commands(script);
        if (auto keys = Keys(menu["onKey"]); !keys.empty())
            d["keys"] = std::move(keys);
        if (menu["blurRadius"].is_number() && menu["blurRadius"].get<double>() > 0)
            d["blurworld"] = Json::array({Number(menu["blurRadius"])});
        if (menu["fullScreen"].is_number() && menu["fullScreen"] != 0)
            d["fullscreen"] = Json::array({Number(menu["fullScreen"])});
        if (menu.contains("focusColor"))
            d["focuscolor"] = Numbers(menu["focusColor"]);
        Json compiled = Json::object();
        try
        {
            compiled["visible"] = menu["visibleExp"].is_null() ? std::string("n:1") : HudExpression(menu["visibleExp"]);
        }
        catch (const std::exception& error)
        {
            problems.push_back(name + " visible: " + error.what());
            compiled["visible"] = "n:1";
        }
        d["compiled"] = std::move(compiled);
        std::size_t i = 0;
        for (const auto& item : menu["items"])
            d["items"].push_back(Item(item, problems, name + " item " + std::to_string(++i)));
        return d;
    }
    } // namespace

    Json CompiledMenuSource(const std::string& name, const Json& menu)
    {
        std::vector<std::string> problems;
        auto out = MenuSource(name, menu, problems);
        if (!problems.empty())
            throw std::runtime_error(mod::Join(problems, "; "));
        return out;
    }

    ScriptMenus ReadScriptMenus(const fs::path& project, const std::vector<fs::path>& mapRoots)
    {
        ScriptMenus out;
        const auto opened = ScriptOpenedMenus(project);
        if (opened.empty())
            return out;
        auto zones = SourceZones(mapRoots);
        // the mod's own zone is loaded over the map's
        std::ranges::stable_partition(zones, [](const fs::path& zone) { return mod::Lower(files::Utf8(zone.filename())) == "mod.ff"; });
        for (const auto& zone : zones)
        {
            const auto all = Json::parse(oat::ReadMenus({zone, {}})); // a local: items() of a temporary dangles
            for (const auto& [name, menu] : all.items())
            {
                const auto key = mod::Lower(name);
                if (!opened.contains(key) || out.menus.contains(key))
                    continue;
                std::vector<std::string> problems;
                out.menus[key] = MenuSource(key, menu, problems);
                for (const auto& problem : problems)
                    out.warnings.push_back("SCRIPT_MENU " + problem);
            }
        }
        return out;
    }
} // namespace w2b::pipeline
