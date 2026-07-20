cmake_minimum_required(VERSION 3.20)
project(TheDawningV3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This project currently links Windows SDK libraries directly and manages
# runtime DLLs explicitly. Avoid vcpkg's app-local post-build probe, which emits
# noisy pwsh fallback messages on systems without PowerShell Core.
set(VCPKG_APPLOCAL_DEPS OFF CACHE BOOL "Disable vcpkg app-local dependency copying for this target" FORCE)

# =============================================================================
# Source files
# =============================================================================
set(APP_SOURCES
    src/main.cpp
    src/app.cpp
)

set(APP_HEADERS
    src/app.h
)

set(CORE_SOURCES
    src/core/log.cpp
    src/core/input.cpp
    src/core/timer.cpp
    src/core/window.cpp
    src/core/shadow_cascades.cpp
)

set(CORE_HEADERS
    src/core/types.h
    src/core/log.h
    src/core/input.h
    src/core/timer.h
    src/core/window.h
    src/core/shadow_cascades.h
)

set(ASSET_SOURCES
    src/asset/cooked_model.cpp
    src/asset/gltf_importer.cpp
    src/asset/source_snapshot.cpp
)

set(ASSET_HEADERS
    src/asset/cooked_model.h
    src/asset/model_data.h
    src/asset/gltf_importer.h
    src/asset/source_snapshot.h
)

set(RENDER_SOURCES
    src/render/d3d12_device.cpp
    src/render/shader_utils.cpp
    src/render/mesh.cpp
    src/render/texture.cpp
    src/render/camera.cpp
    src/render/renderer.cpp
    src/render/gpu_draw_records.cpp
    src/render/debug_overlay.cpp
    src/render/rt_acceleration.cpp
    src/render/rt_pipeline.cpp
    src/render/path_tracer.cpp
)

set(RENDER_HEADERS
    src/render/d3d12_device.h
    src/render/deferred_release.h
    src/render/descriptor_allocator.h
    src/render/shader_utils.h
    src/render/mesh.h
    src/render/texture.h
    src/render/camera.h
    src/render/renderer.h
    src/render/gpu_draw_records.h
    src/render/debug_overlay.h
    src/render/rt_acceleration.h
    src/render/rt_pipeline.h
    src/render/path_tracer.h
)

set(SHADER_FILES
    shaders/basic_vs.hlsl
    shaders/basic_ps.hlsl
    shaders/sky_vs.hlsl
    shaders/sky_ps.hlsl
    shaders/overlay_vs.hlsl
    shaders/overlay_ps.hlsl
    shaders/bloom_vs.hlsl
    shaders/bloom_prefilter_ps.hlsl
    shaders/bloom_blur_ps.hlsl
    shaders/bloom_common.hlsli
    shaders/tonemap_vs.hlsl
    shaders/tonemap_ps.hlsl
    shaders/path_trace.hlsl
    shaders/brdf_common.hlsli
    shaders/display_common.hlsli
    shaders/sky_common.hlsli
)

set(ECS_HEADERS
    src/ecs/entity.h
    src/ecs/component_pool.h
    src/ecs/components.h
    src/ecs/registry.h
)

set(SCENE_SOURCES
    src/scene/resource_manager.cpp
    src/scene/scene.cpp
    src/scene/model_loader.cpp
)

set(SCENE_HEADERS
    src/scene/resource_handle.h
    src/scene/handle_slot_map.h
    src/scene/resource_manager.h
    src/scene/scene.h
)

set(TEST_SOURCES
    tests/test_main.cpp
    tests/test_math.cpp
    tests/test_ecs.cpp
    tests/test_deferred_release.cpp
    tests/test_descriptor_allocator.cpp
    tests/test_resource_handle.cpp
    tests/test_asset_compiler.cpp
    tests/test_gltf_importer.cpp
    tests/test_gpu_draw_records.cpp
    tests/test_shadow_cascades.cpp
)

set(TEST_HEADERS
    tests/test_framework.h
)

# =============================================================================
# Executable
# =============================================================================
add_executable(TheDawningV3 WIN32
    ${APP_SOURCES}
    ${APP_HEADERS}
    ${CORE_SOURCES}
    ${CORE_HEADERS}
    ${ASSET_SOURCES}
    ${ASSET_HEADERS}
    ${RENDER_SOURCES}
    ${RENDER_HEADERS}
    ${ECS_HEADERS}
    ${SCENE_SOURCES}
    ${SCENE_HEADERS}
    ${SHADER_FILES}
)

add_executable(TheDawningAssetInspector
    tools/asset_inspector.cpp
    ${ASSET_SOURCES}
    ${ASSET_HEADERS}
)

add_executable(TheDawningAssetCompiler
    tools/asset_compiler.cpp
    ${ASSET_SOURCES}
    ${ASSET_HEADERS}
)

target_include_directories(TheDawningAssetInspector PRIVATE src)
target_include_directories(TheDawningAssetCompiler PRIVATE src)

set_target_properties(TheDawningAssetInspector PROPERTIES
    VS_GLOBAL_VcpkgApplocalDeps false
)

set_target_properties(TheDawningAssetCompiler PROPERTIES
    VS_GLOBAL_VcpkgApplocalDeps false
)

set_target_properties(TheDawningV3 PROPERTIES
    VS_GLOBAL_VcpkgApplocalDeps false
)

target_include_directories(TheDawningV3 PRIVATE src)

# =============================================================================
# Libraries
# =============================================================================
target_link_libraries(TheDawningV3 PRIVATE
    d3d12
    dxgi
    d3dcompiler
    dxguid
    gdi32
    windowscodecs
)

# =============================================================================
# Copy shaders to output directory (so the exe can find them at runtime)
# =============================================================================
add_custom_command(TARGET TheDawningV3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TheDawningV3>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:TheDawningV3>/shaders"
    COMMENT "Copying shaders to output directory"
)

add_custom_command(TARGET TheDawningV3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TheDawningV3>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:TheDawningV3>/assets"
    COMMENT "Copying assets to output directory"
)

# =============================================================================
# Source groups for Visual Studio
# =============================================================================
source_group("Core"    FILES ${CORE_SOURCES} ${CORE_HEADERS})
source_group("Asset"   FILES ${ASSET_SOURCES} ${ASSET_HEADERS})
source_group("Render"  FILES ${RENDER_SOURCES} ${RENDER_HEADERS})
source_group("ECS"     FILES ${ECS_HEADERS})
source_group("Scene"   FILES ${SCENE_SOURCES} ${SCENE_HEADERS})
source_group("Shaders" FILES ${SHADER_FILES})
source_group("Application" FILES ${APP_SOURCES} ${APP_HEADERS})

# Mark HLSL files as not compiled by MSVC (we compile them ourselves at runtime)
set_source_files_properties(${SHADER_FILES} PROPERTIES
    VS_TOOL_OVERRIDE "None"
    HEADER_FILE_ONLY TRUE
)

# =============================================================================
# Compiler settings
# =============================================================================
if(MSVC)
    target_compile_options(TheDawningV3 PRIVATE
        /W4           # Warning level 4
        /WX-          # Don't treat warnings as errors (yet)
        /MP           # Multi-processor compilation
        /Zi           # Debug info
        /permissive-  # Strict conformance
    )

    target_compile_options(TheDawningAssetInspector PRIVATE
        /W4
        /WX
        /permissive-
    )

    target_compile_options(TheDawningAssetCompiler PRIVATE
        /W4
        /WX
        /permissive-
    )
endif()

# =============================================================================
# Unit tests — TheDawningTests
# =============================================================================
# CPU-only by construction. This target links NO graphics libraries and creates
# no device, so it runs on any machine (including a GPU-less CI runner). It
# covers only GPU-independent code: src/core/types.h, src/ecs/*.h, the CPU-side
# asset importer, src/core/shadow_cascades.cpp, and src/render/gpu_draw_records.cpp.
#
# Do NOT add src/render/* or src/scene/* here — scene/ holds a render::PathTracer
# by value and would drag D3D12 into the test binary.
#
# src/core/shadow_cascades.cpp is a SANCTIONED exception to "tests link no engine
# code": it is deliberately GPU-free (it includes only core/types.h and <cstdint>)
# precisely so tests/test_shadow_cascades.cpp exercises the SHIPPED cascade math
# rather than a copy of it. tests/test_math.cpp already shows what the copy
# approach costs — two cases there read as shadow-matrix tests but rebuild
# LookAt*OrthoLH inline, so they would keep passing if the light-matrix code were
# deleted outright. Compile the real thing, or the test proves nothing.
#
#   cmake --build build --config Debug
#   ctest --test-dir build -C Debug --output-on-failure
# =============================================================================
enable_testing()

add_executable(TheDawningTests
    ${TEST_SOURCES}
    ${TEST_HEADERS}
    src/core/types.h
    src/core/shadow_cascades.h
    src/core/shadow_cascades.cpp
    ${ECS_HEADERS}
    src/scene/resource_handle.h
    src/scene/handle_slot_map.h
    # GPU-facing per-draw record layouts and their sizing arithmetic. Safe here
    # despite living under src/render: gpu_draw_records.h deliberately includes
    # no D3D12 header, precisely so these layouts can be tested on a machine
    # with no device. Do NOT add renderer.cpp or anything else from src/render.
    src/render/gpu_draw_records.cpp
    src/render/gpu_draw_records.h
    ${ASSET_SOURCES}
    ${ASSET_HEADERS}
)

set_target_properties(TheDawningTests PROPERTIES
    VS_GLOBAL_VcpkgApplocalDeps false
)

target_include_directories(TheDawningTests PRIVATE src tests)

# Intentionally no target_link_libraries(): see the note above.

if(MSVC)
    target_compile_options(TheDawningTests PRIVATE
        /W4
        /WX-
        /MP
        /Zi
        /permissive-
        /utf-8        # Test sources are UTF-8; avoids C4819 on non-ASCII comments
    )
endif()

source_group("Tests" FILES ${TEST_SOURCES} ${TEST_HEADERS})

add_test(NAME unit_tests COMMAND TheDawningTests)
