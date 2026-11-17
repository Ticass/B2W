#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <map>
#include <stdexcept>

namespace w2b::weapons
{
    using py::Json;
    using std::filesystem::path;
    using Schema = std::map<std::string, std::string, std::less<>>;

    class WeaponError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    struct Weapon
    {
        std::string name;
        Json fields = Json::object();
        Json dependencies = Json::object();
        Json unsupported = Json::object();
        Json translations = Json::object();

        Json Report() const;
        void AddDependency(std::string_view kind, const std::string& name);
    };

    const std::string& Field(const Json& fields, std::string_view key);
    const std::string& Field(const Schema& fields, std::string_view key);
    double NumericField(const Json& fields, std::string_view key);
    Json ReadInfo(const path& source, std::string_view signature = "WEAPONFILE");
    std::string WriteInfo(const Json& fields, std::string_view signature = "WEAPONFILE");
    Schema ReadSchema(const path& header);
    std::string OutputName(std::string_view kind, std::string name);
    Weapon Convert(std::string_view name, const Json& source, const Schema& t4, const Schema& t6);
    void ApplyWeaponDefaults(Weapon& weapon, const Json& source, const Schema& t6);
}
