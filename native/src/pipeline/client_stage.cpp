// The client scripts of the converted map (see client_stage.h).
#include "client_stage.h"

#include "extract/cache.h"
#include "gsc/port/api.h"
#include "gsc/port/clientport.h"
#include "gsc/port/generators.h"
#include "modutil.h"
#include "weapons/weapon.h"

#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::IsFile;
    using py::Json;

    namespace
    {
        void ReplaceFirst(std::string& text, const std::string& from, const std::string& to)
        {
            if (const auto at = text.find(from); at != std::string::npos)
                text.replace(at, from.size(), to);
        }

        // t6bridge.flame_environment_source: allocate T6's native flame pools
        // for a carried source gas weapon (never force the enabled dvar)
        std::string FlameEnvironment(std::string source, const fs::path& content)
        {
            const auto runtimeFile = content / "weapons.runtime.json";
            if (!IsFile(runtimeFile))
                return source;
            const auto runtime = mod::Json(runtimeFile);
            for (const auto& name : runtime.value("weapons", Json::array()))
            {
                const auto path = content / "weapons" / files::FromUtf8(runtime.at("table").at(name.get<std::string>()).get<std::string>());
                if (!IsFile(path))
                    continue;
                const auto fields = weapons::ReadInfo(path);
                bool flame = false;
                for (const auto key : weapons::FlameTableKeys)
                    flame = flame || !weapons::Field(fields, key).empty();
                if (weapons::Field(fields, "weaponType") == "gas" && flame)
                {
                    ReplaceFirst(source, "init()\n{", "init()\n{\n    // Allocate native pools; never force the enabled dvar.\n    enableflamesystem();");
                    return source;
                }
            }
            return source;
        }
    } // namespace

    Json FinishClientScripts(const fs::path& requestPath)
    {
        const auto request = mod::Json(requestPath);
        const auto projectRoot = files::FromUtf8(request["project_root"].get<std::string>());
        const auto project = request["project"].get<std::string>();
        std::vector<fs::path> roots;
        for (const auto& r : request["roots"])
            roots.push_back(files::FromUtf8(r.get<std::string>()));
        std::map<std::string, std::string> fxTable;
        for (const auto& [k, v] : request["fx_table"].items())
            fxTable[k] = v.get<std::string>();
        std::set<std::string> soundNames;
        for (const auto& n : request["sound_names"])
            soundNames.insert(n.get<std::string>());
        const auto compat = extract::ResourceRoot() / "src/waw2bo2/compat";
        gsc::port::SetCompatDir(compat);
        const auto content = projectRoot / "content_source";
        std::map<std::string, std::string> weaponTable;
        if (IsFile(content / "weapons.runtime.json"))
            for (const auto& [k, v] : mod::Json(content / "weapons.runtime.json").at("table").items())
                weaponTable[k] = v.get<std::string>();
        Json warnings = Json::array();
        auto environment = FlameEnvironment(files::ReadText(compat / "_waw2bo2_environment.csc"), content);
        const auto tesla = gsc::port::clientport::StageTesla(roots, projectRoot, fxTable, weaponTable, soundNames);
        if (!tesla.empty())
            ReplaceFirst(environment, "init()\n{", "init()\n{\n" + tesla);
        files::WriteText(projectRoot / "clientscripts/mp/waw/_waw2bo2_environment.csc", environment);
        const auto clientMain = projectRoot / "clientscripts/mp" / files::FromUtf8(project + ".csc");
        if (request.value("characters", false))
            gsc::port::HookBo2ClientCharacters(clientMain);
        auto clientText = files::ReadText(clientMain);
        // The donor's fog-volume monitor selects its own vision every frame.
        for (const std::string fog = "    level thread init_fog_vol_to_visionset();\n"; clientText.find(fog) != std::string::npos;)
            clientText.erase(clientText.find(fog), fog.size()); // str.replace: every occurrence
        const std::string hook = "    clientscripts\\mp\\waw\\_waw2bo2_environment::init();\n";
        if (clientText.find(std::string(py::Strip(hook))) == std::string::npos)
            ReplaceFirst(clientText, "    start_zombie_stuff();", "    start_zombie_stuff();\n" + hook);
        auto mapClient = request.value("map_client", Json());
        if (!mapClient.is_null() && !(mapClient.is_object() && mapClient.empty()))
        {
            // the map's client main runs once BO2's client main has set up its runtime
            const auto clientHook = gsc::port::clientport::FinishMapClient(projectRoot, mapClient, fxTable, weaponTable, soundNames);
            for (const auto& name : mapClient.value("unbound_fx", Json::array()))
                warnings.push_back("CLIENT_FX_UNBOUND " + name.get<std::string>() + ": loaded by the map's client scripts, not converted");
            const std::string marker = "    waitforclient( 0 );";
            if (clientText.find(std::string(py::Strip(clientHook))) == std::string::npos)
            {
                if (clientText.find(marker) == std::string::npos)
                    return Json{{"value_error", files::Utf8(clientMain.filename()) + ": no waitforclient( 0 ) to start the map client before"}};
                ReplaceFirst(clientText, marker, clientHook + marker);
            }
        }
        files::WriteText(clientMain, clientText);
        return Json{{"warnings", warnings}, {"map_client", mapClient}};
    }
} // namespace w2b::pipeline
