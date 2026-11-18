#include "paths/aliases.h"

#include "gsc/tokens.h"

#include <format>
#include <unordered_map>
#include <zlib.h>

namespace w2b::paths
{
    namespace
    {
        constexpr int ClearanceSpawnFlags = 0x300000;
        constexpr int ClearanceLinkFlags = 0x28;
        constexpr int MaxNodes = 0xFFFF - 128;
        constexpr std::size_t MaxScriptName = 34;

        std::string_view StringField(const Json& node, std::string_view field)
        {
            const auto entry = node.find(field);
            return entry == node.end() || entry->is_null() ? std::string_view{} : entry->get_ref<const std::string&>();
        }

        int Validate(const Json& source, const Json& nodes, std::string_view vis)
        {
            const auto count = source.at("nodeCount").get<int>();
            if (count < 0 || static_cast<std::size_t>(count) != nodes.size())
                throw Error(std::format("nodeCount {} but {} nodes", count, nodes.size()));
            if (count > MaxNodes)
                throw Error(std::format("{} path nodes exceed the uint16 node index range", count));
            const auto visBytes = source.value("visBytes", std::int64_t{});
            const auto bits = static_cast<std::int64_t>(count) * (count - 1);
            const auto lower = bits / 8, upper = (bits + 7) / 8;
            if (visBytes != 0 && visBytes != lower && visBytes != upper)
            {
                const auto sizes = lower == upper ? std::format("[{}]", lower) : std::format("[{}, {}]", lower, upper);
                throw Error(std::format("visBytes {} is outside {} for {} nodes; unknown vis layout", visBytes, sizes, count));
            }
            if (vis.size() != static_cast<std::size_t>(visBytes) * 2)
                throw Error("pathVis hex length does not match visBytes");
            return count;
        }

        std::string ScriptName(std::string_view source, const Alias& alias, const Json& calls, Json& generated)
        {
            const auto script = gsc::Lower(source);
            const auto dog = calls.find(script);
            if (dog == calls.end() || dog->empty())
                return alias ? std::string(Prefix) + *alias : std::string(Mantle);
            const auto key = std::string(alias ? std::string_view(*alias) : Mantle) + "|" + script;
            const auto hash = crc32(0, reinterpret_cast<const Bytef*>(key.data()), static_cast<uInt>(key.size()));
            const auto name = std::format("{}t{:08x}", Prefix, hash);
            generated[name] = {alias ? Json(*alias) : Json(), *dog};
            return name;
        }

        int NodeType(int wawType)
        {
            if (wawType == 8 || wawType == 9)
                return wawType - 2;
            return wawType >= 10 ? wawType + 1 : wawType;
        }

        Json ConvertNode(const Json& node, std::string_view script, std::size_t index, int count, std::size_t& links)
        {
            auto converted = Json::array();
            for (const auto& link : node.at("links"))
            {
                const auto target = link.at(0).get<int>();
                if (target < 0 || target >= count)
                    throw Error(std::format("node {} links to node {} of {}", index, target, count));
                converted.push_back({{"node", target}, {"dist", link.at(1)}, {"disconnectCount", link.at(2)},
                                     {"negotiationLink", link.at(3)}, {"flags", ClearanceLinkFlags}});
            }
            links += converted.size();
            return {{"type", NodeType(node.at("type").get<int>())}, {"spawnflags", node.at("spawnflags").get<std::int64_t>() | ClearanceSpawnFlags},
                    {"targetname", node.at("targetname")}, {"script_linkname", node.at("script_linkname")},
                    {"script_noteworthy", node.at("script_noteworthy")}, {"target", node.at("target")},
                    {"animscript", node.at("animscript").is_null() ? Json() : Json(script)},
                    {"animscriptfunc", node.at("animscriptfunc")}, {"origin", node.at("origin")},
                    {"angle", node.at("angle")}, {"forward", node.at("forward")}, {"radius", node.at("radius")},
                    {"minUseDistSq", node.at("minUseDistSq")}, {"overlap", node.at("overlap")},
                    {"links", std::move(converted)}};
        }

        void ValidateNames(const Names& names)
        {
            std::string tooLong;
            for (const auto& name : names)
                if (name.starts_with(Prefix) && name.size() > MaxScriptName)
                {
                    if (!tooLong.empty())
                        tooLong += ", ";
                    tooLong += py::StringRepr(name);
                }
            if (!tooLong.empty())
                throw Error(std::format("traversal script names over T6's {} characters: [{}]", MaxScriptName, tooLong));
        }
    }

    Names SourceScripts(const Json& manifest)
    {
        Names names;
        for (const auto& node : manifest.at("nodes"))
            if (const auto script = StringField(node, "animscript"); node.at("type") == NegotiationBegin && !script.empty())
                names.emplace(script);
        return names;
    }

    Json Convert(const Json& source, const std::optional<Names>& substates, const Json& dogCalls)
    {
        const auto& nodes = source.at("nodes");
        const auto vis = StringField(source, "pathVis");
        const auto count = Validate(source, nodes, vis);
        Aliases aliases(substates);
        std::unordered_map<std::string_view, const Json*> targets;
        for (const auto& node : nodes)
            if (const auto name = StringField(node, "targetname"); !name.empty())
                targets[name] = &node;
        auto calls = Json::object(), generated = Json::object(), renamed = Json::object();
        for (const auto& [key, entry] : dogCalls.items())
            calls[gsc::Lower(key)] = entry;
        Names scriptNames;
        std::map<std::size_t, std::string> perNode;
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const auto& node = nodes.at(i);
            const auto script = StringField(node, "animscript");
            if (node.at("type") != NegotiationBegin || script.empty() || (!substates && renamed.contains(script)))
                continue;
            std::optional<double> height;
            if (const auto target = targets.find(StringField(node, "target")); target != targets.end())
                height = target->second->at("origin").at(2).get<double>() - node.at("origin").at(2).get<double>();
            auto name = ScriptName(script, aliases.Select(script, height), calls, generated);
            scriptNames.insert(name);
            if (!renamed.contains(script))
                renamed[script] = name;
            if (substates)
                perNode[i] = std::move(name);
        }

        std::size_t links = 0;
        auto converted = Json::array();
        converted.get_ref<Json::array_t&>().reserve(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const auto& node = nodes.at(i);
            auto script = StringField(node, "animscript");
            if (const auto per = perNode.find(i); per != perNode.end())
                script = per->second;
            else if (const auto replacement = renamed.find(script); replacement != renamed.end())
                script = replacement->get_ref<const std::string&>();
            converted.push_back(ConvertNode(node, script, i, count, links));
        }
        ValidateNames(scriptNames);
        auto traversals = renamed;
        if (!perNode.empty())
        {
            traversals = Json::object();
            for (const auto& [index, name] : perNode)
            {
                const auto key = std::string(StringField(nodes.at(index), "animscript")) + " -> " + name;
                auto& seen = traversals[key];
                seen = seen.is_null() ? "1" : std::to_string(std::stoi(seen.get_ref<const std::string&>()) + 1);
                renamed["#" + std::to_string(index)] = name;
            }
        }
        const auto visBytes = vis.size() / 2;
        return {{"manifest", {{"version", 1}, {"nodeCount", count}, {"visBytes", visBytes}, {"pathVis", vis},
                              {"nodes", std::move(converted)}}}, {"renamed", std::move(renamed)},
                {"summary", {{"nodes", count}, {"links", links}, {"vis_bytes", visBytes}, {"traversals", std::move(traversals)},
                             {"scripts", Json::array()}, {"generated", std::move(generated)}}}};
    }
}
