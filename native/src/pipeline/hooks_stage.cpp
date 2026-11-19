// t6bridge.stage_bridge, the hooks on the ported map main: BO2 entry points,
// characters, weapons, box, template inits, perk ownership, the perk table,
// native zombie support and animtrees; the owned client bootstrap.
#include "hooks_stage.h"

#include "gsc/port/api.h"
#include "gsc/port/generators.h"
#include "gsc/port/support.h"
#include "gsc/port/zombiebehavior.h"
#include "extract/cache.h"
#include "modutil.h"

namespace w2b::pipeline
{
    using py::Json;
    namespace port = gsc::port;

    Json HookMapScripts(const fs::path& requestPath)
    {
        const auto request = mod::Json(requestPath);
        const auto projectRoot = files::FromUtf8(request["project_root"].get<std::string>());
        const auto project = request["project"].get<std::string>();
        const std::optional<fs::path> bo2Root = request["bo2_root"].is_null() ? std::nullopt : std::optional(files::FromUtf8(request["bo2_root"].get<std::string>()));
        const bool stockPerks = request.value("bo2_stock_perks", false);
        port::SetCompatDir(extract::ResourceRoot() / "src/waw2bo2/compat");
        Json warnings = Json::array(), errors = Json::array(), content = Json::object();
        const auto mainGsc = projectRoot / "maps" / "mp" / files::FromUtf8(project + ".gsc");
        const auto mainCsc = projectRoot / "clientscripts" / "mp" / files::FromUtf8(project + ".csc");
        if (request.value("scripts", false) && mod::IsFile(mainGsc))
        {
            try
            {
                port::HookBo2Main(mainGsc, request["map_name"].get<std::string>());
                if (request.value("characters", false))
                    port::HookBo2Characters(mainGsc);
                port::HookBo2Weapons(mainGsc);
                port::HookBo2Box(mainGsc);
                for (const auto& line : port::DropPreservedTemplateInits(mainGsc, projectRoot, mainCsc))
                    warnings.push_back("template init dropped, the ported WaW script owns it: " + line);
                if (!bo2Root)
                    throw std::runtime_error("BO2 runtime sources required to stage WaW perk ownership");
                port::StageBo2PerkSupport(projectRoot, *bo2Root, stockPerks);
                const auto perks = port::perktable::Stage(projectRoot);
                content["emulated_perks"] = perks;
                if (perks.contains("error"))
                    warnings.push_back("PERK_TABLE_UNAVAILABLE " + perks["error"].get<std::string>());
                else if (!perks["emulated"].empty())
                    warnings.push_back("EMULATED_PERKS " + mod::Join(perks["emulated"].get<std::vector<std::string>>(), ", ") +
                                       ": the map's perk names T6's perk table lacks; kept in a script-side table");
                port::zombiebehavior::StageNativeSupport(projectRoot, *bo2Root, port::Api()["zombie_runtime"]);
                if (stockPerks)
                    warnings.push_back("BO2_STOCK_PERKS: native BO2 perk machines, purchases, HUD and last stand; the WaW Pack-a-Punch stays");
                port::HookBo2PerkServer(mainGsc);
                port::HookBo2PerkClient(mainCsc);
                port::HookBo2Animtrees(mainGsc, mainCsc, projectRoot, request.value("staged_animtrees", Json::array()).get<std::vector<std::string>>());
            }
            catch (const fs::filesystem_error&)
            {
                throw;
            }
            catch (const std::runtime_error& exc)
            {
                errors.push_back(std::string("scripts: ") + exc.what());
            }
        }
        // a cancelled linker run can leave the generated client entry point absent
        if (bo2Root)
        {
            try
            {
                port::EnsureOwnedClientBootstrap(projectRoot, *bo2Root);
            }
            catch (const fs::filesystem_error&)
            {
                throw;
            }
            catch (const std::runtime_error& exc)
            {
                errors.push_back(std::string("scripts: ") + exc.what());
            }
        }
        return Json{{"warnings", warnings}, {"errors", errors}, {"content", content}};
    }
} // namespace w2b::pipeline
