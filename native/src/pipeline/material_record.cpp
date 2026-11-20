#include "pipeline/material_record.h"

namespace w2b::pipeline
{
    using util::Json;

    Json MaterialRecord::ToJson() const
    {
        Json out{{"name", name}, {"file", file}};
        if (source)
            out["source"] = *source;
        if (sourceName)
            out["source_name"] = *sourceName;
        if (substitutedFrom)
            out["substituted_from"] = *substitutedFrom;
        if (t4Techset)
            out["t4_techset"] = *t4Techset ? Json(**t4Techset) : Json();
        if (t4SortKey)
            out["t4_sort_key"] = *t4SortKey;
        out["t6_techset"] = t6Techset;
        if (!shaderRuntime.is_null())
            out["shader_runtime"] = shaderRuntime;
        if (donor)
            out["donor"] = *donor;
        if (nativeEquivalent)
            out["native_equivalent"] = *nativeEquivalent;
        if (cost)
            out["cost"] = *cost;
        if (notes)
            out["notes"] = *notes;
        return out;
    }

    MaterialRecord MaterialRecord::FromJson(const Json& data)
    {
        const auto text = [&](const char* key) -> std::optional<std::string>
        {
            const auto it = data.find(key);
            return it == data.end() || it->is_null() ? std::nullopt : std::optional(it->get<std::string>());
        };
        MaterialRecord record;
        record.name = data.at("name").get<std::string>();
        record.file = data.at("file").get<std::string>();
        record.source = text("source");
        record.sourceName = text("source_name");
        record.substitutedFrom = text("substituted_from");
        if (const auto it = data.find("t4_techset"); it != data.end())
            record.t4Techset = it->is_null() ? std::optional<std::string>() : std::optional(it->get<std::string>());
        if (const auto it = data.find("t4_sort_key"); it != data.end() && !it->is_null())
            record.t4SortKey = it->get<long long>();
        record.t6Techset = data.at("t6_techset").get<std::string>();
        record.shaderRuntime = data.value("shader_runtime", Json());
        record.donor = text("donor");
        record.nativeEquivalent = text("native_equivalent");
        if (const auto it = data.find("cost"); it != data.end() && !it->is_null())
            record.cost = it->get<long long>();
        if (const auto it = data.find("notes"); it != data.end() && !it->is_null())
            record.notes = it->get<std::vector<std::string>>();
        return record;
    }

    Json ToJson(const std::vector<MaterialRecord>& records)
    {
        auto out = Json::array();
        for (const auto& record : records)
            out.push_back(record.ToJson());
        return out;
    }
} // namespace w2b::pipeline
