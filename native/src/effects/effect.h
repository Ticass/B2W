#pragma once

#include "core/text.h"

#include <map>
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
        util::Json asset;
        std::vector<std::string> notes;
    };

    // WaW material -> the converted material an effect draws
    using MaterialNames = std::map<std::string, std::string, std::less<>>;

    Dependencies FindDependencies(const util::Json& source);
    // source: the dumped WaW effect (.w2bfx.json, a JSON format of its own)
    ConvertedEffect ConvertEffect(const util::Json& source, const MaterialNames& materials);
    std::size_t AllocationSize(const util::Json& effect);

    util::Json ConvertElement(const util::Json& source, const MaterialNames& materials,
                            std::vector<std::string>& notes, const std::string& location);
}
