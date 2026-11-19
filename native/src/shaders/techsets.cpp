#include "shaders/techsets.h"

#include "core/files.h"

#include <nlohmann/json.hpp>

namespace w2b::shaders
{
    namespace
    {
        using py::Json;
        using Sorted = nlohmann::json; // the dumper's sorted key order

        T6Pass PassFromJson(const Json& data)
        {
            T6Pass pass;
            for (const auto& arg : data.at("args"))
                pass.args.push_back({arg.at("buffer").get<long long>(), arg.at("location").get<long long>(), arg.at("size").get<long long>(),
                                     arg.at("type").get<long long>(), arg.at("u").at("value").get<long long>()});
            pass.customSamplerFlags = data.at("customSamplerFlags").get<long long>();
            pass.materialType = data.at("materialType").get<long long>();
            pass.perObjArgCount = data.at("perObjArgCount").get<long long>();
            pass.perPrimArgCount = data.at("perPrimArgCount").get<long long>();
            pass.precompiledIndex = data.at("precompiledIndex").get<long long>();
            pass.stableArgCount = data.at("stableArgCount").get<long long>();
            pass.pixelShader = data.at("pixelShader").at("name").get<std::string>();
            pass.vertexShader = data.at("vertexShader").at("name").get<std::string>();
            const auto& decl = data.at("vertexDecl");
            pass.vertexDecl.hasOptionalSource = decl.at("hasOptionalSource").get<bool>();
            pass.vertexDecl.isLoaded = decl.at("isLoaded").get<bool>();
            pass.vertexDecl.streamCount = decl.at("streamCount").get<long long>();
            for (const auto& route : decl.at("routing"))
                pass.vertexDecl.routing.emplace_back(route.at("source").get<long long>(), route.at("dest").get<long long>());
            return pass;
        }

        Sorted ToSorted(const T6Pass& pass)
        {
            Sorted args = Sorted::array();
            for (const auto& arg : pass.args)
                args.push_back({{"buffer", arg.buffer}, {"location", arg.location}, {"size", arg.size}, {"type", arg.type}, {"u", {{"value", arg.value}}}});
            Sorted routing = Sorted::array();
            for (const auto& [source, dest] : pass.vertexDecl.routing)
                routing.push_back({{"source", source}, {"dest", dest}});
            return {{"args", std::move(args)},
                    {"customSamplerFlags", pass.customSamplerFlags},
                    {"materialType", pass.materialType},
                    {"perObjArgCount", pass.perObjArgCount},
                    {"perPrimArgCount", pass.perPrimArgCount},
                    {"pixelShader", {{"name", pass.pixelShader}}},
                    {"precompiledIndex", pass.precompiledIndex},
                    {"stableArgCount", pass.stableArgCount},
                    {"vertexDecl",
                     {{"hasOptionalSource", pass.vertexDecl.hasOptionalSource},
                      {"isLoaded", pass.vertexDecl.isLoaded},
                      {"routing", std::move(routing)},
                      {"streamCount", pass.vertexDecl.streamCount}}},
                    {"vertexShader", {{"name", pass.vertexShader}}}};
        }

        std::optional<long long> OptionalInt(const Json& data, const char* key)
        {
            const auto it = data.find(key);
            return it == data.end() || it->is_null() ? std::nullopt : std::optional(it->get<long long>());
        }
    } // namespace

    T6TechniqueSet T6TechniqueSetFromJson(const Json& data)
    {
        T6TechniqueSet set;
        set.name = data.at("name").get<std::string>();
        set.worldVertFormat = data.at("worldVertFormat").get<long long>();
        for (const auto& technique : data.at("techniques"))
        {
            T6TechniqueSlot slot;
            if (technique.is_null())
                slot.dropped = true;
            else if (!technique.empty())
            {
                T6Technique t;
                t.flags = technique.at("flags").get<long long>();
                t.passCount = technique.at("passCount").get<long long>();
                t.name = technique.at("name").get<std::string>();
                for (const auto& pass : technique.at("passArray"))
                    t.passes.push_back(PassFromJson(pass));
                slot.technique = std::move(t);
            }
            set.techniques.push_back(std::move(slot));
        }
        return set;
    }

    Json ToJson(const T6TechniqueSet& set)
    {
        Sorted techniques = Sorted::array();
        for (const auto& slot : set.techniques)
        {
            if (!slot.technique)
            {
                techniques.push_back(slot.dropped ? Sorted() : Sorted::object());
                continue;
            }
            Sorted passes = Sorted::array();
            for (const auto& pass : slot.technique->passes)
                passes.push_back(ToSorted(pass));
            techniques.push_back({{"flags", slot.technique->flags},
                                  {"name", slot.technique->name},
                                  {"passArray", std::move(passes)},
                                  {"passCount", slot.technique->passCount}});
        }
        return Json(Sorted{{"name", set.name}, {"techniques", std::move(techniques)}, {"worldVertFormat", set.worldVertFormat}});
    }

    T6TechniqueSet ReadT6TechniqueSet(const std::filesystem::path& path)
    {
        return T6TechniqueSetFromJson(Json::parse(files::ReadText(path)));
    }

    std::set<std::string> ProgramFiles(const T6TechniqueSet& set)
    {
        std::set<std::string> found;
        for (const auto& slot : set.techniques)
            if (slot)
                for (const auto& pass : slot.technique->passes)
                {
                    if (!pass.pixelShader.empty())
                        found.insert("shader_bin/ps_" + pass.pixelShader + ".cso");
                    if (!pass.vertexShader.empty())
                        found.insert("shader_bin/vs_" + pass.vertexShader + ".cso");
                }
        return found;
    }

    WawTechniqueSet ReadWawTechniqueSet(const std::filesystem::path& path)
    {
        const auto data = Json::parse(files::ReadText(path));
        WawTechniqueSet set;
        set.name = data.at("name").get<std::string>();
        for (const auto& technique : data.at("techniques"))
        {
            if (technique.is_null() || technique.empty())
            {
                set.techniques.emplace_back();
                continue;
            }
            WawTechnique t;
            t.name = technique.at("name").get<std::string>();
            for (const auto& pass : technique.at("passes"))
            {
                WawPass p;
                p.pixelShader = pass.at("pixelShader").get<std::string>();
                p.vertexShader = pass.at("vertexShader").get<std::string>();
                for (const auto& arg : pass.at("args"))
                {
                    WawPassArg a;
                    a.dest = arg.at("dest").get<long long>();
                    a.frequency = arg.at("frequency").get<long long>();
                    a.type = arg.at("type").get<long long>();
                    a.value = OptionalInt(arg, "value");
                    if (const auto it = arg.find("literal"); it != arg.end() && !it->is_null())
                        a.literal = it->get<std::vector<double>>();
                    a.index = OptionalInt(arg, "index");
                    a.firstRow = OptionalInt(arg, "firstRow");
                    a.rowCount = OptionalInt(arg, "rowCount");
                    p.args.push_back(std::move(a));
                }
                t.passes.push_back(std::move(p));
            }
            set.techniques.emplace_back(std::move(t));
        }
        return set;
    }
} // namespace w2b::shaders
