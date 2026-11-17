cmake_minimum_required(VERSION 3.24)
project(waw2bo2_native LANGUAGES CXX)

# The converter's steps in C++ (docs/CPP_PORT_PLAN.md). Third-party headers
# come from the OAT T6 checkout's thirdparty folder (tools/restore_native.ps1).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OAT_THIRDPARTY "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/OpenAssetToolsT6/thirdparty" CACHE PATH "OAT thirdparty folder")

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc /MP)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

add_library(w2b_core STATIC
    src/core/pytext.cpp
    src/core/files.cpp
    src/core/hash.cpp
    src/core/cache.cpp
    src/core/process.cpp)
target_include_directories(w2b_core PUBLIC src "${OAT_THIRDPARTY}/json/include")

add_library(w2b_shaders STATIC
    src/shaders/d3d.cpp
    src/shaders/registers.cpp
    src/shaders/translate.cpp
    src/shaders/stage.cpp)
target_link_libraries(w2b_shaders PUBLIC w2b_core)

add_library(w2b_audio STATIC
    src/audio/convert.cpp
    src/audio/stage.cpp)
target_link_libraries(w2b_audio PUBLIC w2b_core)

add_executable(waw2bo2n src/cli/main.cpp)
target_link_libraries(waw2bo2n PRIVATE w2b_shaders w2b_audio)
if(MSVC)
    target_link_options(waw2bo2n PRIVATE /ENTRY:wmainCRTStartup)
endif()

enable_testing()
if(EXISTS "${OAT_THIRDPARTY}/catch2/CMakeLists.txt")
    add_subdirectory("${OAT_THIRDPARTY}/catch2" catch2 EXCLUDE_FROM_ALL)
    add_executable(w2b_tests tests/test_pytext.cpp tests/test_registers.cpp tests/test_translate.cpp)
    target_link_libraries(w2b_tests PRIVATE w2b_shaders Catch2::Catch2WithMain)
    add_test(NAME w2b_tests COMMAND w2b_tests)
endif()
