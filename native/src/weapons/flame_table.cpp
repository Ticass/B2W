#include "weapons/stage.h"

#include "core/files.h"

#include <algorithm>
#include <charconv>

namespace w2b::weapons
{
    namespace
    {
        void ClampTravel(Json& fields)
        {
            for (const std::string_view key : {"flameVar_fireBrightness", "flameVar_dripsBrightness"})
                if (!fields.contains(key))
                    fields[key] = "1";
            const auto reach = std::max(NumericField(fields, "flameVar_streamFuelLength"),
                                        NumericField(fields, "flameVar_streamFlameLength"));
            const auto speed = NumericField(fields, "flameVar_streamChunkSpeed");
            const auto duration = NumericField(fields, "flameVar_streamChunkDuration");
            if (!(reach > 0 && speed > 0 && duration > reach / speed))
                return;
            char text[64];
            const auto [end, error] = std::to_chars(text, text + sizeof(text), reach / speed, std::chars_format::general, 9);
            if (error != std::errc{})
                throw WeaponError("could not format flame-table duration");
            fields["flameVar_streamChunkDuration"] = std::string(text, end);
        }
    }

    Json StageFlameTables(const std::vector<path>& roots, const path& project, Json& dependencies, const path& schema)
    {
        auto entries = Json::array();
        const auto names = GetDependencies(dependencies, "flametable");
        const auto fieldsAllowed = ReadSchema(schema);
        for (const auto& name : names)
        {
            const auto output = OutputName("flametable", name);
            const auto source = FindSource(roots, "weapons" / files::FromUtf8(name));
            if (source.empty())
                throw WeaponError("missing compiled flame table: " + name);
            auto fields = ReadInfo(source, "FLAMETABLEFILE");
            Names unknown;
            for (const auto& [key, value] : fields.items())
                if (!fieldsAllowed.contains(key))
                    unknown.insert(key);
            if (!unknown.empty())
                throw WeaponError("flame table " + name + ": unknown T6 fields " + NameList(unknown));
            ClampTravel(fields);
            fields["name"] = output;
            for (const auto key : FlameMaterialKeys)
                if (const auto& value = Field(fields, key); !value.empty())
                {
                    AddDependency(dependencies, "material", value);
                    fields[key] = OutputName("material", value);
                }
            for (const auto key : FlameSoundKeys)
                if (const auto& value = Field(fields, key); !value.empty())
                {
                    OutputName("sound", value);
                    AddDependency(dependencies, "sound", value);
                    fields[key] = "waw/" + value;
                }
            const auto relative = "weapons" / files::FromUtf8(output);
            files::WriteText(project / relative, WriteInfo(fields, "FLAMETABLEFILE"));
            entries.push_back({{"name", output}, {"waw", name}, {"source", files::Utf8(source)},
                               {"file", PosixPath(relative)}, {"fields", fields.size()}});
        }
        return entries;
    }
}
