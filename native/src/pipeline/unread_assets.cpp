// Assets nothing reads (see unread_assets.h).
#include "unread_assets.h"

#include "core/files.h"
#include "core/regex.h"
#include "modutil.h"
#include "oat/waw_tables.h"
#include "source_zones.h"

#include <map>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;
    using util::Json;

    namespace
    {
        // what the stage reads: dumped kinds (extract::WAW_ASSETS), the impact table and the HUD menus
        const std::set<std::string> READ{"clipmap",  "clipmap_pvs", "mapents", "gfxworld", "gameworldsp", "material", "techniqueset", "image", "fx", "xmodel",
                                         "weapon",   "xanim",       "sound",   "loadedsound", "rawfile", "comworld", "lightdef", "physpreset",
                                         "physconstraints", "snddriverglobals", "localize", "stringtable", "font", "impactfx", "menu", "menulist", "destructibledef"};

        std::set<std::string> ScriptMenus(const fs::path& project)
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
    } // namespace

    std::vector<std::string> UnreadAssets(const fs::path& project, const std::vector<fs::path>& roots)
    {
        std::vector<std::string> out;
        const auto opened = ScriptMenus(project);
        std::set<std::string> menus;
        for (const auto& zone : SourceZones(roots))
        {
            const auto assets = Json::parse(oat::AssetNames({zone, {}}));
            for (const auto& [type, names] : assets.items())
            {
                if (!READ.contains(type))
                {
                    std::vector<std::string> some;
                    for (const auto& name : names)
                        if (some.size() < 6)
                            some.push_back(name.get<std::string>());
                    out.push_back("UNREAD_ASSET " + std::to_string(names.size()) + " " + type + " in " + files::Utf8(zone.filename()) + ", not converted: " +
                                  mod::Join(some, ", ") + (names.size() > some.size() ? ", ..." : ""));
                }
                if (type == "menu")
                    for (const auto& name : names)
                        if (opened.contains(mod::Lower(name.get<std::string>())))
                            menus.insert(name.get<std::string>());
            }
        }
        if (!menus.empty())
            out.push_back("SCRIPT_MENU not converted, scripts open them: " + mod::Join({menus.begin(), menus.end()}, ", "));
        return out;
    }

    std::set<std::string> SoundsLinkerMissed(const std::vector<fs::path>& roots)
    {
        static const regex::Regex missing("Sound file 'sound[/\\\\]([^']+)' not found", regex::IGNORECASE);
        std::set<std::string> out;
        for (const auto& root : roots)
            if (const auto zone = Fastfile(root))
            {
                const auto log = root / zone->stem(); // the rawfile named as the zone
                if (!mod::IsFile(log))
                    continue;
                std::string text; // Windows-1252
                files::ReadBytes(log, text);
                for (auto& c : text)
                    if (static_cast<unsigned char>(c) >= 0x80)
                        c = '?';
                for (const auto& row : missing.FindAll(text))
                {
                    auto name = mod::Lower(row[0]);
                    for (auto& c : name)
                        if (c == '\\')
                            c = '/';
                    out.insert(name);
                }
            }
        return out;
    }
} // namespace w2b::pipeline
