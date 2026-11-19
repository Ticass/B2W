#include "weapons/stage.h"

#include "core/encoding.h"
#include "core/files.h"

namespace w2b::weapons
{
    std::string StagePhysicsPreset(const std::vector<path>& roots, const path& project, const std::string& name, Json& report)
    {
        const auto renamed = OutputName("physpreset", name);
        if (report.contains(name))
            return renamed;
        const auto source = FindSource(roots, path("physic") / files::FromUtf8(name));
        std::string bytes;
        if (source.empty())
            throw WeaponError("physics preset " + name + ": physic/" + name + " absent from the supplied WaW dumps");
        if (!files::ReadBytes(source, bytes))
            throw std::runtime_error("cannot read " + files::Utf8(source));
        const auto text = encoding::WawText(bytes);
        const auto parts = py::Split(text, "\\");
        if (parts.front() != "PHYSIC" || parts.size() % 2 != 1)
            throw WeaponError("physics preset " + name + ": not a PHYSIC infostring");
        InfoFields fields;
        for (std::size_t i = 1; i < parts.size(); i += 2)
            fields[parts[i]] = parts[i + 1];
        if (const auto& prefix = Field(fields, "sndAliasPrefix"); !prefix.empty())
            fields["sndAliasPrefix"] = "waw/" + prefix;
        files::WriteText(project / "physic" / files::FromUtf8(renamed), WriteInfo(fields, "PHYSIC"));
        report[name] = {{"name", renamed}, {"source", files::Utf8(source)}};
        return renamed;
    }
}
