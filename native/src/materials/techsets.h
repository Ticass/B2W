#pragma once

#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace w2b::materials
{
    class TechniqueError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    enum class Family { other, world, layered, model, modelVertex };

    struct Layer
    {
        char blend;
        unsigned index;
        std::set<char> features;
    };

    struct Technique
    {
        std::string name;
        Family family;
        bool lit;
        std::string unlit;
        std::vector<Layer> layers;
        std::vector<std::string> modifiers;
    };

    struct Match
    {
        std::string target;
        int cost;
        std::vector<std::string> notes;
    };

    Technique ParseTechnique(std::string_view name);
    Match MatchTechnique(std::string_view source, std::span<const Technique> candidates,
                         const std::set<std::string>& rejected, bool sourcePrograms, bool layeredVertices);
    std::vector<std::string> LastResort(std::string_view name);
}
