#pragma once
// =============================================================================
// scene/scene.h — Game Scene
// =============================================================================
// Owns the ECS registry and resource manager. Provides helper methods for
// creating common entity configurations and runs update systems per frame.
//
// System execution phases (called from main loop):
//   1. UpdateSystems(dt)    — gameplay logic (rotation, movement, etc.)
//   2. RenderEntities(...)  — iterates renderable entities and issues draw calls
// =============================================================================

#include "../ecs/registry.h"
#include "../ecs/components.h"
#include "resource_manager.h"
#include "handle_slot_map.h"
#include "../render/renderer.h"
#include "../render/path_tracer.h"
#include "../render/d3d12_device.h"
#include "../sim/simulation_system.h"
#include "../sim/snapshot_system.h"
#include "../core/types.h"
#include <vector>

namespace scene
{

class Scene
{
public:
    void Init();
    void Shutdown(render::D3D12Device& device, render::Renderer& renderer);

    // --- Entity creation helpers ---

    // Create a renderable entity with transform, mesh, and material
    ecs::Entity CreateRenderable(const char* name,
                                  MeshHandle mesh,
                                  const ecs::Material& material,
                                  const ecs::Transform& transform = {});

    // Create a spinning renderable
    ecs::Entity CreateSpinner(const char* name,
                               MeshHandle mesh,
                               const ecs::Material& material,
                               const ecs::Transform& transform,
                               float radiansPerSec,
                               const core::Vec3f& axis = { 0, 1, 0 });

    // Destroy an entity
    void DestroyEntity(ecs::Entity entity);

    // --- Systems ---

    // Phase 1: advance the one authoritative fixed-step scheduler.
    sim::SimulationStepResult UpdateSystems(double dt);

    // Runtime bindings consumed by the scheduler on the next fixed step.
    // FTL commands are one-shot; atmosphere and clock bindings persist.
    void QueueFtlCommand(const sim::FtlCommand& command);
    void SetAtmosphereBinding(const sim::AtmosphereBinding& binding);
    void ClearAtmosphereBinding(ecs::Entity entity);
    void SetClockGravityBinding(const sim::ClockGravityBinding& binding);
    void ClearClockGravityBinding(ecs::Entity entity);

    // Restore simulation state onto the same stable body-ID topology while
    // preserving rendering and gameplay components.
    sim::SnapshotBuildResult BuildSimulationSnapshot(double fixedDt) const;
    sim::SnapshotApplyResult ApplySimulationSnapshot(
        const sim::SimSnapshot& snapshot);

    // Phase 2a: Render all visible entities with rasterization
    void RenderEntities(render::D3D12Device& device,
                        render::Renderer& renderer,
                        const core::Vec3d& cameraPosition);

    // Phase 2a-pre: depth-only pass from the light's point of view. Same
    // traversal and the same visibility rules as RenderEntities - a caster the
    // two passes disagree about is a shadow with no object or an object with no
    // shadow, so the filter must not drift.
    void RenderShadowCasters(render::D3D12Device& device,
                             render::Renderer& renderer,
                             const core::Vec3d& cameraPosition);

    // Phase 2b: Build acceleration structures and dispatch path tracing
    void BuildAccelerationStructures(render::D3D12Device& device,
                                     const core::Vec3d& cameraPosition);
    void PathTraceEntities(render::D3D12Device& device,
                           const render::Camera& camera,
                           const core::Vec3f& lightDir,
                           const core::Vec3f& lightColor,
                           const core::Vec3f& ambientColor,
                           render::RTQualityMode qualityMode,
                           const render::RTEnvironmentInputs& environment);
    void CopyPathTraceToBackBuffer(render::D3D12Device& device);

    // --- RT Setup ---
    bool InitPathTracer(render::D3D12Device& device);
    // False if the path tracer's output textures could not be recreated. Caller
    // should fall back to raster; raster itself is unaffected.
    bool ResizePathTracer(render::D3D12Device& device, uint32_t width, uint32_t height);

    // --- Access ---
    ecs::Registry&      GetRegistry()       { return m_registry; }
    ResourceManager&    GetResources()      { return m_resources; }
    render::PathTracer* GetPathTracer()     { return &m_pathTracer; }
    uint32_t            EntityCount() const { return m_registry.EntityCount(); }

    // Size hint for the renderer's per-draw structured buffers: the MeshInstance
    // pool's population, i.e. an UPPER bound on the draws either render pass can
    // issue. It counts invisible entities and entities missing a Transform or
    // Material, all of which the two walks skip - which is exactly what you want
    // for sizing, since over-counting costs a slightly larger allocation while
    // under-counting costs skipped draws. Defined in scene.cpp per RULE 2.
    uint32_t            MeshInstanceCount() const;
    bool                IsRTReady() const   { return m_rtReady; }
    sim::FrameId        ActiveFrame() const { return m_activeFrame; }
    sim::FrameId        MasterFrame() const { return m_masterFrame; }
    double              CoordinateTime() const { return m_coordinateTime; }
    uint64_t            SimulationTick() const { return m_simTick; }
    const sim::FrameGraph& Frames() const { return m_frames; }

    void InvalidatePathTraceHistory();

    // --- RT Helpers (public for init) ---
    void EnsureBLAS(render::D3D12Device& device);

private:
    // System implementations
    void SystemVelocity(double dt);
    void SystemRotation(double dt);

    ecs::Registry        m_registry;
    ResourceManager      m_resources;
    render::PathTracer   m_pathTracer;
    bool                 m_rtReady = false;

    sim::FrameGraph      m_frames;
    sim::FrameId         m_activeFrame = sim::kInvalidFrame;
    sim::FrameId         m_masterFrame = sim::kInvalidFrame;
    double               m_coordinateTime = 0.0;
    uint64_t             m_simTick = 0;
    sim::FlightAssistParams m_flightAssist;
    sim::CloseEncounterConfig m_closeEncounters;
    std::vector<sim::FtlCommand> m_ftlCommands;
    std::vector<sim::AtmosphereBinding> m_atmosphereBindings;
    std::vector<sim::ClockGravityBinding> m_clockGravityBindings;

    // BLAS index per mesh handle (maps mesh handle value → BLAS pool index)
    HandleSlotMap<MeshHandle, uint32_t> m_meshToBLAS;
};

} // namespace scene
