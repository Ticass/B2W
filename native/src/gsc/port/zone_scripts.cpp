#include "gsc/port/zone_scripts.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/pyre.h"

namespace w2b::gsc::port
{
    namespace
    {
        constexpr std::string_view Gone =
            "// waw2bo2: WaW drives zones: the map's own zone manager runs (none here: BO2's never starts)";

        std::string ReadScript(const std::filesystem::path& path)
        {
            std::string bytes;
            if (!files::ReadBytes(path, bytes))
                throw std::runtime_error("cannot read " + files::Utf8(path));
            return encoding::UniversalNewlines(encoding::Utf8Replace(bytes));
        }

        bool DropManager(std::string& script)
        {
            if (script.find(Gone) != script.npos && script.find("_zm_zonemgr::manage_zones") == script.npos)
                return true;
            static const pyre::Regex pattern(
                R"re(^\s*level\.zone_manager_init_func = ::\w+_zone_init;\n)re"
                R"re(\s*init_zones = \[\];\n(?:\s*init_zones\[\d+\] = "[^"]*";\n)+)re"
                R"re(\s*level thread maps\\mp\\zombies\\_zm_zonemgr::manage_zones\( init_zones \);\n)re",
                pyre::MULTILINE);
            std::size_t count = 0;
            auto output = pattern.Sub("    " + std::string(Gone) + "\n", script, 1, &count);
            if (count == 0)
                return false;
            script = std::move(output);
            return true;
        }

        bool GuardSwitch(std::string& script)
        {
            constexpr std::string_view marker = "    master_switch notsolid();";
            constexpr std::string_view guard = "    if ( !isdefined( trig ) || !isdefined( master_switch ) )\n        return;\n";
            const auto start = script.find(marker);
            if (start == script.npos || script.find(guard) != script.npos)
                return false;
            script.insert(start, guard);
            return true;
        }
    }

    ZoneGraph ReadZoneGraphFile(const std::filesystem::path& source)
    {
        return ReadZoneGraph(ReadScript(source));
    }

    ZoneMainResult StageZoneMain(const std::filesystem::path& main, bool dropManager, bool guardSwitch)
    {
        auto script = ReadScript(main);
        ZoneMainResult result;
        const auto dropped = dropManager && DropManager(script);
        if (dropManager && !dropped)
            result.error = "template zone manager start not found in the map script";
        result.guardedSwitch = guardSwitch && GuardSwitch(script);
        if (dropped || result.guardedSwitch)
            files::WriteText(main, script);
        return result;
    }

    py::Json ZoneScriptsRequest(const std::filesystem::path& path)
    {
        const auto request = py::Json::parse(files::ReadText(path));
        auto result = py::Json::object();
        if (const auto source = request.find("source"); source != request.end())
        {
            result = ReadZoneGraphFile(files::FromUtf8(source->get<std::string>())).ToJson();
        }
        if (const auto main = request.find("main"); main != request.end())
        {
            const auto staged = StageZoneMain(files::FromUtf8(main->get<std::string>()),
                                              request.value("dropManager", false), request.value("guardSwitch", false));
            result["error"] = staged.error;
            result["guardedSwitch"] = staged.guardedSwitch;
        }
        return result;
    }
}
