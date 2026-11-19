#include "models/models.h"

#include "core/files.h"
#include "effects/effect.h"
#include "weapons/stage.h"

#include <algorithm>

namespace w2b::models
{
    py::Json EffectModelAssets::EntriesJson() const
    {
        auto json = Json::array();
        for (const auto& entry : entries)
            json.push_back({{"source_model", entry.sourceModel}, {"name", entry.name}, {"file", entry.file}, {"lods", entry.lods}});
        return json;
    }

    EffectModelAssets StageEffectAssets(const Path& root, const std::set<std::string>& names)
    {
        EffectModelAssets staged;
        for (const auto& name : names)
        {
            const auto source = root / "xmodel" / files::FromUtf8(name + ".json");
            if (!files::fs::is_regular_file(source))
            {
                staged.errors.push_back("FX xmodel " + name + ": converted WaW model missing");
                continue;
            }
            const auto definition = Json::parse(files::ReadText(source));
            auto game = definition.value("_game", std::string{});
            std::ranges::transform(game, game.begin(), [](char c)
            {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
            });
            const auto lods = definition.find("lods");
            if (game != "t6" || lods == definition.end() || lods->empty())
            {
                staged.errors.push_back("FX xmodel " + name + ": converted WaW model is invalid");
                continue;
            }
            const auto output = std::string(effects::modelPrefix) + name;
            const auto relative = Path("xmodel") / files::FromUtf8(output + ".json");
            weapons::CopySource(source, root / relative);
            EffectModelAsset entry{name, output, weapons::PosixPath(relative), {}};
            for (const auto& lod : *lods)
                entry.lods.push_back(lod.at("file").get<std::string>());
            staged.entries.push_back(std::move(entry));
        }
        return staged;
    }
}
