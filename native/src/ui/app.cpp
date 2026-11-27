// Launcher actions (see app.h).
#include "app.h"

#include "core/files.h"
#include "core/regex.h"
#include "launcher/diagnostics.h"
#include "shell.h"
#include "steps.h"

#include <algorithm>
#include <fstream>

namespace w2b::ui
{
    namespace l = launcher;
    namespace fs = std::filesystem;

    namespace
    {
        bool IsFile(const fs::path& p)
        {
            std::error_code e;
            return fs::is_regular_file(p, e);
        }
    } // namespace

    App::App(Gpu& gpu, const HWND window, std::optional<fs::path> path)
        : gpu(gpu), window(window), settingsPath(std::move(path)), settings(l::Discover(l::Settings::Load(settingsPath)))
    {
        // Resume and BO2 stock perks are not offered: a value an older launcher saved must not keep
        // applying unseen. Cold rebuild is offered but starts unticked: builds are warm by default.
        settings.redump = settings.resume = settings.bo2_stock_perks = false;
        Refresh();
        if (!settings.fastfile.empty())
            status = "Map selected. Click Build Map when setup is ready.";
        Log("B2W Mod Tools ready (" + l::BuildLabel() + "). Choose your source map, then click Build Map.", Tag::Info);
        LoadReport();
    }

    App::~App()
    {
        if (runner)
            runner->Cancel();
        if (worker.joinable())
            worker.join();
    }

    void App::ShowTab(const std::string& value)
    {
        // artwork:N also picks preview N (Previews::Mode)
        const auto colon = value.find(':');
        const auto name = value.substr(0, colon);
        if (colon != std::string::npos)
            previews.mode = std::atoi(value.c_str() + colon + 1);
        tab = name == "artwork" ? Tab::Artwork : name == "setup" ? Tab::Setup : name == "reports" ? Tab::Reports : Tab::Builder;
        selectTab = true;
    }

    bool App::NeedsTicks() const
    {
        return busy || closing;
    }

    void App::Refresh()
    {
        setupChecks = l::Preflight(settings, false);
        failures = 0;
        for (const auto& c : l::Preflight(settings))
            failures += c.required && !c.ready;
        try
        {
            const auto paths = l::BuildPaths::ForSettings(settings);
            complete = paths.Complete(settings.project, &settings.bo2_stock_perks);
            outputText = "Build files are managed automatically. Use Build Folder to find your package.";
        }
        catch (const std::exception&)
        {
            complete = false;
            outputText.clear();
        }
        installed = l::Installed(settings);
    }

    void App::Save()
    {
        try
        {
            settings.Save(settingsPath);
        }
        catch (const std::exception& error)
        {
            Log(std::string("Could not save settings: ") + error.what(), Tag::Warning);
        }
    }

    void App::ChooseMap()
    {
        if (busy)
            return;
        const auto mods = l::UserDirectory().parent_path() / "Activision/CoDWaW/mods";
        std::error_code e;
        auto picked = shell::OpenFile(window, L"Choose the WaW map fastfile (not mod.ff)", {{L"WaW fastfiles", L"*.ff"}},
                                      fs::exists(mods, e) ? l::Utf8(mods) : std::string());
        if (!picked)
            return;
        auto path = l::MapZone(l::Path(*picked));
        const auto stem = l::Lower(l::Utf8(path.stem()));
        if (stem == "mod" || stem == "mod_load" || stem.ends_with("_patch"))
        {
            const auto maps = l::MapFastfiles(path.parent_path());
            if (maps.size() == 1)
                path = maps[0];
            else if (!(maps.empty() && stem == "mod"))
            {
                shell::Info(window, "Choose the map", "Select the map fastfile itself, rather than mod.ff or a patch zone.");
                return;
            }
        }
        settings.fastfile = l::Utf8(path);
        settings.project = l::ProjectName(settings.fastfile);
        Save();
        tab = Tab::Builder;
        selectTab = true;
        status = "Map selected. Click Build Map when setup is ready.";
        Refresh();
    }

    void App::ChoosePath(const std::string& field)
    {
        if (busy)
            return;
        std::optional<std::string> path;
        if (field == "decoder")
            path = shell::OpenFile(window, L"Choose the audio decoder", {{L"Executable", L"*.exe"}});
        else
        {
            auto title = field;
            std::replace(title.begin(), title.end(), '_', ' ');
            path = shell::ChooseFolder(window, L"Choose " + shell::Wide(title), *settings.Text(field));
        }
        if (path)
        {
            *settings.Text(field) = *path;
            Save();
            Refresh();
        }
    }

