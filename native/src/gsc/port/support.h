#pragma once
// Ports of the small script modules port_map uses: zombieappearance.py,
// stockperks.py, hintdefaults.py, perktable.py and the generated-script parts
// of oneway.py and waterbullets.py.

#include "core/pytext.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::gsc::port
{
    namespace zombieappearance
    {
        std::string ActorName(const std::string& source);
        std::string AppearanceScript(const std::string& source);
        std::string NativeActor(const std::string& tmpl, const std::string& sourcePath);
        std::string SourceActorKind(const std::string& source);
        std::string TemplateTree(const std::string& tmpl);
        std::string NativeClientActor(const std::string& name, const std::string& tree = "zm_nuked_basic");
        std::set<std::string> SourceActorTypes(const std::filesystem::path& projectRoot);
        void BindActorTypes(const std::filesystem::path& projectRoot, const std::set<std::string>& names);
    } // namespace zombieappearance

    namespace stockperks
    {
        bool IsPerkFramework(const std::string& path);
        std::string TranslateValue(const std::string& value);
        std::string TranslateLiterals(const std::string& source);
        std::string StockModel(const std::string& name, const std::filesystem::path& bo2Root);
        std::set<std::string> MachineModels(const std::filesystem::path& bo2Root);
        void TranslateEntities(const std::filesystem::path& path, const std::filesystem::path* bo2Root = nullptr);
        std::string NativeSource(std::string source, const std::string& name, const std::filesystem::path& outRoot, const std::filesystem::path& bo2Root);
        std::vector<std::string> AssetLines(const std::filesystem::path& outRoot);
    } // namespace stockperks

    namespace hintdefaults
    {
        std::string Source(const std::filesystem::path& project);
    }

    namespace perktable
    {
        inline constexpr const char* SCRIPT = "_waw2bo2_perk_table.gsc";
        std::optional<std::filesystem::path> T6Executable();
        std::set<std::string> EnginePerks(const std::filesystem::path& executable);
        std::set<std::string> MapPerks(const std::filesystem::path& projectRoot);
        std::string Source(const std::set<std::string>& emulated);
        py::Json Stage(const std::filesystem::path& projectRoot, const std::optional<std::filesystem::path>& executable = std::nullopt);
    } // namespace perktable

    namespace oneway
    {
        std::string OnewaySource();
    }

    namespace waterbullets
    {
        std::map<std::string, double> WaterDepths(const std::string& tableText);
        std::string WaterBulletsSource(const std::map<std::string, std::vector<double>>& rows);
    } // namespace waterbullets

    // Python's f"{value:g}"
    std::string FormatG(double value);
} // namespace w2b::gsc::port
