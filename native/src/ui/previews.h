#pragma once
// Map Details & Artwork previews, drawn on the GPU in BO2's 1280 x 720 menu space.

#include "gpu.h"
#include "launcher/settings.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace w2b::ui
{
    class Previews
    {
    public:
        enum Mode
        {
            Globe,
            MapSelection,
            FocusedSelection,
            Lobby,
            Loading,
            LayerLarge,
            LayerBlur,
            LayerBlit
        };
        // draws the preview pane; true when a click changed the settings
        bool Draw(Gpu& gpu, launcher::Settings& settings);
        int mode = Globe;

    private:
        struct Alpha
        {
            uintmax_t size = 0;
            long long time = 0;
            int width = 0, height = 0;
            std::vector<float> grid; // 160 x 90 box-filtered alpha
        };
        const Alpha* BlitAlpha(const std::filesystem::path& path);
        std::map<std::wstring, Alpha> alphas;
        float globeLon = 130.0f;
        bool dragging = false, moved = false;
        float dragStartX = 0, dragStartLon = 0;
    };
} // namespace w2b::ui