    void App::Autodetect()
    {
        if (busy)
            return;
        settings = l::Discover(settings);
        Save();
        Refresh();
        Log("Checked Steam libraries and bundled tools. Review the Setup list.", Tag::Info);
    }

    void App::Build()
    {
        if (busy)
            return;
        std::vector<l::Check> failed;
        for (const auto& c : l::Preflight(settings))
            if (c.required && !c.ready)
                failed.push_back(c);
        if (!failed.empty())
        {
            std::string text;
            for (const auto& c : failed)
                text += (text.empty() ? "" : "\n\n") + c.name + "\n" + c.detail;
            shell::Info(window, "Setup required", text);
            const auto mapOnly = [](const std::string& n) { return n == "Source map" || n == "BO2 map name" || n == "Map artwork"; };
            bool other = false, art = false;
            for (const auto& c : failed)
            {
                other = other || !mapOnly(c.name);
                art = art || c.name == "Map artwork";
            }
            tab = failed[0].name == "Map artwork" ? Tab::Artwork : other ? Tab::Setup : art ? Tab::Artwork : Tab::Builder;
            selectTab = true;
            return;
        }
        if (!settings.fastfile.empty() && l::MapZone(l::Path(settings.fastfile)) != l::Path(settings.fastfile))
            settings.fastfile = l::Utf8(l::MapZone(l::Path(settings.fastfile)));
        Save();
        auto snapshot = settings;
        // a warm rebuild reuses the map's extracted files; when an older converter build extracted them, offer
        // to refresh them first so this build's fixes reach the map
        if (!snapshot.redump)
            if (const auto older = l::OlderMapCache(snapshot); !older.empty())
            {
                std::string names;
                for (const auto& by : older)
                    names += (names.empty() ? "" : ", ") + by;
                snapshot.redump = shell::AskYesNo(
                    window, "Refresh this map's cache?",
                    "This map's extracted source files come from " + names + ", and this is " + l::BuildLabel() +
                        ".\n\nA cold rebuild extracts them again so the map gets this build's fixes (a minute or two longer).\n\n"
                        "Yes: cold rebuild.\nNo: warm rebuild with the cached files.");
            }
        warnings = errors = 0;
        currentStep = -1;
        Log(std::string(snapshot.redump ? "\nCold rebuilding " : "\nBuilding ") + settings.project + " from " + settings.fastfile, Tag::Info);
        Start("build", [snapshot](l::ProcessRunner& r) { return l::PerformBuild(snapshot, r); });
    }

    void App::ExtractAll()
    {
        if (busy)
            return;
        std::string text;
        for (const auto& c : l::Preflight(settings, false))
            if (c.required && !c.ready && c.name != "Audio decoder" && c.name != "WaW source tools")
                text += (text.empty() ? "" : "\n\n") + c.name + "\n" + c.detail;
        if (!text.empty())
        {
            shell::Info(window, "Setup required", text);
            return;
        }
        Save();
        Log("Extracting all installed game zones into the shared cache.", Tag::Info);
        const auto snapshot = settings;
        Start("extract", [snapshot](l::ProcessRunner& r) { return l::PerformExtractAll(snapshot, r); });
    }

    void App::Install()
    {
        if (busy || !complete)
            return;
        const auto snapshot = settings;
        Start("install", [snapshot](l::ProcessRunner& r) { return l::PerformInstall(snapshot, r); });
    }

    void App::Launch()
    {
        if (busy)
            return;
        try
        {
            l::LaunchMap(settings);
            Log("Launched " + settings.project + " in Plutonium T6.", Tag::Success);
        }
        catch (const std::exception& error)
        {
            shell::Info(window, "Plutonium required", error.what());
        }
    }

    void App::Stop()
    {
        if (runner && busy)
        {
            status = "Stopping this build\xE2\x80\xA6";
            std::thread([r = runner.get()] { r->Cancel(); }).detach();
        }
    }

