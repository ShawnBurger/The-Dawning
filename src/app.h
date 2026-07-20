#pragma once

#include "core/timer.h"
#include "core/window.h"
#include "render/camera.h"
#include "render/d3d12_device.h"
#include "render/debug_overlay.h"
#include "render/path_tracer.h"
#include "render/renderer.h"
#include "scene/scene.h"

#include <cstdint>
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
    // Forces the per-draw structured buffers to REALLOCATE repeatedly while
    // frames are in flight. See App::RenderFrame - this is the only way that
    // branch executes at all.
    bool smokeForceGrow = false;
    bool smokeUnlocked = false;
    bool gpuValidation = false;
    bool showOverlay = true;
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
    void InitializePathTracingState();
    int RunMainLoop();
    void Shutdown();

    bool EnsurePathTracing();
    void TogglePathTracing();
    bool HandleResize();
    bool ApplySmokeResizeStep();
    bool ApplySmokeDescriptorStress();
    bool ApplySmokeRTMutationStress();
    void UpdateWindowTitle(const core::TimeStep& timeStep);
    void UpdateCamera(const core::TimeStep& timeStep);
    bool RenderFrame(const core::TimeStep& timeStep);
    render::DebugOverlayState BuildOverlayState(const core::TimeStep& timeStep) const;

    AppOptions m_options;

    core::Window m_window;
    render::D3D12Device m_device;
    render::Renderer m_renderer;
    render::DebugOverlay m_debugOverlay;
    render::Camera m_camera;
    scene::Scene m_scene;
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
    uint32_t m_smokeResizeRequests = 0;
    scene::TextureHandle m_smokeDescriptorTexture;
    scene::MeshHandle m_smokeGrowthMesh;
    ecs::Entity m_smokeTextureEntity;
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
    float m_titleTimer = 0.0f;

    render::RTQualityMode m_rtQualityMode = render::RTQualityMode::StablePreview;
    render::RTQualityInfo m_rtQualityInfo = {};
};

} // namespace dawning
