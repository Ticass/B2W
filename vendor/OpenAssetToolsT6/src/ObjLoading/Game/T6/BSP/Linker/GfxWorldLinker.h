#pragma once

#include "Asset/IAssetCreator.h"
#include "Game/T6/BSP/BSP.h"
#include "SearchPath/ISearchPath.h"
#include "Utils/MemoryManager.h"
#include <utility>
#include <vector>

namespace BSP
{
    class GfxWorldLinker
    {
    public:
        GfxWorldLinker(MemoryManager& memory, ISearchPath& searchPath, AssetCreationContext& context);

        [[nodiscard]] T6::GfxWorld* LinkGfxWorld(const BSPData& bsp) const;

    private:
        void LoadDrawData(const BSPData& projInfo, T6::GfxWorld& gfxWorld) const;
        bool LoadMapSurfaces(const BSPData& projInfo, T6::GfxWorld& gfxWorld) const;
        void LoadShadowGeometry(const BSPData& bsp, T6::GfxWorld& gfxWorld) const;
        void AppendLayerVertices(const BSPData& bsp, const BSPSurface& bspSurface, T6::GfxSurface& gfxSurface, T6::GfxWorld& gfxWorld,
                                 std::vector<char>& layerData) const;
        [[nodiscard]] bool LoadXModels(const BSPData& bsp, T6::GfxWorld& gfxWorld) const;
        void CleanGfxWorld(T6::GfxWorld& gfxWorld) const;
        void LoadGfxLights(T6::GfxWorld& gfxWorld) const;
        bool LoadLightGrid(T6::GfxWorld& gfxWorld) const;
        void LoadGfxCells(T6::GfxWorld& gfxWorld) const;
        void LoadModels(T6::GfxWorld& gfxWorld) const;
        bool LoadReflectionProbeData(T6::GfxWorld& gfxWorld) const;
        bool LoadLightmapData(T6::GfxWorld& gfxWorld) const;
        void LoadSkyBox(const BSPData& projInfo, T6::GfxWorld& gfxWorld) const;
        void LoadDynEntData(T6::GfxWorld& gfxWorld) const;
        bool LoadOutdoors(T6::GfxWorld& gfxWorld) const;
        void LoadSunData(T6::GfxWorld& gfxWorld) const;
        void LoadWorldBounds(T6::GfxWorld& gfxWorld) const;

        MemoryManager& m_memory;
        ISearchPath& m_search_path;
        AssetCreationContext& m_context;
        mutable std::vector<std::pair<unsigned, unsigned>> m_brush_surface_ranges;
        mutable std::vector<size_t> m_surface_order; // final surface index -> BSP surface
    };
} // namespace BSP