    void App::Start(const std::string& what, std::function<int(l::ProcessRunner&)> operation)
    {
        if (worker.joinable())
            worker.join();
        busy = true;
        kind = what;
        started = std::chrono::steady_clock::now();
        activeSettings = settings;
        diagnosticsPath.clear();
        failureError.clear();
        runner = std::make_unique<l::ProcessRunner>(
            [this](const std::string& line)
            {
                std::lock_guard lock(eventsMutex);
                events.push_back({false, line});
            },
            settings.verbose);
        status = what == "build" ? "Starting build\xE2\x80\xA6" : what == "extract" ? "Extracting all game assets\xE2\x80\xA6" : "Installing to Plutonium\xE2\x80\xA6";
        worker = std::thread(
            [this, what, operation, r = runner.get()]
            {
                Event done{true, "", what};
                try
                {
                    done.code = operation(*r);
                }
                catch (const std::exception& error)
                {
                    done.code = -1;
                    done.error = error.what();
                }
                std::lock_guard lock(eventsMutex);
                events.push_back(done);
            });
    }

    void App::Drain()
    {
        std::deque<Event> pending;
        {
            std::lock_guard lock(eventsMutex);
            pending.swap(events);
        }
        for (const auto& e : pending)
        {
            if (e.finished)
                OnFinished(e.kind, e.code, e.error);
            else
                OnLine(e.line);
        }
    }

    void App::OnLine(const std::string& line)
    {
        if (line.starts_with("[activity]"))
        {
            Log(line, Tag::Info);
            return;
        }
        if (line.starts_with("[staging]"))
            status = "Step " + std::to_string(currentStep + 1) + " of " + std::to_string(STEP_COUNT) + " \xE2\x80\x94 " + line.substr(std::min<size_t>(10, line.size()));
        static const regex::Regex stage(R"(^==\s+(\d+[a-z]?)\.)");
        if (const auto m = stage.MatchAt(line))
            if (const int index = StageIndex((*m)[1]); index >= 0)
            {
                currentStep = index;
                status = "Step " + std::to_string(currentStep + 1) + " of " + std::to_string(STEP_COUNT) + " \xE2\x80\x94 " + STEPS[currentStep];
            }
        const auto lower = l::Lower(line);
        static const regex::Regex error(R"(\b(error|exception|traceback)\b)");
        static const regex::Regex warning(R"(\bwarning\b)");
        Tag tag = line.starts_with("==") ? Tag::Info : Tag::Plain;
        if (error.Search(lower) && lower.find("0 errors") == std::string::npos)
        {
            tag = Tag::Error;
            errors++;
        }
        else if (warning.Search(lower))
        {
            tag = Tag::Warning;
            warnings++;
        }
        Log(line, tag);
    }

    void App::OnFinished(const std::string& what, const int code, const std::string& error)
    {
        if (worker.joinable())
            worker.join();
        const bool cancelled = runner && runner->Cancelled();
        busy = false;
        lastResult = cancelled ? "Stopped" : code ? "Failed" : "Complete";
        if (cancelled)
        {
            status = what == "extract" ? "Extraction stopped. Run Extract All again to resume." : "Build stopped. No successful build was recorded.";
            Log("Stopped the active build and its child tools.", Tag::Warning);
        }
        else if (code)
        {
            errors++;
            status = what == "extract" ? "Extraction failed. Run Extract All again to resume." : "Build failed. Review the console and reports.";
            Log(error.empty() ? "Native tool exited with code " + std::to_string(code) + "." : error, Tag::Error);
            FailureDetails();
            if (activeSettings)
                try
                {
                    std::string text;
                    for (const auto& c : console)
                        text += c.text + "\n";
                    failureError = error.empty() ? "Native tool exited with code " + std::to_string(code) + "." : error;
                    diagnosticsPath = l::Utf8(l::CreateBundle(*activeSettings, text, failureError));
                    Log("Diagnostics ZIP ready: " + diagnosticsPath, Tag::Info);
                    Log("Use Reports \xE2\x86\x92 Save Diagnostics to share the complete failure report.", Tag::Info);
                }
                catch (const std::exception& bundleError)
                {
                    Log(std::string("Could not save diagnostics: ") + bundleError.what(), Tag::Warning);
                }
        }
        else if (what == "extract")
        {
            status = "Shared game assets extracted. Ready to build maps.";
            Log("Extract All complete. Future builds reuse the shared game cache.", Tag::Success);
        }
        else
        {
            status = what == "build" ? "Map built. Install to Plutonium when ready." : "Installed. Launch Map to playtest.";
            Log(what == "build" ? "Build complete. Package verified and ready to install." : "Installed to Plutonium.", Tag::Success);
            if (what == "build")
                currentStep = STEP_COUNT;
        }
        LoadReport();
        Refresh();
    }

