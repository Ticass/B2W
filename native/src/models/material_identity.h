#pragma once

#include "models/identities.h"

#include <string>
#include <vector>

namespace w2b::models
{
    struct MaterialFile
    {
        std::string name, file; // the staged material and its project-relative JSON
    };

    // a staged material moved to its own name; index is its place in the input list
    struct MaterialRename
    {
        std::size_t index = 0;
        std::string name, file, sourceName;
    };

    struct MaterialIsolation
    {
        RenameList renamed;
        std::vector<MaterialRename> updates;
    };

    MaterialIsolation IsolateMaterialNames(const Path& project, const Path& stock, const std::vector<MaterialFile>& materials,
                                           const std::vector<std::string>& hudNames);
}
