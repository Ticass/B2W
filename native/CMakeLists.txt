cmake_minimum_required(VERSION 3.24)
project(waw2bo2_native LANGUAGES C CXX)

# The converter's steps in C++ (docs/CPP_PORT_PLAN.md). Third-party headers
# come from the OAT T6 checkout's thirdparty folder (tools/restore_native.ps1).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OAT_THIRDPARTY "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/OpenAssetToolsT6/thirdparty" CACHE PATH "OAT thirdparty folder")

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc /MP)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# zlib's inflate (IWD archives are zip files)
set(ZLIB_DIR "${OAT_THIRDPARTY}/zlib")
add_library(w2b_zlib STATIC
    ${ZLIB_DIR}/adler32.c ${ZLIB_DIR}/crc32.c ${ZLIB_DIR}/inflate.c ${ZLIB_DIR}/inftrees.c
    ${ZLIB_DIR}/inffast.c ${ZLIB_DIR}/zutil.c)
set_target_properties(w2b_zlib PROPERTIES LINKER_LANGUAGE C)
target_include_directories(w2b_zlib PUBLIC "${ZLIB_DIR}")
if(MSVC)
    target_compile_options(w2b_zlib PRIVATE /W0)
endif()

add_library(w2b_core STATIC
    src/core/pytext.cpp
    src/core/files.cpp
    src/core/hash.cpp
    src/core/cache.cpp
    src/core/process.cpp
    src/core/encoding.cpp
    src/core/unicode.cpp
    src/core/string_repr.cpp
    src/core/pyre.cpp
    src/core/zip.cpp)
target_include_directories(w2b_core PUBLIC src "${OAT_THIRDPARTY}/json/include")
target_link_libraries(w2b_core PUBLIC w2b_zlib)

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

add_library(w2b_sound STATIC
    src/sound/stage.cpp)
target_link_libraries(w2b_sound PUBLIC w2b_core)

add_library(w2b_textures STATIC src/textures/dds.cpp src/textures/iwi.cpp src/textures/stage.cpp
    src/textures/lightmap.cpp src/textures/lightmap_stage.cpp src/textures/iwi6.cpp
    src/textures/wavelet.cpp src/textures/wavelet_tables.cpp src/textures/iwd_recovery.cpp src/textures/iwd_service.cpp)
target_link_libraries(w2b_textures PUBLIC w2b_core)

add_library(w2b_materials STATIC src/materials/techset_name.cpp src/materials/techset_match.cpp
    src/materials/material.cpp src/materials/prepare.cpp src/materials/donor_index.cpp)
target_link_libraries(w2b_materials PUBLIC w2b_core)

add_library(w2b_effects STATIC src/effects/element.cpp src/effects/effect.cpp src/effects/overlay.cpp src/effects/stage.cpp)
target_link_libraries(w2b_effects PUBLIC w2b_core)

add_library(w2b_world STATIC
    src/world/vertices.cpp
    src/world/bsp_visibility.cpp
    src/world/brush_records.cpp
    src/world/collision_plan.cpp
    src/world/collision_stage.cpp
    src/world/collision_world.cpp
    src/world/grid_models.cpp
    src/world/grid_stage.cpp
    src/world/inspect.cpp
    src/world/light_grid.cpp
    src/world/mesh_merge.cpp
    src/world/one_way_collision.cpp
    src/world/primary_lights.cpp
    src/world/probe_plan.cpp
    src/world/probe_stage.cpp
    src/world/render_world.cpp
    src/world/shadow_geometry.cpp
    src/world/static_collision_records.cpp
    src/world/terrain_ownership.cpp
    src/world/world_trace.cpp
    src/world/fbx_mesh.cpp
    src/world/fbx_writer.cpp
    src/world/fbx_stage.cpp)
target_link_libraries(w2b_world PUBLIC w2b_core w2b_textures)

add_library(w2b_gsc STATIC
    src/gsc/tokens.cpp
    src/gsc/parse.cpp)
target_link_libraries(w2b_gsc PUBLIC w2b_core)

add_library(w2b_gscport STATIC
    src/gsc/port/util.cpp
    src/gsc/port/api.cpp
    src/gsc/port/rewrites.cpp
    src/gsc/port/t6api.cpp
    src/gsc/port/sources.cpp
    src/gsc/port/statements.cpp
    src/gsc/port/translator_resolve.cpp
    src/gsc/port/translator_translate.cpp
    src/gsc/port/translator_extract.cpp
    src/gsc/port/translator_hooks.cpp
    src/gsc/port/zombiebehavior.cpp
    src/gsc/port/support.cpp
    src/gsc/port/generators.cpp
    src/gsc/port/portmap.cpp
    src/gsc/port/clientport.cpp)
target_link_libraries(w2b_gscport PUBLIC w2b_gsc)

add_executable(waw2bo2n src/cli/main.cpp)
target_link_libraries(waw2bo2n PRIVATE w2b_shaders w2b_audio w2b_sound w2b_gsc w2b_gscport w2b_world w2b_textures
    w2b_materials w2b_effects)
if(MSVC)
    target_link_options(waw2bo2n PRIVATE /ENTRY:wmainCRTStartup)
endif()

enable_testing()
if(EXISTS "${OAT_THIRDPARTY}/catch2/CMakeLists.txt")
    add_subdirectory("${OAT_THIRDPARTY}/catch2" catch2 EXCLUDE_FROM_ALL)
    add_executable(w2b_tests tests/test_pytext.cpp tests/test_registers.cpp tests/test_translate.cpp
        tests/test_world_vertices.cpp tests/test_world_fbx.cpp tests/test_collision_world.cpp tests/test_textures.cpp)
    target_link_libraries(w2b_tests PRIVATE w2b_shaders w2b_world Catch2::Catch2WithMain)
    add_test(NAME w2b_tests COMMAND w2b_tests)
endif()
