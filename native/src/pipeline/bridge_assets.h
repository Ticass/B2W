#pragma once
// The bridge stage's WaW asset passes: weapons and their
// dependency closure, the models the scripts set at runtime, the material
// sources of every drawn surface, the source impact materials, and the
// material and image conversions run over them. Each pass appends to the
// stage report (a StageReport as JSON).

#include "core/text.h"
#include "pipeline/bridge_report.h"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
    class WawSource;
}

namespace w2b::textures
{
    class IwdRecovery;
}

namespace w2b::pipeline::bridge
{
    namespace fs = std::filesystem;
    using Names = std::set<std::string>;

    // _stage_weapons + weapons.stage: resolves and stages the map's compiled
    // weapons; `roots` widens to the resolved roots and `sounds` gains the
    // weapons' sounds. nullopt when there is nothing to stage.
    std::optional<util::Json> StageWeapons(BridgeReport& report, std::vector<fs::path>& roots, const fs::path& projectRoot, const Names& weapons,
                                         const Names& scriptModels, extract::StockWaw* stock, Names& sounds);

    // recover_script_models: the wanted models some WaW zone (or, with
    // `source`, a WaW Mod Tools source) defines; `roots` widens to the dumps
    // that hold them, `scripts` gains requested_models and model_sources
    Names RecoverScriptModels(util::Json& scripts, std::vector<std::string>& warnings, const Names& wanted, std::vector<fs::path>& roots, extract::StockWaw* stock,
                              extract::WawSource* source);

    // recover_material_sources: one lookup for world, HUD and weapon
    // materials; returns the BO2 equivalents used for materials no WaW source
    // defines (`roots` widens to the resolved roots)
    std::map<std::string, fs::path> RecoverMaterialSources(BridgeReport& report, const Names& names, std::vector<fs::path>& roots, extract::StockWaw* stock,
                                                           extract::WawSource* source, const fs::path& stockMaterials,
                                                           const std::optional<fs::path>& bo2Root);

    struct MaterialOptions
    {
        std::optional<std::array<double, 4>> falloffPlacement;
        bool lightShadows = true;
        bool layeredVertexData = false;
        std::map<std::string, fs::path> nativeFallbacks;
    };

    // stage_materials (timed "Convert materials"); returns the technique sets used
    Names StageMaterials(BridgeReport& report, const Names& names, const std::vector<fs::path>& roots, const fs::path& stockMaterials,
                         const fs::path& projectRoot, const std::optional<fs::path>& techsetRoot, const MaterialOptions& options);

    // stage_images (timed "Convert images"); returns the images written.
    // `recovered` holds the IWD images decoded so far and gains this pass's.
    std::vector<std::string> StageImages(BridgeReport& report, const std::vector<fs::path>& imageRoots, const fs::path& projectRoot,
                                         const std::optional<std::vector<fs::path>>& iwdDirs, const std::vector<std::string>& extraImages,
                                         textures::IwdRecovery& recovery, util::Json& recovered);

    // impactmaterials.stage: the map's own impact materials for engine-created marks;
    // the report content entry ({} when the map ships none)
    util::Json StageImpactMaterials(const fs::path& projectRoot, const std::vector<fs::path>& roots, const fs::path& techsetDump,
                                  extract::StockWaw* stock, textures::IwdRecovery& recovery, util::Json& recovered, bool layeredVertexData);

    // shaderruntime.light_falloff_placement: WaW's lightFalloffPlacement for
    // the light def every local light shares; nullopt when there is none
    std::optional<std::array<double, 4>> LightFalloffPlacement(const fs::path& primaryLights, const std::vector<fs::path>& roots);

    // write_image_streaming: images/streaming.json for the bridge linker
    void WriteImageStreaming(const fs::path& projectRoot, const Names& images, bool merge);

    // parallel.worker_count
    unsigned WorkerCount(std::size_t total, bool processes = false);
} // namespace w2b::pipeline::bridge
