#pragma once

#include "core/pytext.h"

#include <set>
#include <stdexcept>

namespace w2b::effects
{
    inline constexpr std::string_view outputPrefix = "waw/";
    inline constexpr std::string_view modelPrefix = "waw_fx_model/";
    inline constexpr int sourceTrail = 4;
    enum ElementType { trail = 5, cloud, model, omniLight, spotLight, sound, decal, runner };

    class EffectError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    struct Dependencies
    {
        std::set<std::string> effects;
        std::set<std::string> materials;
        std::set<std::string> models;
        std::set<std::string> sounds;
        std::set<std::string> trailMaterials;
    };

    struct ConvertedEffect
    {
        py::Json asset;
        std::vector<std::string> notes;
    };

    Dependencies FindDependencies(const py::Json& source);
    ConvertedEffect ConvertEffect(const py::Json& source, const py::Json& materials);
    std::size_t AllocationSize(const py::Json& effect);

    py::Json ConvertElement(const py::Json& source, const py::Json& materials,
                            std::vector<std::string>& notes, const std::string& location);
}
