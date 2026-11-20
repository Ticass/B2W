#include "weapons/weapon.h"

#include "core/encoding.h"
#include "core/files.h"

#include <algorithm>
#include <format>
#include <regex>

namespace w2b::weapons
{
    Json Weapon::Report() const
    {
        auto sorted = dependencies;
        for (auto& names : sorted)
        {
            auto values = names.get<std::vector<std::string>>();
            std::ranges::sort(values);
            const auto duplicates = std::ranges::unique(values);
            values.erase(duplicates.begin(), duplicates.end());
            names = std::move(values);
        }
        return {{"name", name}, {"dependencies", std::move(sorted)}, {"unsupported_fields", unsupported},
                {"translations", translations}};
    }

    void Weapon::AddDependency(std::string_view kind, const std::string& asset)
    {
        auto& names = dependencies[kind];
        if (names.is_null())
            names = Json::array();
        names.push_back(asset);
    }

    const std::string& Field(const InfoFields& fields, std::string_view key)
    {
        static const std::string missing;
        const auto* found = fields.Find(key);
        return found ? *found : missing;
    }

    const std::string& Field(const Json& fields, std::string_view key)
    {
        static const std::string missing;
        const auto found = fields.find(key);
        return found == fields.end() ? missing : found->get_ref<const std::string&>();
    }

    const std::string& Field(const Schema& fields, std::string_view key)
    {
        static const std::string missing;
        const auto found = fields.find(key);
        return found == fields.end() ? missing : found->second;
    }

    double NumericField(const InfoFields& fields, std::string_view key)
    {
        const auto& text = Field(fields, key);
        double value = 0;
        if (!text.empty() && !py::ParseFloat(text, value))
            throw WeaponError("could not convert string to float: " + py::StringRepr(text));
        return value;
    }

    InfoFields ReadInfo(const path& source, std::string_view signature)
    {
        std::string bytes;
        if (!files::ReadBytes(source, bytes))
            throw std::runtime_error("cannot read " + files::Utf8(source));
        const auto text = encoding::WawText(bytes);
        const auto parts = py::Split(text, "\\");
        if (signature == "FLAMETABLEFILE" && (parts.size() < 2 || parts[0] != signature))
            throw WeaponError(files::Utf8(source) + ": expected compiled FLAMETABLEFILE");
        if (parts[0] != signature || parts.size() % 2 != 1)
            throw WeaponError("invalid WEAPONFILE key/value stream");

        InfoFields fields;
        fields.fields.reserve(parts.size() / 2);
        for (std::size_t i = 1; i < parts.size(); i += 2)
        {
            if (fields.Has(parts[i]))
                throw WeaponError("duplicate weapon field " + std::string(parts[i]));
            fields.fields.emplace_back(std::string(parts[i]), std::string(parts[i + 1]));
        }
        return fields;
    }

    std::string WriteInfo(const InfoFields& fields, std::string_view signature)
    {
        std::string result(signature);
        for (const auto& [key, value] : fields)
        {
            if (key.find('\\') != key.npos || value.find('\\') != value.npos)
                throw WeaponError("infostring field contains a separator");
            result += '\\';
            result += key;
            result += '\\';
            result += value;
        }
        return result;
    }

    Schema ReadSchema(const path& header)
    {
        Schema fields;
        const auto text = files::ReadText(header);
        static const std::regex pattern(R"schema(\{"([^"]+)".*?([A-Z_0-9]+)\s*\})schema");
        for (const auto line : py::SplitLines(text))
        {
            std::match_results<std::string_view::const_iterator> match;
            if (std::regex_search(line.begin(), line.end(), match, pattern))
                fields[match[1].str()] = match[2].str();
        }
        if (fields.size() < 100)
            throw WeaponError("weapon field schema incomplete: " + files::Utf8(header));
        return fields;
    }

    EnumValues ReadEnumValues(const path& loader, const path& strings)
    {
        std::map<std::string, std::vector<std::string>, std::less<>> tables;
        const auto stringText = files::ReadText(strings);
        static const std::regex table(R"(const\s+char\s*\*\s*(\w+)\s*\[\s*\]\s*\{([^}]*)\})");
        static const std::regex quoted(R"x("([^"]*)")x");
        for (std::sregex_iterator it(stringText.begin(), stringText.end(), table), end; it != end; ++it)
        {
            auto& values = tables[(*it)[1].str()];
            const auto body = (*it)[2].str();
            for (std::sregex_iterator q(body.begin(), body.end(), quoted); q != end; ++q)
                values.push_back((*q)[1].str());
        }
        EnumValues out;
        const auto loaderText = files::ReadText(loader);
        static const std::regex use(R"(case\s+(WFT_\w+)\s*:\s*return\s+ConvertEnumInt\([^,]+,[^,]+,[^,]+,\s*(\w+))");
        for (std::sregex_iterator it(loaderText.begin(), loaderText.end(), use), end; it != end; ++it)
            if (const auto found = tables.find((*it)[2].str()); found != tables.end())
                out[(*it)[1].str()] = found->second;
        if (out.size() < 10)
            throw WeaponError("weapon enum tables incomplete: " + files::Utf8(loader));
        return out;
    }

    std::string OutputName(std::string_view kind, std::string name)
    {
        if (name.empty())
            return {};
        std::ranges::replace(name, '\\', '/');
        constexpr std::string_view invalid(":,\r\n\0", 5);
        if (name.starts_with('/') || name.find_first_of(invalid) != name.npos ||
            std::ranges::any_of(py::Split(name, "/"), [](auto part) { return part == ".."; }))
            throw WeaponError("unsafe " + std::string(kind) + " name: " + py::StringRepr(name));
        std::string output;
        if (kind != "weapon")
            output = "waw_" + std::string(kind) + "/";
        for (const unsigned char c : name)
        {
            if (c < 0x20 || std::string_view("<>\"|?*").find(c) != std::string_view::npos)
                output += std::format("_x{:02x}", c);
            else
                output += static_cast<char>(c);
        }
        return output;
    }
}
