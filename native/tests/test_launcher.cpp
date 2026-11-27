// Launcher services: process streaming, cancellation, naming rules.
#include "launcher/build.h"
#include "launcher/diagnostics.h"
#include "core/zip.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace w2b::launcher;

namespace
{
    std::filesystem::path Temp(const char* name)
    {
        return std::filesystem::temp_directory_path() / "w2b_launcher_tests" / name;
    }
} // namespace

TEST_CASE("runner streams lines, keeps the exit code and writes a CRLF log")
{
    std::vector<std::string> lines;
    ProcessRunner runner([&](const std::string& line) { lines.push_back(line); });
    const auto log = Temp("echo.log");
    const int code = runner.Run({"cmd.exe", "/c", "echo first&& echo second  && exit 3"}, log);
    CHECK(code == 3);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "first");
    CHECK(lines[1] == "second");
    std::ifstream stream(log, std::ios::binary);
    std::stringstream text;
    text << stream.rdbuf();
    CHECK(text.str() == "first\r\nsecond\r\n");
}

TEST_CASE("cancel ends the whole process tree")
{
    ProcessRunner runner([](const std::string&) {});
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        runner.Cancel();
    });
    const auto start = std::chrono::steady_clock::now();
    // a child of the child: ping keeps running unless the job ends it
    runner.Run({"cmd.exe", "/c", "ping -n 30 127.0.0.1 >nul"}, Temp("cancel.log"));
    stopper.join();
    CHECK(runner.Cancelled());
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
    CHECK(runner.Run({"cmd.exe", "/c", "echo late"}, Temp("late.log")) == -1);
}

TEST_CASE("map names follow launcher.project_name")
{
    CHECK(ProjectName("C:/mods/x/nazi_zombie_ccube_u.ff") == "zm_ccube_u_waw");
    CHECK(ProjectName("C:/mods/Clinic Of Evil/mod.ff") == "zm_clinic_of_evil_waw");
    CHECK(ProjectName("C:/mods/x/ZM_Fancy-Map!!.ff") == "zm_fancy_map_waw");
    CHECK(ProjectName("C:/mods/x/___.ff") == "zm_map_waw");
    CHECK(ValidateProject("zm_ok_1").empty());
    CHECK_FALSE(ValidateProject("Bad Name").empty());
}

TEST_CASE("diagnostics carry a built map's warnings, menus and the WaW files it read")
{
    namespace fs = std::filesystem;
    const auto root = Temp("diagnostics");
    fs::remove_all(root);
    Settings settings;
    settings.work = (root / "work").string();
    settings.project = "zm_diag_test";
    settings.waw = (root / "waw").string();
    settings.waw_tools = settings.waw;
    settings.fastfile = (root / "map/nazi_zombie_diag.ff").string();
    auto write = [](const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    };
    write(root / "waw/zone/english/common.ff", std::string(3 << 20, 'z')); // sampled: first and last MiB
    write(root / "waw/raw/ui/ingame.txt", "{\n    loadMenu { \"ui/broken.menu\" }\n}\n");
    write(root / "waw/raw/ui/broken.menu", "#define F(a) a\nF(1\n");
    write(root / "map/nazi_zombie_diag.ff", "ff");
    const auto build = BuildPaths::ForSettings(settings).root;
    const auto zoneRaw = build / "stage/zone_raw/zm_diag_test";
    write(zoneRaw / "bridge_stage.report.json", R"({"errors": [], "warnings": ["FRAMEWORK_BO2 pause menu: Unterminated macro call; BO2 pause menu kept"]})");
    write(zoneRaw / "content_source/waw_menu.json", R"({"script_menus": ["tom"]})");
    write(zoneRaw / "zm/waw_menus.csv", "menu:tom,0 0 150 300 0 0\n");
    write(zoneRaw / "content_source/geometry.json", "{}"); // not about menus, HUD or fonts: left out

    const auto bundle = CreateBundle(settings, "console line\n");
    CHECK(bundle.filename().string().find("-diagnostics-") != std::string::npos);
    std::map<std::string, std::string> files;
    {
        w2b::zip::Archive archive(bundle);
        for (const auto& entry : archive.Entries())
            files[entry.name] = archive.Read(entry);
    }
    CHECK(files["warnings.txt"].find("FRAMEWORK_BO2 pause menu: Unterminated macro call") != std::string::npos);
    CHECK(files.contains("build/stage/zone_raw/zm_diag_test/content_source/waw_menu.json"));
    CHECK(files.contains("build/stage/zone_raw/zm_diag_test/zm/waw_menus.csv"));
    CHECK(!files.contains("build/stage/zone_raw/zm_diag_test/content_source/geometry.json"));
    CHECK(files["raw_ui/broken.menu"] == "#define F(a) a\nF(1\n");
    const auto environment = files["environment.json"];
    CHECK(environment.find("\"english/common.ff\"") != std::string::npos);
    CHECK(environment.find("\"sampled\": true") != std::string::npos);
    CHECK(environment.find("\"nazi_zombie_diag.ff\"") != std::string::npos);
    CHECK(files["console.log"] == "console line\n");
    fs::remove_all(root);
}
