cmake_minimum_required(VERSION 3.20)
project(TheDawningV3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# Source files
# =============================================================================
set(CORE_SOURCES
    src/core/log.cpp
    src/core/input.cpp
    src/core/timer.cpp
    src/core/window.cpp
)

set(CORE_HEADERS
    src/core/types.h
    src/core/log.h
    src/core/input.h
    src/core/timer.h
    src/core/window.h
)

set(RENDER_SOURCES
    src/render/d3d12_device.cpp
    src/render/shader_utils.cpp
    src/render/mesh.cpp
    src/render/texture.cpp
    src/render/camera.cpp
    src/render/renderer.cpp
    src/render/debug_overlay.cpp
    src/render/rt_acceleration.cpp
    src/render/rt_pipeline.cpp
    src/render/path_tracer.cpp
)

set(RENDER_HEADERS
    src/render/d3d12_device.h
    src/render/shader_utils.h
    src/render/mesh.h
    src/render/texture.h
    src/render/camera.h
    src/render/renderer.h
    src/render/debug_overlay.h
    src/render/rt_acceleration.h
    src/render/rt_pipeline.h
    src/render/path_tracer.h
)

set(SHADER_FILES
    shaders/basic_vs.hlsl
    shaders/basic_ps.hlsl
    shaders/overlay_vs.hlsl
    shaders/overlay_ps.hlsl
    shaders/path_trace.hlsl
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
)

set(SCENE_HEADERS
    src/scene/resource_manager.h
    src/scene/scene.h
)

# =============================================================================
# Executable
# =============================================================================
add_executable(TheDawningV3 WIN32
    src/main.cpp
    ${CORE_SOURCES}
    ${CORE_HEADERS}
    ${RENDER_SOURCES}
    ${RENDER_HEADERS}
    ${ECS_HEADERS}
    ${SCENE_SOURCES}
    ${SCENE_HEADERS}
    ${SHADER_FILES}
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

# =============================================================================
# Source groups for Visual Studio
# =============================================================================
source_group("Core"    FILES ${CORE_SOURCES} ${CORE_HEADERS})
source_group("Render"  FILES ${RENDER_SOURCES} ${RENDER_HEADERS})
source_group("ECS"     FILES ${ECS_HEADERS})
source_group("Scene"   FILES ${SCENE_SOURCES} ${SCENE_HEADERS})
source_group("Shaders" FILES ${SHADER_FILES})
source_group("Main"    FILES src/main.cpp)

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
endif()