    void App::FailureDetails()
    {
        if (!activeSettings)
            return;
        std::optional<l::BuildPaths> paths;
        try
        {
            paths = l::BuildPaths::ForSettings(*activeSettings);
        }
        catch (const std::exception&)
        {
            return;
        }
        const auto since = std::filesystem::file_time_type::clock::now()
                           - std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::steady_clock::now() - started) - std::chrono::seconds(2);
        std::vector<std::pair<std::filesystem::file_time_type, fs::path>> logs;
        std::error_code e;
        for (fs::directory_iterator it(paths->stage, e), end; !e && it != end; it.increment(e))
            if (l::Lower(l::Utf8(it->path().extension())) == ".log" && it->last_write_time(e) >= since)
                logs.emplace_back(it->last_write_time(e), it->path());
        for (fs::recursive_directory_iterator it(paths->mod, e), end; !e && it != end; it.increment(e))
            if (it->is_regular_file(e) && l::Lower(l::Utf8(it->path().extension())) == ".log" && it->last_write_time(e) >= since)
                logs.emplace_back(it->last_write_time(e), it->path());
        std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t k = 0; k < std::min<size_t>(2, logs.size()); k++)
        {
            Log("Native log: " + l::Utf8(logs[k].second), Tag::Info);
            std::ifstream stream(logs[k].second, std::ios::binary | std::ios::ate);
            const auto size = static_cast<long long>(stream.tellg());
            std::string tail(static_cast<size_t>(std::min<long long>(size, 12000)), '\0');
            stream.seekg(size - static_cast<long long>(tail.size()));
            stream.read(tail.data(), static_cast<std::streamsize>(tail.size()));
            if (tail.substr(0, 100).find('\0') != std::string::npos)
            {
                std::wstring wide(tail.size() / 2, L'\0');
                std::memcpy(wide.data(), tail.data(), wide.size() * 2);
                tail = files::Utf8(fs::path(wide));
            }
            std::vector<std::string> lines;
            size_t start = 0;
            for (size_t i = 0; i <= tail.size(); i++)
                if (i == tail.size() || tail[i] == '\n')
                {
                    auto line = tail.substr(start, i - start);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    lines.push_back(line);
                    start = i + 1;
                }
            for (size_t i = lines.size() > 35 ? lines.size() - 35 : 0; i < lines.size(); i++)
                Log(lines[i]);
        }
    }

    void App::Log(const std::string& text, const Tag tag)
    {
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); i++)
            if (i == text.size() || text[i] == '\n')
            {
                console.push_back({text.substr(start, i - start), tag});
                start = i + 1;
            }
        if (console.size() > 7000)
            console.erase(console.begin(), console.begin() + 1000);
        scrollConsole = true;
    }

    void App::SaveConsole()
    {
        const auto path = shell::SaveFile(window, L"Save build console", {{L"Build log", L"*.log"}}, L"waw2bo2-build.log", L"log");
        if (!path)
            return;
        std::string text;
        for (size_t i = 0; i < console.size(); i++)
            text += (i ? "\n" : "") + console[i].text;
        try
        {
            files::WriteText(l::Path(*path), text);
        }
        catch (const std::exception& error)
        {
            shell::Error(window, "Save failed", error.what());
        }
    }

    std::optional<l::BuildPaths> App::Paths(const bool quiet)
    {
        try
        {
            return l::BuildPaths::ForSettings(settings);
        }
        catch (const std::exception&)
        {
            if (!quiet)
                shell::Info(window, "Select a map", "Choose a source map first.");
            return std::nullopt;
        }
    }

    void App::OpenBuildFolder()
    {
        if (const auto paths = Paths())
        {
            fs::create_directories(paths->root);
            shell::Open(l::Utf8(paths->root));
        }
    }

    void App::OpenReports()
    {
        if (const auto paths = Paths())
        {
            const auto folder = paths->stage / "zone_raw" / l::Path(settings.project);
            fs::create_directories(folder);
            shell::Open(l::Utf8(folder));
        }
    }

    void App::LoadReport()
    {
        reportText = "No conversion report yet. Build a map to create one.";
        const auto paths = Paths(true);
        if (!paths)
            return;
        const auto file = paths->stage / "zone_raw" / l::Path(settings.project) / "bridge_stage.report.json";
        if (!IsFile(file))
            return;
        try
        {
            const auto data = util::Json::parse(files::ReadText(file));
            const auto errorsList = data.value("errors", util::Json::array()), warningsList = data.value("warnings", util::Json::array());
            std::string text = settings.project + "\n" + std::to_string(errorsList.size()) + " staging errors \xC2\xB7 " + std::to_string(warningsList.size())
                               + " staging warnings\n\n";
            for (const auto& [title, items] : {std::pair<const char*, const util::Json*>{"ERRORS", &errorsList}, {"WARNINGS / COMPATIBILITY", &warningsList}})
            {
                std::string body;
                for (const auto& item : *items)
                    body += (body.empty() ? "" : "\n") + (item.is_string() ? item.get<std::string>() : item.dump());
                text += std::string(title) + "\n" + (body.empty() ? "None" : body) + "\n\n";
            }
            reportText = text + "Full report\n" + l::Utf8(file) + "\n\n" + data.dump(2);
        }
        catch (const std::exception& error)
        {
            reportText = std::string("Cannot read report: ") + error.what();
        }
    }

    void App::SaveDiagnostics()
    {
        // always a fresh bundle: after a build that worked, it carries the game's log of the match played since
        try
        {
            std::string text;
            for (const auto& c : console)
                text += c.text + "\n";
            diagnosticsPath = l::Utf8(l::CreateBundle(activeSettings.value_or(settings), text, failureError));
        }
        catch (const std::exception& error)
        {
            shell::Error(window, "Diagnostics could not be saved", error.what());
            return;
        }
        const auto destination = shell::SaveFile(window, L"Save build diagnostics", {{L"Diagnostics ZIP", L"*.zip"}}, l::Path(diagnosticsPath).filename().wstring(), L"zip");
        if (!destination)
            return;
        std::error_code e;
        if (fs::weakly_canonical(l::Path(*destination), e) != fs::weakly_canonical(l::Path(diagnosticsPath), e))
            fs::copy_file(l::Path(diagnosticsPath), l::Path(*destination), fs::copy_options::overwrite_existing, e);
        if (e)
            shell::Error(window, "Diagnostics could not be saved", e.message());
        else
            Log("Diagnostics saved: " + *destination, Tag::Info);
    }

    void App::ChooseArt(const std::string& key, const bool layer)
    {
        const auto path = shell::OpenFile(window, layer ? L"Upload map layer \xE2\x80\x94 aligned canvas or stock artwork" : L"Upload image",
                                          {{L"Images", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp"}, {L"All files", L"*.*"}});
        if (!path)
            return;
        try
        {
            const auto image = l::DecodeImage(l::Path(*path));
            if (key == "menu_blit" && image.MinAlpha() == 255)
                throw std::runtime_error("Blit needs a transparent background. Choose a PNG or TGA with alpha transparency.");
        }
        catch (const std::exception& error)
        {
            shell::Error(window, layer ? "Invalid map image" : "Invalid image", error.what());
            return;
        }
        *settings.Text(key) = *path;
        if (key == "menu_icon")
            previews.mode = Previews::Globe;
        else if (key == "menu_preview")
            previews.mode = Previews::Lobby;
        else if (key == "menu_loading")
            previews.mode = Previews::Loading;
        Save();
        Refresh();
    }

    void App::ChooseSong()
    {
        if (const auto path = shell::OpenFile(window, L"Upload loading song", {{L"Audio files", L"*.mp3;*.wav;*.flac;*.ogg;*.m4a"}, {L"All files", L"*.*"}}))
        {
            settings.loading_song = *path;
            Save();
            Refresh();
        }
    }

    bool App::RequestClose()
    {
        if (busy)
        {
            if (!closing && !shell::AskYesNo(window, "Build in progress", "Stop this build and close the launcher?"))
                return false;
            closing = true;
            Stop();
            return false;
        }
        Save();
        quit = true;
        return true;
    }
} // namespace w2b::ui
