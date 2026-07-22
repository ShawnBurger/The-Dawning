#pragma once

#include "core/job_system.h"
#include "core/timer.h"
#include "core/window.h"
#include "gameplay/pilot_possession.h"
#include "render/camera.h"
#include "render/d3d12_device.h"
#include "render/debug_overlay.h"
#include "render/path_tracer.h"
#include "render/renderer.h"
#include "scene/scene.h"
#include "scene/assembly_runtime_host.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace dawning
{

struct AppOptions
{
    bool smoke = false;
    bool smokeRT = false;
    bool smokeFullQuality = false;
    bool smokeCapture = false;
    bool smokeResize = false;
    bool smokeUnlocked = false;
    bool smokeFlight = false;
    // --smoke-force-grow: steepen the structured-buffer sizing ramp so the
    // reallocate-and-DeferredRelease branch runs far more often than the default
    // run already makes it. Opt-in heavy case, not the coverage floor - the
    // default ramp is what the smoke harness asserts against.
    bool smokeForceGrow = false;
    bool gpuValidation = false;
    bool showOverlay = true;
    // Force-seed the solar system even under --smoke (default smoke does not seed),
    // and start in a chosen solar-system camera mode. Used to headlessly capture a
    // mode (e.g. --smoke --smoke-capture --star-system --camera-mode=nearbody).
    bool starSystem = false;
    int  startCameraMode = 0; // matches App::CameraMode ordinal; 0 = ShipChase
    // Focus target, as an original (un-offset) local id: 10 Earth, 11 Moon, 20 Mars,
    // 1 Sun. --focus-body=<id>. 0 = unset (near-body defaults to Earth; the terrain
    // Surface preview defaults to the Moon, whose local-scale craters read best up
    // close — Earth-scale continents need near-surface detail octaves, a follow-on).
    uint64_t focusLocalId = 0;
    std::string runtimeContentId = "reference_ship";
    double smokeSeconds = 4.0;
    double smokeRTDelaySeconds = 0.25;
};

class App final
{
public:
    int Run(const char* commandLine);

private:
    bool Initialize();
    bool InitializeScene();
    bool InitializePlayerPossession();
    bool ValidateSmokeInteriorRuntime();
    bool ValidateReferenceSmokeInteriorRuntime();
    bool ValidateProductionSmokeInteriorRuntime();
    bool ValidateSmokePossessionRoundTrip();
    void InitializePathTracingState();
    int RunMainLoop();
    void Shutdown();

    bool EnsurePathTracing();
    void TogglePathTracing();
    bool HandleResize();
    bool ApplySmokeResizeStep();
    bool ApplySmokeDescriptorStress();
    bool ApplySmokeRTMutationStress();
    // Mid-run entity churn. Runs in BOTH smoke modes, unlike the RT mutation
    // stress above: it is what forces the per-draw structured buffers to
    // reallocate with frames in flight, which is a raster-path hazard.
    void ApplySmokeGrowthStress();
    void UpdateWindowTitle(const core::TimeStep& timeStep);
    void UpdatePlayerInput();
    void ClearPlayerShipInput();
    bool HandleUseAction();
    gameplay::PilotPossessionStatus TryExitPilotSeat();
    gameplay::PilotPossessionStatus TryEnterPilotSeat();
    bool BuildPlayerShipRoot(ecs::Transform& root);
    bool BuildAssemblyInteractionQuery(
        scene::AssemblyInteractionQuery& query);
    bool UpdateOnFootSimulation(double dt);
    bool ValidateSmokeAssemblyMotion();
    bool IsReferenceRuntimeContent() const;
    void UpdatePlayerShipVisuals();
    bool UpdateCamera(const core::TimeStep& timeStep);
    // Set the per-mode render scale K (Scene::SetRenderScale) and camera near
    // plane (Camera::SetClipPlanes) for the active solar-system camera mode. Under
    // reversed-Z the near plane is the precision lever; K compresses the world for
    // the orrery. Called every frame before the camera pose is built.
    void ApplyCameraModeRenderState();
    bool AdvanceSimulation(double dt);
    bool RenderFrame(const core::TimeStep& timeStep);
    render::DebugOverlayState BuildOverlayState(const core::TimeStep& timeStep) const;

    AppOptions m_options;

    core::Window m_window;
    render::D3D12Device m_device;
    render::Renderer m_renderer;
    render::DebugOverlay m_debugOverlay;
    render::Camera m_camera;
    scene::Scene m_scene;
    scene::AssemblyRuntimeHost m_runtimeAssembly;
    core::Timer m_timer;

    bool m_windowReady = false;
    bool m_deviceReady = false;
    bool m_rendererReady = false;
    bool m_debugOverlayAttempted = false;
    bool m_debugOverlayReady = false;
    bool m_sceneReady = false;

    bool m_running = false;
    int m_exitCode = 0;
    bool m_showDebugOverlay = true;
    bool m_usePathTracing = false;
    bool m_rtAvailable = false;
    bool m_rtInitAttempted = false;
    bool m_smokeRTStarted = false;
    bool m_captureThisFrame = false;
    bool m_verifyShadowThisFrame = false;
    bool m_verifyDrawRecordsThisFrame = false;
    // Latches when the raster-frame probes have been armed, so they are armed on
    // exactly one frame. That frame is the last RASTER frame, which in the
    // default smoke mode is mid-run rather than the final frame. It carries BOTH
    // the draw-record probe and the shadow-map probe: each needs a frame on which
    // the raster pipeline ran, and the final frame is path-traced by default.
    bool m_smokeRasterVerifyRequested = false;
    // The IBL consumption probe. It runs on a PAIR of frames and the pair is the
    // assertion: the control frame renders with CBPerFrame::iblParams.z forced to
    // 0 and asserts the environment vanishes, the live frame - which is the same
    // frame as the two probes above - asserts it is present and that the cube
    // really is the prefiltered sky. See src/render/ibl_consume_probe.h for why a
    // one-sided assertion here would be worth nothing.
    bool m_verifyIBLControlThisFrame = false;
    bool m_verifyIBLConsumeThisFrame = false;
    bool m_smokeIBLControlRequested  = false;
    // The DXR twins. A SEPARATE PAIR on separate frames, not a reuse of the two
    // above, because the raster probe needs a raster frame and this one needs a
    // path-traced dispatch - in the default smoke mode those are never the same
    // frame. Sharing the flags would have re-created exactly the defect the
    // shadow probe was rebuilt to remove: one schedule serving two shaders, one
    // of which never runs on it.
    //
    // Absent entirely under -RasterOnly, where no dispatch exists to probe. The
    // harness asserts the markers' presence only in the RT modes, and their
    // ABSENCE is visible rather than silently green.
    bool m_verifyRTIBLControlThisFrame = false;
    bool m_verifyRTIBLConsumeThisFrame = false;
    bool m_smokeRTIBLControlRequested  = false;
    bool m_smokeRTIBLConsumeRequested  = false;
    bool m_smokeSnapshotVerified = false;
    bool m_smokeAssemblyBaselineReady = false;
    bool m_smokeAssemblyMotionVerified = false;
    core::Vec3d m_smokeAssemblyInitialRootPosition = {};
    core::Vec3d m_smokeAssemblyInitialModulePosition = {};
    uint32_t m_smokeResizeRequests = 0;
    scene::TextureHandle m_smokeDescriptorTexture;
    scene::MeshHandle m_smokeGrowthMesh;
    ecs::Entity m_smokeTextureEntity;
    ecs::Entity m_smokeCameraTarget;
    ecs::Entity m_playerShip;
    gameplay::PilotSeatBinding m_pilotSeat;
    gameplay::PlayerPossessionState m_playerPossession;
    gameplay::PilotPossessionConfig m_possessionConfig;
    gameplay::OnFootCommand m_onFootCommand;
    bool m_possessionReady = false;
    std::array<ecs::Entity, ecs::ThrusterSet::kMaxThrusters> m_thrusterVisuals = {};
    scene::MeshHandle m_thrusterVisualMesh;
    uint32_t m_thrusterVisualCount = 0;
    float m_pendingPointerDeltaX = 0.0f;
    float m_pendingPointerDeltaY = 0.0f;
    core::Vec3d m_chaseCameraPosition = {};
    float m_chaseCameraYaw = 0.0f;
    float m_chaseCameraPitch = 0.0f;
    bool m_chaseCameraInitialized = false;

    // Solar-system camera modes (Prototype). ShipChase is the default meters-scale
    // follow camera; the others frame the true-scale star system. Cycled with F4.
    enum class CameraMode : uint32_t
    {
        ShipChase = 0, // follow the player ship (K=1, true scale) — the default
        Orrery,        // whole system, compressed view scale (K<<1), free camera
        NearBody,      // parked at a real body at true scale (K=1)
        Free,          // free-fly at true scale (K=1)
        Surface,       // low over a body's surface, rendering chunked-LOD terrain (K=1)
        Count
    };
    CameraMode m_cameraMode = CameraMode::ShipChase;
    uint64_t   m_focusBodyId = 0; // seeded bodyId the near-body/orrery view frames

    // Chunked-LOD terrain preview (Surface camera mode): the quadtree-selected leaf
    // set on the focus body (the Moon), each a displaced cube-sphere patch built
    // once at Init (fine under the camera, coarse toward the horizon) and drawn
    // camera-relative each Surface frame.
    struct TerrainLeaf { render::Mesh mesh; core::Vec3d originBody; uint32_t lastSeen = 0; };
    std::unordered_map<uint64_t, TerrainLeaf> m_terrainCache; // keyed by face/level/cell
    bool        m_terrainBuilt  = false;    // Surface mode active + framing computed
    uint64_t    m_terrainBodyId = 0;        // seeded bodyId the patches sit on
    core::Vec3d m_terrainCamBody{ 0, 0, 0 };   // Surface camera pos, body space (framing)
    core::Vec3d m_terrainFocusBody{ 0, 0, 0 }; // look-at surface point, body space
    uint32_t    m_terrainStreamFrame = 0;   // increments each Surface frame (for LRU)
    double      m_surfaceElapsed = 0.0;      // seconds in Surface mode, drives the descent
    double      m_surfaceEntryTime = -1.0;   // totalTime at Surface entry; <0 = not entered
    // Worker pool for parallel chunk generation, created lazily on the first Surface
    // frame that has cache misses (so non-terrain runs spawn no threads). GenerateChunk
    // + the vertex conversion are pure CPU; only the GPU upload/cache/draw stay on main.
    std::unique_ptr<core::JobSystem> m_terrainJobs;
    // Stream the quadtree leaf set for the current camera each frame: reselect LOD,
    // generate+cache newly-visible patches (write-once upload meshes), draw the
    // resident visible set camera-relative.
    void RenderTerrainPreview();

    // Set once the player ship has been placed on an orbit inside the seeded system
    // (start camera mode 4). Gates the ship's live-orbit HUD/trace so they never read
    // the ship's inert demo-sandbox state when it was never flown into the system.
    bool       m_shipInSystem = false;
    // The ship's osculating orbit about its current SOI primary, recomputed once per
    // render frame (m_shipInSystem only) and consumed by the HUD and the orbit trace.
    scene::OsculatingOrbit m_shipOrbit;
    // Toggled with M: overlay a maneuver-node preview of the orbit a prograde burn
    // would produce (amber), so the player can plan a burn before committing.
    bool m_showManeuverPreview = false;

    ecs::Material m_smokeGrowthMaterial;
    uint32_t m_smokeSavedAlbedoTexture = UINT32_MAX;
    uint32_t m_smokeSavedNormalTexture = UINT32_MAX;
    uint32_t m_smokeSavedOrmTexture = UINT32_MAX;
    uint32_t m_smokeSavedEmissiveTexture = UINT32_MAX;
    std::vector<ecs::Entity> m_smokeGrowthEntities;
    render::DescriptorHandle m_smokeRetiredDescriptor;
    render::DescriptorHandle m_smokeHeldDescriptor;
    uint64_t m_frameCount = 0;
    int64_t m_smokeStartCounter = 0;
    int64_t m_smokeCounterFrequency = 0;
    uint32_t m_smokeMaxOutstandingSubmissions = 0;
    uint64_t m_smokeFirstStructuredBufferReallocationFrame = UINT64_MAX;
    float m_titleTimer = 0.0f;

    render::RTQualityMode m_rtQualityMode = render::RTQualityMode::StablePreview;
    render::RTQualityInfo m_rtQualityInfo = {};
};

} // namespace dawning
