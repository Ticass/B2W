#include "textures/iwd_service.h"

#include "core/files.h"
#include "core/pytext.h"
#include "textures/dds.h"
#include "textures/iwd_recovery.h"

#include <istream>
#include <ostream>

namespace w2b::textures
{
    namespace
    {
        using py::Json;
        using files::fs::path;

        std::vector<path> Paths(const Json& values)
        {
            std::vector<path> paths;
            for (const auto& value : values)
                paths.push_back(files::FromUtf8(value.get<std::string>()));
            return paths;
        }

        Json ImageJson(const RecoveredImage& image)
        {
            auto result = Json{{"path", image.path.empty() ? Json(nullptr) : Json(files::Utf8(image.path))}};
            if (!image.archive.empty())
            {
                auto metadata = Json{{"archive", files::Utf8(image.archive)}, {"entry", image.entry},
                                     {"iwi_format", image.iwiFormat}};
                if (image.decoderTables)
                    metadata["decoder_tables"] = files::Utf8(*image.decoderTables);
                result["recovered"] = std::move(metadata);
            }
            return result;
        }
    }

    void ServeIwdRecovery(std::istream& input, std::ostream& output)
    {
        std::string line;
        if (!std::getline(input, line))
            return;
        const auto config = Json::parse(line);
        const auto binary = config.at("binary").is_null() ? std::nullopt :
            std::optional(files::FromUtf8(config.at("binary").get<std::string>()));
        IwdRecovery recovery(Paths(config.at("directories")), files::FromUtf8(config.at("root").get<std::string>()),
                             binary, Paths(config.at("source_roots")), Paths(config.at("overrides")));
        const auto send = [&](const Json& reply)
        {
            if (!(output << reply.dump(-1, ' ', true) << std::endl))
                throw std::runtime_error("could not send IWD recovery reply");
        };
        send({{"ready", true}});
        while (std::getline(input, line))
        {
            const auto request = Json::parse(line);
            const auto stem = request.at("stem").get<std::string>();
            try
            {
                if (request.contains("source"))
                {
                    const auto& source = request.at("source");
                    send(ImageJson(recovery.Resolve(stem, source.is_null() ? path{} :
                                                              files::FromUtf8(source.get<std::string>()))));
                }
                else
                    send(ImageJson(recovery.Recover(stem)));
            }
            catch (const TextureError& error)
            {
                send({{"error", error.what()}, {"kind", "texture"}});
            }
            catch (const files::fs::filesystem_error& error)
            {
                send({{"error", error.what()}, {"kind", "io"}});
            }
        }
    }
}
