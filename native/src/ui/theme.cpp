// Launcher theme (see theme.h).
#include "theme.h"

#include <filesystem>

namespace w2b::ui::theme
{
    namespace
    {
        ImFont* bold = nullptr;
        ImFont* mono = nullptr;
        ImFont* display = nullptr;
        float size = 12.0f;
        float scale = 1.0f;

        ImFont* Load(const char* file, const float px, const bool symbols)
        {
            auto& io = ImGui::GetIO();
            const auto path = std::string("C:\\Windows\\Fonts\\") + file;
            if (!std::filesystem::exists(path))
                return nullptr;
            static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0, 0};
            ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), px, nullptr, ranges);
            if (font && symbols && std::filesystem::exists("C:\\Windows\\Fonts\\seguisym.ttf"))
            {
                ImFontConfig merge;
                merge.MergeMode = true;
                static const ImWchar marks[] = {0x2190, 0x21FF, 0x25A0, 0x25FF, 0x2600, 0x27BF, 0};
                io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", px, &merge, marks);
            }
            return font;
        }
    } // namespace

    ImFont* Bold()
    {
        return bold ? bold : ImGui::GetFont();
    }

    ImFont* Mono()
    {
        return mono ? mono : ImGui::GetFont();
    }

    ImFont* Display()
    {
        return display ? display : Bold();
    }

    float FontSize()
    {
        return size;
    }

    float Scale()
    {
        return scale;
    }

    void Apply(const float dpiScale)
    {
        // WinForms' default: Segoe UI 9 pt (12 px at 96 DPI)
        scale = dpiScale;
        size = 12.0f * dpiScale;
        auto& io = ImGui::GetIO();
        io.Fonts->Clear();
        if (!Load("segoeui.ttf", size, true))
            io.Fonts->AddFontDefault();
        bold = Load("segoeuib.ttf", size, true);
        mono = Load("consola.ttf", 12.0f * dpiScale, false);
        // menu previews: a condensed face close to BO2's menu font
        for (const char* face : {"bahnschrift.ttf", "impact.ttf", "arialbd.ttf"})
            if ((display = Load(face, 26.0f * dpiScale, false)) != nullptr)
                break;

        ImGuiStyle style;
        ImGui::StyleColorsLight(&style);
        style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = style.TabRounding = style.GrabRounding = style.ScrollbarRounding = 0;
        style.WindowBorderSize = 0;
        style.ChildBorderSize = 1;
        style.FrameBorderSize = 1;
        // WinForms metrics: 20 px text boxes, 6 px gaps, 17 px scroll bars
        style.WindowPadding = ImVec2(6, 6);
        style.FramePadding = ImVec2(4, 4);
        style.ItemSpacing = ImVec2(6, 5);
        style.ItemInnerSpacing = ImVec2(4, 4);
        style.ScrollbarSize = 17;
        style.TabBarBorderSize = 1;
        auto* c = style.Colors;
        const ImVec4 face{0.941f, 0.941f, 0.941f, 1}, white{1, 1, 1, 1}, border{0.678f, 0.678f, 0.678f, 1};
        c[ImGuiCol_Text] = ImVec4(0, 0, 0, 1);
        c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.55f, 1);
        c[ImGuiCol_WindowBg] = face;
        c[ImGuiCol_ChildBg] = face;
        c[ImGuiCol_PopupBg] = white;
        c[ImGuiCol_MenuBarBg] = white;
        c[ImGuiCol_Border] = border;
        c[ImGuiCol_FrameBg] = white;
        c[ImGuiCol_FrameBgHovered] = white;
        c[ImGuiCol_FrameBgActive] = white;
        c[ImGuiCol_Button] = ImVec4(0.882f, 0.882f, 0.882f, 1);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.898f, 0.945f, 0.984f, 1);
        c[ImGuiCol_ButtonActive] = ImVec4(0.800f, 0.894f, 0.969f, 1);
        c[ImGuiCol_Header] = ImVec4(0.800f, 0.894f, 0.969f, 1);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.898f, 0.945f, 0.984f, 1);
        c[ImGuiCol_HeaderActive] = ImVec4(0.800f, 0.894f, 0.969f, 1);
        c[ImGuiCol_Tab] = ImVec4(0.902f, 0.902f, 0.902f, 1);
        c[ImGuiCol_TabHovered] = ImVec4(0.898f, 0.945f, 0.984f, 1);
        c[ImGuiCol_TabSelected] = white;
        c[ImGuiCol_TabSelectedOverline] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_CheckMark] = ImVec4(0, 0, 0, 1);
        c[ImGuiCol_PlotHistogram] = ImVec4(0.024f, 0.690f, 0.145f, 1);
        c[ImGuiCol_TableHeaderBg] = face;
        c[ImGuiCol_TableRowBgAlt] = ImVec4(0.973f, 0.973f, 0.973f, 1);
        c[ImGuiCol_ScrollbarBg] = face;
        style.ScaleAllSizes(dpiScale);
        ImGui::GetStyle() = style;
    }
} // namespace w2b::ui::theme
