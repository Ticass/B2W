#pragma once
// The launcher's look: the classic Windows tool window of the T6 Mod Tools
// Launcher (light gray, square controls, Segoe UI) with the old launcher's
// accent colors.

#include <imgui.h>

namespace w2b::ui::theme
{
    inline const ImVec4 INK{0.141f, 0.169f, 0.196f, 1};
    inline const ImVec4 MUTED{0.384f, 0.420f, 0.451f, 1};
    inline const ImVec4 ACCENT{0.141f, 0.420f, 0.600f, 1};
    inline const ImVec4 GREEN{0.129f, 0.455f, 0.267f, 1};
    inline const ImVec4 AMBER{0.675f, 0.408f, 0.043f, 1};
    inline const ImVec4 RED{0.686f, 0.204f, 0.204f, 1};
    inline const ImVec4 GROUP_TITLE{0.0f, 0.0f, 0.0f, 1};
    inline const ImVec4 PROCESS_BG{1.0f, 1.0f, 0.882f, 1};
    inline const ImVec4 CONSOLE_BG{0.0f, 0.0f, 0.0f, 1};
    inline const ImVec4 STOP_IDLE{0.937f, 0.502f, 0.502f, 1};
    inline const ImVec4 STOP_ACTIVE{0.886f, 0.306f, 0.306f, 1};
    inline const ImVec4 STOP_HOVER{0.957f, 0.408f, 0.408f, 1};

    // loads Segoe UI (+ Segoe UI Symbol for marks), Consolas, and the style
    void Apply(float dpiScale);
    ImFont* Bold();
    ImFont* Mono();
    float FontSize();
} // namespace w2b::ui::theme
