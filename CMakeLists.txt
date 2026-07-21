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
    src/core/sky_radiance.cpp
    src/core/sh_irradiance.cpp
)

set(CORE_HEADERS
    src/core/types.h
    src/core/log.h
    src/core/input.h
    src/core/timer.h
    src/core/window.h
    src/core/shadow_cascades.h
    src/core/sky_radiance.h
    src/core/sh_irradiance.h
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
    src/render/environment_ibl.cpp
    src/render/gpu_draw_records.cpp
    src/render/ibl_consume_probe.cpp
    src/render/debug_overlay.cpp
    src/render/rt_acceleration.cpp
    src/render/rt_pipeline.cpp
    src/render/rt_texture_lod.cpp
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
    src/render/ibl_consume_probe.h
    src/render/debug_overlay.h
    src/render/rt_acceleration.h
    src/render/rt_pipeline.h
    src/render/rt_texture_lod.h
    src/render/path_tracer.h
    src/render/environment_ibl.h
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
    shaders/gpu_draw_records.hlsli
    shaders/brdf_common.hlsli
    shaders/display_common.hlsli
    shaders/sky_common.hlsli
    shaders/ibl_environment.hlsli
    shaders/ibl_prefilter_ps.hlsl
    shaders/ibl_probe_ps.hlsl
    shaders/ibl_common.hlsli
    shaders/ibl_eval_probe_ps.hlsl
    shaders/ibl_consume_probe.hlsli
)

set(ECS_HEADERS
    src/ecs/entity.h
    src/ecs/component_pool.h
    src/ecs/components.h
    src/ecs/registry.h
)

# Flight & physics (Phase 4). GPU-free by construction: sim/*.h include only
# core/types.h and ecs/components.h (both device-free), so every file here also
# links into TheDawningTests below and the analytic tests drive the SHIPPED
# integrator. Do NOT add anything that includes a D3D12 or render header here.
set(SIM_SOURCES
    src/sim/rigid_body.cpp
    src/sim/thrusters.cpp
    src/sim/flight_control.cpp
    src/sim/physics_system.cpp
    src/sim/reference_frame.cpp
    src/sim/kepler.cpp
    src/sim/nbody.cpp
    src/sim/relativity.cpp
    src/sim/relativity_system.cpp
    src/sim/atmosphere.cpp
    src/sim/atmosphere_system.cpp
    src/sim/ftl.cpp
    src/sim/ftl_system.cpp
    src/sim/collision.cpp
    src/sim/sim_serialize.cpp
    src/sim/orbit_system.cpp
    src/sim/simulation_system.cpp
    src/sim/snapshot_system.cpp
)

