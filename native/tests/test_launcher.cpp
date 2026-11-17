// Launcher services: process streaming, cancellation, naming rules.
#include "launcher/build.h"

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
