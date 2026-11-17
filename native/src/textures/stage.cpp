#include "textures/stage.h"

#include "core/files.h"
#include "core/parallel.h"

namespace w2b::textures
{
    py::Json HeaderJson(const IwiHeader& header)
    {
        return {{"format", static_cast<unsigned>(header.format)}, {"flags", header.flags}, {"width", header.width},
                {"height", header.height}, {"depth", header.depth}, {"fileSizeForPicmip", header.fileSizes}};
    }

    IwiHeader ConvertFile(const files::fs::path& source, const files::fs::path& destination, bool faceMajor, bool baseOnly)
    {
        std::string bytes;
        if (!files::ReadBytes(source, bytes))
            throw std::runtime_error("could not read DDS " + files::Utf8(source));
        auto result = DdsToIwi(bytes, faceMajor, baseOnly);
        files::WriteBytes(destination, result.bytes);
        return result.header;
    }

    py::Json ConvertBatch(const files::fs::path& request)
    {
        using py::Json;
        const auto args = Json::parse(files::ReadText(request));
        const auto& groups = args.at("groups");
        std::vector<Json> output(groups.size());
        // A destination may be shared by multiple asset names. Only groups
        // with different destinations may run concurrently.
        parallel::For(groups.size(), [&](std::size_t index)
        {
            auto converted = Json::array();
            for (const auto& task : groups.at(index))
            {
                try
                {
                    const auto header = ConvertFile(files::FromUtf8(task.at(0).get<std::string>()),
                                                    files::FromUtf8(task.at(1).get<std::string>()));
                    converted.push_back(HeaderJson(header));
                }
                catch (const TextureError& error)
                {
                    converted.push_back({{"error", error.what()}});
                }
            }
            output[index] = std::move(converted);
        }, args.at("workers").get<unsigned>());
        return output;
    }
}