set(SIM_HEADERS
    src/sim/rigid_body.h
    src/sim/thrusters.h
    src/sim/flight_control.h
    src/sim/physics_system.h
    src/sim/reference_frame.h
    src/sim/kepler.h
    src/sim/nbody.h
    src/sim/relativity.h
    src/sim/relativity_system.h
    src/sim/atmosphere.h
    src/sim/atmosphere_system.h
    src/sim/ftl.h
    src/sim/ftl_system.h
    src/sim/collision.h
    src/sim/sim_serialize.h
    src/sim/orbit_system.h
    src/sim/simulation_system.h
    src/sim/snapshot_system.h
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
    tests/test_shadow_cascades.cpp
    tests/test_gpu_draw_records.cpp
    tests/test_rt_texture_lod.cpp
    tests/test_sky_energy.cpp
    tests/test_sh_irradiance.cpp
    tests/test_ibl_consume_probe.cpp
    tests/test_rigid_body.cpp
    tests/test_flight_control.cpp
    tests/test_physics_system.cpp
    tests/test_reference_frame.cpp
    tests/test_kepler.cpp
    tests/test_nbody.cpp
    tests/test_relativity.cpp
    tests/test_relativity_system.cpp
    tests/test_atmosphere.cpp
    tests/test_atmosphere_system.cpp
    tests/test_ftl.cpp
    tests/test_ftl_system.cpp
    tests/test_collision.cpp
    tests/test_sim_serialize.cpp
    tests/test_orbit_system.cpp
    tests/test_simulation_system.cpp
    tests/test_snapshot_system.cpp
    tests/test_timer.cpp
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
    ${SIM_SOURCES}
    ${SIM_HEADERS}
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

# DXC is loaded dynamically by shader_utils.cpp for Shader Model 6 / DXR. Make
# each configuration self-contained instead of relying on DLLs manually copied
# into one build output. DAWNING_DXC_RUNTIME_DIR supports standalone DXC/NuGet
# layouts; otherwise prefer the newest x64 Windows SDK runtime pair.
set(DAWNING_DXC_RUNTIME_DIR "" CACHE PATH
    "Directory containing dxcompiler.dll and dxil.dll")

if(WIN32)
    set(_dxc_runtime_dir "${DAWNING_DXC_RUNTIME_DIR}")
    if(NOT _dxc_runtime_dir)
        file(GLOB _windows_sdk_x64_bins LIST_DIRECTORIES true
            "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64")
        list(SORT _windows_sdk_x64_bins COMPARE NATURAL ORDER DESCENDING)
        foreach(_candidate IN LISTS _windows_sdk_x64_bins)
            if(EXISTS "${_candidate}/dxcompiler.dll" AND
               EXISTS "${_candidate}/dxil.dll")
                set(_dxc_runtime_dir "${_candidate}")
                break()
            endif()
        endforeach()
    endif()

    if(EXISTS "${_dxc_runtime_dir}/dxcompiler.dll" AND
       EXISTS "${_dxc_runtime_dir}/dxil.dll")
        add_custom_command(TARGET TheDawningV3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dxc_runtime_dir}/dxcompiler.dll"
                "$<TARGET_FILE_DIR:TheDawningV3>/dxcompiler.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dxc_runtime_dir}/dxil.dll"
                "$<TARGET_FILE_DIR:TheDawningV3>/dxil.dll"
            COMMENT "Copying DXC runtime to output directory"
        )
        message(STATUS "DXC runtime: ${_dxc_runtime_dir}")
    else()
        message(WARNING
            "DXC runtime not found. Set DAWNING_DXC_RUNTIME_DIR to a directory "
            "containing dxcompiler.dll and dxil.dll; DXR will be unavailable.")
    endif()
endif()

# =============================================================================
# Source groups for Visual Studio
# =============================================================================
source_group("Core"    FILES ${CORE_SOURCES} ${CORE_HEADERS})
source_group("Asset"   FILES ${ASSET_SOURCES} ${ASSET_HEADERS})
source_group("Render"  FILES ${RENDER_SOURCES} ${RENDER_HEADERS})
source_group("ECS"     FILES ${ECS_HEADERS})
source_group("Sim"     FILES ${SIM_SOURCES} ${SIM_HEADERS})
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
# asset importer, and src/core/shadow_cascades.cpp.
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

find_package(Python3 COMPONENTS Interpreter)

