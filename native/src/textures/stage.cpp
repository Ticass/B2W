#include "textures/stage.h"

#include "core/files.h"
#include "core/parallel.h"

namespace w2b::textures
{
    util::Json HeaderJson(const IwiHeader& header)
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
}
