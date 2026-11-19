#pragma once
// The bridge's world pass: render and collision geometry, probes, visibility,
// map physics and, through the navigation pass, the entities and AI paths.

#include "core/pytext.h"
#include "world/render_world.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::pipeline
{
    struct NavigationRequest
    {
        std::string project;
        std::filesystem::path stage;
        std::optional<std::filesystem::path> bo2Root, wawMapScript;
        std::vector<std::filesystem::path> traversalRoots, mapRoots;
        py::Json levelWeapons;         // the stock common zone's weapons
        bool bo2StockPerks = false;
        std::string respawnExclusion;  // gsc_api zombie_runtime.source_respawn.native_exclusion_tag
    };

    struct GeometryRequest
    {
        std::filesystem::path gfx, clip, projectRoot;
        std::vector<std::filesystem::path> roots, imageRoots;
        extract::StockWaw* stock = nullptr;
        bool compensated = true;
        // the T4 dumps beside the render and clip worlds
        std::filesystem::path probeSource, visibilitySource, dynents, constraints;
        std::optional<NavigationRequest> navigation;
    };

    struct GeometrySurface
    {
        std::string material;
        unsigned lightmap = 0;
    };

    struct GeometryStage
    {
        py::Json content = py::Json::object(); // report content sections, in order
        py::Json collision;
        std::vector<std::string> warnings, errors;
        std::optional<std::string> visibilityWarning; // goes after the probe warnings
        std::vector<GeometrySurface> surfaces;
        std::vector<world::WorldStaticModel> staticModels;
        bool shadowLights = false, layered = false, hasWater = false;
        py::Json probes;                       // the probe report (images, warnings, errors, content, kept)
        std::optional<std::string> sky;
        py::Json dynents, constraints;         // map physics
        std::set<std::string> clipModels;
        py::Json navigation;                   // null without a navigation request
    };

    GeometryStage StageBridgeGeometry(const GeometryRequest& request);
}