add_executable(TheDawningTests
    ${TEST_SOURCES}
    ${TEST_HEADERS}
    src/core/types.h
    src/core/timer.h
    src/core/timer.cpp
    src/core/shadow_cascades.h
    src/core/shadow_cascades.cpp
    # CPU evaluation of the procedural sky. GPU-free for the same reason
    # shadow_cascades.cpp is, but NOT on the same footing: it is a hand-written
    # MIRROR of shaders/sky_common.hlsli rather than shipped code, and the engine
    # does not call it. Read the header before relying on any test over it -
    # tests/test_sky_energy.cpp carries a tripwire that keeps the two in step,
    # and states precisely what that tripwire does not prove.
    src/core/sky_radiance.h
    src/core/sky_radiance.cpp
    # L2 spherical-harmonic diffuse irradiance. GPU-free on the same sanctioned
    # footing as shadow_cascades.cpp: it includes only core/types.h and <cmath>,
    # so tests/test_sh_irradiance.cpp exercises the SHIPPED projector rather than
    # a copy of it. Its BASIS has an HLSL twin, which the header names and which
    # a GPU probe watches - no CPU test here can see that half.
    src/core/sh_irradiance.h
    src/core/sh_irradiance.cpp
    ${ECS_HEADERS}
    src/scene/resource_handle.h
    src/scene/handle_slot_map.h
    # GPU-facing per-draw record layouts and their sizing arithmetic. Safe here
    # despite living under src/render: gpu_draw_records.h deliberately includes
    # no D3D12 header, precisely so these layouts can be tested on a machine
    # with no device. Do NOT add renderer.cpp or anything else from src/render.
    src/render/gpu_draw_records.cpp
    src/render/gpu_draw_records.h
    # Ray-cone texture LOD arithmetic. Also safe here for the same reason:
    # rt_texture_lod.h includes only <cstdint>/<cmath> and no D3D12 header, so
    # the LOD formulae compile on a machine with no device. Do NOT add
    # path_tracer.cpp or rt_pipeline.cpp - both pull in d3d12.h.
    src/render/rt_texture_lod.cpp
    src/render/rt_texture_lod.h
    # The IBL consumption probe's layout and verdict logic. Safe here for the
    # third time and the same reason: ibl_consume_probe.h includes only <cstdint>
    # and no D3D12 header, so tests/test_ibl_consume_probe.cpp drives the SHIPPED
    # reduction rather than a copy of it. The GPU supplies the 64-byte block;
    # every threshold and every verdict below it is decided here.
    src/render/ibl_consume_probe.cpp
    src/render/ibl_consume_probe.h
    # Flight & physics integrator. GPU-free on the same sanctioned footing as
    # shadow_cascades.cpp: sim/*.h include only core/types.h, ecs/components.h and
    # ecs/registry.h (all device-free), so the flight tests drive the SHIPPED
    # semi-implicit-Euler integrator, thruster wrench math, control allocation,
    # flight-assist law and the ECS physics system — not a copy. physics_system.cpp
    # includes only ecs/registry.h (STL-only, already compiled here by test_ecs.cpp)
    # plus the other sim headers, so it stays test-eligible. Do NOT add anything
    # here that pulls in a D3D12 or render header.
    src/sim/rigid_body.cpp
    src/sim/rigid_body.h
    src/sim/thrusters.cpp
    src/sim/thrusters.h
    src/sim/flight_control.cpp
    src/sim/flight_control.h
    src/sim/physics_system.cpp
    src/sim/physics_system.h
    # SIM STAGE 0 coordinate architecture. GPU-free on the same sanctioned footing
    # as shadow_cascades.cpp: reference_frame.h includes only core/types.h and
    # <cstdint>/<vector>, so tests/test_reference_frame.cpp drives the SHIPPED
    # sector-split world position and frame graph rather than a copy. Do NOT add
    # anything here that pulls in a D3D12 or render header.
    src/sim/reference_frame.cpp
    src/sim/reference_frame.h
    # SIM STAGE 1 N-body orbital core. GPU-free on the same sanctioned footing:
    # kepler.h/nbody.h include only core/types.h, ecs/components.h and the other
    # sim headers (all device-free), so tests/test_kepler.cpp and test_nbody.cpp
    # drive the SHIPPED Markley solver, universal-variable propagator, Forest-Ruth
    # symplectic integrator, shared softening and LOD transition rather than a copy.
    # Do NOT add anything here that pulls in a D3D12 or render header.
    src/sim/kepler.cpp
    src/sim/kepler.h
    src/sim/nbody.cpp
    src/sim/nbody.h
    # SIM STAGE 2 relativistic dynamics + time dilation. GPU-free on the same
    # sanctioned footing: relativity.h includes only core/types.h, ecs/components.h
    # and the other sim headers (all device-free) and REUSES nbody.h's kSpeedOfLight
    # and SofteningLength, so tests/test_relativity.cpp drives the SHIPPED
    # momentum-space stepper, dilation clocks and master-frame β composition rather
    # than a copy. Do NOT add anything here that pulls in a D3D12 or render header.
    src/sim/relativity.cpp
    src/sim/relativity.h
    src/sim/relativity_system.cpp
    src/sim/relativity_system.h
    # SIM STAGE 3 atmospheric flight. GPU-free on the same sanctioned footing:
    # atmosphere.h includes only core/types.h, so tests/test_atmosphere.cpp drives
    # the SHIPPED USSA76 model, the semi-implicit stiff-drag update and the firewalled
    # Sutton-Graves heating rather than a copy. Do NOT add anything here that pulls
    # in a D3D12 or render header.
    src/sim/atmosphere.cpp
    src/sim/atmosphere.h
    # Frame-aware ECS adapter for atmospheric drag/lift. GPU-free and linked
    # here so its transactional registry updates exercise the shipped code.
    src/sim/atmosphere_system.cpp
    src/sim/atmosphere_system.h
    # SIM STAGE 4 FTL / warp / wormhole. GPU-free on the same sanctioned footing:
    # ftl.h includes only reference_frame.h + core/types.h, so tests/test_ftl.cpp
    # drives the SHIPPED atomic teleport (retained-state set), the double velocity
    # rotation and the warp moving-origin rather than a copy. Do NOT add anything
    # here that pulls in a D3D12 or render header.
    src/sim/ftl.cpp
    src/sim/ftl.h
    # ECS/reference-frame adapter for the retained-state teleport above. It
    # includes only GPU-free ECS and sim headers, so these tests exercise the
    # shipped transactional registry commit and frame conversion directly.
    src/sim/ftl_system.cpp
    src/sim/ftl_system.h
    # SIM STAGE 5 collision / close-encounter policy. GPU-free: collision.h includes
    # only nbody.h, so tests/test_collision.cpp drives the SHIPPED subdivision,
    # swept-contact detection, mass-fraction bounce and union-find merge.
    src/sim/collision.cpp
    src/sim/collision.h
    # SIM STAGE 6 save/load codec. GPU-free: sim_serialize.h includes only
    # core/types.h + reference_frame.h, so tests/test_sim_serialize.cpp drives the
    # SHIPPED versioned/checksummed serializer and bounded-cursor deserializer.
    src/sim/sim_serialize.cpp
    src/sim/sim_serialize.h
    # Passive orbital ECS adapter. Gathers NBodyActive/OnRails owners into the
    # shipped collision/Kepler kernels and reconciles their real components.
    src/sim/orbit_system.cpp
    src/sim/orbit_system.h
    # Complete fixed-step simulation phase order over the GPU-free adapters.
    src/sim/simulation_system.cpp
    src/sim/simulation_system.h
    # Topology-preserving live ECS bridge for the versioned simulation codec.
    src/sim/snapshot_system.cpp
    src/sim/snapshot_system.h
    ${ASSET_SOURCES}
    ${ASSET_HEADERS}
)

set_target_properties(TheDawningTests PROPERTIES
    VS_GLOBAL_VcpkgApplocalDeps false
)

target_include_directories(TheDawningTests PRIVATE src tests)

# Absolute source path to the sky shader, so the mirror tripwire in
# tests/test_sky_energy.cpp can read it regardless of the working directory the
# test binary is launched from. The repo stays relocatable: this is resolved by
# CMake at configure time, not hardcoded.
target_compile_definitions(TheDawningTests PRIVATE
    TD_SHADER_SKY_COMMON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky_common.hlsli"
)

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
if(Python3_Interpreter_FOUND)
    add_test(
        NAME asset_manifest_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_asset_manifest.py
    )
    set_tests_properties(asset_manifest_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(
        NAME meshy_client_contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_meshy_client.py
    )
    set_tests_properties(meshy_client_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
else()
    message(WARNING
        "Python 3 was not found; offline Meshy/assembly contract tests are disabled.")
endif()
