// Launcher drawing: the T6 Mod Tools Launcher layout (sidebar, tabs, right
// column, process box and console) with the launcher's workflows.
#include "app.h"

#include "launcher/diagnostics.h"
#include "shell.h"
#include "steps.h"
#include "theme.h"

#include <imgui_internal.h>

#include <algorithm>

namespace w2b::ui
{
    namespace l = launcher;

    namespace
    {
        // the T6 Mod Tools Launcher's column widths (pixels at 100 % scale)
#define SIDE (142.0f * theme::Scale())
#define RIGHT (150.0f * theme::Scale())

        struct OpenGroup
        {
            ImVec2 top;
            const char* title;
        };
        std::vector<OpenGroup> openGroups;

        // a WinForms GroupBox: the title sits on the frame's top line
        bool BeginGroup(const char* title, const ImVec2& size)
        {
            const ImVec2 top = ImGui::GetCursorScreenPos();
            openGroups.push_back({top, title});
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.55f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::S(7), theme::S(5)));
            ImVec2 extent = size;
            if (extent.x <= 0) // keep the frame's right line inside the parent's clip
                extent.x = ImGui::GetContentRegionAvail().x + extent.x - 1;
            const bool open = ImGui::BeginChild(title, extent, ImGuiChildFlags_AlwaysUseWindowPadding | (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0));
            ImGui::PopStyleVar();
            return open;
        }

        void EndGroup()
        {
            ImGui::EndChild();
            const auto group = openGroups.back();
            openGroups.pop_back();
            const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            auto* draw = ImGui::GetWindowDrawList();
            const float line = group.top.y + ImGui::GetFontSize() * 0.5f;
            draw->AddRect(ImVec2(min.x, line), max, ImGui::GetColorU32(ImGuiCol_Border));
            const ImVec2 text = ImGui::CalcTextSize(group.title);
            const ImVec2 at(min.x + theme::S(7), group.top.y);
            draw->AddRectFilled(ImVec2(at.x - theme::S(2), at.y), ImVec2(at.x + text.x + theme::S(2), at.y + text.y), ImGui::GetColorU32(ImGuiCol_WindowBg));
            draw->AddText(at, ImGui::GetColorU32(theme::GROUP_TITLE), group.title);
            ImGui::Dummy(ImVec2(0, theme::S(3)));
        }

        // standard WinForms push button: 23 px tall
        bool ClassicButton(const char* label, ImVec2 size = ImVec2(0, 0))
        {
            if (size.y == 0)
                size.y = theme::S(23);
            return ImGui::Button(label, size);
        }

        // standard WinForms check box: a 13 px square
        bool ClassicCheckbox(const char* label, bool* value)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(theme::S(2), std::max(0.0f, (theme::S(13) - ImGui::GetFontSize()) * 0.5f)));
            const bool changed = ImGui::Checkbox(label, value);
            ImGui::PopStyleVar();
            return changed;
        }

        void Muted(const std::string& text, const float wrap = 0.0f)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::MUTED);
            if (wrap > 0)
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
            else
                ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        int Resize(ImGuiInputTextCallbackData* data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                auto* text = static_cast<std::string*>(data->UserData);
                text->resize(static_cast<size_t>(data->BufTextLen));
                data->Buf = text->data();
            }
            return 0;
        }

        ImU32 TagColor(const Tag tag)
        {
            switch (tag)
            {
            case Tag::Error:
                return IM_COL32(0xff, 0x89, 0x89, 255);
            case Tag::Warning:
                return IM_COL32(0xeb, 0xc1, 0x74, 255);
            case Tag::Info:
                return IM_COL32(0x80, 0xbb, 0xdf, 255);
            case Tag::Success:
                return IM_COL32(0x8e, 0xd6, 0xa1, 255);
            default:
                return IM_COL32(0xcb, 0xd5, 0xdc, 255);
            }
        }

        // draws `texture` fitted in the next `size` area; returns the image rect
        ImRect Fit(const Texture& texture, const ImVec2& size)
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float scale = std::min(size.x / std::max(1, texture.width), size.y / std::max(1, texture.height));
            const ImVec2 shown(texture.width * scale, texture.height * scale);
            const ImVec2 at(origin.x + (size.x - shown.x) * 0.5f, origin.y + (size.y - shown.y) * 0.5f);
            ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(texture.view), at, ImVec2(at.x + shown.x, at.y + shown.y));
            return ImRect(at, ImVec2(at.x + shown.x, at.y + shown.y));
        }
    } // namespace

    bool App::Input(const char* id, std::string& value, const float width)
    {
        ImGui::SetNextItemWidth(width);
        ImGui::BeginDisabled(busy);
        value.reserve(value.size() + 1);
        const bool changed = ImGui::InputText(id, value.data(), value.capacity() + 1, ImGuiInputTextFlags_CallbackResize, Resize, &value);
        const bool done = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::EndDisabled();
        if (done)
        {
            Save();
            Refresh();
        }
        return changed;
    }

    bool App::Button(const char* label, const ImVec2& size, const bool enabled)
    {
        ImGui::BeginDisabled(!enabled || busy);
        const bool pressed = ClassicButton(label, size);
        ImGui::EndDisabled();
        return pressed;
    }

    bool App::PathRow(const char* label, const std::string& field, const float labelWidth)
    {
        ImGui::PushID(field.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelWidth);
        const float browse = ImGui::CalcTextSize("Browse\xE2\x80\xA6").x + ImGui::GetStyle().FramePadding.x * 2;
        const bool changed = Input("##path", *settings.Text(field), ImGui::GetContentRegionAvail().x - browse - 6);
        ImGui::SameLine();
        if (Button("Browse\xE2\x80\xA6"))
            ChoosePath(field);
        ImGui::PopID();
        return changed;
    }

    void App::Frame()
    {
        Drain();
        // the T6 launcher's title: the tool and which build it is, then the BO2 install it works on
        if (const auto title = "B2W Mod Tools Launcher (" + l::BuildLabel() + ")" + (settings.bo2.empty() ? std::string() : " - " + settings.bo2); title != windowTitle)
        {
            windowTitle = title;
            SetWindowTextW(window, shell::Wide(title).c_str());
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("##launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar
                         | ImGuiWindowFlags_NoBringToFrontOnFocus);
        DrawMenu();
        const float footer = theme::S(23) + ImGui::GetStyle().ItemSpacing.y + theme::S(4);
        const float total = ImGui::GetContentRegionAvail().y;
        const float bottom = std::max(theme::S(170), total * 0.34f);
        const float top = total - bottom - footer - ImGui::GetFrameHeightWithSpacing() * 1.4f;

        DrawSidebar(top);
        ImGui::SameLine();
        ImGui::BeginGroup();
        const float centerWidth = ImGui::GetContentRegionAvail().x - RIGHT - ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::BeginChild("##center", ImVec2(centerWidth, top)))
        {
            if (ImGui::BeginTabBar("##tabs"))
            {
                const auto tabItem = [&](const char* name, const Tab which)
                {
                    const bool open = ImGui::BeginTabItem(name, nullptr, selectTab && tab == which ? ImGuiTabItemFlags_SetSelected : 0);
                    if (open)
                    {
                        if (!selectTab)
                            tab = which;
                        ImGui::EndTabItem();
                    }
                };
                tabItem("Mod Builder", Tab::Builder);
                tabItem("Map Details & Artwork", Tab::Artwork);
                tabItem("Setup", Tab::Setup);
                tabItem("Reports", Tab::Reports);
                selectTab = false;
                ImGui::EndTabBar();
            }
            if (ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_Borders))
            {
                switch (tab)
                {
                case Tab::Builder:
                    DrawBuilder();
                    break;
                case Tab::Artwork:
                    DrawArtwork();
                    break;
                case Tab::Setup:
                    DrawSetup();
                    break;
                case Tab::Reports:
                    DrawReports();
                    break;
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
        ImGui::EndGroup();
        ImGui::SameLine();
        DrawRight(top);

        // status line and progress, as the launcher's thin strip under the tabs
        ImGui::Dummy(ImVec2(SIDE, 0));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(status.c_str());
        const float progress = currentStep < 0 ? 0.0f : std::min(1.0f, static_cast<float>(currentStep) / STEP_COUNT);
        ImGui::ProgressBar(progress, ImVec2(-1, 6), "");
        ImGui::EndGroup();

        DrawBottom(bottom);
        DrawFooter();
        ImGui::End();
    }

    void App::DrawMenu()
    {
        if (!ImGui::BeginMenuBar())
            return;
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open WaW map\xE2\x80\xA6", "Ctrl+O", false, !busy))
                ChooseMap();
            if (ImGui::MenuItem("Open build folder"))
                OpenBuildFolder();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
                PostMessageW(window, WM_CLOSE, 0, 0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools"))
        {
            if (ImGui::MenuItem("Game & tool paths"))
                tab = Tab::Setup, selectTab = true;
            if (ImGui::MenuItem("Conversion reports"))
                LoadReport(), tab = Tab::Reports, selectTab = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("Quick help"))
                shell::Info(window, "Quick start",
                            "1. Open Setup and check the detected game folders.\n2. Browse to the WaW map .ff in its original mod folder.\n"
                            "3. Click Build Map. The tool manages intermediate files.\n4. Review Reports, then click Install to Plutonium.\n"
                            "5. Click Launch Map and compare it with the WaW original.\n\nBO2 Mod Tools and game assets are required. Portable builds include the "
                            "converter, WaW extractor, BO2 bridge, and audio decoder.\n\nBuilds stay separate from installed maps until you click Install. Close the game "
                            "before installing changed files.");
            if (ImGui::MenuItem("Full usage guide"))
                shell::Open("https://github.com/Ticass/B2W/blob/main/docs/USAGE.md");
            if (ImGui::MenuItem("About"))
                shell::Info(window, "About",
                            "B2W Mod Tools " + l::BuildLabel() + " (version " + l::Version() + ")"
                                + "\nWorld at War custom-map converter\n\nDevelopment preview. Review the conversion report and playtest the result.\n\n"
                                  "OpenAssetTools: GPL-3.0; Dear ImGui: MIT; see included licenses.");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
            ChooseMap();
    }

    void App::DrawSidebar(const float height)
    {
        ImGui::BeginChild("##side", ImVec2(SIDE, height));
        // the brand plate in place of the game box art
        const ImVec2 at = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        // the B2W plate where the T6 launcher shows its box art (108 x 108)
        const float w = SIDE - theme::S(4), plate = theme::S(108);
        const ImVec2 box(at.x + (w - plate) * 0.5f, at.y);
        draw->AddRectFilled(box, ImVec2(box.x + plate, box.y + plate), IM_COL32(0x20, 0x28, 0x2f, 255));
        const auto centered = [&](ImFont* font, const float size, const float y, const char* text, const ImU32 color)
        {
            const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
            draw->AddText(font, size, ImVec2(box.x + (plate - extent.x) * 0.5f, box.y + theme::S(y)), color, text);
        };
        centered(theme::Bold(), theme::S(10), 12, "WAW \xE2\x86\x92 BO2", IM_COL32(0xd4, 0xdc, 0xe2, 255));
        centered(theme::Bold(), theme::S(40), 26, "B2W", IM_COL32_WHITE);
        draw->AddLine(ImVec2(box.x + theme::S(14), box.y + theme::S(78)), ImVec2(box.x + plate - theme::S(14), box.y + theme::S(78)), IM_COL32(0x55, 0x7b, 0x94, 255), theme::S(2));
        centered(theme::Bold(), theme::S(10), 84, "MOD TOOLS", IM_COL32(0x9f, 0xbe, 0xd1, 255));
        ImGui::Dummy(ImVec2(w, plate + theme::S(6)));

        if (BeginGroup("Game", ImVec2(w, 0)))
        {
            ImGui::PushFont(theme::Bold(), 0.0f);
            ImGui::TextUnformatted("Black Ops II Zombies");
            ImGui::PopFont();
            Muted("Plutonium T6");
        }
        EndGroup();
        if (BeginGroup("Tools", ImVec2(w, 0)))
        {
            const ImVec2 full(-1, 0);
            if (Button("Game Paths", full))
                tab = Tab::Setup, selectTab = true;
            if (Button("Map Details & Artwork", full))
                tab = Tab::Artwork, selectTab = true;
            if (Button("Build Folder", full))
                OpenBuildFolder();
            if (Button("Conversion Report", full))
                LoadReport(), tab = Tab::Reports, selectTab = true;
            if (Button("Quick Help", full))
                shell::Info(window, "Quick start", "Setup \xE2\x86\x92 choose a map \xE2\x86\x92 Build Map \xE2\x86\x92 review Reports \xE2\x86\x92 Install \xE2\x86\x92 Launch Map.\n\nHelp \xE2\x86\x92 Quick help has the details.");
        }
        EndGroup();
        ImGui::EndChild();
    }

    void App::DrawRight(const float height)
    {
        ImGui::BeginChild("##right", ImVec2(RIGHT, height));
        if (BeginGroup("Build Steps", ImVec2(-1, 0)))
        {
            for (int i = 0; i < STEP_COUNT; i++)
            {
                const bool done = i < currentStep, active = i == currentStep;
                ImGui::PushStyleColor(ImGuiCol_Text, done ? theme::GREEN : active ? theme::ACCENT : theme::MUTED);
                ImGui::Text("%s  %s", done ? "\xE2\x9C\x93" : active ? "\xE2\x96\xB6" : "\xE2\x97\x8B", STEPS[i]);
                ImGui::PopStyleColor();
            }
        }
        EndGroup();
        if (BeginGroup("Install & Run", ImVec2(-1, 0)))
        {
            if (Button("Install to Plutonium", ImVec2(-1, 0), complete))
                Install();
            if (Button("Launch Map", ImVec2(-1, 0), installed))
                Launch();
        }
        EndGroup();
        if (BeginGroup("Map", ImVec2(-1, 0)))
        {
            ImGui::PushFont(theme::Bold(), 0.0f);
            ImGui::TextUnformatted(settings.project.empty() ? "(not set)" : settings.project.c_str());
            ImGui::PopFont();
            Muted(lastResult.empty() ? "No build in this session" : "Last build: " + lastResult, RIGHT - 20);
        }
        EndGroup();
        ImGui::EndChild();
    }

    void App::DrawBuilder()
    {
        const float left = ImGui::GetContentRegionAvail().x;
        if (BeginGroup("Source WaW Map", ImVec2(left, 0)))
        {
            Muted("Choose the map .ff from its original mod folder.");
            const float browse = ImGui::CalcTextSize("Browse\xE2\x80\xA6").x + ImGui::GetStyle().FramePadding.x * 2;
            Input("##fastfile", settings.fastfile, ImGui::GetContentRegionAvail().x - browse - 6);
            ImGui::SameLine();
            if (Button("Browse\xE2\x80\xA6"))
                ChooseMap();
            ImGui::TextUnformatted("BO2 map name");
            Input("##project", settings.project);
        }
        EndGroup();
        if (BeginGroup("Build Map", ImVec2(left, 0)))
        {
            Muted("Geometry \xC2\xB7 Scripts \xC2\xB7 Weapons \xC2\xB7 FX \xC2\xB7 Sounds");
            ImGui::PushFont(theme::Bold(), 0.0f);
            if (Button("Build Map", ImVec2(-1, theme::S(30))))
                Build();
            ImGui::PopFont();
            if (!busy)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, failures ? theme::MUTED : theme::GREEN);
                ImGui::TextWrapped("%s", failures ? (std::to_string(failures) + " setup items need attention. Click Build Map to see what is missing.").c_str()
                                                  : "Ready to build. Shared game assets prepare automatically.");
                ImGui::PopStyleColor();
            }
            if (!outputText.empty())
                Muted(outputText);
        }
        EndGroup();
    }

    void App::DrawSetup()
    {
        ImGui::AlignTextToFramePadding();
        Muted("Point to your game folders once. These settings are remembered.");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Auto-detect").x - 4);
        if (Button("Auto-detect"))
            Autodetect();
        if (BeginGroup("Game Installations", ImVec2(-1, 0)))
        {
            PathRow("World at War", "waw", theme::S(150));
            PathRow("Black Ops II + Mod Tools", "bo2", theme::S(150));
            PathRow("WaW Mod Tools (optional)", "waw_tools", theme::S(150));
        }
        EndGroup();
        ImGui::BeginDisabled(busy);
        if (ClassicCheckbox("Recover missing FX from WaW Mod Tools sources", &settings.source_fx))
            Save(), Refresh();
        ImGui::EndDisabled();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Advanced Paths").x - 4);
        if (Button("Advanced Paths"))
            advancedShown = !advancedShown;
        if (advancedShown)
        {
            if (BeginGroup("Advanced Paths", ImVec2(-1, 0)))
            {
                PathRow("WaW extractor folder", "t4", theme::S(150));
                PathRow("BO2 bridge tools folder", "t6", theme::S(150));
                PathRow("Audio decoder", "decoder", theme::S(150));
                PathRow("Build files folder", "work", theme::S(150));
            }
            EndGroup();
        }
        if (BeginGroup("Shared Game Assets", ImVec2(-1, 0)))
        {
            ImGui::AlignTextToFramePadding();
            Muted("Build Map prepares shared assets automatically. Extract All prepares them in advance.", ImGui::GetContentRegionAvail().x - 110);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Extract All").x - 4);
            if (Button("Extract All"))
                ExtractAll();
        }
        EndGroup();
        if (ImGui::BeginTable("##checks", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(-1, std::max(130.0f, ImGui::GetContentRegionAvail().y))))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Requirement", ImGuiTableColumnFlags_WidthFixed, 190);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 95);
            ImGui::TableSetupColumn("Details");
            ImGui::TableHeadersRow();
            for (const auto& c : setupChecks)
            {
                ImGui::TableNextRow();
                const ImVec4 color = c.ready ? theme::GREEN : c.required ? theme::RED : theme::INK;
                ImGui::TableNextColumn();
                ImGui::TextColored(color, "%s", c.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(color, "%s", c.ready ? "Ready" : c.required ? "Required" : "Optional");
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", c.detail.c_str());
            }
            ImGui::EndTable();
        }
    }

    void App::DrawReports()
    {
        Muted("Review unsupported features and compatibility decisions after building. Something wrong in game? Play the map with Play, "
              "then Save Diagnostics and post the ZIP: it carries the build's warnings, the game's log and your WaW file list.");
        const float buttons = ImGui::GetFrameHeightWithSpacing() + 4;
        ImGui::InputTextMultiline("##report", reportText.data(), reportText.size() + 1, ImVec2(-1, ImGui::GetContentRegionAvail().y - buttons),
                                  ImGuiInputTextFlags_ReadOnly);
        if (Button("Refresh Report"))
            LoadReport();
        ImGui::SameLine();
        if (Button("Save Diagnostics\xE2\x80\xA6"))
            SaveDiagnostics();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Open All Reports").x - 4);
        if (Button("Open All Reports"))
            OpenReports();
    }

    void App::DrawBottom(const float height)
    {
        // Processes: what runs now, and the red box that stops it
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::GROUP_TITLE);
        ImGui::TextUnformatted("Processes");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::PROCESS_BG);
        const float stopHeight = theme::S(56);
        if (ImGui::BeginChild("##processes", ImVec2(SIDE, height - stopHeight - ImGui::GetTextLineHeightWithSpacing() - 8), ImGuiChildFlags_Borders))
        {
            if (busy)
            {
                ImGui::PushFont(theme::Bold(), 0.0f);
                ImGui::TextWrapped("%s", kind == "build" ? "Building map" : kind == "extract" ? "Extracting game assets" : "Installing map");
                ImGui::PopFont();
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
                ImGui::Text("%02lld:%02lld elapsed", seconds / 60, seconds % 60);
                if (activeSettings)
                    ImGui::TextWrapped("%s", activeSettings->project.c_str());
            }
            else
                Muted("No active build");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        // the stop box: red while idle like the T6 launcher, live while running
        ImGui::PushStyleColor(ImGuiCol_Button, busy ? theme::STOP_ACTIVE : theme::STOP_IDLE);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, busy ? theme::STOP_HOVER : theme::STOP_IDLE);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, busy ? theme::STOP_HOVER : theme::STOP_IDLE);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.12f, 0.12f, 1));
        const char* label = busy ? (kind == "extract" ? "Stop Extraction\n\nEnds the process and\nevery tool it started" : "Stop Build\n\nEnds the process and\nevery tool it started")
                                 : "No Active Process\n\nStart one and then use\nthis button to stop it";
        if (ImGui::Button(label, ImVec2(SIDE, stopHeight)) && busy)
            Stop();
        ImGui::PopStyleColor(4);
        ImGui::EndGroup();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::CONSOLE_BG);
        if (ImGui::BeginChild("##console", ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
        {
            ImGui::PushFont(theme::Mono(), theme::FontSize());
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(console.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                {
                    const auto& line = console[static_cast<size_t>(i)];
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(TagColor(line.tag)));
                    ImGui::TextUnformatted(line.text.c_str());
                    ImGui::PopStyleColor();
                }
            if (scrollConsole)
                ImGui::SetScrollHereY(1.0f);
            scrollConsole = false;
            ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void App::DrawFooter()
    {
        ImGui::Dummy(ImVec2(SIDE, 0));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d Warnings", warnings);
        ImGui::SameLine();
        ImGui::TextColored(theme::AMBER, "\xE2\x9A\xA0");
        ImGui::SameLine(0, 30);
        ImGui::Text("%d Errors", errors);
        ImGui::SameLine();
        ImGui::TextColored(theme::RED, "\xE2\x9C\x96");
        const float right = ImGui::CalcTextSize("Verbose ConsoleSave ConsoleClear Console").x + 90;
        ImGui::SameLine(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - right));
        ImGui::BeginDisabled(busy);
        if (ClassicCheckbox("Verbose Console", &settings.verbose))
            Save();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ClassicButton("Save Console"))
            SaveConsole();
        ImGui::SameLine();
        if (ClassicButton("Clear Console"))
            console.clear();
    }

    void App::DrawArtwork()
    {
        const float inputs = std::min(430.0f, ImGui::GetContentRegionAvail().x * 0.45f);
        if (ImGui::BeginChild("##art_inputs", ImVec2(inputs, 0)))
        {
            const auto field = [&](const char* key, const char* label, const char* hint, const int mode)
            {
                ImGui::PushFont(theme::Bold(), 0.0f);
                ImGui::TextUnformatted(label);
                ImGui::PopFont();
                ImGui::PushID(key);
                Input("##v", *settings.Text(key));
                if (ImGui::IsItemActivated() && mode >= 0)
                    previews.mode = mode;
                ImGui::PopID();
                Muted(hint);
            };
            if (BeginGroup("Text shown in game", ImVec2(-1, 0)))
            {
                field("menu_title", "Map name", "Globe signpost, map selection, lobby and loading screen.", -1);
                field("menu_description", "Description", "Under the map name on the map selection screen.", Previews::MapSelection);
                field("menu_mode", "Mode line", "Replaces \"TRANZIT\" under the map name in the lobby and on the loading screen.", Previews::Lobby);
                field("menu_location", "Location", "Under the map name on the loading screen.", Previews::Loading);
            }
            EndGroup();
            const auto upload = [&](const char* key, const char* label, const bool layer)
            {
                ImGui::PushID(key);
                ImGui::PushFont(theme::Bold(), 0.0f);
                ImGui::TextUnformatted(label);
                ImGui::PopFont();
                const auto& value = *settings.Text(key);
                auto name = value.empty() ? std::string("No image selected") : l::Utf8(l::Path(value).filename());
                if (name.size() > 31)
                    name = name.substr(0, 30) + "\xE2\x80\xA6";
                ImGui::AlignTextToFramePadding();
                Muted(name);
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 130);
                if (Button("Upload\xE2\x80\xA6"))
                    ChooseArt(key, layer);
                ImGui::SameLine();
                if (Button("Clear"))
                    settings.Text(key)->clear(), Save(), Refresh();
                ImGui::PopID();
            };
            if (BeginGroup("Map selection layers", ImVec2(-1, 0)))
            {
                Muted("Aligned layers: three images on the same canvas. Large has a transparent cutout, Blit fills it, Blur is the blurred background. "
                      "Stock layout: Blit 512 \xC3\x97 256, Large and Blur 2048 \xC3\x97 2048.");
                upload("menu_blit", "Blit", true);
                upload("menu_large", "Large", true);
                upload("menu_blur", "Blur", true);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Start location (Blit pixels x,y)");
                ImGui::SameLine();
                Input("##point", settings.menu_location_point, 110);
                Muted("Blank: where the Blit is densest.");
                if (Button("Clear map layers"))
                    settings.menu_blit.clear(), settings.menu_large.clear(), settings.menu_blur.clear(), Save(), Refresh();
            }
            EndGroup();
            if (BeginGroup("More images (optional)", ImVec2(-1, 0)))
            {
                upload("menu_icon", "Globe icon", false);
                Muted("Transparent PNG, drawn on the globe and in the lobby list. Default: cut from your art.");
                upload("menu_preview", "Lobby preview", false);
                Muted("Custom Games map picture. Fills the whole frame. Default: your completed art.");
                upload("menu_loading", "Loading screen", false);
                Muted("Full-screen image while the map loads. Default: your completed art.");
            }
            EndGroup();
            if (BeginGroup("Globe position", ImVec2(-1, 0)))
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Longitude");
                ImGui::SameLine();
                Input("##lon", settings.menu_longitude, theme::S(70));
                if (ImGui::IsItemActivated())
                    previews.mode = Previews::Globe;
                ImGui::SameLine();
                ImGui::TextUnformatted("Latitude");
                ImGui::SameLine();
                Input("##lat", settings.menu_latitude, theme::S(70));
                if (ImGui::IsItemActivated())
                    previews.mode = Previews::Globe;
                Muted("BO2's own convention: east is negative (Alcatraz is 122, Origins -11).");
            }
            EndGroup();
            if (BeginGroup("Loading song (optional)", ImVec2(-1, 0)))
            {
                Input("##song", settings.loading_song);
                if (Button("Upload song\xE2\x80\xA6"))
                    ChooseSong();
                ImGui::SameLine();
                if (Button("Clear song"))
                    settings.loading_song.clear(), Save(), Refresh();
                Muted("MP3, WAV, FLAC, OGG or M4A, played while the map loads.");
            }
            EndGroup();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##art_preview", ImVec2(0, 0)))
            DrawPreview();
        ImGui::EndChild();
    }

    void App::DrawPreview()
    {
        if (previews.Draw(gpu, settings))
            Save(), Refresh();
    }
} // namespace w2b::ui
