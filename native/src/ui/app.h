#pragma once
// The desktop launcher: state and actions (app.cpp), drawn by views.cpp.
// Same workflows as the former tkinter launcher (gui.py, gui_artwork.py).

#include "gpu.h"
#include "previews.h"
#include "launcher/build.h"

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace w2b::ui
{
    namespace fs = std::filesystem;
    enum class Tag
    {
        Plain,
        Info,
        Warning,
        Error,
        Success
    };

    struct ConsoleLine
    {
        std::string text;
        Tag tag = Tag::Plain;
    };

    enum class Tab
    {
        Builder,
        Artwork,
        Setup,
        Reports
    };

    class App
    {
    public:
        // settingsPath: another settings.json than the user's (--settings=)
        App(Gpu& gpu, HWND window, std::optional<fs::path> settingsPath = std::nullopt);
        ~App();
        void Frame();          // one ImGui frame of the whole window
        bool Busy() const
        {
            return busy;
        }
        bool RequestClose();   // false while a build is stopping
        bool WantsQuit() const
        {
            return quit;
        }
        bool NeedsTicks() const;
        void ShowTab(const std::string& name); // --tab=builder|artwork|setup|reports

    private:
        // ---- actions (app.cpp)
        void Refresh();
        void Save();
        void ChooseMap();
        void ChoosePath(const std::string& field);
        void Autodetect();
        void Build();
        void ExtractAll();
        void Install();
        void Launch();
        void Stop();
        void Start(const std::string& kind, std::function<int(launcher::ProcessRunner&)> operation);
        void Drain();
        void OnLine(const std::string& line);
        void OnFinished(const std::string& kind, int code, const std::string& error);
        void FailureDetails();
        void Log(const std::string& text, Tag tag = Tag::Plain);
        void SaveConsole();
        void LoadReport();
        void OpenBuildFolder();
        void OpenReports();
        void SaveDiagnostics();
        void ChooseArt(const std::string& key, bool layer);
        void ChooseSong();
        std::optional<launcher::BuildPaths> Paths(bool quiet = false);

        // ---- drawing (views.cpp)
        void DrawMenu();
        void DrawSidebar(float height);
        void DrawRight(float height);
        void DrawBuilder();
        void DrawArtwork();
        void DrawSetup();
        void DrawReports();
        void DrawBottom(float height);
        void DrawFooter();
        void DrawPreview();
        bool PathRow(const char* label, const std::string& field, float labelWidth);
        bool Input(const char* id, std::string& value, float width = -1.0f);
        bool Button(const char* label, const ImVec2& size = ImVec2(0, 0), bool enabled = true);

        Gpu& gpu;
        HWND window;
        std::optional<fs::path> settingsPath;
        launcher::Settings settings;
        std::vector<launcher::Check> setupChecks;
        int failures = 0;
        bool complete = false, installed = false;
        std::string outputText;
        bool quit = false;
        Tab tab = Tab::Builder;
        bool selectTab = false;
        bool advancedShown = false;

        // the running operation
        std::atomic<bool> busy{false};
        std::string kind;
        std::chrono::steady_clock::time_point started;
        std::optional<launcher::Settings> activeSettings;
        std::unique_ptr<launcher::ProcessRunner> runner;
        std::thread worker;
        std::mutex eventsMutex;
        struct Event
        {
            bool finished = false;
            std::string line, kind, error;
            int code = 0;
        };
        std::deque<Event> events;
        std::string status = "Choose a WaW map to begin.";
        std::string lastResult;
        int currentStep = -1, warnings = 0, errors = 0;
        bool closing = false;

        std::deque<ConsoleLine> console;
        bool scrollConsole = false;
        std::string reportText;
        std::string diagnosticsPath;

        // artwork preview
        Previews previews;
        std::string windowTitle;
        std::string artStatus;
    };
} // namespace w2b::ui
