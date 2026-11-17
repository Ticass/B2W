#pragma once

#include "assets/resolver.h"
#include "weapons/weapon.h"

namespace w2b::assets
{
    class DependencyReader
    {
    public:
        explicit DependencyReader(Path base) : base_(std::move(base)) {}
        AssetSet Read(std::string_view kind, const Path& source);
        AssetSet ReadWeapon(const Path& source, const std::function<void(std::string_view, AssetSet&)>& bounce);
        static AssetSet ReadModel(const py::Json& model);
        static void AddGeometry(const Path& source, AssetSet& assets);

    private:
        Path base_;
        weapons::Schema schema_;
    };
}
