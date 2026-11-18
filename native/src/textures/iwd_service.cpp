#include "textures/iwd_service.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/pytext.h"
#include "textures/dds.h"
#include "textures/iwd_recovery.h"

#include <cstdio>
#include <io.h>
#include <istream>
#include <process.h>
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

    void ServeIwdRecovery(std::istream& input, std::ostream& output, const std::map<std::string, RecoveryOperation>& operations)
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
            if (request.contains("op"))
            {
                // {"op", "request"}: {"result"} or {"error", "kind": "operation"}
                // the operation's own progress output must not reach the reply
                // stream: it goes to a file and returns with the reply
                Json reply;
                std::string log;
                {
                    std::fflush(stdout);
                    output.flush();
                    const auto logPath = files::fs::temp_directory_path() / ("w2b_operation_" + std::to_string(_getpid()) + ".log");
                    const int saved = _dup(1);
                    FILE* sink = _wfopen(logPath.c_str(), L"wb");
                    if (saved >= 0 && sink)
                        _dup2(_fileno(sink), 1);
                    try
                    {
                        const auto& operation = operations.at(request.at("op").get<std::string>());
                        reply = {{"result", operation(request.at("request"), recovery)}};
                    }
                    catch (const std::exception& error)
                    {
                        reply = {{"error", error.what()}, {"kind", "operation"}};
                    }
                    std::fflush(stdout);
                    if (saved >= 0)
                    {
                        _dup2(saved, 1);
                        _close(saved);
                    }
                    if (sink)
                        std::fclose(sink);
                    files::ReadBytes(logPath, log);
                    std::error_code e;
                    files::fs::remove(logPath, e);
                }
                reply["log"] = encoding::Utf8Replace(log);
                send(reply);
                continue;
            }
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
