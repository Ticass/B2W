// A map's own HUD menus (see hud_menus.h).
#include "hud_menus.h"

#include "core/files.h"
#include "core/regex.h"
#include "extract/stockwaw.h"
#include "modutil.h"
#include "oat/waw_menus.h"

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
        // the HUD menus T4 draws during a zombies game
        constexpr std::array HUD_MENUS{"weaponinfo", "dpad", "competitivemodescores"};
        constexpr int FIRST_FUNCTION = 23;

        // t4sp.exe's operator names (0x8d7be8), from 23 on functions
        constexpr std::array OPERATORS{"NOOP", ")", "*", "/", "%", "+", "-", "!", "<", "<=", ">", ">=", "==", "!=", "&&", "||", "(", ",", "&", "|", "~", "<<", ">>"};
        constexpr std::array FUNCTIONS{"sin", "cos", "min", "max", "milliseconds", "dvarint", "dvarbool", "dvarfloat", "dvarstring", "stat", "ui_active",
                                       "flashbanged", "scoped", "scoreboard_visible", "inkillcam", "player", "selecting_location", "team", "otherteam",
                                       "marinesfield", "opforfield", "menuisopen", "writingdata", "inlobby", "inprivateparty", "privatepartyhost",
                                       "privatepartyhostinlobby", "aloneinparty", "adsjavelin", "weaplockblink", "weapattacktop", "weapattackdirect",
                                       "secondsastime", "tablelookup", "locstring", "localvarint", "localvarbool", "localvarfloat", "localvarstring", "timeleft",
                                       "secondsascountdown", "gamemsgwndactive", "int", "string", "float", "gametypename", "gametype", "gametypedescription",
                                       "scoreatrank", "friendsonline", "spectatingclient", "statrangeanybitsset", "keybinding", "actionslotusable", "hudfade",
                                       "maxrecommendedplayers", "acceptinginvite", "gamehost", "issplitscreen", "issplitscreenhost", "splitscreennum",
                                       "iscinematicfinished", "issquadleader", "getsquadid", "isartillery", "getplayerssquadid", "hasfriends", "haspendingfriends"};

        std::string Escape(const std::string& text)
        {
            static constexpr char hex[] = "0123456789abcdef";
            std::string out;
            for (const unsigned char c : text)
                if (std::isalnum(c) || c == '_' || c == '.' || c == '/' || c == '-' || c >= 0x80)
                    out += static_cast<char>(c);
                else
                    (out += '%') += {hex[c >> 4], hex[c & 15]};
            return out;
        }

        int Binary(const int op)
        {
            switch (op)
            {
            case 15:
                return 1;
            case 14:
                return 2;
            case 19:
                return 3;
            case 18:
                return 4;
            case 12:
            case 13:
                return 5;
            case 8:
            case 9:
            case 10:
            case 11:
                return 6;
            case 21:
            case 22:
                return 7;
            case 5:
            case 6:
                return 8;
            case 2:
            case 3:
            case 4:
                return 9;
            default:
                return 0;
            }
        }

        class Expression
        {
        public:
            explicit Expression(const Json& tokens) : tokens_(tokens)
            {
                // the compiler opens every expression with a parenthesis it does not close
                if (Op() == 16)
                    at_++;
            }

            std::string Read()
            {
                Parse(1);
                if (Op() == 1)
                    at_++;
                if (at_ != tokens_.size())
                    throw std::runtime_error("expression goes on after its end");
                return out_;
            }

        private:
            int Op() const
            {
                return at_ < tokens_.size() && tokens_[at_].contains("op") ? tokens_[at_]["op"].get<int>() : -1;
            }

            void Put(const std::string& token)
            {
                out_ += (out_.empty() ? "" : " ") + token;
            }

            void Parse(const int least)
            {
                Primary();
                for (int level; (level = Binary(Op())) >= least && level > 0;)
                {
                    const auto op = Op();
                    at_++;
                    Parse(level + 1);
                    Put(std::string("op:") + OPERATORS[static_cast<std::size_t>(op)]);
                }
            }

            void Primary()
            {
                if (at_ >= tokens_.size())
                    throw std::runtime_error("expression ends early");
                const auto& token = tokens_[at_];
                const auto op = Op();
                at_++;
                if (op < 0)
                {
                    if (token.contains("string"))
                        Put("s:" + Escape(token["string"].is_string() ? token["string"].get<std::string>() : std::string()));
                    else
                        Put("n:" + (token.contains("int") ? std::to_string(token["int"].get<long long>()) : util::FloatRepr(token["float"].get<double>())));
                    return;
                }
                if (op == 7 || op == 6)
                {
                    Primary();
                    Put(op == 7 ? "op:!" : "op:neg");
                    return;
                }
                if (op == 16)
                {
                    Parse(1);
                    Close();
                    return;
                }
                const auto index = op - FIRST_FUNCTION;
                if (index < 0 || index >= static_cast<int>(FUNCTIONS.size()))
                    throw std::runtime_error("operator " + std::to_string(op) + " does not start a value");
                int count = 0;
                if (Op() != 1)
                    for (;;)
                    {
                        Parse(1);
                        count++;
                        if (Op() != 17)
                            break;
                        at_++;
                    }
                Close();
                Put("f:" + std::string(FUNCTIONS[static_cast<std::size_t>(index)]) + ":" + std::to_string(count));
            }

            void Close()
            {
                // the last one may be left out, as the outer one always is
                if (Op() == 1)
                    at_++;
                else if (at_ < tokens_.size())
                    throw std::runtime_error("unbalanced parenthesis");
            }

            const Json& tokens_;
            std::size_t at_ = 0;
            std::string out_;
        };

        // the fastfile a root was dumped from
        std::optional<fs::path> Fastfile(const fs::path& root)
        {
            const auto extraction = root / "extraction.json";
            if (!mod::IsFile(extraction))
                return std::nullopt;
            const auto data = mod::Json(extraction);
            const auto file = data.value("inputs", Json::object()).value("file", std::string());
            const auto path = files::FromUtf8(file);
            return file.empty() || !mod::IsFile(path) ? std::nullopt : std::optional(path);
        }

        Json Menus(const fs::path& fastfile)
        {
            auto all = Json::parse(oat::ReadMenus({fastfile, {}}));
            Json out = Json::object();
            // "," marks a material of another zone
            const auto resolve = [](Json& window)
            {
                auto& name = window["background"];
                if (name.is_string() && name.get_ref<const std::string&>().starts_with(','))
                    name = name.get<std::string>().substr(1);
            };
            for (auto& [name, menu] : all.items())
            {
                if (menu.contains("window"))
                    resolve(menu["window"]);
                for (auto& item : menu["items"])
                    resolve(item["window"]);
                out[mod::Lower(name)] = menu;
            }
            return out;
        }

        // the compiled menus of the stock zones that hold these, small zones first
        std::set<std::string> StockMenus(const extract::StockWaw& stock, const std::string& name)
        {
            std::vector<std::pair<std::uintmax_t, fs::path>> zones;
            std::error_code e;
            for (const auto& zone : stock.ZonesDefining("menu", name))
            {
                const auto path = stock.ZoneDir() / files::FromUtf8(zone + ".ff");
                if (const auto size = fs::file_size(path, e); !e)
                    zones.emplace_back(size, path);
            }
            std::ranges::sort(zones);
            std::set<std::string> out;
            for (std::size_t i = 0; i < zones.size() && i < 3; i++)
                if (const auto menus = Menus(zones[i].second); menus.contains(name))
                    out.insert(util::JsonDumpsSorted(menus[name]));
            return out;
        }

        std::string Numbers(const Json& values)
        {
            std::string out;
            for (const auto& v : values)
                out += (out.empty() ? "" : " ") + (v.is_number_integer() ? std::to_string(v.get<long long>()) : util::FloatRepr(v.get<double>()));
            return out;
        }

        std::string Rect(const Json& rect)
        {
            return Numbers(Json::array({rect["x"], rect["y"], rect["w"], rect["h"], rect["horzAlign"], rect["vertAlign"]}));
        }

        std::string Text(const Json& value)
        {
            return value.is_string() ? value.get<std::string>() : std::string();
        }

        // the materials the map ships that a material expression can hold: the ones its scripts name, and the ones that
        // start with a string of the expressions themselves ("hud_num_" + a digit)
        std::set<std::string> ExpressionMaterials(const fs::path& project, const std::vector<fs::path>& roots, const std::set<std::string>& strings)
        {
            std::set<std::string> shipped;
            for (const auto& root : roots)
            {
                const auto extraction = root / "extraction.json";
                if (!mod::IsFile(extraction))
                    continue;
                const auto data = mod::Json(extraction);
                const auto index = data.value("index", Json::object());
                for (const auto& name : index.value("material", Json::array()))
                    if (name.is_string() && name.get<std::string>().find('/') == std::string::npos)
                        shipped.insert(mod::Lower(name.get<std::string>()));
            }
            static const regex::Regex literal("\"([A-Za-z0-9_~$-]{3,60})\"");
            std::set<std::string> out;
            for (const auto& script : files::GlobSuffix(project / "maps/mp/waw", ".gsc"))
                for (const auto& row : literal.FindAll(files::ReadText(script)))
                    if (const auto name = mod::Lower(row[0]); shipped.contains(name))
                        out.insert(name);
            for (const auto& text : strings)
                for (auto name = shipped.lower_bound(text); name != shipped.end() && name->starts_with(text); ++name)
                    out.insert(*name);
            return out;
        }
    } // namespace

    std::string HudExpression(const Json& tokens)
    {
        return Expression(tokens).Read();
    }

    std::string HudFontRole(const int font, const double scale)
    {
        const auto size = scale * 1080 / 480;
        if (font == 6)
            return "objective";
        if (font == 1)
            return "score"; // normalFont
        if (font == 3)
            return "small";
        if (font == 5)
            return "console";
        if (size <= 0.25)
            return "small";
        if (font == 4)
            return size >= 0.4 ? "bold" : "score";
        return size >= 0.55 ? "extrabig" : size >= 0.4 ? "big" : "score";
    }

    HudMenus ReadHudMenus(const fs::path& project, const std::vector<fs::path>& roots, extract::StockWaw* stock,
                          const std::function<std::optional<std::string>(const std::string&)>& sprite)
    {
        HudMenus out;
        if (!stock)
            return out;
        std::vector<fs::path> zones;
        for (const auto& root : roots)
            if (const auto file = Fastfile(root); file && file->parent_path() != stock->ZoneDir() && std::ranges::find(zones, *file) == zones.end())
                zones.push_back(*file);
        // the mod's own zone is loaded over the map's
        std::ranges::stable_partition(zones, [](const fs::path& zone) { return mod::Lower(files::Utf8(zone.filename())) == "mod.ff"; });
        std::vector<Json> loaded;
        for (const auto& zone : zones)
            loaded.push_back(Menus(zone));
        bool dynamic = false;
        std::set<std::string> strings;
        for (const std::string name : HUD_MENUS)
        {
            std::optional<std::set<std::string>> originals;
            const Json* menu = nullptr;
            for (const auto& menus : loaded)
            {
                if (!menus.contains(name))
                    continue;
                if (!originals)
                    originals = StockMenus(*stock, name);
                if (!originals->contains(util::JsonDumpsSorted(menus[name])))
                {
                    menu = &menus[name];
                    break;
                }
            }
            if (!menu)
                continue;
            out.menus.push_back(name);
            const auto& items = (*menu)["items"];
            out.rows.push_back({"hud:" + name, std::to_string(items.size())});
            for (std::size_t i = 0; i < items.size(); i++)
            {
                const auto& item = items[i];
                const auto& window = item["window"];
                const auto expression = [&](const char* key) -> std::string
                {
                    if (item[key].is_null())
                        return "";
                    try
                    {
                        return HudExpression(item[key]);
                    }
                    catch (const std::exception& error)
                    {
                        out.warnings.push_back("HUD_MENU " + name + " item " + std::to_string(i + 1) + " " + key + ": " + error.what());
                        return "";
                    }
                };
                std::string expressions;
                for (const auto& [key, label] : std::array<std::pair<const char*, const char*>, 7>{{{"textExp", "text"},
                                                                                                    {"materialExp", "material"},
                                                                                                    {"rectXExp", "x"},
                                                                                                    {"rectYExp", "y"},
                                                                                                    {"rectWExp", "w"},
                                                                                                    {"rectHExp", "h"},
                                                                                                    {"forecolorAExp", "a"}}})
                    if (const auto code = expression(key); !code.empty())
                        expressions += (expressions.empty() ? "" : "|") + std::string(label) + "=" + code;
                if (!item["materialExp"].is_null())
                {
                    dynamic = true;
                    for (const auto& token : item["materialExp"])
                        if (token.contains("string") && Text(token["string"]).size() >= 3)
                            strings.insert(mod::Lower(Text(token["string"])));
                }
                const auto background = Text(window["background"]);
                const auto art = background.empty() ? std::nullopt : sprite(background);
                const auto scale = item["textScale"].get<double>();
                const auto role = HudFontRole(item["font"].get<int>(), scale);
                const auto type = item["type"].get<int>(), ownerDraw = window["ownerDraw"].get<int>();
                // texts, and the engine's own texts: ammo values, the weapon's name, offhand counts
                if (!item["text"].is_null() || !item["textExp"].is_null() || ownerDraw == 5 || ownerDraw == 81 || ownerDraw == 105 || ownerDraw == 106 || ownerDraw == 119)
                    out.fonts.insert(role);
                out.rows.push_back({"hud:" + name + ":" + std::to_string(i + 1), Rect(window["rect"]), std::to_string(type), std::to_string(ownerDraw),
                                    std::to_string(window["style"].get<int>()), art ? *art : background, Numbers(window["foreColor"]), Numbers(window["backColor"]),
                                    Escape(Text(item["text"])), role, util::FloatRepr(scale), std::to_string(item["textAlignMode"].get<int>()),
                                    util::FloatRepr(item["textAlignX"].get<double>()), util::FloatRepr(item["textAlignY"].get<double>()),
                                    std::to_string(item["textStyle"].get<int>()), expression("visibleExp"), expressions});
            }
        }
        if (dynamic)
            for (const auto& name : ExpressionMaterials(project, roots, strings))
                if (const auto art = sprite(name))
                    out.rows.push_back({"art:" + name, *art});
        return out;
    }
} // namespace w2b::pipeline
