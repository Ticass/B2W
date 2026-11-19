#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::textures { class IwdRecovery; }

namespace w2b::pipeline
{
    struct BridgeImagesRequest
    {
        std::filesystem::path projectRoot;
        std::vector<std::filesystem::path> imageRoots;
        std::optional<std::vector<std::filesystem::path>> iwdDirs;
        std::vector<std::string> extraImages, materialFiles; // materialFiles: the staged materials' JSON, project-relative
        std::set<std::string> nativeImages;                  // images taken from T6 equivalents
        py::Json knownRecovered;                             // IWD images recovery already decoded
        bool diagnosticLightmap = false, diagnosticProbes = false;
        unsigned workers = 1;
    };

    struct BridgeImages
    {
        std::vector<std::string> written;
        py::Json images; // report image records
        std::vector<std::string> warnings, errors;
        py::Json recovered;
    };

    BridgeImages StageBridgeImages(const BridgeImagesRequest& request, textures::IwdRecovery& recovery);
}
