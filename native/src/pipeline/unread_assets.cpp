// Assets nothing reads (see unread_assets.h).
#include "unread_assets.h"

#include "core/files.h"
#include "core/regex.h"
#include "modutil.h"
#include "oat/waw_tables.h"
#include "script_menus.h"
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
    } // namespace

    std::vector<std::string> UnreadAssets(const fs::path& project, const std::vector<fs::path>& roots)
    {
        std::vector<std::string> out;
        auto missing = ScriptOpenedMenus(project); // the ones no zone of the map defines
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
                        missing.erase(mod::Lower(name.get<std::string>()));
            }
        }
        // WaW's own (briefing, the pause menus) live in its stock zones
        if (!missing.empty())
            out.push_back("SCRIPT_MENU_NOT_IN_MAP scripts open " + mod::Join({missing.begin(), missing.end()}, ", ") + ": not in the map's zones (stock WaW's or none)");
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
