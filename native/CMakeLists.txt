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
    ${ZLIB_DIR}/inffast.c ${ZLIB_DIR}/zutil.c ${ZLIB_DIR}/deflate.c ${ZLIB_DIR}/trees.c)
set_target_properties(w2b_zlib PROPERTIES LINKER_LANGUAGE C)
target_include_directories(w2b_zlib PUBLIC "${ZLIB_DIR}")
if(MSVC)
    target_compile_options(w2b_zlib PRIVATE /W0)
endif()

add_library(w2b_core STATIC
    src/core/text.cpp
    src/core/key_values.cpp
    src/core/asset_paths.cpp
    src/core/files.cpp
    src/core/share_files.cpp
    src/core/hash.cpp
    src/core/cache.cpp
    src/core/process.cpp
    src/core/profile.cpp
    src/core/encoding.cpp
    src/core/unicode.cpp
    src/core/string_repr.cpp
    src/core/regex.cpp
    src/core/zip.cpp
    src/core/zipwrite.cpp
    src/core/timeline.cpp)
target_include_directories(w2b_core PUBLIC src "${OAT_THIRDPARTY}/json/include")
target_link_libraries(w2b_core PUBLIC w2b_zlib)

# OpenAssetTools linked into the converter (vendor/OpenAssetTools, built as
# Release x64 by tools/restore_native.ps1): zones load and dump in this process
# instead of in Unlinker.exe. Only src/oat compiles against OAT's C++23 headers.
set(OAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/OpenAssetTools" CACHE PATH "OpenAssetTools checkout (WaW zones)")
set(OAT_LIB_DIR "${OAT_DIR}/build/lib/Release_x64")
set(OAT_LIBS Unlinking ZoneLoading ObjWriting ObjLoading ObjImage ZoneCommon Cryptography ObjCommon Common Parser XMemCompress Utils
    minilzo salsa20 lzx minizip zlib libtomcrypt libtommath lz4)
add_library(w2b_oat STATIC src/oat/run_unlinker.cpp src/oat/loaded_zones.cpp src/oat/zone_dumper.cpp src/oat/waw_assets.cpp src/oat/waw_menus.cpp src/oat/waw_tables.cpp)
set_target_properties(w2b_oat PROPERTIES CXX_STANDARD 23)
target_include_directories(w2b_oat PUBLIC src)
target_include_directories(w2b_oat SYSTEM PRIVATE
    "${OAT_DIR}/build/premake" "${OAT_DIR}/src/Unlinking" "${OAT_DIR}/src/ZoneCommon" "${OAT_DIR}/src/Utils" "${OAT_DIR}/src/Common"
    "${OAT_DIR}/src/ObjCommon" "${OAT_DIR}/build/src/ObjCommon" "${OAT_DIR}/src/ObjWriting" "${OAT_DIR}/build/src/ObjWriting" "${OAT_DIR}/src/ObjLoading" "${OAT_DIR}/src/ObjImage"
    "${OAT_DIR}/src/ZoneLoading" "${OAT_DIR}/src/ZoneCode" "${OAT_DIR}/build/src/ZoneCode" "${OAT_DIR}/src/Parser" "${OAT_DIR}/src/Cryptography"
    "${OAT_DIR}/src/XMemCompress" "${OAT_DIR}/thirdparty/json/single_include" "${OAT_DIR}/thirdparty/zlib")
target_compile_definitions(w2b_oat PRIVATE ARCH_x64 __STDC_LIB_EXT1__ __STDC_WANT_LIB_EXT1__=1 JSON_DIAGNOSTICS=1 HAS_NLOHMANN_JSON ZLIB_CONST)
# the linked code's identity: the OAT commit and the converter's patch to it
execute_process(COMMAND git -C "${OAT_DIR}" rev-parse --short=12 HEAD OUTPUT_VARIABLE OAT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set(OAT_PATCH "${OAT_DIR}/../OpenAssetTools.patch")
file(SHA256 "${OAT_PATCH}" OAT_PATCH_HASH)
string(SUBSTRING "${OAT_PATCH_HASH}" 0 12 OAT_PATCH_HASH)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${OAT_PATCH}")
target_compile_definitions(w2b_oat PRIVATE W2B_OAT_VERSION="oat-${OAT_COMMIT}+patch-${OAT_PATCH_HASH}")
foreach(lib IN LISTS OAT_LIBS)
    target_link_libraries(w2b_oat PRIVATE "${OAT_LIB_DIR}/${lib}.lib")
endforeach()

add_executable(w2b_lint src/cli/lint_main.cpp)
target_link_libraries(w2b_lint PRIVATE w2b_core)
target_compile_definitions(w2b_lint PRIVATE W2B_NATIVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_library(w2b_shaders STATIC
    src/shaders/d3d.cpp
    src/shaders/techsets.cpp
    src/shaders/registers.cpp
    src/shaders/translate.cpp
    src/shaders/stage.cpp)
target_link_libraries(w2b_shaders PUBLIC w2b_core)

add_library(w2b_shaderruntime STATIC src/shaders/output.cpp src/shaders/runtime.cpp src/shaders/runtime_analysis.cpp src/shaders/runtime_contracts.cpp
    src/shaders/native_probes.cpp)
target_link_libraries(w2b_shaderruntime PUBLIC w2b_shaders w2b_materials w2b_gsc w2b_core)

add_library(w2b_audio STATIC
    src/audio/convert.cpp
    src/audio/ffmpeg.cpp
    src/audio/flac_batch.cpp
    src/audio/stage.cpp)
target_link_libraries(w2b_audio PUBLIC w2b_core)

add_library(w2b_sound STATIC
    src/sound/stage.cpp src/sound/names.cpp src/sound/pcm_binding.cpp
    src/sound/curve_driver.cpp src/sound/curve_match.cpp src/sound/curve_binding.cpp src/sound/bank.cpp
    src/sound/bank_csv.cpp src/sound/streamed_bank.cpp)
target_link_libraries(w2b_sound PUBLIC w2b_core PRIVATE w2b_audio)

add_library(w2b_textures STATIC src/textures/dds.cpp src/textures/pillow_decode.cpp src/textures/iwi.cpp src/textures/stage.cpp src/textures/block_compress.cpp
    src/textures/lightmap.cpp src/textures/lightmap_stage.cpp src/textures/iwi6.cpp
    src/textures/wavelet.cpp src/textures/wavelet_tables.cpp src/textures/iwd_recovery.cpp)
target_link_libraries(w2b_textures PUBLIC w2b_core)

add_library(w2b_materials STATIC src/materials/techset_name.cpp src/materials/techset_match.cpp src/materials/fxmaterials.cpp
    src/materials/material.cpp src/materials/prepare.cpp src/materials/donor_index.cpp src/materials/waw_material.cpp src/materials/t6_material.cpp)
target_link_libraries(w2b_materials PUBLIC w2b_core)

add_library(w2b_effects STATIC src/effects/element.cpp src/effects/effect.cpp src/effects/overlay.cpp src/effects/stage.cpp)
target_link_libraries(w2b_effects PUBLIC w2b_core)

set(TOMCRYPT_DIR "${OAT_THIRDPARTY}/libtomcrypt/src")
add_library(w2b_base64 STATIC
    "${TOMCRYPT_DIR}/misc/base64/base64_decode.c" "${TOMCRYPT_DIR}/misc/base64/base64_encode.c")
target_include_directories(w2b_base64 SYSTEM PUBLIC "${TOMCRYPT_DIR}/headers")
target_compile_definitions(w2b_base64 PRIVATE ARGTYPE=4)

add_library(w2b_weapons STATIC src/weapons/infostring.cpp src/weapons/convert.cpp src/weapons/defaults.cpp
    src/weapons/sources.cpp src/weapons/plan.cpp src/weapons/flame_table.cpp src/weapons/stage.cpp
    src/weapons/models.cpp src/weapons/skeleton.cpp src/weapons/rebind.cpp src/weapons/viewmodels.cpp
    src/weapons/physics_preset.cpp src/weapons/water_bullets.cpp)
target_link_libraries(w2b_weapons PUBLIC w2b_core PRIVATE w2b_base64)

add_library(w2b_models STATIC src/models/bounds.cpp src/models/geometry.cpp src/models/stage.cpp src/models/effect_assets.cpp
    src/models/vertex_colours.cpp src/models/surface_sort.cpp src/models/surface_stage.cpp
    src/models/identity_scripts.cpp src/models/identities.cpp src/models/material_identity.cpp
    src/models/projectile_collision.cpp)
target_link_libraries(w2b_models PUBLIC w2b_weapons w2b_world w2b_materials PRIVATE w2b_base64 w2b_gscport)

add_library(w2b_assets STATIC src/assets/paths.cpp src/assets/dependencies.cpp src/assets/resolver.cpp)
target_link_libraries(w2b_assets PUBLIC w2b_weapons w2b_effects)

add_library(w2b_physics STATIC src/physics/dynents.cpp src/physics/constraints.cpp src/physics/ropes.cpp
    src/physics/suspensions.cpp src/physics/stage.cpp)
target_link_libraries(w2b_physics PUBLIC w2b_weapons w2b_mapentity)

add_library(w2b_mapentity STATIC src/mapents/map_entity.cpp)
target_link_libraries(w2b_mapentity PUBLIC w2b_core)
add_library(w2b_mapents STATIC src/mapents/parse.cpp src/mapents/convert.cpp src/mapents/wall_buys.cpp src/mapents/stage.cpp
    src/mapents/write.cpp src/mapents/zones.cpp)
target_link_libraries(w2b_mapents PUBLIC w2b_world w2b_mapentity PRIVATE w2b_gscport)

add_library(w2b_paths STATIC src/paths/aliases.cpp src/paths/convert.cpp src/paths/dogs.cpp src/paths/stage.cpp
    src/paths/window_ends.cpp src/paths/window_traversals.cpp)
target_link_libraries(w2b_paths PUBLIC w2b_gsc w2b_mapents)

add_library(w2b_barricades STATIC src/barriers/xanim.cpp src/barriers/floor.cpp src/barriers/boards.cpp
    src/barriers/repair_timing.cpp src/barriers/repair.cpp src/barriers/stage.cpp)
target_link_libraries(w2b_barricades PUBLIC w2b_mapents w2b_physics)

add_library(w2b_world STATIC
    src/world/zone_manifest.cpp
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
    src/gsc/ast.cpp
    src/gsc/parse.cpp)
target_link_libraries(w2b_gsc PUBLIC w2b_core)

add_library(w2b_gscport STATIC
    src/gsc/port/staging.cpp
    src/gsc/port/zone_graph.cpp
    src/gsc/port/zone_scripts.cpp
    src/gsc/port/util.cpp
    src/gsc/port/api.cpp
    src/gsc/port/rewrites.cpp
    src/gsc/port/vm_rules.cpp
    src/gsc/port/types.cpp
    src/gsc/port/link_check.cpp
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
target_link_libraries(w2b_gscport PUBLIC w2b_gsc w2b_mapentity)

# the converter version (VERSION at the repository root)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" W2B_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
# which build this is (Test build #212, Nightly 2026-11-27, Stable 0.2.16): set by CI, empty for a local build
set(W2B_BUILD_LABEL "$ENV{W2B_BUILD_LABEL}")
add_library(w2b_launcher STATIC src/launcher/settings.cpp src/launcher/images.cpp src/launcher/build.cpp src/launcher/diagnostics.cpp)
target_link_libraries(w2b_launcher PUBLIC w2b_core)
target_compile_definitions(w2b_launcher PRIVATE W2B_VERSION="${W2B_VERSION}" W2B_BUILD_LABEL="${W2B_BUILD_LABEL}")

# the desktop launcher: Dear ImGui on Direct3D 11 (vendor/imgui, MIT)
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/imgui")
add_library(w2b_imgui STATIC ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(w2b_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(w2b_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
add_executable(B2W WIN32 src/app.manifest src/ui/main.cpp src/ui/app.cpp src/ui/views.cpp src/ui/theme.cpp src/ui/gpu.cpp src/ui/shell.cpp src/ui/previews.cpp)
target_link_libraries(B2W PRIVATE w2b_launcher w2b_imgui d3d11 dxgi d3dcompiler dwmapi)

add_library(w2b_extract STATIC src/extract/cache.cpp src/extract/stock.cpp src/extract/stockwaw.cpp src/extract/wawsource.cpp src/extract/unlinker.cpp
    src/extract/material_sources.cpp)
target_link_libraries(w2b_extract PUBLIC w2b_core w2b_materials w2b_shaders PRIVATE w2b_oat)
add_library(w2b_strings STATIC src/strings/localization.cpp)
target_link_libraries(w2b_strings PUBLIC w2b_core w2b_gsc w2b_weapons w2b_extract)

add_library(w2b_visions STATIC src/visions/visions.cpp)
target_link_libraries(w2b_visions PUBLIC w2b_core w2b_gsc w2b_extract w2b_shaders w2b_materials)

add_library(w2b_pipeline STATIC src/pipeline/finish.cpp src/pipeline/audit.cpp src/pipeline/knowledge.cpp src/pipeline/driver.cpp
    src/pipeline/modsupport.cpp src/pipeline/hud.cpp src/pipeline/csvtable.cpp src/pipeline/modzone.cpp
    src/pipeline/lobby.cpp src/pipeline/shaderbind.cpp src/pipeline/buildmod.cpp
    src/pipeline/menu_song.cpp src/pipeline/menu_stage.cpp src/pipeline/menu_runtime.cpp
    src/pipeline/menu_artwork.cpp src/pipeline/menu_pixels.cpp src/pipeline/menu_media.cpp
    src/pipeline/rumbles.cpp
    src/pipeline/materials_stage.cpp src/pipeline/effects_stage.cpp
    src/pipeline/scripts_stage.cpp src/pipeline/hooks_stage.cpp src/pipeline/sound_mix.cpp src/pipeline/amb_stage.cpp src/pipeline/sound_refs.cpp src/pipeline/coverage_stage.cpp src/pipeline/hud_stage.cpp src/pipeline/hud_menus.cpp src/pipeline/source_zones.cpp src/pipeline/impact_table.cpp src/pipeline/unread_assets.cpp src/pipeline/destructibles.cpp src/pipeline/ingame_menus.cpp src/pipeline/script_menus.cpp src/pipeline/weapon_runtime.cpp src/pipeline/weapon_visuals.cpp src/pipeline/skybox_stage.cpp src/pipeline/client_stage.cpp src/pipeline/lighting_tail.cpp src/pipeline/bridge_tail.cpp
    src/pipeline/actor_trees.cpp src/pipeline/bridge_sound.cpp src/pipeline/bridge_geometry.cpp src/pipeline/geometry_materials.cpp
    src/pipeline/bridge_navigation.cpp src/pipeline/bridge_inventory.cpp src/pipeline/bridge_images.cpp
    src/pipeline/bridge_assets.cpp src/pipeline/bridge_stage.cpp src/pipeline/bridge_report.cpp src/pipeline/material_record.cpp)
target_link_libraries(w2b_pipeline PUBLIC w2b_gscport w2b_gsc w2b_extract w2b_launcher w2b_audio w2b_weapons w2b_textures w2b_shaders w2b_shaderruntime w2b_materials w2b_effects w2b_assets w2b_barricades w2b_paths PRIVATE w2b_oat)
target_compile_definitions(w2b_pipeline PRIVATE W2B_VERSION="${W2B_VERSION}" W2B_BUILD_LABEL="${W2B_BUILD_LABEL}")

add_executable(waw2bo2n src/cli/main.cpp src/cli/dev_tools.cpp src/cli/gsc_tools.cpp src/app.manifest)
target_link_libraries(waw2bo2n PRIVATE w2b_oat w2b_extract w2b_strings w2b_visions w2b_pipeline w2b_launcher w2b_shaders w2b_audio w2b_sound w2b_gsc w2b_gscport w2b_world w2b_textures
    w2b_materials w2b_effects w2b_weapons w2b_assets w2b_physics w2b_mapents w2b_paths w2b_barricades w2b_models)
if(MSVC)
    target_link_options(waw2bo2n PRIVATE /ENTRY:wmainCRTStartup)
endif()

enable_testing()
if(EXISTS "${OAT_THIRDPARTY}/catch2/CMakeLists.txt")
    add_subdirectory("${OAT_THIRDPARTY}/catch2" catch2 EXCLUDE_FROM_ALL)
    add_executable(w2b_tests tests/test_text.cpp tests/test_registers.cpp tests/test_translate.cpp
        tests/test_world_vertices.cpp tests/test_world_fbx.cpp tests/test_collision_world.cpp tests/test_textures.cpp tests/test_launcher.cpp tests/test_extract.cpp tests/test_pipeline.cpp tests/test_t6_material.cpp tests/test_gsc.cpp)
    target_link_libraries(w2b_tests PRIVATE w2b_shaders w2b_world w2b_launcher w2b_extract w2b_pipeline w2b_models w2b_visions Catch2::Catch2WithMain)
    add_executable(fake_unlinker tests/fake_unlinker.cpp)
    add_dependencies(w2b_tests fake_unlinker)
    target_compile_definitions(w2b_tests PRIVATE FAKE_UNLINKER="$<TARGET_FILE:fake_unlinker>")
    add_test(NAME w2b_tests COMMAND w2b_tests)
endif()
