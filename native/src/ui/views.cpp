// Launcher drawing: the T6 Mod Tools Launcher layout (sidebar, tabs, right
// column, process box and console) with the workflows of the old gui.py.
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
        constexpr float SIDE = 158.0f, RIGHT = 210.0f;

        // a WinForms-style group box: title over a bordered frame
        bool BeginGroup(const char* title, const ImVec2& size)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::GROUP_TITLE);
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
            return ImGui::BeginChild(title, size, ImGuiChildFlags_Borders | (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0));
        }

        void EndGroup()
        {
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0, 2));
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
        const bool pressed = ImGui::Button(label, size);
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
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("##launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar
                         | ImGuiWindowFlags_NoBringToFrontOnFocus);
        DrawMenu();
        const float footer = ImGui::GetFrameHeightWithSpacing() + 4;
        const float total = ImGui::GetContentRegionAvail().y;
        const float bottom = std::max(170.0f, total * 0.36f);
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
                            std::string("WaW \xE2\x86\x92 BO2 Mod Tools ") + l::Version()
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
        const float w = SIDE - 4, h = 118;
        draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(0x20, 0x28, 0x2f, 255));
        const auto centered = [&](ImFont* font, const float size, const float y, const char* text, const ImU32 color)
        {
            const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
            draw->AddText(font, size, ImVec2(at.x + (w - extent.x) * 0.5f, at.y + y), color, text);
        };
        centered(theme::Bold(), theme::FontSize() * 0.95f, 16, "WORLD AT WAR", IM_COL32(0xd4, 0xdc, 0xe2, 255));
        centered(theme::Bold(), theme::FontSize() * 2.1f, 38, "\xE2\x86\x92 BO2", IM_COL32_WHITE);
        draw->AddLine(ImVec2(at.x + 20, at.y + 86), ImVec2(at.x + w - 20, at.y + 86), IM_COL32(0x55, 0x7b, 0x94, 255), 2);
        centered(theme::Bold(), theme::FontSize() * 0.85f, 94, "MOD TOOLS", IM_COL32(0x9f, 0xbe, 0xd1, 255));
        ImGui::Dummy(ImVec2(w, h + 6));

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
            const auto flag = [&](const char* label, bool& value)
            {
                ImGui::BeginDisabled(busy);
                if (ImGui::Checkbox(label, &value))
                    Save(), Refresh();
                ImGui::EndDisabled();
            };
            flag("Refresh source files (slower rebuild)", settings.redump);
            flag("Resume: keep my edits to the staged files (skip re-staging)", settings.resume);
            flag("Use BO2 stock perks and machines (keeps the WaW Pack-a-Punch)", settings.bo2_stock_perks);
            flag("Remaster with BO2 materials (props ported from BO2)", settings.remaster);
            ImGui::PushFont(theme::Bold(), 0.0f);
            if (Button("Build Map", ImVec2(-1, ImGui::GetFrameHeight() * 1.6f)))
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
            PathRow("World at War", "waw", 190);
            PathRow("Black Ops II + Mod Tools", "bo2", 190);
            PathRow("WaW Mod Tools (optional)", "waw_tools", 190);
        }
        EndGroup();
        ImGui::BeginDisabled(busy);
        if (ImGui::Checkbox("Recover missing FX from WaW Mod Tools sources", &settings.source_fx))
            Save(), Refresh();
        ImGui::EndDisabled();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Advanced Paths").x - 4);
        if (Button("Advanced Paths"))
            advancedShown = !advancedShown;
        if (advancedShown)
        {
            if (BeginGroup("Advanced Paths", ImVec2(-1, 0)))
            {
                PathRow("WaW extractor folder", "t4", 190);
                PathRow("BO2 bridge tools folder", "t6", 190);
                PathRow("Audio decoder", "decoder", 190);
                PathRow("Build files folder", "work", 190);
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
        Muted("Review unsupported features and compatibility decisions after building.");
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
        const float stopHeight = 70;
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
            ImGui::PushFont(theme::Mono(), theme::FontSize() * 13.0f / 15.0f);
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
        if (ImGui::Checkbox("Verbose Console", &settings.verbose))
            Save();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Save Console"))
            SaveConsole();
        ImGui::SameLine();
        if (ImGui::Button("Clear Console"))
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
                    previewMode = mode;
                ImGui::PopID();
                Muted(hint);
            };
            if (BeginGroup("Text shown in game", ImVec2(-1, 0)))
            {
                field("menu_title", "Map name", "Globe signpost, map selection, lobby and loading screen.", -1);
                field("menu_description", "Description", "Under the map name on the map selection screen.", 3);
                field("menu_mode", "Mode line", "Replaces \"TRANZIT\" under the map name in the lobby.", -1);
                field("menu_location", "Location", "Under the map name on the loading screen.", -1);
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
                Input("##lon", settings.menu_longitude, 80);
                ImGui::SameLine();
                ImGui::TextUnformatted("Latitude");
                ImGui::SameLine();
                Input("##lat", settings.menu_latitude, 80);
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
        static const char* modes[] = {"Map selection", "Large", "Blur", "Blit"};
        ImGui::PushStyleColor(ImGuiCol_Text, theme::GROUP_TITLE);
        ImGui::TextUnformatted("In-game preview");
        ImGui::PopStyleColor();
        for (int i = 0; i < 4; i++)
        {
            if (i)
                ImGui::SameLine();
            if (ImGui::RadioButton(modes[i], previewMode == i))
                previewMode = i;
        }
        std::string errorText;
        std::vector<std::string> errorsList, missing;
        Texture layers[3];
        const char* keys[] = {"menu_blit", "menu_large", "menu_blur"};
        const char* names[] = {"Blit", "Large", "Blur"};
        for (int i = 0; i < 3; i++)
        {
            const auto& value = *settings.Text(keys[i]);
            if (value.empty())
            {
                missing.push_back(names[i]);
                continue;
            }
            layers[i] = gpu.Image(l::Path(value), &errorText);
            if (!errorText.empty())
                errorsList.push_back(std::string(names[i]) + ": " + errorText);
        }
        const ImVec2 area(ImGui::GetContentRegionAvail().x, std::max(180.0f, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 4));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + area.x, origin.y + area.y), IM_COL32(0x0c, 0x0e, 0x12, 255));
        const Texture* shown = previewMode == 1 ? &layers[1] : previewMode == 2 ? &layers[2] : previewMode == 3 ? &layers[0] : nullptr;
        if (previewMode == 0 && layers[1] && layers[0])
        {
            // aligned layers: Blit under the Large cutout, on the Large canvas
            const auto rect = Fit(layers[1], area);
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddImage(reinterpret_cast<ImTextureID>(layers[0].view), rect.Min, rect.Max);
            draw->AddImage(reinterpret_cast<ImTextureID>(layers[1].view), rect.Min, rect.Max);
        }
        else if (shown && *shown)
            Fit(*shown, area);
        else
        {
            const char* text = previewMode == 0 ? "Upload Blit and Large to preview the map selection." : "Upload this layer to preview it.";
            const ImVec2 size = ImGui::CalcTextSize(text);
            ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + (area.x - size.x) * 0.5f, origin.y + (area.y - size.y) * 0.5f), IM_COL32(0x9a, 0xa4, 0xad, 255), text);
        }
        ImGui::Dummy(area);
        std::string note;
        for (const auto& e : errorsList)
            note += (note.empty() ? "" : "\n") + e;
        if (note.empty())
        {
            std::string list;
            for (const auto& m : missing)
                list += (list.empty() ? "" : ", ") + m;
            const bool aligned = layers[0] && layers[1] && layers[2]
                                 && ((layers[0].width == layers[1].width && layers[1].width == layers[2].width && layers[0].height == layers[1].height
                                      && layers[1].height == layers[2].height)
                                     || (layers[0].width == 512 && layers[0].height == 256 && layers[1].width == 2048 && layers[1].height == 2048 && layers[2].width == 2048
                                         && layers[2].height == 2048));
            note = !missing.empty() ? "Map selection layers still needed: " + list
                   : aligned        ? "Map selection layers use a supported layout."
                                    : "Warning: Layer canvas sizes differ. They will be resized to the same canvas, but objects may not line up and the result may look "
                                      "distorted. You can still preview and build.";
        }
        Muted(note);
        Muted("The globe, lobby and loading-screen previews of the former launcher follow in a later update.");
    }
} // namespace w2b::ui
