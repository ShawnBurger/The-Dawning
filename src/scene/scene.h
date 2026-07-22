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

// A body's live osculating orbit about its current sphere-of-influence primary,
// derived each frame from Cartesian state (position + velocity) via
// sim::StateToElements. This is how a force-integrated body like the player ship —
// which carries no OrbitState of its own — gets a Keplerian description for the HUD
// and a predicted-path trace. `valid` is false when the body is missing, has no
// resolvable primary, or the fit is degenerate (radial / parabolic).
struct OsculatingOrbit
{
    bool                 valid         = false;
    uint64_t             primaryBodyId = 0;
    core::Vec3d          primaryPos;             // primary world position (m)
    double               primaryMu     = 0.0;    // primary GM (m^3 s^-2)
    ecs::OrbitalElements elements;               // osculating, this instant
    double               altitude      = 0.0;    // |r| from primary centre (m)
    double               speed         = 0.0;    // |v| relative to primary (m/s)
    double               periapsis     = 0.0;    // r_min = a(1-e) (m); 0 if invalid
    double               apoapsis      = 0.0;    // r_max = a(1+e) (m); 0 if not elliptic
    double               period        = 0.0;    // s; 0 if not a closed ellipse
};

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

    // Seed the deterministic reference star system (Sun + two planets + a moon,
    // true-scale, real mu) into the master frame: creates the sim entities, gives
    // each body the shared `bodySphere` mesh at its true radius, and enables live
    // sphere-of-influence transitions so on-rails bodies re-patch primaries as
    // they cross SOI boundaries. Seeded bodyIds live in a high reserved namespace
    // so they cannot collide with gameplay bodies (the ship is bodyId 1).
    // `meshRadius` is the radius the sphere mesh was generated at. Returns the
    // number of celestial bodies created. Idempotent guard: seeds at most once.
    uint32_t SeedStarSystem(MeshHandle bodySphere, float meshRadius);

    // Switch celestial bodies between TRUE radius (near-body / ship views) and
    // exaggerated ORRERY markers. At the orrery scale (K~1e-9) a true-radius
    // planet is sub-pixel, so in orrery mode each body is scaled to render at a
    // fixed marker size (Sun ~14 units, planets floored to 7) — the K cancels,
    // giving a constant on-screen size regardless of the compression. Idempotent;
    // call each frame with the active mode.
    void ApplyStarSystemRenderMode(bool orrery);

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

    // Draw an analytic single-scattering atmosphere shell for every seeded body that
    // has an atmosphere (Earth, Mars). Runs after RenderEntities, in true-scale views
    // only (K == 1 — at the orrery scale an atmosphere is sub-pixel). The per-planet
    // sun direction is normalize(star - planet) (the star sits at the frame origin),
    // computed in double; centre/radius are camera-relative (RULE 1). No-op if no
    // star system is seeded or the view is compressed.
    void RenderAtmospheres(render::D3D12Device& device,
                           render::Renderer& renderer,
                           const core::Vec3d& cameraPosition,
                           const core::Mat4x4& viewProj);

    // Build camera-relative line segments (render::Renderer::LineVertex pairs) for
    // every seeded on-rails body's orbit trace, ready for Renderer::DrawLines. Each
    // orbit is sampled about its primary via sim::SampleOrbitPath, offset to world,
    // then subtracted-in-double and scaled by the render scale K (RULE 1). Empty if
    // no star system is seeded. Used by the orrery/map view.
    std::vector<render::Renderer::LineVertex>
    BuildOrbitTraceVertices(const core::Vec3d& cameraPosition,
                            uint64_t focusBodyId = 0) const;

    // Build camera-relative billboard vertices (render::Renderer::BillboardVertex,
    // six per body) for every seeded celestial body, ready for
    // Renderer::DrawBillboards. Each body becomes one constant-pixel marker at its
    // camera-relative, K-scaled center (RULE 1), colored and sized by body. Empty
    // if no star system is seeded. Used by the near-body / ship views so a distant
    // planet or the star — sub-pixel at true scale — still reads as a legible dot.
    std::vector<render::Renderer::BillboardVertex>
    BuildBodyMarkerVertices(const core::Vec3d& cameraPosition) const;

    // Derive `bodyId`'s osculating orbit about its current SOI primary from the live
    // Cartesian state (Transform.position + RigidBody.linearVelocity). Read-only;
    // used for the player ship, which is force-integrated and carries no OrbitState.
    // Returns valid=false if the body/primary is missing or the fit is degenerate.
    OsculatingOrbit DeriveOsculatingOrbit(uint64_t bodyId) const;

    // Predict the orbit that would result from an impulsive PROGRADE burn of deltaV
    // (m/s; negative = retrograde) applied at the base orbit's current point. Goes
    // base elements -> Cartesian (ElementsToState), adds deltaV along the velocity,
    // -> osculating elements (StateToElements). This is the maneuver-node preview:
    // it answers "if I burn here, where does my orbit go" without touching the ship.
    OsculatingOrbit PreviewProgradeBurn(const OsculatingOrbit& base, double deltaV) const;

    // Build camera-relative line segments for a derived orbit (e.g. the ship's), the
    // same way BuildOrbitTraceVertices does for on-rails bodies: SampleOrbitPath about
    // the primary, offset to world, subtracted-in-double and scaled by K (RULE 1).
    // Empty if the orbit is invalid. Used to draw the ship's predicted path.
    std::vector<render::Renderer::LineVertex>
    BuildDerivedOrbitTraceVertices(const core::Vec3d& cameraPosition,
                                   const OsculatingOrbit& orbit,
                                   float r, float g, float b, float a) const;

    // Build billboard markers at the periapsis (cyan) and apoapsis (magenta) of the
    // ship's orbit and the focused body's orbit — the KSP-style apsis map markers.
    // Apoapsis is omitted for open (e >= 1) arcs. Camera-relative and K-scaled like
    // the other overlays. Empty if no star system is seeded.
    std::vector<render::Renderer::BillboardVertex>
    BuildApsisMarkerVertices(const core::Vec3d& cameraPosition,
                             const OsculatingOrbit& shipOrbit,
                             uint64_t focusBodyId) const;

    // Phase 2a-pre: depth-only pass from the light's point of view. Same
    // traversal and the same visibility rules as RenderEntities - a caster the
    // two passes disagree about is a shadow with no object or an object with no
    // shadow, so the filter must not drift.
    void RenderShadowCasters(render::D3D12Device& device,
                             render::Renderer& renderer,
                             const core::Vec3d& cameraPosition);

    // Depth-only pass from the CAMERA's point of view, for the SSAO prepass. Same
    // traversal, same visibility filter and — critically — the same camera-relative
    // matrix as RenderEntities, so the prepass depth equals what the opaque pass
    // produces. Feeds the SSAO pass; no material/texture binding.
    void RenderDepthPrepass(render::D3D12Device& device,
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
    const ecs::Registry& GetRegistry() const { return m_registry; }
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

    // Per-mode render scale K (see ecs::Transform::ToCameraRelativeMatrix): the
    // camera mode sets it before rendering — 1 for true-scale views, ~1e-9 for
    // the orrery. Applied AFTER the double camera subtract, so it never costs
    // precision.
    void   SetRenderScale(double k) { m_renderScale = k; }
    double RenderScale() const      { return m_renderScale; }

    // Surface-mode terrain compositing. When set to a seeded bodyId, RenderEntities
    // draws that body's smooth planet sphere at a hair-under-true radius so the
    // displaced terrain patches (drawn over it by App::RenderTerrainPreview) always
    // win the reversed-Z depth test — the sphere still fills the far limb and any
    // not-yet-streamed gaps, but no longer z-fights the terrain where both cover the
    // near hemisphere (worst over zero-elevation ocean, where sphere==terrain==R).
    // 0 = no terrain body (every body drawn at true radius).
    void SetTerrainSurfaceBody(uint64_t bodyId) { m_terrainSurfaceBodyId = bodyId; }

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
    // Patched-conics: once a star system is seeded, UpdateSystems runs the SOI
    // transition phase every step (off until then so the empty sandbox pays
    // nothing and its behaviour is unchanged).
    bool                 m_starSystemActive = false;
    double               m_soiHysteresis    = 0.01;
    double               m_renderScale      = 1.0; // K; per-mode, see SetRenderScale
    uint64_t             m_terrainSurfaceBodyId = 0; // see SetTerrainSurfaceBody
    float                m_bodyMeshRadius   = 0.5f; // radius the body sphere mesh spans
    sim::FlightAssistParams m_flightAssist;
    sim::CloseEncounterConfig m_closeEncounters;
    std::vector<sim::FtlCommand> m_ftlCommands;
    std::vector<sim::AtmosphereBinding> m_atmosphereBindings;
    std::vector<sim::ClockGravityBinding> m_clockGravityBindings;

    // BLAS index per mesh handle (maps mesh handle value → BLAS pool index)
    HandleSlotMap<MeshHandle, uint32_t> m_meshToBLAS;
};

} // namespace scene
