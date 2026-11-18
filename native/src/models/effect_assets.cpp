#include "models/models.h"

#include "core/files.h"
#include "effects/effect.h"
#include "weapons/stage.h"

#include <algorithm>

namespace w2b::models
{
    Json StageEffectAssets(const Path& root, const std::set<std::string>& names)
    {
        auto errors = Json::array();
        auto entries = Json::array();
        for (const auto& name : names)
        {
            const auto source = root / "xmodel" / files::FromUtf8(name + ".json");
            if (!files::fs::is_regular_file(source))
            {
                errors.push_back("FX xmodel " + name + ": converted WaW model missing");
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
                errors.push_back("FX xmodel " + name + ": converted WaW model is invalid");
                continue;
            }
            const auto output = std::string(effects::modelPrefix) + name;
            const auto relative = Path("xmodel") / files::FromUtf8(output + ".json");
            weapons::CopySource(source, root / relative);
            auto lodFiles = Json::array();
            for (const auto& lod : *lods)
                lodFiles.push_back(lod.at("file"));
            entries.push_back({{"source_model", name}, {"name", output}, {"file", weapons::PosixPath(relative)},
                               {"lods", std::move(lodFiles)}});
        }
        return {{"entries", std::move(entries)}, {"errors", std::move(errors)}};
    }

    Json StageEffectAssetsRequest(const Path& requestPath)
    {
        const auto request = Json::parse(files::ReadText(requestPath));
        return StageEffectAssets(files::FromUtf8(request.at("project").get<std::string>()),
                                 request.at("names").get<std::set<std::string>>());
    }
}
