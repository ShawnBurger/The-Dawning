#include "app.h"
#include "scene/model_loader.h"
#include "terrain/cube_sphere.h"
#include "terrain/chunk_mesh.h"
#include "terrain/planet_quadtree.h"
#include "core/planet_height.h"

#include "core/input.h"
#include "core/log.h"
#include "gameplay/on_foot_controller.h"
#include "gameplay/pilot_possession.h"
#include "gameplay/playable_ship.h"
#include "scene/star_system_seed.h"   // kStarSystemBodyIdBase
#include "render/mesh.h"
#include "render/texture.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>   // cooked runtime-asset path
#include <iterator>     // std::size, for the pillar distance table
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace
{

constexpr const char* kSmokeCaptureFile = "smoke_capture.ppm";
constexpr double kSmokeFixedDeltaSeconds = 1.0 / 60.0;

// The player ship's GravitationalBody id. Distinct from the seeded celestial bodies,
// which live at kStarSystemBodyIdBase + local id (the star's builder id is also 1,
// hence the offset). Used to look the ship up for its live osculating orbit.
constexpr uint64_t kPlayerShipBodyId = 1;

// Per-body terrain configuration (mirrors PlanetParamsFor + the true radii). type:
// 0 Earth-like, 1 Mars-like, 2 Moon-like, 3 generic. amplitude is exaggerated for a
// readable single-body descent demo.
struct TerrainBodyCfg
{
    double      radius;
    float       amplitude, seed, seaLevel, coastWidth;
    int         type;
    core::Vec3f landLow, landHigh, deep, shallow, ice, ambient;
    float       iceLat, depthScale;
};

TerrainBodyCfg ConfigForTerrainBody(uint64_t localId)
{
    switch (localId)
    {
        case 10: return { 6.371e6, 26000.0f, 11.0f, 0.52f, 0.02f, 0,   // Earth
                          {0.090f,0.230f,0.100f}, {0.320f,0.280f,0.220f},
                          {0.015f,0.050f,0.140f}, {0.050f,0.220f,0.320f},
                          {0.850f,0.880f,0.920f}, {0.015f,0.020f,0.030f}, 0.80f, 0.25f };
        case 20: return { 3.3895e6, 22000.0f, 22.0f, 1.0f, 0.0f, 1,    // Mars
                          {0.420f,0.200f,0.110f}, {0.620f,0.360f,0.230f},
                          {0,0,0}, {0,0,0},
                          {0.900f,0.900f,0.920f}, {0.020f,0.012f,0.008f}, 0.82f, 0.0f };
        case 11: return { 1.7374e6, 16000.0f, 33.0f, 1.0f, 0.0f, 2,    // Moon
                          {0.045f,0.045f,0.050f}, {0.400f,0.400f,0.420f},
                          {0,0,0}, {0,0,0},
                          {0,0,0}, {0.010f,0.010f,0.012f}, 2.0f, 0.0f };
        default: return { 2.5e6, 18000.0f, 7.0f, 1.0f, 0.0f, 3,        // generic
                          {0.300f,0.300f,0.320f}, {0.500f,0.500f,0.520f},
                          {0,0,0}, {0,0,0},
                          {0.800f,0.820f,0.850f}, {0.014f,0.014f,0.016f}, 2.0f, 0.0f };
    }
}

// Display name for a seeded body by its LOCAL (un-offset) id. Shared by the HUD's
// focus block and the ship-orbit block's primary label.
const char* SeededBodyName(uint64_t localId)
{
    switch (localId)
    {
        case 1:  return "SUN";
        case 10: return "EARTH";
        case 11: return "MOON";
        case 20: return "MARS";
        default: return "BODY";
    }
}

uint64_t SmokeFrameForTime(double seconds)
{
    return static_cast<uint64_t>(std::ceil(seconds / kSmokeFixedDeltaSeconds));
}

bool HasOption(const std::string& args, const char* option)
{
    return args.find(option) != std::string::npos;
}

double ReadDoubleOption(const std::string& args, const char* option, double fallback)
{
    size_t pos = args.find(option);
    if (pos == std::string::npos)
        return fallback;

    pos += std::strlen(option);
    char* end = nullptr;
    const double value = std::strtod(args.c_str() + pos, &end);
    return end != args.c_str() + pos ? value : fallback;
}

std::string ReadTokenOption(
    const std::string& args,
    const char* option,
    std::string fallback)
{
    size_t searchFrom = 0;
    while (searchFrom < args.size())
    {
        const size_t optionPos = args.find(option, searchFrom);
        if (optionPos == std::string::npos)
            return fallback;
        if (optionPos == 0 || args.find_first_of(" \t\r\n", optionPos - 1) ==
                                  optionPos - 1)
        {
            const size_t valuePos = optionPos + std::strlen(option);
            const size_t valueEnd = args.find_first_of(" \t\r\n", valuePos);
            return args.substr(valuePos, valueEnd - valuePos);
        }
        searchFrom = optionPos + 1;
    }
    return fallback;
}

dawning::AppOptions ParseOptions(const char* commandLine)
{
    dawning::AppOptions options;
    const std::string args = commandLine ? commandLine : "";

    options.smoke = HasOption(args, "--smoke");
    options.smokeRT = HasOption(args, "--smoke-rt");
    options.smokeFullQuality = HasOption(args, "--smoke-full");
    options.smokeCapture = HasOption(args, "--smoke-capture");
    options.smokeResize = HasOption(args, "--smoke-resize");
    options.smokeUnlocked = HasOption(args, "--smoke-unlocked");
    options.smokeFlight = HasOption(args, "--smoke-flight");
    options.smokeForceGrow = HasOption(args, "--smoke-force-grow");
    options.gpuValidation = HasOption(args, "--gpu-validation");
    options.showOverlay = !HasOption(args, "--no-overlay");
    options.runtimeContentId = ReadTokenOption(
        args, "--content=", std::move(options.runtimeContentId));
    options.smokeSeconds = ReadDoubleOption(args, "--smoke-seconds=", options.smokeSeconds);
    options.smokeRTDelaySeconds = ReadDoubleOption(args, "--smoke-rt-delay=", options.smokeRTDelaySeconds);
    options.starSystem = HasOption(args, "--star-system");
    if (HasOption(args, "--camera-mode=orrery"))        options.startCameraMode = 1;
    else if (HasOption(args, "--camera-mode=nearbody")) options.startCameraMode = 2;
    else if (HasOption(args, "--camera-mode=free"))     options.startCameraMode = 3;
    else if (HasOption(args, "--camera-mode=surface"))  options.startCameraMode = 5; // terrain preview
    else if (HasOption(args, "--camera-mode=ship"))     options.startCameraMode = 4; // ship-in-system
    else                                                options.startCameraMode = 0;
    // Near-body focus target as an un-offset local id (10 Earth / 11 Moon / 20 Mars).
    // Validate before narrowing: casting a negative / out-of-range / NaN double to
    // uint64_t is undefined behaviour, and no body id lives outside [0, 1e6). A NaN
    // fails the >= comparison, so it too keeps the default.
    const double focusRequested =
        ReadDoubleOption(args, "--focus-body=", static_cast<double>(options.focusLocalId));
    if (focusRequested >= 0.0 && focusRequested < 1.0e6)
        options.focusLocalId = static_cast<uint64_t>(focusRequested);

    if (options.smoke && !HasOption(args, "--show-overlay"))
        options.showOverlay = false;
    if (options.smokeFullQuality)
        options.smokeRT = true;
    if (options.smokeSeconds < 0.5)
        options.smokeSeconds = 0.5;
    if (options.smokeRTDelaySeconds < 0.0)
        options.smokeRTDelaySeconds = 0.0;
    if (options.smokeRT && options.smokeSeconds <= options.smokeRTDelaySeconds + 0.5)
        options.smokeSeconds = options.smokeRTDelaySeconds + 0.5;

    return options;
}

bool FileExists(const char* path)
{
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void SetWorkingDirectoryToExecutable()
{
    char exePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) // 0 = failed; >= MAX_PATH = truncated (maybe unterminated)
    {
        core::Log::Warn("Could not resolve executable path; leaving working directory");
        return;
    }
    char* lastSlash = std::strrchr(exePath, '\\');
    if (lastSlash)
        *lastSlash = '\0';
    SetCurrentDirectoryA(exePath);
    core::Log::Infof("Working directory: %s", exePath);
}

} // namespace

namespace dawning
{

int App::Run(const char* commandLine)
{
    core::Log::Init();
    core::Log::Info("=== The Dawning V3 Engine Starting (Layer 4: Materials) ===");

    m_options = ParseOptions(commandLine);
    if (m_options.smoke)
    {
        core::Log::Infof("Smoke mode enabled (rt=%s, full=%s, unlocked=%s, flight=%s, seconds=%.2f)",
                         m_options.smokeRT ? "yes" : "no",
                         m_options.smokeFullQuality ? "yes" : "no",
                         m_options.smokeUnlocked ? "yes" : "no",
                         m_options.smokeFlight ? "yes" : "no",
                         m_options.smokeSeconds);
    }

    SetWorkingDirectoryToExecutable();

    if (Initialize())
        m_exitCode = RunMainLoop();
    else
        m_exitCode = 1;

    Shutdown();

    if (m_exitCode == 0 && core::Log::ErrorCount() > 0)
    {
        core::Log::Errorf("Run logged %u error(s); failing with exit code 4",
                          core::Log::ErrorCount());
        m_exitCode = 4;
    }

    core::Log::Info("=== The Dawning V3 Engine stopped ===");
    core::Log::Shutdown();
    return m_exitCode;
}

bool App::Initialize()
{
    core::WindowDesc windowDesc;
    windowDesc.title = "The Dawning V3";
    windowDesc.width = 1920;
    windowDesc.height = 1080;

    if (!m_window.Init(windowDesc))
    {
        core::Log::Error("Failed to create window");
        return false;
    }
    m_windowReady = true;

#ifdef NDEBUG
    constexpr bool enableDebug = false;
#else
    constexpr bool enableDebug = true;
#endif

    if (!m_device.Init(m_window.GetHWND(), m_window.GetWidth(), m_window.GetHeight(),
                       enableDebug, m_options.gpuValidation))
    {
        core::Log::Error("Failed to initialize D3D12");
        return false;
    }
    m_deviceReady = true;

    if (!m_renderer.Init(m_device))
    {
        core::Log::Error("Failed to initialize renderer");
        return false;
    }
    m_rendererReady = true;

    m_renderer.SetDirectionalLight(
        core::Vec3f(0.5f, 0.8f, 0.3f).Normalized(),
        { 1.0f, 0.97f, 0.92f },
        { 0.12f, 0.14f, 0.22f });

    m_debugOverlayAttempted = true;
    m_debugOverlayReady = m_debugOverlay.Init(m_device);
    m_showDebugOverlay = m_options.showOverlay;
    if (!m_debugOverlayReady)
        core::Log::Warn("Debug overlay failed to initialize - continuing without it");

    // Flight/targeting HUD (self-contained render module). Non-fatal: the flying
    // views simply show no HUD if it fails to build.
    m_hudReady = m_hud.Init(m_device.Device());
    if (!m_hudReady)
        core::Log::Warn("HUD renderer failed to initialize - continuing without it");

    m_camera.Init({ 0.0f, 2.0f, -5.0f }, 0.0f, -10.0f);
    m_camera.SetMoveSpeed(5.0f);
    m_camera.SetFOV(70.0f);

    m_scene.Init();
    m_sceneReady = true;
    if (!InitializeScene())
    {
        core::Log::Error("Failed to initialize scene GPU resources");
        return false;
    }
    InitializePathTracingState();

    m_timer.Init();
    return true;
}

bool App::InitializeScene()
{
    if (!m_device.WaitForCurrentFrame() || !m_device.ResetCommandList())
        return false;

    ComPtr<ID3D12Resource> cubeVBUp, cubeIBUp;
    ComPtr<ID3D12Resource> planeVBUp, planeIBUp;
    ComPtr<ID3D12Resource> sphereVBUp, sphereIBUp;
    ComPtr<ID3D12Resource> groundTexUp, cubeTexUp;
    ComPtr<ID3D12Resource> groundNormalTexUp, cubeNormalTexUp;

    auto cubeData = render::GenerateCube(core::Color::White());
    // 200x200 rather than 10x10. The shadow cascade covers 48 world units, so a
    // 10-unit ground plane meant every caster in the scene fell inside a single
    // cascade and nothing could exercise - or fail to exercise - the others.
    // A material feature no scene uses is untested code; this project has been
    // bitten by that enough times to stop repeating it.
    // 20 tiles across 200 units keeps one texture repeat per 10 world units,
    // which is exactly the density the old 10x10 plane had. Without this the
    // enlarged plane stretches the same single repeat over 20x the area and the
    // ground loses all its detail - a regression introduced by growing the
    // plane, not by anything to do with shadows.
    auto planeData = render::GeneratePlane(200.0f, 200.0f, 40, 40,
                                           core::Color::White(), 20.0f);
    auto sphereData = render::GenerateSphere(0.5f, 32, 16, core::Color::White());

    auto cubeMesh = render::CreateMesh(
        m_device.Device(), m_device.CmdList(),
        cubeData.vertices.data(), static_cast<uint32_t>(cubeData.vertices.size()),
        cubeData.indices.data(), static_cast<uint32_t>(cubeData.indices.size()),
        cubeVBUp, cubeIBUp);

    auto planeMesh = render::CreateMesh(
        m_device.Device(), m_device.CmdList(),
        planeData.vertices.data(), static_cast<uint32_t>(planeData.vertices.size()),
        planeData.indices.data(), static_cast<uint32_t>(planeData.indices.size()),
        planeVBUp, planeIBUp);

    auto sphereMesh = render::CreateMesh(
        m_device.Device(), m_device.CmdList(),
        sphereData.vertices.data(), static_cast<uint32_t>(sphereData.vertices.size()),
        sphereData.indices.data(), static_cast<uint32_t>(sphereData.indices.size()),
        sphereVBUp, sphereIBUp);

    CreateDirectoryA("assets", nullptr);
    CreateDirectoryA("assets\\textures", nullptr);

    const char* groundKTXPath = "assets\\textures\\ground_grid.ktx";
    const char* cubeKTXPath = "assets\\textures\\blue_panels.ktx";
    const char* groundPNGPath = "assets\\textures\\ground_grid.png";
    const char* cubePNGPath = "assets\\textures\\blue_panels.png";
    const char* groundDDSPath = "assets\\textures\\ground_grid.dds";
    const char* cubeDDSPath = "assets\\textures\\blue_panels.dds";
    const char* groundNormalKTXPath = "assets\\textures\\ground_normal.ktx";
    const char* cubeNormalKTXPath = "assets\\textures\\cube_normal.ktx";
    const char* groundNormalPNGPath = "assets\\textures\\ground_normal.png";
    const char* cubeNormalPNGPath = "assets\\textures\\cube_normal.png";
    const char* groundNormalDDSPath = "assets\\textures\\ground_normal.dds";
    const char* cubeNormalDDSPath = "assets\\textures\\cube_normal.dds";

    if (!FileExists(groundDDSPath))
    {
        render::WriteCheckerDDSTextureRGBA8(
            groundDDSPath, 512, 512, 32,
            { 0.82f, 0.86f, 0.91f, 1.0f },
            { 0.48f, 0.53f, 0.60f, 1.0f });
    }
    if (!FileExists(cubeDDSPath))
    {
        render::WriteCheckerDDSTextureRGBA8(
            cubeDDSPath, 256, 256, 32,
            { 0.20f, 0.42f, 0.92f, 1.0f },
            { 0.03f, 0.08f, 0.20f, 1.0f });
    }

    render::Texture groundTexture;
    if (FileExists(groundKTXPath))
    {
        groundTexture.Adopt(render::CreateTexture2DFromKTXFile(
            m_device.Device(), m_device.CmdList(),
            groundKTXPath, groundTexUp, L"GroundAlbedoTexture"));
    }
    if (!groundTexture.IsValid() && FileExists(groundPNGPath))
    {
        groundTexture.Adopt(render::CreateTexture2DFromWICFile(
            m_device.Device(), m_device.CmdList(),
            groundPNGPath, groundTexUp, L"GroundAlbedoTexture"));
    }
    if (!groundTexture.IsValid())
    {
        groundTexture.Adopt(render::CreateTexture2DFromDDSFile(
            m_device.Device(), m_device.CmdList(),
            groundDDSPath, groundTexUp, L"GroundAlbedoTexture"));
    }
    if (!groundTexture.IsValid())
    {
        auto pixels = render::GenerateCheckerTextureRGBA8(
            512, 512, 32,
            { 0.82f, 0.86f, 0.91f, 1.0f },
            { 0.48f, 0.53f, 0.60f, 1.0f });
        groundTexture.Adopt(render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 512, 512, groundTexUp, L"GroundAlbedoTexture"));
    }
    groundTexture.descriptor = m_renderer.RegisterTexture(m_device.Device(), groundTexture);

    render::Texture cubeTexture;
    if (FileExists(cubeKTXPath))
    {
        cubeTexture.Adopt(render::CreateTexture2DFromKTXFile(
            m_device.Device(), m_device.CmdList(),
            cubeKTXPath, cubeTexUp, L"BluePanelAlbedoTexture"));
    }
    if (!cubeTexture.IsValid() && FileExists(cubePNGPath))
    {
        cubeTexture.Adopt(render::CreateTexture2DFromWICFile(
            m_device.Device(), m_device.CmdList(),
            cubePNGPath, cubeTexUp, L"BluePanelAlbedoTexture"));
    }
    if (!cubeTexture.IsValid())
    {
        cubeTexture.Adopt(render::CreateTexture2DFromDDSFile(
            m_device.Device(), m_device.CmdList(),
            cubeDDSPath, cubeTexUp, L"BluePanelAlbedoTexture"));
    }
    if (!cubeTexture.IsValid())
    {
        auto pixels = render::GenerateCheckerTextureRGBA8(
            256, 256, 32,
            { 0.20f, 0.42f, 0.92f, 1.0f },
            { 0.03f, 0.08f, 0.20f, 1.0f });
        cubeTexture.Adopt(render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 256, 256, cubeTexUp, L"BluePanelAlbedoTexture"));
    }
    cubeTexture.descriptor = m_renderer.RegisterTexture(m_device.Device(), cubeTexture);

    render::Texture groundNormalTexture;
    if (FileExists(groundNormalKTXPath))
    {
        groundNormalTexture.Adopt(render::CreateTexture2DFromKTXFile(
            m_device.Device(), m_device.CmdList(),
            groundNormalKTXPath, groundNormalTexUp, L"GroundNormalTexture"));
    }
    if (!groundNormalTexture.IsValid() && FileExists(groundNormalPNGPath))
    {
        groundNormalTexture.Adopt(render::CreateTexture2DFromWICFile(
            m_device.Device(), m_device.CmdList(),
            groundNormalPNGPath, groundNormalTexUp, L"GroundNormalTexture"));
    }
    if (!groundNormalTexture.IsValid() && FileExists(groundNormalDDSPath))
    {
        groundNormalTexture.Adopt(render::CreateTexture2DFromDDSFile(
            m_device.Device(), m_device.CmdList(),
            groundNormalDDSPath, groundNormalTexUp, L"GroundNormalTexture"));
    }
    if (!groundNormalTexture.IsValid())
    {
        auto pixels = render::GenerateWaveNormalTextureRGBA8(512, 512, 8.0f, 0.008f);
        groundNormalTexture.Adopt(render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 512, 512, groundNormalTexUp, L"GroundNormalTexture"));
    }
    groundNormalTexture.descriptor =
        m_renderer.RegisterTexture(m_device.Device(), groundNormalTexture);

    ComPtr<ID3D12Resource> groundOrmTexUp;
    ComPtr<ID3D12Resource> cubeOrmTexUp;
    ComPtr<ID3D12Resource> cubeEmissiveTexUp;

    render::Texture cubeNormalTexture;
    if (FileExists(cubeNormalKTXPath))
    {
        cubeNormalTexture.Adopt(render::CreateTexture2DFromKTXFile(
            m_device.Device(), m_device.CmdList(),
            cubeNormalKTXPath, cubeNormalTexUp, L"CubeNormalTexture"));
    }
    if (!cubeNormalTexture.IsValid() && FileExists(cubeNormalPNGPath))
    {
        cubeNormalTexture.Adopt(render::CreateTexture2DFromWICFile(
            m_device.Device(), m_device.CmdList(),
            cubeNormalPNGPath, cubeNormalTexUp, L"CubeNormalTexture"));
    }
    if (!cubeNormalTexture.IsValid() && FileExists(cubeNormalDDSPath))
    {
        cubeNormalTexture.Adopt(render::CreateTexture2DFromDDSFile(
            m_device.Device(), m_device.CmdList(),
            cubeNormalDDSPath, cubeNormalTexUp, L"CubeNormalTexture"));
    }
    if (!cubeNormalTexture.IsValid())
    {
        auto pixels = render::GenerateWaveNormalTextureRGBA8(256, 256, 6.0f, 0.012f);
        cubeNormalTexture.Adopt(render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 256, 256, cubeNormalTexUp, L"CubeNormalTexture"));
    }
    cubeNormalTexture.descriptor =
        m_renderer.RegisterTexture(m_device.Device(), cubeNormalTexture);

    // Packed occlusion/roughness/metallic (glTF: AO=R, rough=G, metal=B).
    // Generated rather than loaded because assets/textures/ ships only a README,
    // so a file-based path would leave this feature unexercised on a clean clone
    // - and an unexercised material path is untested code.
    render::Texture groundOrmTexture;
    {
        auto pixels = render::GenerateCheckerORMTextureRGBA8(512, 512, 64,
                                                             0.85f, 0.35f,   // rough: matte vs polished
                                                             0.0f,  0.0f);   // ground stays dielectric
        groundOrmTexture.Adopt(render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 512, 512, groundOrmTexUp, L"GroundORMTexture"));
    }
    groundOrmTexture.descriptor =
        m_renderer.RegisterTexture(m_device.Device(), groundOrmTexture);

    render::Texture cubeOrmTexture;
    {
        // Alternating dielectric and metal cells, so the metallic channel is
        // visibly doing something rather than just riding the material scalar.
        auto pixels = render::GenerateCheckerORMTextureRGBA8(256, 256, 32,
                                                             0.55f, 0.15f,
                                                             0.0f,  1.0f);
        cubeOrmTexture.Adopt(render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 256, 256, cubeOrmTexUp, L"CubeORMTexture"));
    }
    cubeOrmTexture.descriptor =
        m_renderer.RegisterTexture(m_device.Device(), cubeOrmTexture);

    // Emissive mask for the cube, generated for the same reason as the ORM maps.
    // Only one material in the demo is emissive: emission is added on top of
    // everything else, so making it universal would just raise the floor of the
    // whole image and prove nothing.
    render::Texture cubeEmissiveTexture;
    {
        auto pixels = render::GeneratePanelEmissiveTextureRGBA8(256, 256, 64, 0.5f);
        cubeEmissiveTexture.Adopt(render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 256, 256, cubeEmissiveTexUp, L"CubeEmissiveTexture"));
    }
    cubeEmissiveTexture.descriptor =
        m_renderer.RegisterTexture(m_device.Device(), cubeEmissiveTexture);

    // One generic data manifest now owns the cooked assembly and all typed
    // resource bindings. It records model uploads onto this same open command
    // list but publishes no ECS entities until the upload batch retires below.
    const asset::RuntimeContentSelectionResult contentSelection =
        asset::BuildRuntimeContentManifestPath(m_options.runtimeContentId);
    if (!contentSelection.accepted)
    {
        core::Log::Errorf(
            "Runtime content selection rejected: %s",
            contentSelection.error.c_str());
        return false;
    }
    core::Log::Infof(
        "[SMOKE] runtime_content_selection=ok content=%s",
        contentSelection.contentId.c_str());

    const scene::AssemblyRuntimeHostResult runtimePrepared =
        m_runtimeAssembly.BeginLoad(
            m_scene, m_device, m_renderer,
            contentSelection.manifestPath);
    if (!runtimePrepared.Succeeded())
    {
        core::Log::Errorf("Required runtime content failed to prepare [%s]: %s",
                          scene::AssemblyRuntimeHostStatusName(
                              runtimePrepared.status),
                          runtimePrepared.error.c_str());
        return false;
    }

    const HRESULT closeHr = m_device.CmdList()->Close();
    if (FAILED(closeHr))
    {
        core::Log::Errorf("Scene upload command list Close failed: 0x%08X", closeHr);
        return false;
    }
    ID3D12CommandList* uploadLists[] = { m_device.CmdList() };
    m_device.CmdQueue()->ExecuteCommandLists(1, uploadLists);
    if (!m_device.WaitForGpu())
        return false;

    const scene::AssemblyRuntimeHostResult runtimeCommitted =
        m_runtimeAssembly.CommitAfterUploadRetirement(m_scene);
    if (!runtimeCommitted.Succeeded())
    {
        core::Log::Errorf("Required runtime content failed to commit [%s]: %s",
                          scene::AssemblyRuntimeHostStatusName(
                              runtimeCommitted.status),
                          runtimeCommitted.error.c_str());
        return false;
    }
    if (m_options.smoke && !ValidateSmokeInteriorRuntime())
        return false;

    auto& resources = m_scene.GetResources();
    const auto cube = resources.AddMesh(std::move(cubeMesh), "Cube");
    const auto plane = resources.AddMesh(std::move(planeMesh), "Plane");
    const auto sphere = resources.AddMesh(std::move(sphereMesh), "Sphere");
    const auto groundAlbedo = resources.AddTexture(std::move(groundTexture), "GroundGrid");
    const auto cubeAlbedo = resources.AddTexture(std::move(cubeTexture), "BluePanels");
    const auto groundNormal = resources.AddTexture(
        std::move(groundNormalTexture), "GroundWaveNormal");
    const auto cubeNormal = resources.AddTexture(
        std::move(cubeNormalTexture), "CubeWaveNormal");
    const auto groundOrm = resources.AddTexture(
        std::move(groundOrmTexture), "GroundCheckerORM");
    const auto cubeOrm = resources.AddTexture(
        std::move(cubeOrmTexture), "CubeCheckerORM");
    const auto cubeEmissive = resources.AddTexture(
        std::move(cubeEmissiveTexture), "CubePanelEmissive");
    if (!resources.IsValidMesh(cube) || !resources.IsValidMesh(plane) ||
        !resources.IsValidMesh(sphere) ||
        !resources.IsValidTexture(groundAlbedo) ||
        !resources.IsValidTexture(cubeAlbedo) ||
        !resources.IsValidTexture(groundNormal) ||
        !resources.IsValidTexture(cubeNormal) ||
        !resources.IsValidTexture(groundOrm) ||
        !resources.IsValidTexture(cubeOrm) ||
        !resources.IsValidTexture(cubeEmissive))
    {
        core::Log::Error("Required demo resources failed to register");
        return false;
    }
    m_smokeDescriptorTexture = groundAlbedo;

    core::Log::Infof("Meshes registered: cube=%u plane=%u sphere=%u",
                     cube.Index(), plane.Index(), sphere.Index());
    core::Log::Infof("Textures registered: ground=%u cube=%u groundNormal=%u cubeNormal=%u",
                     groundAlbedo.Index(), cubeAlbedo.Index(),
                     groundNormal.Index(), cubeNormal.Index());
    core::Log::Infof("[SMOKE] orm_textures=ok ground=%u cube=%u",
                     groundOrm.Index(), cubeOrm.Index());
    core::Log::Infof("[SMOKE] emissive_textures=ok cube=%u", cubeEmissive.Index());

    m_scene.CreateRenderable(
        "GroundPlane", plane,
        ecs::Material{ { 0.54f, 0.57f, 0.62f, 1.0f }, 0.9f, 0.0f,
                       groundAlbedo.value, groundNormal.value, groundOrm.value },
        ecs::Transform{ { 0, 0, 0 }, core::Quatf::Identity(), { 1, 1, 1 } });

    m_smokeGrowthMesh = cube;
    m_smokeGrowthMaterial =
        ecs::Material{ { 0.6f, 0.8f, 1.0f, 1.0f }, 0.3f, 0.9f,
                       cubeAlbedo.value, cubeNormal.value, cubeOrm.value,
                       core::Color{ 0.25f, 0.85f, 1.0f, 1.0f }, 2.5f,
                       cubeEmissive.value };
    const scene::AssemblyInstance* playerAssembly = m_runtimeAssembly.Instance();
    if (!playerAssembly || !playerAssembly->IsAlive() ||
        playerAssembly->RootEntity().IsNull() ||
        playerAssembly->ModuleEntities().empty())
    {
        core::Log::Error("Playable ship has no committed assembly root");
        return false;
    }
    m_playerShip = playerAssembly->RootEntity();

    auto& registry = m_scene.GetRegistry();
    if (!registry.IsAlive(m_playerShip) ||
        registry.TryGet<ecs::Transform>(m_playerShip) == nullptr ||
        registry.Has<ecs::MeshInstance>(m_playerShip))
    {
        core::Log::Error("Playable assembly root is not a meshless live transform");
        return false;
    }
    registry.Assign<ecs::RigidBody>(m_playerShip, gameplay::MakeFighterRigidBody());
    registry.Assign<ecs::ThrusterSet>(m_playerShip, gameplay::MakeFighterThrusterSet());
    registry.Assign<ecs::FlightControl>(m_playerShip, ecs::FlightControl{});
    registry.Assign<ecs::SpatialFrame>(
        m_playerShip, ecs::SpatialFrame{ m_scene.ActiveFrame() });
    ecs::GravitationalBody playerGravity;
    playerGravity.bodyId = kPlayerShipBodyId;
    playerGravity.radius = 2.2;
    playerGravity.isSource = false;
    playerGravity.owner = ecs::OrbitOwner::ForceIntegrated;
    registry.Assign<ecs::GravitationalBody>(m_playerShip, playerGravity);
    m_smokeTextureEntity = playerAssembly->ModuleEntities().front();

    if (m_options.smoke)
    {
        // The cascade and texture-mutation oracles need their legacy central
        // shape to remain stable across renderer work. It is deliberately a
        // smoke-only probe with no gameplay, physics, or assembly identity.
        ecs::Material smokeProbeMaterial = m_smokeGrowthMaterial;
        smokeProbeMaterial.albedo = { 0.18f, 0.32f, 0.58f, 1.0f };
        smokeProbeMaterial.emissiveStrength = 0.45f;
        m_smokeTextureEntity = m_scene.CreateRenderable(
            "SmokeShipProbe",
            cube,
            smokeProbeMaterial,
            ecs::Transform{
                { 0, 1.2, -2.0 },
                core::Quatf::Identity(),
                { 3.0f, 0.9f, 4.4f }
            });
        if (m_smokeTextureEntity.IsNull())
            return false;
        m_smokeCameraTarget = IsReferenceRuntimeContent()
            ? m_smokeTextureEntity
            : m_playerShip;
        core::Log::Info(
            "[SMOKE] smoke_camera_probe=isolated gameplay_identity=assembly_root");
    }

    const scene::AssemblyInteriorResult presentationReady =
        m_runtimeAssembly.SynchronizePresentation(m_scene);
    if (!presentationReady.Succeeded())
    {
        core::Log::Errorf(
            "Playable assembly presentation failed [%s]: %s",
            scene::AssemblyInteriorStatusName(presentationReady.status),
            presentationReady.error.c_str());
        return false;
    }
    core::Log::Infof(
        "[SMOKE] assembly_root_presentation=ok root=%u player_ship=same modules=%zu moving_parts=%zu root_mesh=absent",
        m_playerShip.id,
        playerAssembly->ModuleEntities().size(),
        playerAssembly->MovingPartEntities().size());
    const ecs::Transform* initialRoot =
        registry.TryGet<ecs::Transform>(m_playerShip);
    const ecs::Transform* initialModule = registry.TryGet<ecs::Transform>(
        playerAssembly->ModuleEntities().front());
    if (!initialRoot || !initialModule)
        return false;
    m_smokeAssemblyInitialRootPosition = initialRoot->position;
    m_smokeAssemblyInitialModulePosition = initialModule->position;
    m_smokeAssemblyBaselineReady = true;
    core::Log::Info(
        "[SMOKE] assembly_children=coherent frame=assembly_local");

    // Exhaust creation adds more Transform components and can reallocate that
    // pool, so keep value copies while the visual entities are being spawned.
    const ecs::Transform shipTransform = registry.Get<ecs::Transform>(m_playerShip);
    const ecs::ThrusterSet thrusters = registry.Get<ecs::ThrusterSet>(m_playerShip);
    m_thrusterVisualMesh = cube;
    m_thrusterVisualCount = thrusters.count < ecs::ThrusterSet::kMaxThrusters
        ? thrusters.count
        : ecs::ThrusterSet::kMaxThrusters;
    const ecs::Material exhaustMaterial{
        { 0.08f, 0.35f, 0.95f, 1.0f }, 0.18f, 0.0f,
        UINT32_MAX, UINT32_MAX, UINT32_MAX,
        { 0.12f, 0.55f, 1.0f, 1.0f }, 0.0f, UINT32_MAX
    };
    for (uint32_t i = 0; i < m_thrusterVisualCount; ++i)
    {
        char exhaustName[32] = {};
        std::snprintf(exhaustName, sizeof(exhaustName), "PlayerExhaust_%02u", i);
        const gameplay::ThrusterVisualState visual =
            gameplay::BuildThrusterVisualState(shipTransform, thrusters.thrusters[i]);
        m_thrusterVisuals[i] = m_scene.CreateRenderable(
            exhaustName, cube, exhaustMaterial, visual.transform);
        if (m_thrusterVisuals[i].IsNull())
            return false;
        // Inactive exhaust is not a draw at all. Keeping an invisible
        // MeshInstance here would inflate Scene::MeshInstanceCount and force
        // renderer buffers to grow for entities neither pass can consume.
        registry.Remove<ecs::MeshInstance>(m_thrusterVisuals[i]);
    }

    core::Log::Infof("Playable assembly ship ready: entity=%u thrusters=%u mode=COUPLED",
                     m_playerShip.id, m_thrusterVisualCount);
    if (!InitializePlayerPossession())
        return false;
    if (m_options.smoke && !ValidateSmokePossessionRoundTrip())
        return false;

    m_scene.CreateRenderable(
        "RedSphere", sphere,
        ecs::Material{ { 0.8f, 0.15f, 0.1f, 1.0f }, 0.4f, 0.1f },
        ecs::Transform{ { -2.5f, 0.75f, 0 }, core::Quatf::Identity(),
                        { 1.5f, 1.5f, 1.5f } });

    m_scene.CreateRenderable(
        "GoldSphere", sphere,
        ecs::Material{ { 0.9f, 0.7f, 0.2f, 1.0f }, 0.25f, 1.0f },
        ecs::Transform{ { 2.5f, 0.75f, 0 }, core::Quatf::Identity(),
                        { 1.5f, 1.5f, 1.5f } });

    // Pillars marching away from the camera, deliberately spanning the shadow
    // range rather than clustering inside it. Under a SINGLE cascade the ones
    // past ~24 units receive no shadow at all, which is the artifact cascades
    // exist to fix - and which is now visible in a capture rather than being a
    // claim in a comment.
    //
    // The DXR path traces shadow rays and is unaffected by cascade extent, so it
    // shadows all of these correctly. That makes path tracing the reference the
    // raster cascades are checked against.
    static constexpr float kPillarDistances[] = {
        12.0f, 20.0f, 30.0f, 45.0f, 65.0f, 90.0f
    };
    for (int i = 0; i < static_cast<int>(std::size(kPillarDistances)); ++i)
    {
        char pillarName[32] = {};
        std::snprintf(pillarName, sizeof(pillarName), "Pillar_%d", i);
        const float z = kPillarDistances[i];
        // Scale with distance so each stays a similar size on screen, keeping
        // all of them legible in one capture.
        const float scale = 1.0f + z * 0.06f;

        m_scene.CreateRenderable(
            pillarName, cube,
            ecs::Material{ { 0.75f, 0.72f, 0.68f, 1.0f }, 0.65f, 0.0f },
            ecs::Transform{ { (i % 2 == 0) ? -3.5f : 3.5f, scale * 0.5f, z },
                            core::Quatf::Identity(),
                            { scale * 0.5f, scale, scale * 0.5f } });
    }

    for (int i = -3; i <= 3; ++i)
    {
        char name[32] = {};
        std::snprintf(name, sizeof(name), "SmallCube_%d", i + 3);
        const float x = static_cast<float>(i) * 1.5f;

        m_scene.CreateSpinner(
            name, cube,
            ecs::Material{ { 0.9f, 0.9f, 0.9f, 1.0f }, 0.7f, 0.0f },
            ecs::Transform{ { x, 0.15f, 3.0f }, core::Quatf::Identity(),
                            { 0.3f, 0.3f, 0.3f } },
            2.0f + static_cast<float>(i) * 0.3f);
    }

    // Seed the true-scale reference solar system into the live game (Sun + two
    // planets + a moon, real mu / real AU) with sphere-of-influence transitions
    // running. The bodies sit ~1 AU out, so they are invisible in the default
    // meters-scale chase camera until a solar-system camera mode frames them; the
    // sim, however, is now driving a real world every fixed step. Gated out of the
    // smoke run so the smoke's entity/snapshot baseline is unchanged.
    if (!m_options.smoke || m_options.starSystem)
    {
        const uint32_t bodies = m_scene.SeedStarSystem(sphere, 0.5f);
        core::Log::Infof("Solar system seeded: %u bodies", bodies);
        m_cameraMode = (m_options.startCameraMode == 4)
            ? CameraMode::ShipChase // ship-in-system uses the chase camera
            : (m_options.startCameraMode == 5)
            ? CameraMode::Surface   // 5 is overloaded (4 is ship-in-system → chase)
            : static_cast<CameraMode>(m_options.startCameraMode);
        if (m_options.startCameraMode == 2) // near-body: focus the requested body (default Earth)
            m_focusBodyId = scene::kStarSystemBodyIdBase +
                            (m_options.focusLocalId ? m_options.focusLocalId : 10);
        if (m_cameraMode == CameraMode::Surface) // terrain preview (default Moon)
        {
            const uint64_t sfLocal = m_options.focusLocalId ? m_options.focusLocalId : 11;
            m_focusBodyId   = scene::kStarSystemBodyIdBase + sfLocal;
            m_terrainBodyId = m_focusBodyId;
            const TerrainBodyCfg cfg = ConfigForTerrainBody(sfLocal);

            // Frame a LIT LAND point with relief (Earth is ~70% ocean, so a fixed
            // point is usually dark flat water). Search the sphere with the CPU height
            // twin for the best-scoring direction: land elevation weighted by how
            // sunward it faces (the star sits at the frame origin).
            core::Vec3d bodyPos;
            auto& reg = m_scene.GetRegistry();
            if (auto* pool = reg.GetPool<ecs::GravitationalBody>())
                for (uint32_t i = 0; i < pool->Count(); ++i)
                    if (pool->DataAt(i).bodyId == m_terrainBodyId)
                    {
                        const ecs::Entity e = reg.EntityAtIndex(pool->EntityAt(i));
                        if (const auto* t = reg.TryGet<ecs::Transform>(e)) bodyPos = t->position;
                        break;
                    }
            const core::Vec3d sunDir = (core::Vec3d{ 0.0, 0.0, 0.0 } - bodyPos).Normalized();
            core::Vec3d best{ 0.0, 0.0, 1.0 };
            double bestScore = -1.0;
            const int N = 700;
            const double golden = 2.399963229728653; // pi*(3-sqrt(5))
            for (int i = 0; i < N; ++i)
            {
                const double y   = 1.0 - (i + 0.5) * 2.0 / N;
                const double rad = std::sqrt((std::max)(0.0, 1.0 - y * y));
                const double phi = i * golden;
                const core::Vec3d d{ std::cos(phi) * rad, y, std::sin(phi) * rad };
                const float elev = core::PlanetHeight(
                    core::Vec3f{ static_cast<float>(d.x), static_cast<float>(d.y),
                                 static_cast<float>(d.z) },
                    cfg.type, cfg.seed, cfg.seaLevel, cfg.coastWidth);
                const double lit = d.x * sunDir.x + d.y * sunDir.y + d.z * sunDir.z;
                const double score = static_cast<double>(elev) * ((lit > 0.25) ? lit : 0.0);
                if (score > bestScore) { bestScore = score; best = d; }
            }
            m_terrainFocusBody = { best.x * cfg.radius, best.y * cfg.radius, best.z * cfg.radius };
            m_terrainCamBody   = { best.x * (cfg.radius + 200000.0),
                                   best.y * (cfg.radius + 200000.0),
                                   best.z * (cfg.radius + 200000.0) };
            m_terrainBuilt = true;
        }

        // Teleport the playable ship onto a live orbit about Earth for EVERY
        // star-system run (any camera mode), so it shares the system's frame, feels
        // multi-body gravity (SOI live), and can be framed by the near-body map view
        // as well as flown in the chase camera. The ship is a small object 1 AU out,
        // so it costs the orrery / distant views nothing visible.
        {
            auto& reg = m_scene.GetRegistry();
            core::Vec3d earthPos, earthVel; double earthRadius = 0.0, earthMu = 0.0;
            bool found = false;
            if (auto* pool = reg.GetPool<ecs::GravitationalBody>())
                for (uint32_t i = 0; i < pool->Count(); ++i)
                    if (pool->DataAt(i).bodyId == scene::kStarSystemBodyIdBase + 10)
                    {
                        const ecs::Entity e = reg.EntityAtIndex(pool->EntityAt(i));
                        earthPos = reg.Get<ecs::Transform>(e).position;
                        earthVel = reg.Get<ecs::RigidBody>(e).linearVelocity;
                        earthRadius = pool->DataAt(i).radius;
                        earthMu = pool->DataAt(i).mu;
                        found = true;
                        break;
                    }
            if (found && reg.IsAlive(m_playerShip))
            {
                ecs::Transform& t = reg.Get<ecs::Transform>(m_playerShip);
                // In front of Earth (−Z) and slightly up, so the ship's default
                // +Z forward — and the chase camera behind it — look toward Earth,
                // which then fills the background beyond the ship.
                const core::Vec3d offset{ 0.0, earthRadius * 0.35, -earthRadius * 4.0 };
                t.position = earthPos + offset;
                if (auto* rb = reg.TryGet<ecs::RigidBody>(m_playerShip))
                {
                    // Put the ship on a CIRCULAR orbit about Earth rather than
                    // co-moving with it: v = Earth's velocity + the local circular
                    // speed sqrt(mu/r) perpendicular to the radius, so it coasts on a
                    // closed orbit the player can then reshape with the engines. A
                    // co-orbital ship (v == earthVel) has zero velocity relative to
                    // Earth and simply falls straight in.
                    const double r = offset.Length();
                    core::Vec3d perp = offset.Cross(core::Vec3d{ 0.0, 1.0, 0.0 });
                    if (perp.Length() < 1e-6) // radius nearly parallel to world up
                        perp = offset.Cross(core::Vec3d{ 1.0, 0.0, 0.0 });
                    const double vCirc = (earthMu > 0.0 && r > 0.0)
                        ? std::sqrt(earthMu / r) : 0.0;
                    rb->linearVelocity = earthVel + perp.Normalized() * vCirc;
                    rb->prevPosition = t.position;
                    // Newtonian flight: with the assist OFF, zero stick means zero
                    // force, so the ship coasts on its orbit. Coupled assist brakes
                    // world velocity toward zero and would fight the orbit.
                    if (auto* fc = reg.TryGet<ecs::FlightControl>(m_playerShip))
                        fc->mode = ecs::FlightMode::Decoupled;
                    m_shipInSystem = true; // the ship now has a real orbit to surface

                    core::Log::Infof(
                        "Ship spawned on a circular orbit about Earth: alt %.0f km, "
                        "v_circular %.1f m/s (Newtonian flight)",
                        (r - earthRadius) / 1000.0, vCirc);
                }
            }
        }
    }

    core::Log::Infof("Scene populated: %u entities", m_scene.EntityCount());
    return true;
}

bool App::IsReferenceRuntimeContent() const
{
    return m_runtimeAssembly.Manifest().sceneId == "ship.reference.runtime";
}

bool App::ValidateSmokeInteriorRuntime()
{
    const std::string& sceneId = m_runtimeAssembly.Manifest().sceneId;
    if (sceneId == "ship.reference.runtime")
        return ValidateReferenceSmokeInteriorRuntime();
    if (sceneId == "ship.frontier.courier_mk1.runtime")
        return ValidateProductionSmokeInteriorRuntime();

    core::Log::Errorf(
        "No smoke interior profile is registered for scene: %s",
        sceneId.c_str());
    return false;
}

bool App::ValidateReferenceSmokeInteriorRuntime()
{
    const auto closedCollision =
        m_runtimeAssembly.InteractiveCollisionSnapshot();
    if (!closedCollision || !closedCollision->collisionWorld)
    {
        core::Log::Error("On-foot smoke has no closed collision snapshot");
        return false;
    }
    gameplay::OnFootState closedWalk;
    closedWalk.capsule.center = { 0.0, 0.32, -2.0 };
    gameplay::OnFootCommand walkForward;
    walkForward.moveForward = 1.0;
    bool closedBlocked = false;
    for (uint32_t step = 0; step < 120; ++step)
    {
        const gameplay::OnFootStepResult walked =
            gameplay::StepOnFootController(
                *closedCollision,
                closedWalk,
                walkForward,
                kSmokeFixedDeltaSeconds);
        if (!walked.Succeeded())
        {
            core::Log::Errorf(
                "Closed on-foot smoke step failed [%s/%s]",
                gameplay::OnFootControllerStatusName(walked.status),
                scene::InteriorCollisionStatusName(walked.collisionStatus));
            return false;
        }
        closedBlocked = closedBlocked || walked.blocked;
        closedWalk = walked.state;
    }
    if (!closedBlocked || closedWalk.capsule.center.z >= -1.35)
    {
        core::Log::Errorf(
            "Closed inner door did not block controller (z=%.6f)",
            closedWalk.capsule.center.z);
        return false;
    }

    const scene::AssemblyInteriorResult activated =
        m_runtimeAssembly.ActivateInteraction(m_scene, "outer_hatch");
    const scene::AssemblyInteriorResult advanced = activated.Succeeded()
        ? m_runtimeAssembly.AdvanceInterior(m_scene, 2.0)
        : activated;
    const scene::AssemblyInteriorRuntime& interior =
        m_runtimeAssembly.Interior();
    const uint32_t portalIndex = interior.InteractionPortalIndex(
        activated.stableIndex);
    if (!activated.Succeeded() || !advanced.Succeeded() ||
        interior.InteractionStateName(activated.stableIndex) != "open" ||
        portalIndex == asset::kAssemblyNoIndex ||
        !interior.IsPortalTraversable(portalIndex))
    {
        core::Log::Errorf(
            "Interior smoke interaction failed [%s]: %s",
            scene::AssemblyInteriorStatusName(advanced.status),
            advanced.error.c_str());
        return false;
    }
    core::Log::Info(
        "[SMOKE] interior_interaction=ok interaction=outer_hatch state=open portal=outer_entry traversable=yes");

    const scene::AssemblyInteriorResult innerActivated =
        m_runtimeAssembly.ActivateInteraction(m_scene, "inner_door");
    const scene::AssemblyInteriorResult innerAdvanced =
        innerActivated.Succeeded()
            ? m_runtimeAssembly.AdvanceInterior(m_scene, 2.0)
            : innerActivated;
    const uint32_t innerPortal = interior.InteractionPortalIndex(
        innerActivated.stableIndex);
    const auto openCollision =
        m_runtimeAssembly.InteractiveCollisionSnapshot();
    if (!innerActivated.Succeeded() || !innerAdvanced.Succeeded() ||
        interior.InteractionStateName(innerActivated.stableIndex) != "open" ||
        innerPortal == asset::kAssemblyNoIndex ||
        !interior.IsPortalTraversable(innerPortal) || !openCollision ||
        !openCollision->collisionWorld)
    {
        core::Log::Errorf(
            "Open on-foot smoke setup failed [%s]: %s",
            scene::AssemblyInteriorStatusName(innerAdvanced.status),
            innerAdvanced.error.c_str());
        return false;
    }

    gameplay::OnFootState openWalk;
    openWalk.capsule.center = { 0.0, 0.32, -2.0 };
    for (uint32_t step = 0; step < 120; ++step)
    {
        const gameplay::OnFootStepResult walked =
            gameplay::StepOnFootController(
                *openCollision,
                openWalk,
                walkForward,
                kSmokeFixedDeltaSeconds);
        if (!walked.Succeeded())
        {
            core::Log::Errorf(
                "Open on-foot smoke step failed [%s/%s]",
                gameplay::OnFootControllerStatusName(walked.status),
                scene::InteriorCollisionStatusName(walked.collisionStatus));
            return false;
        }
        openWalk = walked.state;
    }
    if (openWalk.capsule.center.z <= -0.5 ||
        openCollision->dynamicBoxes.size() != 2)
    {
        core::Log::Errorf(
            "Open inner door did not admit controller (z=%.6f blockers=%zu)",
            openWalk.capsule.center.z,
            openCollision->dynamicBoxes.size());
        return false;
    }
    core::Log::Info(
        "[SMOKE] on_foot_controller=ok closed=blocked open=traversable blockers=2");
    return true;
}

bool App::ValidateProductionSmokeInteriorRuntime()
{
    const scene::AssemblyInstance* instance = m_runtimeAssembly.Instance();
    if (!instance || !instance->IsAlive() || !instance->Plan() ||
        !instance->Plan()->Resources() ||
        !instance->Plan()->Resources()->assembly)
    {
        core::Log::Error("Production interior smoke has no live assembly");
        return false;
    }
    const asset::CookedAssembly& assembly =
        *instance->Plan()->Resources()->assembly;
    const auto initialCollision =
        m_runtimeAssembly.InteractiveCollisionSnapshot();
    if (!initialCollision || !initialCollision->collisionWorld ||
        initialCollision->dynamicBoxes.empty())
    {
        core::Log::Error(
            "Production interior smoke has no closed dynamic blockers");
        return false;
    }

    const scene::AssemblyInteriorRuntime& interior =
        m_runtimeAssembly.Interior();
    size_t closureCount = 0;
    for (uint32_t interactionIndex = 0;
         interactionIndex < assembly.interactions.size();
         ++interactionIndex)
    {
        const asset::AssemblyInteraction& interaction =
            assembly.interactions[interactionIndex];
        if (interaction.portalIndex == asset::kAssemblyNoIndex)
            continue;
        if (interaction.portalIndex >= assembly.portals.size() ||
            interaction.movingPartIndex == asset::kAssemblyNoIndex ||
            interior.InteractionPortalIndex(interactionIndex) !=
                interaction.portalIndex ||
            interior.InteractionStateName(interactionIndex) != "closed" ||
            interior.IsPortalTraversable(interaction.portalIndex))
        {
            core::Log::Errorf(
                "Production closure precondition failed: %s",
                interaction.id.c_str());
            return false;
        }

        const scene::AssemblyInteriorResult activated =
            m_runtimeAssembly.ActivateInteraction(m_scene, interactionIndex);
        const scene::AssemblyInteriorResult advanced = activated.Succeeded()
            ? m_runtimeAssembly.AdvanceInterior(m_scene, 2.0)
            : activated;
        if (!activated.Succeeded() || activated.stableIndex != interactionIndex ||
            !advanced.Succeeded() ||
            interior.InteractionStateName(interactionIndex) != "open" ||
            !interior.IsPortalTraversable(interaction.portalIndex))
        {
            core::Log::Errorf(
                "Production closure activation failed [%s]: %s",
                scene::AssemblyInteriorStatusName(advanced.status),
                interaction.id.c_str());
            return false;
        }
        ++closureCount;
    }

    const auto openCollision =
        m_runtimeAssembly.InteractiveCollisionSnapshot();
    const size_t expectedClosedBoxes = closureCount * 2u;
    const size_t expectedOpenBoxes = closureCount;
    if (closureCount != assembly.portals.size() || !openCollision ||
        !openCollision->collisionWorld ||
        initialCollision->dynamicBoxes.size() != expectedClosedBoxes ||
        openCollision->dynamicBoxes.size() != expectedOpenBoxes ||
        openCollision->revision <= initialCollision->revision)
    {
        core::Log::Errorf(
            "Production interior did not open every portal (closures=%zu portals=%zu blockers=%zu)",
            closureCount,
            assembly.portals.size(),
            openCollision ? openCollision->dynamicBoxes.size() : 0u);
        return false;
    }

    core::Log::Infof(
        "[SMOKE] production_interior=ok closures=%zu portals=%zu initial_blockers=%zu final_blockers=%zu",
        closureCount,
        assembly.portals.size(),
        initialCollision->dynamicBoxes.size(),
        openCollision->dynamicBoxes.size());
    return true;
}

bool App::InitializePlayerPossession()
{
    const scene::AssemblyInstance* instance = m_runtimeAssembly.Instance();
    if (!instance || !instance->IsAlive() || !instance->Plan() ||
        !instance->Plan()->Resources() ||
        !instance->Plan()->Resources()->assembly)
    {
        core::Log::Error("Player possession has no live assembly topology");
        return false;
    }

    const gameplay::PilotSeatBindingResult resolved =
        gameplay::ResolvePilotSeatBinding(
            *instance->Plan()->Resources()->assembly);
    if (!resolved.Succeeded())
    {
        core::Log::Errorf(
            "Pilot-seat binding failed [%s]: %s",
            gameplay::PilotPossessionStatusName(resolved.status),
            resolved.error.c_str());
        return false;
    }
    m_pilotSeat = resolved.binding;

    const scene::AssemblyInteriorRuntime& interior =
        m_runtimeAssembly.Interior();
    if (interior.InteractionStateIndex(m_pilotSeat.interactionIndex) !=
        m_pilotSeat.availableStateIndex)
    {
        core::Log::Error("Pilot seat did not begin in its authored available state");
        return false;
    }

    const gameplay::PilotPossessionResult initialized =
        gameplay::InitializeShipPossession(
            m_pilotSeat,
            m_pilotSeat.occupiedStateIndex,
            m_possessionConfig);
    if (!initialized.Succeeded())
    {
        core::Log::Errorf(
            "Ship possession staging failed [%s]: %s",
            gameplay::PilotPossessionStatusName(initialized.status),
            initialized.error.c_str());
        return false;
    }

    const scene::AssemblyInteriorResult occupied =
        m_runtimeAssembly.ActivateInteraction(
            m_scene, m_pilotSeat.interactionIndex);
    if (!occupied.Succeeded() ||
        interior.InteractionStateIndex(m_pilotSeat.interactionIndex) !=
            m_pilotSeat.occupiedStateIndex)
    {
        core::Log::Errorf(
            "Pilot seat could not enter occupied state [%s]: %s",
            scene::AssemblyInteriorStatusName(occupied.status),
            occupied.error.c_str());
        return false;
    }
    m_playerPossession = initialized.state;
    m_possessionReady = true;
    core::Log::Info(
        "[SMOKE] pilot_possession_ready=ok context=ship seat=occupied spawn=pilot_exit_spawn");
    return true;
}

bool App::ValidateSmokePossessionRoundTrip()
{
    if (!m_possessionReady || !gameplay::OwnsShipInput(m_playerPossession))
        return false;

    const scene::AssemblyInteriorRuntime& interior =
        m_runtimeAssembly.Interior();
    const gameplay::PilotPossessionStatus exit = TryExitPilotSeat();
    if (exit != gameplay::PilotPossessionStatus::Success ||
        !gameplay::OwnsOnFootInput(m_playerPossession) ||
        interior.InteractionStateIndex(m_pilotSeat.interactionIndex) !=
            m_pilotSeat.availableStateIndex)
    {
        core::Log::Errorf(
            "Smoke pilot exit failed [%s]",
            gameplay::PilotPossessionStatusName(exit));
        return false;
    }

    const core::Vec3d localBeforeStep =
        m_playerPossession.onFoot.capsule.center;
    m_onFootCommand = {};
    m_onFootCommand.moveForward = 1.0;
    if (!UpdateOnFootSimulation(kSmokeFixedDeltaSeconds))
    {
        core::Log::Error("Smoke on-foot possession did not advance a fixed step");
        return false;
    }
    const core::Vec3d localStep =
        m_playerPossession.onFoot.capsule.center - localBeforeStep;
    if (localStep.Dot(core::Vec3d::FromFloat(m_pilotSeat.spawn.forward)) <=
        1.0e-8)
    {
        core::Log::Error(
            "Smoke on-foot possession did not advance along the spawn heading");
        return false;
    }
    m_onFootCommand = {};

    ecs::Transform root;
    const gameplay::OnFootCameraResult camera = BuildPlayerShipRoot(root)
        ? gameplay::BuildOnFootCameraPose(
              m_playerPossession, root, m_possessionConfig)
        : gameplay::OnFootCameraResult{};
    if (!camera.Succeeded() ||
        !m_camera.InitBasis(
            camera.pose.position, camera.pose.forward, camera.pose.up))
    {
        core::Log::Errorf(
            "Smoke on-foot camera root composition failed [%s]: %s",
            gameplay::PilotPossessionStatusName(camera.status),
            camera.error.c_str());
        return false;
    }

    const gameplay::PilotPossessionStatus entry = TryEnterPilotSeat();
    if (entry != gameplay::PilotPossessionStatus::Success ||
        !gameplay::OwnsShipInput(m_playerPossession) ||
        interior.InteractionStateIndex(m_pilotSeat.interactionIndex) !=
            m_pilotSeat.occupiedStateIndex)
    {
        core::Log::Errorf(
            "Smoke pilot re-entry failed [%s]",
            gameplay::PilotPossessionStatusName(entry));
        return false;
    }
    core::Log::Info(
        "[SMOKE] pilot_possession=ok exit=on_foot step=advanced reentry=ship seat=occupied root=composed");
    return true;
}

void App::InitializePathTracingState()
{
    m_usePathTracing = false;
    m_rtAvailable = false;
    m_rtInitAttempted = false;
    m_rtQualityMode = m_options.smokeFullQuality
        ? render::RTQualityMode::FullPathTrace
        : render::RTQualityMode::StablePreview;
    m_rtQualityInfo = render::GetRTQualityInfo(m_rtQualityMode);

    core::Log::Info("Path tracing will initialize on first F1 press");
    core::Log::Infof("Initial RT quality mode: %s (%u spp, %u bounce%s)",
                     m_rtQualityInfo.name,
                     m_rtQualityInfo.samplesPerPixel,
                     m_rtQualityInfo.maxBounces,
                     m_rtQualityInfo.maxBounces == 1 ? "" : "s");
    core::Log::Info("Flight controls: WASD/arrows translate, Space/Ctrl lift, captured mouse steers, IJKL attitude, Q/E roll, V coupled/decoupled");
    core::Log::Info("Interaction controls: F uses the nearest authored control in view");
    core::Log::Info("Render controls: F1 toggles raster/path tracing, F2 toggles RT quality, F3 toggles overlay");
}

bool App::EnsurePathTracing()
{
    if (m_rtInitAttempted)
        return m_rtAvailable;

    m_rtInitAttempted = true;
    if (!m_scene.InitPathTracer(m_device))
    {
        core::Log::Warn("Path tracing not available - DXR initialization failed");
        return false;
    }

    if (!m_device.WaitForCurrentFrame() || !m_device.ResetCommandList())
    {
        core::Log::Error("Unable to begin BLAS initialization command list");
        return false;
    }
    m_scene.EnsureBLAS(m_device);

    const HRESULT closeHr = m_device.CmdList()->Close();
    if (FAILED(closeHr))
    {
        core::Log::Errorf("BLAS command list Close failed: 0x%08X", closeHr);
        return false;
    }
    ID3D12CommandList* blasLists[] = { m_device.CmdList() };
    m_device.CmdQueue()->ExecuteCommandLists(1, blasLists);
    if (!m_device.WaitForGpu())
        return false;

    m_rtAvailable = true;
    core::Log::Info("Path tracing initialized");
    return true;
}

void App::TogglePathTracing()
{
    if (!EnsurePathTracing())
        return;

    m_usePathTracing = !m_usePathTracing;
    core::Log::Infof("Render mode: %s",
                     m_usePathTracing ? "PATH TRACING" : "RASTERIZATION");
}

int App::RunMainLoop()
{
    core::Log::Info("=== Entering main loop (WASD/arrows + mouse, click to capture, ESC to release) ===");
    m_running = true;
    if (m_options.smoke)
    {
        LARGE_INTEGER frequency = {};
        LARGE_INTEGER start = {};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        m_smokeCounterFrequency = frequency.QuadPart;
        m_smokeStartCounter = start.QuadPart;
    }
    m_smokeRTStarted = false;
    m_captureThisFrame = false;
    m_frameCount = 0;
    m_smokeMaxOutstandingSubmissions = 0;
    m_exitCode = 0;
    m_titleTimer = 0.0f;

    const uint64_t smokeRTStartFrame = SmokeFrameForTime(m_options.smokeRTDelaySeconds);
    const uint64_t smokeEndFrame = SmokeFrameForTime(m_options.smokeSeconds);

    // THE RASTER VERIFICATION FRAME, and it is deliberately NOT the final frame.
    //
    // TWO probes ride it: the draw-record probe and the shadow-map probe. Both
    // need a RASTER frame. The draw probe is written by basic_vs, shadow_vs and
    // basic_ps, and a path-traced frame runs none of the three. The shadow probe
    // reads the cascaded shadow map, and a path-traced frame does not render the
    // shadow pass at all - RenderFrame's `if (renderedPathTracing)` branch skips
    // BeginShadowPass outright, so there is no fresh depth to read and
    // ShadowCascadesRendered() reports the count for a pass that never began.
    //
    // Both used to ride along with the capture on smokeEndFrame. Under
    // -RasterOnly that is a raster frame and both ran; in the DEFAULT smoke mode
    // the run ends in path tracing, so probeDrawRecords and probeShadow were both
    // false on the single frame that ever asked for them and the default run
    // carried NO probe coverage at all. That is the same class of gap as an
    // assertion sitting behind a flag nobody passes: the check exists, reads as
    // coverage, and is never reached. The draw probe was fixed first; the shadow
    // and cascade assertions were left behind on the same broken schedule, with
    // the harness's `if ($RasterOnly)` gate documenting the hole rather than
    // closing it.
    //
    // So VERIFICATION runs on the LAST RASTER FRAME of whichever mode is active -
    // smokeEndFrame under -RasterOnly, and the frame before path tracing takes
    // over otherwise. With the default 0.25 s delay that is frame 14, which is
    // AFTER ApplySmokeGrowthStress's frame-8 churn: the draw probe therefore
    // reads an object buffer that has already been grown and deferred-released
    // mid-run, not the pristine first allocation.
    //
    // CAPTURE stays on smokeEndFrame. The frame an image is taken on and the
    // frame correctness is verified on are separate concerns - the capture
    // asserts the default mode's final path-traced image, which is exactly what
    // it should assert, and decoupling is what lets verification move to a frame
    // where the raster pipeline actually ran.
    //
    // The smokeRTStartFrame > 1 guard covers --smoke-rt-delay=0, where no raster
    // frame exists at all. That configuration cannot be probed; it falls back to
    // the end frame, where both probe flags stay false and the harness's
    // draw_probe_frame / shadow_probe_frame markers are absent rather than
    // silently wrong.
    const uint64_t smokeRasterVerifyFrame =
        (m_options.smokeRT && smokeRTStartFrame > 1) ? smokeRTStartFrame - 1
                                                     : smokeEndFrame;
    m_smokeRasterVerifyRequested = false;
    if (m_options.smoke)
    {
        core::Log::Infof("[SMOKE] timeline=fixed fixed_hz=60 target_frames=%llu",
                         static_cast<unsigned long long>(smokeEndFrame));
    }

    while (m_running)
    {
        ++m_frameCount;

        core::input::BeginFrame();
        if (!m_window.ProcessMessages())
        {
            m_running = false;
            break;
        }

        const auto& input = core::input::GetState();
        if (input.KeyPressed(VK_ESCAPE))
        {
            if (m_window.IsCaptured())
                m_window.ReleaseMouse();
            else
                m_running = false;
        }

        if (input.mouse.buttonPressed[0] && !m_window.IsCaptured())
            m_window.CaptureMouse();

        if (input.KeyPressed(VK_F2))
        {
            m_rtQualityMode = (m_rtQualityMode == render::RTQualityMode::StablePreview)
                ? render::RTQualityMode::FullPathTrace
                : render::RTQualityMode::StablePreview;
            m_rtQualityInfo = render::GetRTQualityInfo(m_rtQualityMode);
            core::Log::Infof("RT quality mode: %s (%u spp, %u bounce%s)",
                             m_rtQualityInfo.name,
                             m_rtQualityInfo.samplesPerPixel,
                             m_rtQualityInfo.maxBounces,
                             m_rtQualityInfo.maxBounces == 1 ? "" : "s");
        }

        if (input.KeyPressed(VK_F3))
        {
            m_showDebugOverlay = !m_showDebugOverlay;
            core::Log::Infof("Debug overlay: %s", m_showDebugOverlay ? "ON" : "OFF");
        }

        if (input.KeyPressed(VK_F1))
            TogglePathTracing();

        // F4 cycles the solar-system camera mode. Reset the chase smoother so it
        // does not drag the camera across a mode jump.
        if (input.KeyPressed(VK_F4))
        {
            static const char* kCameraModeNames[] =
                { "ShipChase", "Orrery", "NearBody", "Free", "Surface" };
            // F4 cycles only the four free-look modes. Surface is excluded: it is a
            // start-mode preview whose framing / lit-land search / terrain streaming
            // is set up once at seed time, so cycling into it would show no terrain.
            m_cameraMode = static_cast<CameraMode>(
                (static_cast<uint32_t>(m_cameraMode) + 1u) %
                (static_cast<uint32_t>(CameraMode::Count) - 1u));
            m_chaseCameraInitialized = false;
            // Restart the surface descent clock on any exit from Surface, so a later
            // re-entry plays the altitude ramp from the top rather than snapping.
            if (m_cameraMode != CameraMode::Surface) m_surfaceEntryTime = -1.0;
            const uint32_t nameIdx = static_cast<uint32_t>(m_cameraMode);
            core::Log::Infof("Camera mode: %s",
                             nameIdx < std::size(kCameraModeNames) ? kCameraModeNames[nameIdx] : "?");
        }

        // Time warp (KSP-style): ',' slower, '.' faster, '/' reset. Deterministic:
        // the fixed-step accumulator just drains more/fewer steps per frame, capped
        // by Timer::kMaxStepsPerTick, so physics stays exact at any warp.
        if (input.KeyPressed(VK_OEM_PERIOD))
            m_timer.SetTimeScale((std::min)(m_timer.GetTimeScale() * 2.0, 100000.0));
        if (input.KeyPressed(VK_OEM_COMMA))
            m_timer.SetTimeScale((std::max)(m_timer.GetTimeScale() * 0.5, 1.0));
        if (input.KeyPressed(VK_OEM_2))
            m_timer.SetTimeScale(1.0);

        // F5 cycles which seeded body the near-body view frames (Earth, Moon,
        // Mars, Sun — see kFocusCycle below). Harmless in other modes.
        if (input.KeyPressed(VK_F5))
        {
            static const uint64_t kFocusCycle[] = { 10, 11, 20, 1 }; // Earth, Moon, Mars, Sun
            uint32_t next = 0;
            const uint64_t current = m_focusBodyId
                ? (m_focusBodyId - scene::kStarSystemBodyIdBase) : 10;
            for (uint32_t i = 0; i < 4; ++i)
                if (kFocusCycle[i] == current) { next = (i + 1) % 4; break; }
            m_focusBodyId = scene::kStarSystemBodyIdBase + kFocusCycle[next];
            m_chaseCameraInitialized = false;
            core::Log::Infof("Near-body focus: bodyId %llu",
                             static_cast<unsigned long long>(m_focusBodyId));
        }

        // M toggles the maneuver-node preview: the orbit a prograde burn would give.
        if (input.KeyPressed('M'))
        {
            m_showManeuverPreview = !m_showManeuverPreview;
            core::Log::Infof("Maneuver preview: %s",
                             m_showManeuverPreview ? "on" : "off");
        }

        // T cycles the HUD target through the seeded bodies (Shift+T steps back);
        // when nothing is locked it acquires the nearest. G clears the lock.
        if (input.KeyPressed('T'))
        {
            const std::vector<gameplay::TargetCandidate> cands = GatherTargetCandidates();
            const bool back = input.KeyDown(VK_SHIFT);
            if (m_targetBodyId == 0 && !back)
                m_targetBodyId = gameplay::SelectNearest(cands, m_camera.Position());
            else
                m_targetBodyId = gameplay::CycleTarget(cands, m_targetBodyId, back ? -1 : 1);
            core::Log::Infof("Target: bodyId %llu",
                             static_cast<unsigned long long>(m_targetBodyId));
        }
        if (input.KeyPressed('G'))
        {
            m_targetBodyId = 0;
            core::Log::Info("Target cleared");
        }

        if (!m_options.smoke && input.KeyPressed('F') && !HandleUseAction())
        {
            m_exitCode = 12;
            m_running = false;
            break;
        }

        core::TimeStep timeStep = m_timer.Tick();
        bool advancedFlightStep = false;
        if (m_options.smoke)
        {
            timeStep.dt = kSmokeFixedDeltaSeconds;
            timeStep.totalTime = static_cast<double>(m_frameCount) * kSmokeFixedDeltaSeconds;
            timeStep.frameCount = m_frameCount;
            timeStep.fps = 60.0f;
        }

        if (!ApplySmokeResizeStep())
        {
            m_exitCode = 6;
            m_running = false;
            break;
        }
        if (!ApplySmokeDescriptorStress())
        {
            m_exitCode = 7;
            m_running = false;
            break;
        }
        if (!ApplySmokeRTMutationStress())
        {
            m_exitCode = 8;
            m_running = false;
            break;
        }
        // Deliberately NOT gated on --smoke-rt, unlike the mutation stress above.
        // This is what forces the per-draw structured buffers to REALLOCATE
        // mid-run with frames in flight, and that swap is a raster-path hazard,
        // not an RT one: it is Renderer::BeginFrame's root SRVs that address the
        // buffers being released. Leaving it inside the RT-only block meant a
        // -RasterOnly run performed only the frame-one grow, which has nothing
        // outstanding behind it and stays green even with the deferred-release
        // fence deleted outright. Verified: with DeferredRelease replaced by an
        // immediate Reset, the default run dies here and -RasterOnly passed.
        ApplySmokeGrowthStress();

        if (m_options.smoke)
        {
            if (m_options.smokeRT && !m_smokeRTStarted &&
                m_frameCount >= smokeRTStartFrame)
            {
                m_smokeRTStarted = true;
                if (EnsurePathTracing())
                {
                    if (!m_usePathTracing)
                    {
                        m_usePathTracing = true;
                        core::Log::Info("Smoke mode: path tracing enabled");
                    }
                }
                else
                {
                    core::Log::Error("Smoke mode failed: path tracing unavailable");
                    m_exitCode = 2;
                    m_running = false;
                    break; // exit now so RenderFrame can't overwrite exit code 2
                }
            }

            // Armed once, on the last raster frame - see smokeRasterVerifyFrame
            // above. Separate from the end-of-run block below precisely because
            // the two frames are not the same frame in the default mode.
            //
            // BOTH probes are armed here. They are logged under separate markers
            // because they verify different things and could legitimately be
            // scheduled apart later; the harness asserts each one's presence, so
            // a probe silently losing its arming site fails loudly.
            // The IBL consumption probe's NEGATIVE CONTROL, one frame AHEAD of
            // the verify frame - so it is a raster frame by the same argument
            // that makes the verify frame one, and it is the last frame before
            // it. Armed first because the ordering is what the pair means:
            // control, then live, with nothing between them that could change
            // the scene.
            //
            // The control renders one frame with the environment switched off. It
            // is not the capture frame and it is not the last frame, so nothing
            // observable outside the probe depends on it.
            if (!m_smokeIBLControlRequested &&
                smokeRasterVerifyFrame > 1 &&
                m_frameCount >= smokeRasterVerifyFrame - 1)
            {
                m_smokeIBLControlRequested = true;
                m_verifyIBLControlThisFrame = true;
                core::Log::Infof("[SMOKE] ibl_consume_control_frame=%llu",
                                 static_cast<unsigned long long>(m_frameCount));
            }

            if (!m_smokeRasterVerifyRequested && m_frameCount >= smokeRasterVerifyFrame)
            {
                m_smokeRasterVerifyRequested = true;
                m_verifyDrawRecordsThisFrame = true;
                m_verifyShadowThisFrame      = true;
                m_verifyIBLConsumeThisFrame  = true;
                core::Log::Infof("[SMOKE] draw_probe_frame=%llu",
                                 static_cast<unsigned long long>(m_frameCount));
                core::Log::Infof("[SMOKE] shadow_probe_frame=%llu",
                                 static_cast<unsigned long long>(m_frameCount));
                core::Log::Infof("[SMOKE] ibl_consume_frame=%llu",
                                 static_cast<unsigned long long>(m_frameCount));
            }

            // THE DXR IBL PROBE PAIR, on PATH-TRACED frames.
            //
            // Scheduled two and three frames after path tracing takes over rather
            // than at the end of the run, for one reason: the control frame
            // renders with the environment switched off, and putting it adjacent
            // to the CAPTURE frame would mean the captured image sat one frame
            // after a deliberately wrong one. It does not actually matter here -
            // the demo spins continuously, so the scene signature changes every
            // frame and rt_accumulation_frame is 0 at the end of every run, which
            // the harness already asserts - but relying on that would make this
            // schedule depend on a property of the demo scene rather than on the
            // probe's own requirements.
            //
            // smokeRTStartFrame + 1 is the first frame on which path tracing is
            // certainly running: the switch happens at smokeRTStartFrame, and
            // EnsurePathTracing can fail there and fall back.
            //
            // Armed only when --smoke-rt is on. Under -RasterOnly there is no
            // dispatch, so the flags stay false and the markers are ABSENT rather
            // than green - which is what the harness checks.
            // STABLE PREVIEW ONLY, and that is a real restriction rather than a
            // convenience. This probe observes the stable preview's environment
            // block; under --rt-quality=full that branch does not execute at all,
            // so arming it there would produce a dispatch that wrote nothing and
            // a verdict of "failed" for a feature that is correctly absent.
            // The harness asserts these markers are ABSENT in the full mode, so
            // the restriction is visible rather than silent.
            const bool rtStablePreview =
                m_rtQualityMode == render::RTQualityMode::StablePreview;

            if (m_options.smokeRT && rtStablePreview && !m_smokeRTIBLControlRequested &&
                m_smokeRTStarted && m_frameCount >= smokeRTStartFrame + 1)
            {
                m_smokeRTIBLControlRequested = true;
                m_verifyRTIBLControlThisFrame = true;
                core::Log::Infof("[SMOKE] rt_ibl_consume_control_frame=%llu",
                                 static_cast<unsigned long long>(m_frameCount));
            }
            else if (m_options.smokeRT && rtStablePreview && !m_smokeRTIBLConsumeRequested &&
                     m_smokeRTIBLControlRequested && m_smokeRTStarted &&
                     m_frameCount >= smokeRTStartFrame + 2)
            {
                m_smokeRTIBLConsumeRequested = true;
                m_verifyRTIBLConsumeThisFrame = true;
                core::Log::Infof("[SMOKE] rt_ibl_consume_frame=%llu",
                                 static_cast<unsigned long long>(m_frameCount));
            }

            if (m_frameCount >= smokeEndFrame)
            {
                // CAPTURE ONLY. m_verifyShadowThisFrame used to be raised here
                // too; it is armed on smokeRasterVerifyFrame instead, because in
                // the default mode this frame is path-traced and renders no
                // shadow pass, which made every shadow and cascade marker absent
                // from the run that actually executes by default.
                m_captureThisFrame = m_options.smokeCapture;
                core::Log::Infof("[SMOKE] mode=%s", m_options.smokeRT ? "rt" : "raster");
                core::Log::Infof("[SMOKE] cb_ring_peak=%u cb_ring_capacity=%u",
                                 m_renderer.ConstantRingPeakBytes(),
                                 m_renderer.ConstantRingCapacity());
                // The number of ring bytes ONE CBPerFrame upload costs, aligned
                // exactly the way UploadCB aligns it. The harness computes its
                // flat ring budget from this instead of mirroring 512 as a
                // literal, so growing CBPerFrame moves the budget with it
                // rather than eating the budget's slack. What the gate is for is
                // per-DRAW traffic leaking back into the ring; CBPerFrame's own
                // size is already pinned by the static_asserts in renderer.h and
                // does not need a second, silently-consumed guard.
                core::Log::Infof("[SMOKE] cb_per_frame_bytes=%u",
                                 render::AlignCBSize(
                                     static_cast<uint32_t>(sizeof(render::CBPerFrame))));
                core::Log::Infof("[SMOKE] rt_available=%s", m_rtAvailable ? "yes" : "no");
                core::Log::Infof("[SMOKE] rt_active=%s",
                                 (m_usePathTracing && m_rtAvailable) ? "yes" : "no");
                core::Log::Infof("[SMOKE] rt_quality=%s",
                                 m_rtQualityMode == render::RTQualityMode::FullPathTrace
                                     ? "full"
                                     : "stable");
                if (m_usePathTracing && m_rtAvailable)
                {
                    core::Log::Infof("[SMOKE] rt_accumulation_frame=%u",
                                     m_scene.GetPathTracer()->AccumulationFrameIndex());
                }
                core::Log::Infof("[SMOKE] overlay=%s",
                                 m_debugOverlayReady ? "ok" : "unavailable");
                core::Log::Infof("[SMOKE] frames=%llu",
                                 static_cast<unsigned long long>(m_frameCount));
                LARGE_INTEGER smokeEnd = {};
                QueryPerformanceCounter(&smokeEnd);
                const double elapsedSeconds =
                    m_smokeCounterFrequency > 0
                        ? static_cast<double>(smokeEnd.QuadPart - m_smokeStartCounter) /
                              static_cast<double>(m_smokeCounterFrequency)
                        : 0.0;
                const double throughput =
                    elapsedSeconds > 0.0 ? static_cast<double>(m_frameCount) / elapsedSeconds : 0.0;
                core::Log::Infof("[SMOKE] present=%s",
                                 m_options.smokeUnlocked ? "immediate" : "vsync");
                core::Log::Infof("[SMOKE] rt_frame_sync=%s",
                                 "frames_in_flight");
                core::Log::Infof("[SMOKE] max_outstanding_submissions=%u",
                                 m_smokeMaxOutstandingSubmissions);
                core::Log::Infof("[SMOKE] elapsed_ms=%.3f throughput_fps=%.3f",
                                 elapsedSeconds * 1000.0, throughput);
                core::Log::Infof("[SMOKE] resize_requests=%u", m_smokeResizeRequests);

                // Per-draw structured-buffer occupancy. shadow_records and
                // main_records are the two passes' disjoint slices of the object
                // buffer; they must be equal, because RenderShadowCasters and
                // RenderEntities walk the same pool with identical filters. That
                // parity is what makes 2 x MeshInstanceCount() sufficient object
                // capacity, and it is enforced nowhere else - so assert the
                // invariant rather than a magic entity count that breaks when the
                // demo scene changes.
                core::Log::Infof(
                    "[SMOKE] object_records_peak=%u material_records_peak=%u "
                    "object_capacity=%u material_capacity=%u "
                    "shadow_records=%u main_records=%u",
                    m_renderer.ObjectRecordsPeak(),
                    m_renderer.MaterialRecordsPeak(),
                    m_renderer.ObjectBufferCapacity(),
                    m_renderer.MaterialBufferCapacity(),
                    m_renderer.ShadowRecords(),
                    m_renderer.MainRecords());
                // Proof that the reallocation branch actually ran, that it ran
                // with frames in flight, and when it first ran.
                //
                // The in-flight count is the one with teeth and it is NOT
                // derivable from the total. Frame zero's grow releases a buffer
                // no command list has ever bound, so the deferred-release queue
                // has nothing to protect there: with the fence guard deleted
                // outright a total-only assertion stays GREEN. Measured, not
                // assumed. See src/render/gpu_draw_records.h.
                //
                // The first-reallocation frame is kept from the other side of
                // this merge because it says something neither count does: that
                // replacement happened late enough for all three frame slots to
                // be outstanding, rather than during start-up.
                core::Log::Infof("[SMOKE] structured_buffer_reallocations=%u "
                                 "structured_buffer_reallocations_in_flight=%u",
                                 m_renderer.StructuredBufferReallocations(),
                                 m_renderer.StructuredBufferReallocationsInFlight());
                core::Log::Infof("[SMOKE] first_in_flight_reallocation_frame=%llu",
                                 static_cast<unsigned long long>(
                                     m_smokeFirstStructuredBufferReallocationFrame));
                core::Log::Info("Smoke mode complete");
                m_running = false;
            }
        }

        UpdatePlayerInput();

        if (!HandleResize())
            continue;
        if (m_window.IsMinimized())
        {
            // Simulation is suspended with rendering. Discard elapsed fixed
            // steps so restoring the window cannot trigger an unbounded catch-up.
            m_timer.DiscardFixedSteps();
            m_pendingPointerDeltaX = 0.0f;
            m_pendingPointerDeltaY = 0.0f;
            Sleep(10);
            continue;
        }

        if (m_options.smoke)
        {
            // The smoke timeline is frame-driven so capture hashes do not depend
            // on how quickly this machine happens to render the test.
            m_timer.DiscardFixedSteps();
            if (!AdvanceSimulation(kSmokeFixedDeltaSeconds))
                break;
            advancedFlightStep = true;
        }
        else
        {
            bool simulationAccepted = true;
            while (m_timer.ConsumeFixedStep())
            {
                if (!AdvanceSimulation(m_timer.GetFixedDt()))
                {
                    simulationAccepted = false;
                    break;
                }
                advancedFlightStep = true;
            }
            if (!simulationAccepted)
                break;
        }

        if (advancedFlightStep)
        {
            m_pendingPointerDeltaX = 0.0f;
            m_pendingPointerDeltaY = 0.0f;
        }

        UpdatePlayerShipVisuals();
        if (!UpdateCamera(timeStep))
        {
            m_exitCode = 12;
            m_running = false;
            break;
        }
        UpdateWindowTitle(timeStep);

        if (!RenderFrame(timeStep))
        {
            m_exitCode = m_device.IsDeviceLost() ? 3 : 5;
            m_running = false;
        }
    }

    return m_exitCode;
}

bool App::AdvanceSimulation(double dt)
{
    const sim::SimulationStepResult result = m_scene.UpdateSystems(dt);
    if (!result.accepted)
    {
        core::Log::Errorf("Simulation fixed step rejected after stage %u",
                          static_cast<uint32_t>(result.completedStage));
        m_exitCode = 9;
        m_running = false;
        return false;
    }

    if (result.resetRenderHistory)
        m_scene.InvalidatePathTraceHistory();
    if (result.drainFixedAccumulator)
        m_timer.DiscardFixedSteps();

    if (m_runtimeAssembly.IsLive())
    {
        const scene::AssemblyInteriorResult interior =
            m_runtimeAssembly.AdvanceInterior(m_scene, dt);
        if (!interior.Succeeded())
        {
            core::Log::Errorf(
                "Interior fixed step rejected [%s]: %s",
                scene::AssemblyInteriorStatusName(interior.status),
                interior.error.c_str());
            m_exitCode = 11;
            m_running = false;
            return false;
        }
        if (interior.changed)
            m_scene.InvalidatePathTraceHistory();
    }

    if (!ValidateSmokeAssemblyMotion())
    {
        m_exitCode = 11;
        m_running = false;
        return false;
    }

    if (!UpdateOnFootSimulation(dt))
    {
        m_exitCode = 12;
        m_running = false;
        return false;
    }

    if (m_options.smoke && !m_smokeSnapshotVerified)
    {
        const sim::SnapshotBuildResult saved =
            m_scene.BuildSimulationSnapshot(dt);
        if (!saved.accepted)
        {
            core::Log::Errorf("Smoke snapshot build failed: %s",
                              saved.error.c_str());
            m_exitCode = 10;
            m_running = false;
            return false;
        }

        const sim::SnapshotApplyResult loaded =
            m_scene.ApplySimulationSnapshot(saved.snapshot);
        if (!loaded.accepted || !m_timer.SetFixedDt(saved.snapshot.fixedDt))
        {
            core::Log::Errorf("Smoke snapshot apply failed: %s",
                              loaded.error.c_str());
            m_exitCode = 10;
            m_running = false;
            return false;
        }

        m_smokeSnapshotVerified = true;
        core::Log::Infof("[SMOKE] simulation_scheduler=ok sim_tick=%llu",
                         static_cast<unsigned long long>(m_scene.SimulationTick()));
        core::Log::Infof("[SMOKE] snapshot_roundtrip=ok snapshot_bodies=%u",
                         static_cast<uint32_t>(saved.snapshot.bodies.size()));
    }
    return true;
}

void App::UpdateWindowTitle(const core::TimeStep& timeStep)
{
    m_titleTimer += static_cast<float>(timeStep.dt);
    if (m_titleTimer < 0.5f)
        return;

    m_titleTimer = 0.0f;
    char modeText[64] = {};
    if (m_usePathTracing)
    {
        std::snprintf(modeText, sizeof(modeText), "PATH TRACING %s %uspp/%ub",
                      m_rtQualityInfo.shortName,
                      m_rtQualityInfo.samplesPerPixel,
                      m_rtQualityInfo.maxBounces);
    }
    else
    {
        std::snprintf(modeText, sizeof(modeText), "RASTERIZED");
    }

    const auto& registry = m_scene.GetRegistry();
    const auto* control = registry.TryGet<ecs::FlightControl>(m_playerShip);
    const auto* body = registry.TryGet<ecs::RigidBody>(m_playerShip);
    const char* flightMode = control && control->mode == ecs::FlightMode::Decoupled
        ? "DECOUPLED"
        : "COUPLED";
    const char* controlContext = gameplay::OwnsOnFootInput(m_playerPossession)
        ? "ON FOOT"
        : "PILOT";
    const double speed = body ? body->linearVelocity.Length() : 0.0;

    char title[224] = {};
    std::snprintf(title, sizeof(title),
                  "The Dawning V3 | %.1f fps | %u entities | %s | %s | FLIGHT %s %.1f m/s [F use | V mode | F1 render | F2 quality]",
                  timeStep.fps, m_scene.EntityCount(), modeText, controlContext,
                  flightMode, speed);
    SetWindowTextA(m_window.GetHWND(), title);
}

void App::UpdatePlayerInput()
{
    const auto& input = core::input::GetState();
    const gameplay::LocalMovementInput movement =
        gameplay::ResolveMovementBindings({
            input.KeyDown('W'), input.KeyDown('S'),
            input.KeyDown('A'), input.KeyDown('D'),
            input.KeyDown(VK_UP), input.KeyDown(VK_DOWN),
            input.KeyDown(VK_LEFT), input.KeyDown(VK_RIGHT),
            input.KeyDown(VK_SPACE), input.KeyDown(VK_CONTROL),
        });

    if (m_window.IsCaptured())
    {
        constexpr float maxPendingPixels = 112.0f;
        m_pendingPointerDeltaX = (std::max)(-maxPendingPixels, (std::min)(
            maxPendingPixels,
            m_pendingPointerDeltaX + static_cast<float>(input.mouse.deltaX)));
        m_pendingPointerDeltaY = (std::max)(-maxPendingPixels, (std::min)(
            maxPendingPixels,
            m_pendingPointerDeltaY + static_cast<float>(input.mouse.deltaY)));
    }
    else
    {
        m_pendingPointerDeltaX = 0.0f;
        m_pendingPointerDeltaY = 0.0f;
    }

    if (gameplay::OwnsOnFootInput(m_playerPossession))
    {
        ClearPlayerShipInput();
        m_onFootCommand = {};
        m_onFootCommand.moveForward = static_cast<double>(
            gameplay::DigitalAxis(movement.forward, movement.backward));
        m_onFootCommand.moveRight = static_cast<double>(
            gameplay::DigitalAxis(movement.right, movement.left));
        m_onFootCommand.sprint = input.KeyDown(VK_SHIFT);
        m_onFootCommand.jumpDown = input.KeyDown(VK_SPACE);
        return;
    }

    m_onFootCommand = {};
    if (!gameplay::OwnsShipInput(m_playerPossession))
    {
        ClearPlayerShipInput();
        m_pendingPointerDeltaX = 0.0f;
        m_pendingPointerDeltaY = 0.0f;
        return;
    }

    auto& registry = m_scene.GetRegistry();
    auto* control = registry.TryGet<ecs::FlightControl>(m_playerShip);
    auto* thrusters = registry.TryGet<ecs::ThrusterSet>(m_playerShip);
    if (!control || !thrusters)
    {
        ClearPlayerShipInput();
        m_pendingPointerDeltaX = 0.0f;
        m_pendingPointerDeltaY = 0.0f;
        return;
    }

    gameplay::PilotInputSnapshot snapshot;
    snapshot.thrustForward = movement.forward;
    snapshot.thrustBackward = movement.backward;
    snapshot.strafeLeft = movement.left;
    snapshot.strafeRight = movement.right;
    snapshot.liftUp = movement.up;
    snapshot.liftDown = movement.down;
    snapshot.pitchUp = input.KeyDown('I');
    snapshot.pitchDown = input.KeyDown('K');
    snapshot.yawLeft = input.KeyDown('J');
    snapshot.yawRight = input.KeyDown('L');
    snapshot.rollLeft = input.KeyDown('Q');
    snapshot.rollRight = input.KeyDown('E');
    snapshot.toggleMode = input.KeyPressed('V');
    snapshot.pointerPitch =
        gameplay::PointerSteeringDemand(m_pendingPointerDeltaY);
    snapshot.pointerYaw =
        gameplay::PointerSteeringDemand(m_pendingPointerDeltaX);

    if (m_options.smokeFlight)
    {
        const uint64_t endFrame = SmokeFrameForTime(m_options.smokeSeconds);
        snapshot = gameplay::PilotInputSnapshot{};
        m_pendingPointerDeltaX = 0.0f;
        m_pendingPointerDeltaY = 0.0f;
        snapshot.toggleMode = m_frameCount == 2;
        snapshot.thrustForward = m_frameCount + 12 > endFrame;
    }

    const ecs::FlightMode previousMode = control->mode;
    gameplay::ApplyPilotInput(snapshot, *control);
    if (control->mode == ecs::FlightMode::Coupled)
    {
        // Coupled assist is an ideal wrench in the current Stage 2 law, so no
        // physical nozzle is active. Clear any decoupled throttle left over from
        // the preceding fixed step before presenting its state to the renderer.
        gameplay::ClearThrusterThrottles(*thrusters);
    }

    if (control->mode != previousMode)
    {
        core::Log::Infof("Flight mode: %s",
                         control->mode == ecs::FlightMode::Coupled
                             ? "COUPLED"
                             : "DECOUPLED");
    }
}

void App::ClearPlayerShipInput()
{
    auto& registry = m_scene.GetRegistry();
    if (auto* control = registry.TryGet<ecs::FlightControl>(m_playerShip))
    {
        control->linearDemand = {};
        control->angularDemand = {};
    }
    if (auto* thrusters = registry.TryGet<ecs::ThrusterSet>(m_playerShip))
        gameplay::ClearThrusterThrottles(*thrusters);
}

bool App::BuildPlayerShipRoot(ecs::Transform& root)
{
    if (!m_possessionReady || m_playerShip.IsNull())
        return false;
    const auto* ship =
        m_scene.GetRegistry().TryGet<ecs::Transform>(m_playerShip);
    if (!ship)
        return false;
    if (!gameplay::IsValidPossessionRoot(
            *ship, m_possessionConfig.uniformRootScaleTolerance))
    {
        return false;
    }
    root = *ship;
    return true;
}

bool App::BuildAssemblyInteractionQuery(
    scene::AssemblyInteractionQuery& query)
{
    ecs::Transform liveRoot;
    const scene::AssemblyInstance* instance = m_runtimeAssembly.Instance();
    if (!gameplay::OwnsOnFootInput(m_playerPossession) ||
        !BuildPlayerShipRoot(liveRoot) || !instance || !instance->IsAlive() ||
        !instance->Plan())
    {
        return false;
    }

    core::Vec3d localPosition;
    core::Vec3d localForward;
    if (!gameplay::WorldPointToAssemblyLocal(
            liveRoot,
            m_camera.Position(),
            localPosition,
            m_possessionConfig.uniformRootScaleTolerance) ||
        !gameplay::WorldDirectionToAssemblyLocal(
            liveRoot,
            m_camera.Forward(),
            localForward,
            m_possessionConfig.uniformRootScaleTolerance))
    {
        return false;
    }

    scene::AssemblyInteractionQuery candidate;
    candidate.assemblyPosition = localPosition;
    candidate.assemblyForward = localForward.ToFloat();
    candidate.maxDistanceMeters =
        m_possessionConfig.maximumSeatUseDistanceMeters;
    candidate.minimumForwardDot =
        m_possessionConfig.minimumSeatFacingDot;
    query = candidate;
    return true;
}

gameplay::PilotPossessionStatus App::TryExitPilotSeat()
{
    if (!m_possessionReady)
        return gameplay::PilotPossessionStatus::NotInitialized;
    ecs::Transform root;
    if (!BuildPlayerShipRoot(root))
        return gameplay::PilotPossessionStatus::InternalError;
    const auto collision = m_runtimeAssembly.InteractiveCollisionSnapshot();
    if (!collision)
        return gameplay::PilotPossessionStatus::CollisionFailure;

    const scene::AssemblyInteriorRuntime& interior =
        m_runtimeAssembly.Interior();
    const gameplay::PilotPossessionResult staged = gameplay::StagePilotExit(
        m_pilotSeat,
        m_playerPossession,
        interior.InteractionStateIndex(m_pilotSeat.interactionIndex),
        *collision,
        m_possessionConfig);
    if (!staged.Succeeded())
        return staged.status;

    const scene::AssemblyInteriorResult released =
        m_runtimeAssembly.ActivateInteraction(
            m_scene, m_pilotSeat.interactionIndex);
    if (!released.Succeeded() ||
        interior.InteractionStateIndex(m_pilotSeat.interactionIndex) !=
            m_pilotSeat.availableStateIndex)
    {
        return gameplay::PilotPossessionStatus::InternalError;
    }

    m_playerPossession = staged.state;
    ClearPlayerShipInput();
    m_onFootCommand = {};
    m_pendingPointerDeltaX = 0.0f;
    m_pendingPointerDeltaY = 0.0f;
    m_chaseCameraInitialized = false;
    core::Log::Info("Control context: ON FOOT");
    return gameplay::PilotPossessionStatus::Success;
}

gameplay::PilotPossessionStatus App::TryEnterPilotSeat()
{
    if (!m_possessionReady)
        return gameplay::PilotPossessionStatus::NotInitialized;
    ecs::Transform root;
    if (!BuildPlayerShipRoot(root))
        return gameplay::PilotPossessionStatus::InternalError;
    const auto collision = m_runtimeAssembly.InteractiveCollisionSnapshot();
    if (!collision)
        return gameplay::PilotPossessionStatus::CollisionFailure;

    const scene::AssemblyInteriorRuntime& interior =
        m_runtimeAssembly.Interior();
    const gameplay::PilotPossessionResult staged = gameplay::StagePilotEntry(
        m_pilotSeat,
        m_playerPossession,
        interior.InteractionStateIndex(m_pilotSeat.interactionIndex),
        *collision,
        m_possessionConfig);
    if (!staged.Succeeded())
        return staged.status;

    const scene::AssemblyInteriorResult occupied =
        m_runtimeAssembly.ActivateInteraction(
            m_scene, m_pilotSeat.interactionIndex);
    if (!occupied.Succeeded() ||
        interior.InteractionStateIndex(m_pilotSeat.interactionIndex) !=
            m_pilotSeat.occupiedStateIndex)
    {
        return gameplay::PilotPossessionStatus::InternalError;
    }

    m_playerPossession = staged.state;
    m_onFootCommand = {};
    m_pendingPointerDeltaX = 0.0f;
    m_pendingPointerDeltaY = 0.0f;
    m_chaseCameraInitialized = false;
    core::Log::Info("Control context: PILOT");
    return gameplay::PilotPossessionStatus::Success;
}

bool App::HandleUseAction()
{
    if (gameplay::OwnsShipInput(m_playerPossession))
    {
        const gameplay::PilotPossessionStatus status = TryExitPilotSeat();
        if (status == gameplay::PilotPossessionStatus::Success)
            return true;
        if (status == gameplay::PilotPossessionStatus::SpawnBlocked ||
            status == gameplay::PilotPossessionStatus::SeatUnavailable)
        {
            core::Log::Warnf(
                "Pilot exit rejected [%s]",
                gameplay::PilotPossessionStatusName(status));
            return true;
        }
        core::Log::Errorf(
            "Pilot exit failed [%s]",
            gameplay::PilotPossessionStatusName(status));
        return false;
    }

    if (!gameplay::OwnsOnFootInput(m_playerPossession))
        return false;

    scene::AssemblyInteractionQuery query;
    if (!BuildAssemblyInteractionQuery(query))
    {
        core::Log::Error("Interaction query could not cross the ship-root boundary");
        return false;
    }
    const scene::AssemblyInteriorResult found =
        m_runtimeAssembly.Interior().FindNearest(query);
    if (found.status == scene::AssemblyInteriorStatus::NotFound)
        return true;
    if (!found.Succeeded())
    {
        core::Log::Errorf(
            "Interaction query failed [%s]: %s",
            scene::AssemblyInteriorStatusName(found.status),
            found.error.c_str());
        return false;
    }
    if (found.stableIndex == m_pilotSeat.interactionIndex)
    {
        const gameplay::PilotPossessionStatus entry = TryEnterPilotSeat();
        if (entry == gameplay::PilotPossessionStatus::Success)
            return true;
        if (entry == gameplay::PilotPossessionStatus::SeatUnavailable ||
            entry == gameplay::PilotPossessionStatus::OutOfRange ||
            entry == gameplay::PilotPossessionStatus::NotFacing)
        {
            core::Log::Warnf(
                "Pilot entry rejected [%s]",
                gameplay::PilotPossessionStatusName(entry));
            return true;
        }
        core::Log::Errorf(
            "Pilot entry failed [%s]",
            gameplay::PilotPossessionStatusName(entry));
        return false;
    }

    const scene::AssemblyInteriorResult interaction =
        m_runtimeAssembly.ActivateInteraction(m_scene, found.stableIndex);
    if (interaction.Succeeded())
    {
        const std::string_view state = m_runtimeAssembly.Interior()
            .InteractionStateName(interaction.stableIndex);
        core::Log::Infof(
            "Interaction %u entered state '%.*s'",
            interaction.stableIndex,
            static_cast<int>(state.size()),
            state.data());
        return true;
    }
    if (interaction.status == scene::AssemblyInteriorStatus::Locked)
    {
        core::Log::Warn("The selected interaction is locked");
        return true;
    }
    core::Log::Errorf(
        "Interaction failed [%s]: %s",
        scene::AssemblyInteriorStatusName(interaction.status),
        interaction.error.c_str());
    return false;
}

bool App::UpdateOnFootSimulation(double dt)
{
    if (!gameplay::OwnsOnFootInput(m_playerPossession))
        return true;
    const auto collision = m_runtimeAssembly.InteractiveCollisionSnapshot();
    if (!collision)
    {
        core::Log::Error("On-foot simulation has no live collision snapshot");
        return false;
    }

    const gameplay::PilotPossessionResult looked = gameplay::ApplyOnFootLook(
        m_playerPossession,
        m_pendingPointerDeltaX,
        m_pendingPointerDeltaY,
        m_possessionConfig);
    if (!looked.Succeeded())
    {
        core::Log::Errorf(
            "On-foot look step failed [%s]: %s",
            gameplay::PilotPossessionStatusName(looked.status),
            looked.error.c_str());
        return false;
    }

    gameplay::OnFootCommand command = m_onFootCommand;
    command.viewForward = gameplay::OnFootViewForward(looked.state);
    const gameplay::OnFootStepResult stepped = gameplay::StepOnFootController(
        *collision,
        looked.state.onFoot,
        command,
        dt);
    if (!stepped.Succeeded())
    {
        core::Log::Errorf(
            "On-foot fixed step failed [%s/%s]",
            gameplay::OnFootControllerStatusName(stepped.status),
            scene::InteriorCollisionStatusName(stepped.collisionStatus));
        return false;
    }

    m_playerPossession = looked.state;
    m_playerPossession.onFoot = stepped.state;
    // One raw-input batch belongs to one accepted fixed step. A catch-up loop
    // may execute more steps immediately, but those steps receive zero look
    // delta rather than replaying the same mouse counts.
    m_pendingPointerDeltaX = 0.0f;
    m_pendingPointerDeltaY = 0.0f;
    return true;
}

bool App::ValidateSmokeAssemblyMotion()
{
    if (!m_options.smokeFlight || m_smokeAssemblyMotionVerified ||
        m_frameCount != SmokeFrameForTime(m_options.smokeSeconds))
    {
        return true;
    }

    const scene::AssemblyInstance* instance = m_runtimeAssembly.Instance();
    if (!m_smokeAssemblyBaselineReady || !instance || !instance->IsAlive() ||
        instance->RootEntity() != m_playerShip ||
        instance->ModuleEntities().empty())
    {
        core::Log::Error("Assembly root-motion smoke baseline is unavailable");
        return false;
    }

    const auto& registry = m_scene.GetRegistry();
    const ecs::Transform* root =
        registry.TryGet<ecs::Transform>(m_playerShip);
    const ecs::Transform* module = registry.TryGet<ecs::Transform>(
        instance->ModuleEntities().front());
    if (!root || !module)
    {
        core::Log::Error("Assembly root-motion smoke entities are unavailable");
        return false;
    }

    const core::Vec3d rootDisplacement =
        root->position - m_smokeAssemblyInitialRootPosition;
    const core::Vec3d moduleDisplacement =
        module->position - m_smokeAssemblyInitialModulePosition;
    const double rootDistance = rootDisplacement.Length();
    const double moduleDistance = moduleDisplacement.Length();
    const double hierarchyError =
        (moduleDisplacement - rootDisplacement).Length();
    constexpr double minimumMotionMeters = 1.0e-4;
    constexpr double maximumHierarchyErrorMeters = 1.0e-5;
    if (!std::isfinite(rootDistance) || !std::isfinite(moduleDistance) ||
        !std::isfinite(hierarchyError) || rootDistance <= minimumMotionMeters ||
        moduleDistance <= minimumMotionMeters ||
        hierarchyError > maximumHierarchyErrorMeters)
    {
        core::Log::Errorf(
            "Assembly root-motion smoke failed: root=%.9f child=%.9f error=%.9f",
            rootDistance,
            moduleDistance,
            hierarchyError);
        return false;
    }

    m_smokeAssemblyMotionVerified = true;
    core::Log::Infof(
        "[SMOKE] assembly_root_motion=ok hierarchy_motion=coherent root_delta=%.6f child_delta=%.6f hierarchy_error=%.9f",
        rootDistance,
        moduleDistance,
        hierarchyError);
    return true;
}

void App::UpdatePlayerShipVisuals()
{
    auto& registry = m_scene.GetRegistry();
    const auto* ship = registry.TryGet<ecs::Transform>(m_playerShip);
    const auto* thrusters = registry.TryGet<ecs::ThrusterSet>(m_playerShip);
    if (!ship || !thrusters)
        return;

    const uint32_t count = m_thrusterVisualCount < thrusters->count
        ? m_thrusterVisualCount
        : thrusters->count;
    for (uint32_t i = 0; i < count; ++i)
    {
        const ecs::Entity visualEntity = m_thrusterVisuals[i];
        auto* transform = registry.TryGet<ecs::Transform>(visualEntity);
        auto* material = registry.TryGet<ecs::Material>(visualEntity);
        if (!transform || !material)
            continue;

        const gameplay::ThrusterVisualState visual =
            gameplay::BuildThrusterVisualState(*ship, thrusters->thrusters[i]);
        *transform = visual.transform;
        material->emissiveStrength = visual.emissiveStrength;

        if (visual.visible)
        {
            if (!registry.Has<ecs::MeshInstance>(visualEntity))
            {
                registry.Assign<ecs::MeshInstance>(
                    visualEntity,
                    ecs::MeshInstance{ m_thrusterVisualMesh.value, true });
            }
        }
        else
        {
            registry.Remove<ecs::MeshInstance>(visualEntity);
        }
    }

    if (m_options.smokeFlight &&
        m_frameCount == SmokeFrameForTime(m_options.smokeSeconds))
    {
        const auto* body = registry.TryGet<ecs::RigidBody>(m_playerShip);
        const auto* control = registry.TryGet<ecs::FlightControl>(m_playerShip);
        const float mainThrottle = thrusters->count > 4
            ? thrusters->thrusters[4].throttle
            : 0.0f;
        core::Log::Infof(
            "[SMOKE] playable_ship=ok flight_mode=%s speed=%.3f main_throttle=%.3f",
            control && control->mode == ecs::FlightMode::Decoupled
                ? "decoupled"
                : "coupled",
            body ? body->linearVelocity.Length() : 0.0,
            mainThrottle);
    }
}

void App::ApplyCameraModeRenderState()
{
    // K (Scene render scale) + near plane (the reversed-Z precision lever) per
    // mode. Reversed-Z is infinite-far, so the far argument is informational.
    switch (m_cameraMode)
    {
        case CameraMode::Orrery:
            // Compress the whole true-scale system to ~150 units at 1 AU so it
            // fits in float and a small depth range; tiny near, fast free move.
            m_scene.SetRenderScale(1.0e-9);
            m_camera.SetClipPlanes(0.01f, 1.0e6f);
            m_camera.SetMoveSpeed(5.0e10f);
            break;
        case CameraMode::NearBody:
            m_scene.SetRenderScale(1.0);
            m_camera.SetClipPlanes(1.0f, 1.0e12f);
            m_camera.SetMoveSpeed(5.0e6f);
            break;
        case CameraMode::Free:
            m_scene.SetRenderScale(1.0);
            m_camera.SetClipPlanes(0.1f, 1.0e12f);
            m_camera.SetMoveSpeed(1.0e7f);
            break;
        case CameraMode::Surface:
            // Low over a body's surface at true scale. Small near plane (never
            // NearBody's 1.0, which would clip close terrain), fast-but-not-orbital
            // move speed for skimming the surface.
            m_scene.SetRenderScale(1.0);
            m_camera.SetClipPlanes(0.05f, 1.0e12f);
            m_camera.SetMoveSpeed(5.0e4f);
            break;
        case CameraMode::ShipChase:
        default:
            m_scene.SetRenderScale(1.0);
            m_camera.SetClipPlanes(0.1f, 10000.0f);
            m_camera.SetMoveSpeed(5.0f);
            break;
    }
    // Bodies render at true radius everywhere except the orrery, where they are
    // scaled up to visible markers (K set just above cancels in the scale math).
    m_scene.ApplyStarSystemRenderMode(m_cameraMode == CameraMode::Orrery);

    // In Surface mode the focus body is drawn twice — its smooth sphere by
    // Scene::RenderEntities and its displaced patches by RenderTerrainPreview.
    // Tell the scene which body that is so its sphere is pulled a hair below the
    // terrain (see Scene::SetTerrainSurfaceBody); clear it in every other mode.
    m_scene.SetTerrainSurfaceBody(
        (m_cameraMode == CameraMode::Surface && m_terrainBuilt) ? m_terrainBodyId : 0);
}

bool App::UpdateCamera(const core::TimeStep& timeStep)
{
    ApplyCameraModeRenderState();

    if (gameplay::OwnsOnFootInput(m_playerPossession))
    {
        ecs::Transform root;
        if (!BuildPlayerShipRoot(root))
        {
            core::Log::Error("On-foot camera lost the player ship root");
            return false;
        }
        const gameplay::OnFootCameraResult pose =
            gameplay::BuildOnFootCameraPose(
                m_playerPossession, root, m_possessionConfig);
        if (!pose.Succeeded() ||
            !m_camera.InitBasis(
                pose.pose.position, pose.pose.forward, pose.pose.up))
        {
            core::Log::Errorf(
                "On-foot camera pose failed [%s]: %s",
                gameplay::PilotPossessionStatusName(pose.status),
                pose.error.c_str());
            return false;
        }
        m_chaseCameraInitialized = false;
        return true;
    }

    const auto& registry = m_scene.GetRegistry();

    // Orrery mode: frame the WHOLE system from a fixed vantage looking at the Sun
    // (frame origin). The standoff is a multiple of the outer orbit so Sun ->
    // Mars all fit; K (set in ApplyCameraModeRenderState) compresses it to view.
    if (m_cameraMode == CameraMode::Orrery)
    {
        constexpr double kAU = 1.495978707e11;
        const gameplay::ChaseCameraPose pose = gameplay::BuildNearBodyCameraPose(
            core::Vec3d{ 0.0, 0.0, 0.0 }, // Sun at the frame origin
            1.52 * kAU,                   // "radius" = Mars' orbit, so the system fits
            1.35, 40.0f, 28.0f);
        m_chaseCameraInitialized = false;
        m_camera.Init(pose.position, pose.yawDegrees, pose.pitchDegrees);
        return true;
    }

    // Near-body mode: park the camera at a standoff from a seeded celestial body,
    // framing it at true scale. Falls through to free-fly if no body is seeded
    // (e.g. the default smoke run).
    if (m_cameraMode == CameraMode::NearBody)
    {
        const uint64_t focus = m_focusBodyId
            ? m_focusBodyId
            : (scene::kStarSystemBodyIdBase + 10); // default: Earth
        core::Vec3d bodyPos;
        double bodyRadius = 0.0;
        bool found = false;
        if (auto* pool = m_scene.GetRegistry().GetPool<ecs::GravitationalBody>())
        {
            for (uint32_t i = 0; i < pool->Count(); ++i)
                if (pool->DataAt(i).bodyId == focus)
                {
                    const ecs::Entity e = registry.EntityAtIndex(pool->EntityAt(i));
                    if (const auto* t = registry.TryGet<ecs::Transform>(e))
                    {
                        bodyPos = t->position;
                        bodyRadius = pool->DataAt(i).radius;
                        found = true;
                    }
                    break;
                }
        }
        if (found)
        {
            // If the ship is orbiting THIS body, pull the camera back to frame its
            // whole orbit — a KSP-style map view where the trace and the apsis markers
            // are legible — rather than hugging the planet. Stand off ~1.6x the
            // orbit's apoapsis so the full ellipse fits the 70-degree FOV; otherwise
            // the usual 3-radii body framing.
            double standoffRadii = 3.0;
            if (m_shipInSystem && bodyRadius > 0.0)
            {
                const scene::OsculatingOrbit shipOrbit =
                    m_scene.DeriveOsculatingOrbit(kPlayerShipBodyId);
                if (shipOrbit.valid && shipOrbit.primaryBodyId == focus)
                {
                    const double reach = (shipOrbit.apoapsis > 0.0)
                        ? shipOrbit.apoapsis : shipOrbit.altitude;
                    const double framed = 1.6 * reach / bodyRadius;
                    if (framed > standoffRadii)
                        standoffRadii = framed;
                }
            }
            const gameplay::ChaseCameraPose pose =
                gameplay::BuildNearBodyCameraPose(bodyPos, bodyRadius, standoffRadii);
            m_chaseCameraInitialized = false; // fixed standoff, no chase smoothing
            m_camera.Init(pose.position, pose.yawDegrees, pose.pitchDegrees);
            return true;
        }
    }

    // Surface preview: an oblique aerial pose over the terrain patch, so the
    // displaced relief reads against the curved limb. up = the patch radial.
    if (m_cameraMode == CameraMode::Surface && m_terrainBuilt)
    {
        core::Vec3d bodyPos;
        bool found = false;
        if (auto* pool = m_scene.GetRegistry().GetPool<ecs::GravitationalBody>())
            for (uint32_t i = 0; i < pool->Count(); ++i)
                if (pool->DataAt(i).bodyId == m_terrainBodyId)
                {
                    const ecs::Entity e = registry.EntityAtIndex(pool->EntityAt(i));
                    if (const auto* t = registry.TryGet<ecs::Transform>(e))
                    { bodyPos = t->position; found = true; }
                    break;
                }
        if (found)
        {
            // Seconds since Surface was entered, not total run time: latch the entry
            // stamp on the first Surface frame (the F4 handler clears it to -1 on any
            // exit, so a later re-entry restarts the descent from 0). This keeps the
            // 3s altitude ramp playing on every entry, not only when Surface is the
            // start mode at totalTime~0.
            if (m_surfaceEntryTime < 0.0) m_surfaceEntryTime = timeStep.totalTime;
            m_surfaceElapsed = timeStep.totalTime - m_surfaceEntryTime;

            // DESCEND: drive altitude down over the run so the streamer refines LOD
            // as we approach, and drift forward across the surface. m_terrainFocusBody
            // is the fixed look-at surface point; recompute the framing each frame.
            const double R = m_terrainFocusBody.Length();
            const core::Vec3d fdir{ m_terrainFocusBody.x / R, m_terrainFocusBody.y / R,
                                    m_terrainFocusBody.z / R };
            core::Vec3d tan{ fdir.y * 0.0 - fdir.z * 1.0, fdir.z * 0.0 - fdir.x * 0.0,
                             fdir.x * 1.0 - fdir.y * 0.0 };
            double tl = tan.Length();
            if (tl < 1e-6) { tan = { 1.0, 0.0, 0.0 }; tl = 1.0; }
            tan = { tan.x / tl, tan.y / tl, tan.z / tl };

            // Altitude range scales with the body: descend from ~6% of the radius
            // down to just above the (exaggerated) peaks, so the camera never flies
            // into terrain.
            const TerrainBodyCfg cfg = ConfigForTerrainBody(m_terrainBodyId - scene::kStarSystemBodyIdBase);
            const double startAlt = R * 0.06;
            const double endAlt   = cfg.amplitude * 2.0 + 15000.0;
            const double tphase   = (m_surfaceElapsed < 3.0) ? (m_surfaceElapsed / 3.0) : 1.0;
            const double altitude = startAlt + (endAlt - startAlt) * tphase;
            const double backDist = altitude * 1.3;
            const double fwdDrift = R * 0.03 * tphase;   // fly forward as we descend

            m_terrainCamBody = { fdir.x * (R + altitude) - tan.x * backDist + tan.x * fwdDrift,
                                 fdir.y * (R + altitude) - tan.y * backDist + tan.y * fwdDrift,
                                 fdir.z * (R + altitude) - tan.z * backDist + tan.z * fwdDrift };
            const core::Vec3d look{ m_terrainFocusBody.x + tan.x * fwdDrift,
                                    m_terrainFocusBody.y + tan.y * fwdDrift,
                                    m_terrainFocusBody.z + tan.z * fwdDrift };

            const core::Vec3d camPos{ bodyPos.x + m_terrainCamBody.x,
                                      bodyPos.y + m_terrainCamBody.y,
                                      bodyPos.z + m_terrainCamBody.z };
            core::Vec3d fwdD{ look.x - m_terrainCamBody.x, look.y - m_terrainCamBody.y,
                              look.z - m_terrainCamBody.z };
            const double fl = fwdD.Length();
            const double ul = m_terrainCamBody.Length();
            const core::Vec3f fwd{ static_cast<float>(fwdD.x / fl),
                                   static_cast<float>(fwdD.y / fl),
                                   static_cast<float>(fwdD.z / fl) };
            const core::Vec3f upf{ static_cast<float>(m_terrainCamBody.x / ul),
                                   static_cast<float>(m_terrainCamBody.y / ul),
                                   static_cast<float>(m_terrainCamBody.z / ul) };
            m_chaseCameraInitialized = false;
            if (m_camera.InitBasis(camPos, fwd, upf))
                return true;
        }
    }

    const ecs::Entity chaseTarget =
        m_options.smoke && !m_options.starSystem && !m_smokeCameraTarget.IsNull()
            ? m_smokeCameraTarget   // default smoke probe; NOT when flying the star system
            : m_playerShip;
    const ecs::Transform* ship = (m_cameraMode == CameraMode::ShipChase)
        ? registry.TryGet<ecs::Transform>(chaseTarget)
        : nullptr;
    if (ship)
    {
        if (m_options.smoke && !m_options.starSystem &&
            !IsReferenceRuntimeContent())
        {
            const core::Vec3f localOffset{ 20.0f, 9.0f, -25.0f };
            const core::Vec3f worldOffset = ship->rotation.Rotate(localOffset);
            const core::Vec3f up =
                ship->rotation.Rotate({ 0.0f, 1.0f, 0.0f }).Normalized();
            const core::Vec3f forward = (-worldOffset).Normalized();
            m_chaseCameraInitialized = false;
            return m_camera.InitBasis(
                ship->position + core::Vec3d::FromFloat(worldOffset),
                forward,
                up);
        }

        const gameplay::ChaseCameraPose target =
            gameplay::BuildChaseCameraPose(*ship);
        gameplay::ChaseCameraPose pose = target;
        if (m_chaseCameraInitialized)
        {
            pose = gameplay::SmoothChaseCameraPose(
                { m_chaseCameraPosition, m_chaseCameraYaw, m_chaseCameraPitch },
                target, timeStep.dt);
        }
        m_chaseCameraPosition = pose.position;
        m_chaseCameraYaw = pose.yawDegrees;
        m_chaseCameraPitch = pose.pitchDegrees;
        m_chaseCameraInitialized = true;
        m_camera.Init(pose.position, pose.yawDegrees, pose.pitchDegrees);
        return true;
    }

    m_chaseCameraInitialized = false;

    const auto& input = core::input::GetState();
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    if (m_window.IsCaptured())
    {
        mouseDeltaX = static_cast<float>(input.mouse.deltaX);
        mouseDeltaY = static_cast<float>(input.mouse.deltaY);
    }

    const gameplay::LocalMovementInput movement =
        gameplay::ResolveMovementBindings({
            input.KeyDown('W'), input.KeyDown('S'),
            input.KeyDown('A'), input.KeyDown('D'),
            input.KeyDown(VK_UP), input.KeyDown(VK_DOWN),
            input.KeyDown(VK_LEFT), input.KeyDown(VK_RIGHT),
            input.KeyDown('E') || input.KeyDown(VK_SPACE),
            input.KeyDown('Q') || input.KeyDown(VK_CONTROL),
        });
    m_camera.Update(
        static_cast<float>(timeStep.dt), mouseDeltaX, mouseDeltaY,
        movement.forward, movement.backward, movement.left, movement.right,
        movement.up, movement.down,
        input.KeyDown(VK_SHIFT));
    return true;
}

bool App::HandleResize()
{
    if (!m_window.WasResized())
        return true;

    if (!m_device.Resize(m_window.GetWidth(), m_window.GetHeight()))
    {
        if (m_device.IsDeviceLost())
        {
            core::Log::Error("Swap chain resize failed because the GPU device was lost");
            m_exitCode = 3;
            m_running = false;
            return false;
        }

        core::Log::Error("Swap chain resize failed; skipping frame and retrying");
        Sleep(10);
        return false;
    }

    // The HDR scene target is back-buffer sized, so it has to follow. Unlike the
    // path tracer there is no raster fallback if this fails - the raster path has
    // nowhere else to draw - so treat it the same as a swap-chain resize failure
    // and retry rather than rendering into a stale or absent target.
    if (!m_renderer.ResizeHDRTarget(m_device,
                                    static_cast<uint32_t>(m_device.Width()),
                                    static_cast<uint32_t>(m_device.Height())))
    {
        core::Log::Error("HDR scene target resize failed; skipping frame and retrying");
        Sleep(10);
        return false;
    }

    if (m_rtAvailable &&
        !m_scene.ResizePathTracer(m_device, m_device.Width(), m_device.Height()))
    {
        core::Log::Error("Path tracer resize failed; falling back to raster");
        m_usePathTracing = false;
    }

    m_window.ClearResizeFlag();
    return true;
}

bool App::ApplySmokeResizeStep()
{
    if (!m_options.smokeResize)
        return true;

    struct ResizeStep
    {
        uint64_t frame;
        int clientWidth;
        int clientHeight;
    };

    constexpr ResizeStep steps[] = {
        { 15, 1280, 720 },
        { 30, 1024, 768 },
        { 45, 1920, 1080 },
    };

    if (m_smokeResizeRequests >= _countof(steps))
        return true;

    const ResizeStep& step = steps[m_smokeResizeRequests];
    if (m_frameCount < step.frame)
        return true;

    const HWND hwnd = m_window.GetHWND();
    RECT windowRect = { 0, 0, step.clientWidth, step.clientHeight };
    const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
    if (!AdjustWindowRectEx(&windowRect, style, FALSE, exStyle))
    {
        core::Log::Errorf("Smoke resize AdjustWindowRectEx failed: %lu", GetLastError());
        return false;
    }

    const int outerWidth = windowRect.right - windowRect.left;
    const int outerHeight = windowRect.bottom - windowRect.top;
    if (!SetWindowPos(hwnd, nullptr, 0, 0, outerWidth, outerHeight,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
    {
        core::Log::Errorf("Smoke resize SetWindowPos failed: %lu", GetLastError());
        return false;
    }

    ++m_smokeResizeRequests;
    core::Log::Infof("[SMOKE] resize_request=%u client=%dx%d",
                     m_smokeResizeRequests, step.clientWidth, step.clientHeight);
    return true;
}

bool App::ApplySmokeDescriptorStress()
{
    if (!m_options.smoke)
        return true;

    const render::Texture* texture =
        m_scene.GetResources().GetTexture(m_smokeDescriptorTexture);
    if (!texture || !texture->IsValid())
    {
        core::Log::Error("Smoke descriptor stress texture is unavailable");
        return false;
    }

    if (m_frameCount == 4)
    {
        m_smokeRetiredDescriptor =
            m_renderer.RegisterTexture(m_device.Device(), *texture);
        if (!m_smokeRetiredDescriptor.IsValid())
        {
            core::Log::Error("Smoke descriptor stress could not allocate retirement slot");
            return false;
        }

        m_renderer.ReleaseTextureDescriptor(m_device, m_smokeRetiredDescriptor);
        m_smokeHeldDescriptor =
            m_renderer.RegisterTexture(m_device.Device(), *texture);
        if (!m_smokeHeldDescriptor.IsValid() ||
            m_smokeHeldDescriptor == m_smokeRetiredDescriptor)
        {
            core::Log::Error("Raster descriptor was reused before its fence completed");
            return false;
        }
        core::Log::Info("[SMOKE] descriptor_reuse_before_fence=blocked");
    }
    else if (m_frameCount == 12)
    {
        const render::DescriptorHandle recycled =
            m_renderer.RegisterTexture(m_device.Device(), *texture);
        if (!recycled.IsValid() || recycled.index != m_smokeRetiredDescriptor.index ||
            recycled.generation == m_smokeRetiredDescriptor.generation)
        {
            core::Log::Errorf("Raster descriptor was not recycled after fence completion "
                              "(expected slot %u with a new generation, got %u gen=%u)",
                              m_smokeRetiredDescriptor.index,
                              recycled.index, recycled.generation);
            return false;
        }

        m_renderer.ReleaseTextureDescriptor(m_device, m_smokeHeldDescriptor);
        m_renderer.ReleaseTextureDescriptor(m_device, recycled);
        m_smokeHeldDescriptor = {};
        core::Log::Info("[SMOKE] descriptor_reuse_after_fence=reused");
    }

    return true;
}

bool App::ApplySmokeRTMutationStress()
{
    if (!m_options.smokeRT)
        return true;

    auto& registry = m_scene.GetRegistry();
    if (m_frameCount == 5)
    {
        if (!registry.Has<ecs::Material>(m_smokeTextureEntity))
        {
            core::Log::Error("RT smoke texture mutation entity is unavailable");
            return false;
        }
        auto& material = registry.Get<ecs::Material>(m_smokeTextureEntity);
        m_smokeSavedAlbedoTexture = material.albedoTextureHandle;
        m_smokeSavedNormalTexture = material.normalTextureHandle;
        m_smokeSavedOrmTexture = material.ormTextureHandle;
        m_smokeSavedEmissiveTexture = material.emissiveTextureHandle;
        material.albedoTextureHandle = UINT32_MAX;
        material.normalTextureHandle = UINT32_MAX;
        material.ormTextureHandle = UINT32_MAX;
        material.emissiveTextureHandle = UINT32_MAX;
    }
    else if (m_frameCount == 7)
    {
        auto& material = registry.Get<ecs::Material>(m_smokeTextureEntity);
        material.albedoTextureHandle = m_smokeSavedAlbedoTexture;
        material.normalTextureHandle = m_smokeSavedNormalTexture;
        material.ormTextureHandle = m_smokeSavedOrmTexture;
        material.emissiveTextureHandle = m_smokeSavedEmissiveTexture;
        core::Log::Info("[SMOKE] rt_texture_churn=passed");
    }

    return true;
}

// =============================================================================
// ApplySmokeGrowthStress — the mid-run entity churn, in BOTH smoke modes
// =============================================================================
// Adds 80 renderables at frame 8 and destroys them at frame 16. It does two
// separate jobs, which is why it is no longer inside ApplySmokeRTMutationStress:
//
//   * In an RT run it churns TLAS topology, which is what rt_topology_churn
//     reports. That was its original and only purpose.
//   * In EVERY run it drives Renderer::EnsureFrameStructuredBuffer's
//     reallocate-and-DeferredRelease branch, mid-run, with earlier frames still
//     executing. 17 demo renderables need 34 object records, so frame one grows
//     the buffer to 34 + kCapacityHeadroom = 98; 97 renderables need 194, which
//     that cannot absorb. See src/render/gpu_draw_records.h.
//
// The second job is a RASTER hazard - Renderer::BeginFrame's root SRVs are what
// address the released buffers - so gating the whole thing on --smoke-rt left
// -RasterOnly performing only the frame-one grow. That grow has nothing
// outstanding behind it, so it cannot observe a missing fence: with
// DeferredRelease replaced by an immediate Reset, a -RasterOnly run passed
// completely green while the default run died on this churn.
//
// The marker names keep their rt_ prefix because tools/smoke_test.ps1 asserts
// rt_topology_churn by name in its path-tracing branch. Renaming them would be a
// silent contract change for a cosmetic gain.
//
// Frame 16 is far enough ahead of the capture frame that the churn entities are
// long destroyed before any pixel or draw-index assertion runs, and they sit at
// world x/z = 100000 regardless, well outside the view.
void App::ApplySmokeGrowthStress()
{
    if (m_frameCount == 8)
    {
        m_smokeGrowthEntities.reserve(80);
        for (uint32_t i = 0; i < 80; ++i)
        {
            char name[32] = {};
            std::snprintf(name, sizeof(name), "RTGrowth_%u", i);
            const ecs::Transform transform{
                { 100000.0 + static_cast<double>(i) * 4.0, 0.5, 100000.0 },
                core::Quatf::Identity(),
                { 1.0f, 1.0f, 1.0f }
            };
            m_smokeGrowthEntities.push_back(
                m_scene.CreateRenderable(name, m_smokeGrowthMesh,
                                         m_smokeGrowthMaterial, transform));
        }
        core::Log::Infof("[SMOKE] rt_growth_entities=%zu", m_smokeGrowthEntities.size());
    }
    else if (m_frameCount == 16)
    {
        for (const ecs::Entity entity : m_smokeGrowthEntities)
            m_scene.DestroyEntity(entity);
        m_smokeGrowthEntities.clear();
        core::Log::Info("[SMOKE] rt_topology_churn=passed");
    }
}

render::DebugOverlayState App::BuildOverlayState(const core::TimeStep& timeStep) const
{
    render::DebugOverlayState state = {};
    state.visible = m_showDebugOverlay;
    state.pathTracing = m_usePathTracing;
    state.rtAvailable = m_rtAvailable;
    state.rtInitAttempted = m_rtInitAttempted;
    state.mouseCaptured = m_window.IsCaptured();
    state.fps = timeStep.fps;
    state.frameMs = timeStep.dt * 1000.0;
    state.entityCount = m_scene.EntityCount();
    state.rtQuality = m_rtQualityInfo;
    state.cameraPosition = m_camera.Position();

    // Navigation block — only meaningful once the reference star system is seeded.
    if (m_options.starSystem)
    {
        state.navActive = true;

        static const char* kModeNames[] = { "SHIP", "ORRERY", "NEAR-BODY", "FREE", "SURFACE" };
        const uint32_t modeIdx = static_cast<uint32_t>(m_cameraMode);
        state.cameraModeName =
            modeIdx < std::size(kModeNames) ? kModeNames[modeIdx] : "?";
        state.timeWarp = m_timer.GetTimeScale();

        // The F5-selected focus body (near-body/orrery framing), defaulting to Earth.
        const uint64_t focusId =
            m_focusBodyId ? m_focusBodyId : (scene::kStarSystemBodyIdBase + 10);
        const uint64_t localId = focusId - scene::kStarSystemBodyIdBase;
        state.focusName = SeededBodyName(localId);

        // Look the body up in the (const) registry the same way BuildOrbitTraceVertices
        // does: distance is camera -> body centre, orbit comes from its OrbitState.
        const auto& registry = m_scene.GetRegistry();
        if (const auto* pool = registry.GetPool<ecs::GravitationalBody>())
        {
            for (uint32_t i = 0; i < pool->Count(); ++i)
            {
                if (pool->DataAt(i).bodyId != focusId)
                    continue;
                const ecs::Entity e = registry.EntityAtIndex(pool->EntityAt(i));
                if (const auto* t = registry.TryGet<ecs::Transform>(e))
                    state.focusDistance = (t->position - m_camera.Position()).Length();
                if (const auto* os = registry.TryGet<ecs::OrbitState>(e))
                {
                    state.focusHasOrbit = true;
                    state.focusSemiMajorAxis = os->elements.semiMajorAxis;
                    state.focusEccentricity  = os->elements.eccentricity;
                    // Closed-ellipse period T = 2*pi*sqrt(a^3/mu); undefined for a
                    // hyperbola (a<0) or a missing primary mu, reported as 0.
                    const double a = os->elements.semiMajorAxis;
                    if (a > 0.0 && os->primaryMu > 0.0)
                    {
                        constexpr double kTwoPi = 6.283185307179586;
                        state.focusPeriodSeconds =
                            kTwoPi * std::sqrt(a * a * a / os->primaryMu);
                    }
                }
                break;
            }
        }

        // Ship-orbit sub-block: the player ship's own live osculating orbit (computed
        // once per frame into m_shipOrbit), shown only once it has been flown into the
        // system. This is the orbital-agency readout.
        if (m_shipInSystem && m_shipOrbit.valid)
        {
            state.shipOrbitActive   = true;
            state.shipPrimaryName   =
                SeededBodyName(m_shipOrbit.primaryBodyId - scene::kStarSystemBodyIdBase);
            state.shipAltitude      = m_shipOrbit.altitude;
            state.shipSpeed         = m_shipOrbit.speed;
            state.shipSemiMajorAxis = m_shipOrbit.elements.semiMajorAxis;
            state.shipEccentricity  = m_shipOrbit.elements.eccentricity;
            state.shipPeriapsis     = m_shipOrbit.periapsis;
            state.shipApoapsis      = m_shipOrbit.apoapsis;
            state.shipPeriodSeconds = m_shipOrbit.period;
        }

        // Target readout: the HUD's locked target (info cached by RenderHud).
        if (m_hudTargetInfo.valid)
        {
            state.targetActive       = true;
            state.targetName         = SeededBodyName(m_targetBodyId - scene::kStarSystemBodyIdBase);
            state.targetRange        = m_hudTargetInfo.rangeMeters;
            state.targetClosingSpeed = m_hudTargetInfo.closingSpeed;
            state.targetRelation     = static_cast<int>(m_hudTargetInfo.relation);
        }
    }

    return state;
}

namespace
{
// Exact key for a quadtree leaf: face, level, and the integer cell indices at that
// level. Two selections that land on the same patch produce the same key, so a
// resident patch is reused across frames rather than regenerated.
uint64_t TerrainLeafKey(const terrain::QuadPatch& p)
{
    const double cells = std::ldexp(1.0, p.level - 1);          // 2^(level-1)
    const long long iu = std::llround((p.u0 + 1.0) * cells);
    const long long iv = std::llround((p.v0 + 1.0) * cells);
    return (static_cast<uint64_t>(p.face)  << 56)
         | (static_cast<uint64_t>(p.level) << 48)
         | ((static_cast<uint64_t>(iu) & 0xFFFFFFull) << 24)
         |  (static_cast<uint64_t>(iv) & 0xFFFFFFull);
}
} // namespace

void App::RenderTerrainPreview()
{
    if (!m_terrainBuilt) return;
    auto& registry = m_scene.GetRegistry();
    core::Vec3d bodyPos;
    bool found = false;
    if (auto* pool = registry.GetPool<ecs::GravitationalBody>())
        for (uint32_t i = 0; i < pool->Count(); ++i)
            if (pool->DataAt(i).bodyId == m_terrainBodyId)
            {
                const ecs::Entity e = registry.EntityAtIndex(pool->EntityAt(i));
                if (const auto* t = registry.TryGet<ecs::Transform>(e))
                { bodyPos = t->position; found = true; }
                break;
            }
    if (!found) return;

    ++m_terrainStreamFrame;
    const TerrainBodyCfg cfg = ConfigForTerrainBody(m_terrainBodyId - scene::kStarSystemBodyIdBase);
    const double bodyR    = cfg.radius;
    const double amplitude = cfg.amplitude;

    // Surface params (from the body config); sunDir toward the star at the frame
    // origin, computed in double before narrowing (RULE 1). These match the sphere's
    // planet_ps colours so the terrain shades identically.
    render::Renderer::PlanetConstants pc = {};
    const core::Vec3f sunDir = (core::Vec3d{ 0.0, 0.0, 0.0 } - bodyPos).Normalized().ToFloat();
    pc.sunDir[0] = sunDir.x; pc.sunDir[1] = sunDir.y; pc.sunDir[2] = sunDir.z;
    pc.sunColor[0] = 1.0f; pc.sunColor[1] = 0.97f; pc.sunColor[2] = 0.92f; pc.sunColor[3] = 1.3f;
    pc.params0[0] = static_cast<float>(cfg.type); pc.params0[1] = cfg.seaLevel;
    pc.params0[2] = cfg.seed; pc.params0[3] = 1.0f;
    pc.deepColor[0]    = cfg.deep.x;    pc.deepColor[1]    = cfg.deep.y;    pc.deepColor[2]    = cfg.deep.z;
    pc.deepColor[3]    = cfg.depthScale;
    pc.shallowColor[0] = cfg.shallow.x; pc.shallowColor[1] = cfg.shallow.y; pc.shallowColor[2] = cfg.shallow.z;
    pc.shallowColor[3] = cfg.coastWidth;
    pc.landLow[0]  = cfg.landLow.x;  pc.landLow[1]  = cfg.landLow.y;  pc.landLow[2]  = cfg.landLow.z;
    pc.landHigh[0] = cfg.landHigh.x; pc.landHigh[1] = cfg.landHigh.y; pc.landHigh[2] = cfg.landHigh.z;
    pc.landHigh[3] = 0.95f;
    pc.iceColor[0] = cfg.ice.x; pc.iceColor[1] = cfg.ice.y; pc.iceColor[2] = cfg.ice.z; pc.iceColor[3] = cfg.iceLat;
    pc.ambient[0] = cfg.ambient.x; pc.ambient[1] = cfg.ambient.y; pc.ambient[2] = cfg.ambient.z;

    // Reselect the LOD for the LIVE camera-relative-to-body position, so the patch
    // set adapts as the camera moves over the surface.
    const core::Vec3d camPos = m_camera.Position();
    const core::Vec3d camBody{ camPos.x - bodyPos.x, camPos.y - bodyPos.y, camPos.z - bodyPos.z };
    const core::Vec3d focusDir{ m_terrainFocusBody.x, m_terrainFocusBody.y, m_terrainFocusBody.z };
    const double fdl = focusDir.Length();
    const core::Vec3d fdir = (fdl > 0.0)
        ? core::Vec3d{ focusDir.x / fdl, focusDir.y / fdl, focusDir.z / fdl }
        : core::Vec3d{ 0, 0, 1 };

    terrain::QuadtreeConfig qc;
    qc.planetRadius = bodyR; qc.amplitude = amplitude;
    qc.pixelError = 9.0; qc.maxLevel = 7;
    // pixelError is a REAL-pixel threshold only if the screen-error projection
    // uses the live viewport and FOV: SelectQuadtreeLOD forms
    // screenError = (worldError/dist) * viewportHeight/(2*tanHalfFovY), so leaving
    // these at the struct defaults (1080p, 60deg) would under-tessellate on a
    // taller/wider display and over-tessellate on a short window. Mirror the SSAO
    // path's projection (m_device.Height(), the camera's 70deg FOV).
    qc.viewportHeight = static_cast<double>(m_device.Height());
    qc.tanHalfFovY = std::tan(
        static_cast<double>(m_camera.GetFOV()) * 0.5 * core::DEG_TO_RAD);
    // Cap the leaf set to the drawn-patch budget. Each visible patch consumes one
    // aligned CB-ring slot in DrawTerrain (UploadCB of PlanetConstants) and one
    // object+material record; the ring is 1024 slots shared with per-frame and
    // per-pass uploads, and the Surface-mode structured-buffer reserve is +1024.
    // 512 leaves both comfortable headroom so the LOD can never request more
    // patches than either buffer can hold (the shipped surface view uses ~340).
    qc.maxLeaves = 512;
    std::vector<terrain::QuadPatch> patches;
    terrain::SelectQuadtreeLOD(qc, camBody, patches);

    // Collect the visible patch set (after the cheap horizon cull), and split it
    // into cache hits and misses. The misses' meshes are generated in PARALLEL
    // below, then uploaded + cached + drawn on the main thread.
    struct VisiblePatch { uint64_t key; terrain::QuadPatch qp; };
    std::vector<VisiblePatch> visible;
    visible.reserve(patches.size());
    std::vector<size_t> missIdx;   // indices into `visible` that are not yet cached
    for (const terrain::QuadPatch& qp : patches)
    {
        const core::Vec3d cdir = terrain::FaceToDirection(
            qp.face, 0.5 * (qp.u0 + qp.u1), 0.5 * (qp.v0 + qp.v1));
        if (cdir.x * fdir.x + cdir.y * fdir.y + cdir.z * fdir.z < 0.0)
            continue;
        const uint64_t key = TerrainLeafKey(qp);
        if (m_terrainCache.find(key) == m_terrainCache.end())
            missIdx.push_back(visible.size());
        visible.push_back({ key, qp });
    }

    // Generate the missing patches' meshes in parallel. terrain::GenerateChunk and
    // the ChunkVertex->render::Vertex / objectDir conversion are pure CPU — no GPU,
    // no shared mutable state, no logger — so one patch per worker index is race-free
    // (JobSystem runs each index exactly once, and each writes only its own `gen`
    // slot). This turns the first Surface frame's ~hundreds-of-chunks build from a
    // serial hitch into a parallel one, and keeps the descent's per-frame streaming
    // off the critical path of a single core. GPU upload + cache insert stay on main.
    struct GenResult { std::vector<render::Vertex> verts; std::vector<uint16_t> indices; core::Vec3d origin; };
    std::vector<GenResult> gen(missIdx.size());
    auto genOne = [&](uint32_t m)
    {
        // The JobSystem contract is that a job MUST NOT throw (a throwing job
        // std::terminates its worker). GenerateChunk and the vector resizes below
        // allocate, so a std::bad_alloc is possible; catch it and leave this slot
        // empty (r.verts stays empty -> skipped in the upload loop, drawn next frame)
        // rather than taking the process down from a worker thread.
        try
        {
            const terrain::QuadPatch& qp = visible[missIdx[m]].qp;
            terrain::ChunkParams tp;
            tp.face = qp.face; tp.u0 = qp.u0; tp.u1 = qp.u1; tp.v0 = qp.v0; tp.v1 = qp.v1;
            tp.gridN = 33;
            tp.planetRadius = bodyR; tp.amplitudeMeters = amplitude;
            tp.type = cfg.type; tp.seed = cfg.seed; tp.seaLevel = cfg.seaLevel; tp.coastWidth = cfg.coastWidth;
            terrain::ChunkMesh chunk = terrain::GenerateChunk(tp);

            GenResult& r = gen[m];
            r.origin  = chunk.origin;
            r.indices = std::move(chunk.indices);
            r.verts.resize(chunk.vertices.size());
            for (size_t i = 0; i < chunk.vertices.size(); ++i)
            {
                const terrain::ChunkVertex& cv = chunk.vertices[i];
                const core::Vec3d bp{ chunk.origin.x + cv.position.x,
                                      chunk.origin.y + cv.position.y,
                                      chunk.origin.z + cv.position.z };
                const double bl = bp.Length();
                const core::Vec3f od = (bl > 0.0)
                    ? core::Vec3f{ static_cast<float>(bp.x / bl),
                                   static_cast<float>(bp.y / bl),
                                   static_cast<float>(bp.z / bl) }
                    : core::Vec3f{ 0.0f, 0.0f, 1.0f };
                r.verts[i].position = cv.position;
                r.verts[i].normal   = cv.normal;
                r.verts[i].color    = core::Color{ od.x, od.y, od.z, 1.0f };
                r.verts[i].uv       = cv.uv;
            }
        }
        catch (...)
        {
            gen[m].verts.clear(); // leave this patch for a later frame
        }
    };
    if (missIdx.size() > 1)
    {
        if (!m_terrainJobs) m_terrainJobs = std::make_unique<core::JobSystem>();
        m_terrainJobs->ParallelFor(static_cast<uint32_t>(missIdx.size()), genOne, 1);
    }
    else
    {
        for (uint32_t m = 0; m < missIdx.size(); ++m) genOne(m); // one miss: no dispatch overhead
    }

    // Upload the freshly-generated meshes and insert them into the cache — main
    // thread only (D3D12 resource creation and the shared cache map).
    for (size_t m = 0; m < missIdx.size(); ++m)
    {
        GenResult& r = gen[m];
        if (r.verts.empty()) continue;
        render::Mesh lm = render::CreateUploadMesh(
            m_device.Device(),
            r.verts.data(), static_cast<uint32_t>(r.verts.size()),
            r.indices.data(), static_cast<uint32_t>(r.indices.size()));
        if (!lm.IsValid()) continue;
        m_terrainCache.emplace(visible[missIdx[m]].key, TerrainLeaf{ lm, r.origin, 0 });
    }

    // Draw all visible cached patches, camera-relative.
    for (const VisiblePatch& vp : visible)
    {
        auto it = m_terrainCache.find(vp.key);
        if (it == m_terrainCache.end()) continue; // generation or upload failed
        it->second.lastSeen = m_terrainStreamFrame;

        const core::Vec3d tD{ bodyPos.x + it->second.originBody.x - camPos.x,
                              bodyPos.y + it->second.originBody.y - camPos.y,
                              bodyPos.z + it->second.originBody.z - camPos.z };
        const core::Mat4x4 world = core::Mat4x4::Translation(tD.ToFloat());
        m_renderer.DrawTerrain(m_device, it->second.mesh, world, pc);
    }

    // Evict patches not drawn for several frames — well past the GPU's in-flight
    // window (kFrameCount), so dropping the Mesh (releasing its buffers) can never
    // free memory the GPU is still reading. Bounds the cache as the camera descends.
    for (auto it = m_terrainCache.begin(); it != m_terrainCache.end();)
    {
        if (it->second.lastSeen + 6u < m_terrainStreamFrame)
            it = m_terrainCache.erase(it);
        else
            ++it;
    }
}

std::vector<gameplay::TargetCandidate> App::GatherTargetCandidates()
{
    std::vector<gameplay::TargetCandidate> out;
    if (!m_options.starSystem) return out;

    ecs::Registry& reg = m_scene.GetRegistry();
    auto* pool = reg.GetPool<ecs::GravitationalBody>();
    if (!pool) return out;
    out.reserve(pool->Count());
    for (uint32_t i = 0; i < pool->Count(); ++i)
    {
        const ecs::GravitationalBody& g = pool->DataAt(i);
        if (g.bodyId == kPlayerShipBodyId) continue; // never target the player ship
        const ecs::Entity e = reg.EntityAtIndex(pool->EntityAt(i));
        const ecs::Transform* t = reg.TryGet<ecs::Transform>(e);
        if (!t) continue;

        gameplay::TargetCandidate c;
        c.id       = g.bodyId;
        c.name     = SeededBodyName(g.bodyId - scene::kStarSystemBodyIdBase);
        c.worldPos = t->position;
        if (const ecs::RigidBody* rb = reg.TryGet<ecs::RigidBody>(e))
            c.worldVel = rb->linearVelocity;
        c.radius   = g.radius;
        c.relation = gameplay::TargetRelation::Neutral; // bodies are neutral; ships later
        out.push_back(c);
    }
    return out;
}

void App::RenderHud()
{
    if (!m_hudReady || !m_options.starSystem) return;
    if (m_cameraMode == CameraMode::Orrery) return; // the map view has its own overlays

    const float w = static_cast<float>(m_device.Width());
    const float h = static_cast<float>(m_device.Height());
    const float cx = w * 0.5f, cy = h * 0.5f;
    const core::Mat4x4 vp = m_camera.ViewProjectionMatrix(w / h);
    const core::Vec3d camPos = m_camera.Position();
    const double K = m_scene.RenderScale(); // 1 in the true-scale flying views
    m_hudTargetInfo = {}; // recomputed below if a target is locked and the ship exists

    // Project an absolute world point to screen pixels (camera-relative, RULE 1).
    auto toScreen = [&](const core::Vec3d& world) -> render::ScreenPoint {
        const core::Vec3f rel{
            static_cast<float>((world.x - camPos.x) * K),
            static_cast<float>((world.y - camPos.y) * K),
            static_cast<float>((world.z - camPos.z) * K) };
        return render::WorldToScreen(vp, rel, w, h);
    };
    // Clamp an off-screen (but in-front) point to an inset screen border, returning
    // the edge pixel and the outward angle for a chevron.
    auto clampToBorder = [&](float sx, float sy, float& outX, float& outY, float& outAng) {
        const float margin = 48.0f;
        float dx = sx - cx, dy = sy - cy;
        if (std::fabs(dx) < 1e-3f && std::fabs(dy) < 1e-3f) { dx = 0.0f; dy = -1.0f; }
        const float hx = cx - margin, hy = cy - margin;
        const float s = (std::min)(hx / (std::fabs(dx) + 1e-4f), hy / (std::fabs(dy) + 1e-4f));
        outX = cx + dx * s; outY = cy + dy * s;
        outAng = std::atan2(dy, dx);
    };

    // --- ship state ---------------------------------------------------------
    ecs::Registry& reg = m_scene.GetRegistry();
    core::Vec3d shipPos{ 0, 0, 0 }, shipVel{ 0, 0, 0 };
    core::Vec3f shipFwd{ 0.0f, 0.0f, 1.0f };
    bool haveShip = false;
    if (m_shipInSystem && reg.IsAlive(m_playerShip))
    {
        if (const ecs::Transform* t = reg.TryGet<ecs::Transform>(m_playerShip))
        {
            shipPos = t->position;
            shipFwd = core::Mat4x4::FromQuaternion(t->rotation.Normalized())
                          .TransformDirection({ 0.0f, 0.0f, 1.0f });
            haveShip = true;
        }
        if (const ecs::RigidBody* rb = reg.TryGet<ecs::RigidBody>(m_playerShip))
            shipVel = rb->linearVelocity;
    }

    const core::Color kReticleCol { 0.80f, 0.90f, 1.00f, 0.75f };
    const core::Color kProCol     { 0.45f, 1.00f, 0.65f, 0.90f };
    const core::Color kRetCol     { 0.95f, 0.55f, 0.40f, 0.75f };
    const core::Color kNeutral    { 0.55f, 0.85f, 1.00f, 0.95f };
    const core::Color kHostile    { 1.00f, 0.35f, 0.28f, 0.95f };
    const core::Color kFriendly   { 0.40f, 1.00f, 0.55f, 0.95f };
    const core::Color kLeadCol    { 0.40f, 0.80f, 1.00f, 1.00f };

    m_hud.Begin();

    // --- boresight reticle: where the ship's nose points -------------------
    {
        render::ScreenPoint sp{ cx, cy, true, true };
        if (haveShip)
        {
            const core::Vec3d nose{ shipPos.x + static_cast<double>(shipFwd.x) * 1.0e6,
                                    shipPos.y + static_cast<double>(shipFwd.y) * 1.0e6,
                                    shipPos.z + static_cast<double>(shipFwd.z) * 1.0e6 };
            const render::ScreenPoint proj = toScreen(nose);
            if (proj.visible) sp = proj;
        }
        m_hud.AddReticle(sp.x, sp.y, 20.0f, 7.0f, kReticleCol, 2.0f);
        m_hud.AddRect(sp.x - 1.0f, sp.y - 1.0f, 2.0f, 2.0f, kReticleCol); // centre dot
    }

    // --- prograde / retrograde velocity vectors ----------------------------
    const double speed = std::sqrt(shipVel.x * shipVel.x + shipVel.y * shipVel.y + shipVel.z * shipVel.z);
    if (haveShip && speed > 0.5)
    {
        const double inv = 1.0 / speed;
        const core::Vec3d vhat{ shipVel.x * inv, shipVel.y * inv, shipVel.z * inv };
        const double R = 1.0e6;
        const render::ScreenPoint pro = toScreen({ shipPos.x + vhat.x * R, shipPos.y + vhat.y * R, shipPos.z + vhat.z * R });
        const render::ScreenPoint ret = toScreen({ shipPos.x - vhat.x * R, shipPos.y - vhat.y * R, shipPos.z - vhat.z * R });
        if (pro.visible && pro.onScreen)
        {
            m_hud.AddCircle(pro.x, pro.y, 11.0f, kProCol, 2.0f, 24);
            m_hud.AddReticle(pro.x, pro.y, 17.0f, 11.0f, kProCol, 2.0f); // ticks outside the ring
            m_hud.AddRect(pro.x - 1.5f, pro.y - 1.5f, 3.0f, 3.0f, kProCol);
        }
        else if (pro.visible)
        {
            float ex, ey, ang; clampToBorder(pro.x, pro.y, ex, ey, ang);
            m_hud.AddChevron(ex, ey, 12.0f, ang, kProCol, 3.0f);
        }
        if (ret.visible && ret.onScreen)
        {
            m_hud.AddCircle(ret.x, ret.y, 9.0f, kRetCol, 2.0f, 20);
            m_hud.AddLine(ret.x - 9.0f, ret.y, ret.x + 9.0f, ret.y, kRetCol, 2.0f);
            m_hud.AddLine(ret.x, ret.y - 9.0f, ret.x, ret.y + 9.0f, kRetCol, 2.0f);
        }
    }

    // --- target bracket + lead pip -----------------------------------------
    if (m_targetBodyId != 0)
    {
        const std::vector<gameplay::TargetCandidate> cands = GatherTargetCandidates();
        const int idx = gameplay::FindCandidate(cands, m_targetBodyId);
        if (idx >= 0)
        {
            const gameplay::TargetCandidate& tc = cands[static_cast<size_t>(idx)];
            if (haveShip) m_hudTargetInfo = gameplay::ComputeTargetInfo(shipPos, shipVel, tc);
            const core::Color col = (tc.relation == gameplay::TargetRelation::Hostile)  ? kHostile
                                  : (tc.relation == gameplay::TargetRelation::Friendly) ? kFriendly
                                                                                        : kNeutral;
            const render::ScreenPoint sp = toScreen(tc.worldPos);
            if (sp.visible && sp.onScreen)
            {
                // Bracket sized by the target's on-screen angular radius (project a
                // point one body-radius to the side), clamped to a legible range.
                float half = 24.0f;
                if (tc.radius > 0.0)
                {
                    // camera "right" from the view matrix rows is complex; approximate
                    // angular size from range and radius via the vertical FOV.
                    const double range = std::sqrt(
                        (tc.worldPos.x - camPos.x) * (tc.worldPos.x - camPos.x) +
                        (tc.worldPos.y - camPos.y) * (tc.worldPos.y - camPos.y) +
                        (tc.worldPos.z - camPos.z) * (tc.worldPos.z - camPos.z));
                    if (range > 1.0)
                    {
                        const double tanHalfFov = std::tan(m_camera.GetFOV() * 0.5 * core::DEG_TO_RAD);
                        const double px = (tc.radius / range) / tanHalfFov * (h * 0.5);
                        half = static_cast<float>((std::min)((std::max)(px * 1.35, 16.0), 240.0));
                    }
                }
                m_hud.AddBracket(sp.x, sp.y, half, half * 0.4f, col, 2.0f);
            }
            else if (sp.visible)
            {
                float ex, ey, ang; clampToBorder(sp.x, sp.y, ex, ey, ang);
                m_hud.AddChevron(ex, ey, 14.0f, ang, col, 3.0f);
            }

            // Lead pip: where to aim to hit the target with the reference round.
            if (haveShip)
            {
                const gameplay::FiringSolution fs = gameplay::ComputeFiringSolution(
                    shipPos, shipVel, tc.worldPos, tc.worldVel, kHudMuzzleSpeed);
                if (fs.valid)
                {
                    const render::ScreenPoint lp = toScreen(fs.worldPos);
                    if (lp.visible && lp.onScreen)
                        m_hud.AddDiamondOutline(lp.x, lp.y, 7.0f, kLeadCol, 2.0f);
                }
                else if (sp.visible && sp.onScreen)
                {
                    // No firing solution (target outruns the round): a red no-solution
                    // ring on the bracket, matching the genre convention.
                    m_hud.AddCircle(sp.x, sp.y, 6.0f, core::Color{ 1.0f, 0.35f, 0.28f, 0.95f }, 2.0f, 16);
                }
            }
        }
    }

    m_hud.Draw(m_device, w, h, m_frameCount);
}

bool App::RenderFrame(const core::TimeStep& timeStep)
{
    if (!m_device.WaitForCurrentFrame() || !m_device.ResetCommandList())
    {
        core::Log::Error("Unable to begin render command list");
        return false;
    }
    auto* commandList = m_device.CmdList();
    m_renderer.ReclaimTextureDescriptors(m_device);

    // Refresh the ship's live osculating orbit once per frame (only if it was flown
    // into the system). Both the navigation HUD and the orbit-trace overlay read it.
    m_shipOrbit = m_shipInSystem ? m_scene.DeriveOsculatingOrbit(kPlayerShipBodyId)
                                 : scene::OsculatingOrbit{};

    // Advance the renderer's per-frame slot BEFORE any pass records anything,
    // and unconditionally - above the raster/path-tracing branch. The shadow
    // pass below runs before BeginFrame, so leaving the advance to BeginFrame
    // meant the shadow pass wrote into the previous frame's buffer. The
    // path-tracing branch never calls BeginFrame at all, so the slot also went
    // stale across RT frames and the first shadow pass after an F1 toggle back
    // to raster wrote at whatever offset the last raster frame left behind.
    // Sizing hint for the per-draw structured buffers.
    //
    // Smoke inflates it on a ramp so the buffers REALLOCATE repeatedly by
    // default. That branch - allocate kFrameCount replacements, unmap and
    // DeferredRelease the old ones, swap - is the only code in this change that
    // can use-after-free, because kFrameCount frames may still be reading the
    // outgoing buffers, and in an ordinary run it never executes at all: the
    // demo scene's draw count sits under kMinObjectCapacity, so
    // EnsureFrameStructuredBuffer early-outs every frame after the first.
    //
    // BOTH SMOKE MODES, not raster only. This ramp was gated on
    // `m_options.smoke && !m_options.smokeRT`, which silently left the DXR run
    // with only the reallocations App::ApplySmokeGrowthStress's frame-8 churn
    // produces: MEASURED, the default run performed 4 replacements with 2 in
    // flight, against 15 and 13 for -RasterOnly. The buffers are allocated,
    // grown and deferred-released by BeginFrameResources on EVERY frame
    // regardless of which path consumes them, so the fence hazard is identical
    // in both modes and there is no reason for the DXR run to carry a quarter of
    // the coverage. Making it unconditional brings the default run to the same
    // ramp as -RasterOnly.
    //
    // --smoke-force-grow steepens it further. It is the opt-in heavy case, and
    // it is deliberately NOT the coverage floor: the harness asserts against
    // what the DEFAULT ramp produces, so the switch can only ever add.
    //
    // The ramp is deliberately fast enough to cross a capacity boundary while
    // earlier frames are still in flight, which is precisely the condition the
    // DeferredRelease fence guard exists for. Run it under --gpu-validation to
    // put the swap under GPU-based validation as well.
    uint32_t maxDrawsHint = m_scene.MeshInstanceCount();
    if (m_options.smoke)
    {
        const uint32_t rampPerFrame = m_options.smokeForceGrow ? 16u : 4u;
        maxDrawsHint += static_cast<uint32_t>(m_frameCount) * rampPerFrame;
    }
    // Terrain leaves each cost one object/material record (Surface mode only). The
    // streamed leaf set fills in during the frame (after this hint is taken), so
    // reserve a generous fixed allowance for the visible-hemisphere patch count.
    if (m_cameraMode == CameraMode::Surface)
        maxDrawsHint += 1024u;
    const bool renderedPathTracing = m_usePathTracing && m_rtAvailable;
    const bool probeDrawRecords = m_verifyDrawRecordsThisFrame &&
                                  !renderedPathTracing;
    // The IBL consumption probe. TWO frames, and the pair is the assertion: the
    // control frame renders with the environment switched off and asserts every
    // word reads zero, the live frame asserts every word is nonzero and that the
    // cube really is the environment cube. See src/render/ibl_consume_probe.h -
    // an assertion that merely passes with the feature present is the exact thing
    // this repository keeps shipping.
    const bool probeIBLControl = m_verifyIBLControlThisFrame && !renderedPathTracing;
    const bool probeIBLConsume = m_verifyIBLConsumeThisFrame && !renderedPathTracing;

    // The DXR twins, and the SAME pair structure for the same reason. Gated on
    // `renderedPathTracing` - the very predicate that selects the render branch
    // below - rather than on m_usePathTracing, so they run exactly when there is
    // a dispatch to observe. That is the lesson the shadow probe was rebuilt to
    // learn: a probe armed on a frame its shader does not run on is an assertion
    // that exists and is never reached.
    const bool probeRTIBLControl = m_verifyRTIBLControlThisFrame && renderedPathTracing;
    const bool probeRTIBLConsume = m_verifyRTIBLConsumeThisFrame && renderedPathTracing;

    // ONE flag arms the pixel-stage probe permutation, because basic_ps writes
    // BOTH probes from the same PSO behind the same root-constant gate. Keeping
    // the ARMING unified is deliberate: two schedules that must stay in step is
    // the shape of the bug that left the shadow probe on a path-traced frame. The
    // READBACKS are separate, which is what lets the control frame run the pixel
    // probe without also re-reading the draw records.
    const bool probeRasterPixels = probeDrawRecords || probeIBLControl || probeIBLConsume;
    m_renderer.BeginFrameResources(m_device, maxDrawsHint, probeRasterPixels);

    // Not latched on the renderer side - the value is supplied every frame, so a
    // missed reset cannot leave the environment switched off for the rest of a
    // run. This is the ONLY caller that ever passes true.
    m_renderer.SetIBLDisabledForFrame(probeIBLControl);
    // The first IN-FLIGHT replacement, not the first replacement.
    //
    // This tracked StructuredBufferReallocations() when it arrived, and against
    // the capacity floors on this branch that would now always latch frame 0:
    // Renderer::Init allocates AT kMinObjectCapacity and the demo scene crosses
    // it immediately, so a total-based "first" is the harmless start-up grow and
    // an assertion that it happened late would be asserting the opposite of the
    // truth. The interesting question is when the first replacement ran with
    // frames already outstanding, which is what the in-flight counter isolates.
    if (m_options.smoke &&
        m_smokeFirstStructuredBufferReallocationFrame == UINT64_MAX &&
        m_renderer.StructuredBufferReallocationsInFlight() > 0)
    {
        m_smokeFirstStructuredBufferReallocationFrame = m_frameCount;
    }

    if (renderedPathTracing)
    {
        m_device.TransitionResource(
            m_device.CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        m_scene.BuildAccelerationStructures(m_device, m_camera.Position());

        const core::Vec3f lightDirection = core::Vec3f(0.5f, 0.8f, 0.3f).Normalized();
        const core::Vec3f lightColor = { 1.0f, 0.97f, 0.92f };
        const core::Vec3f ambientColor = { 0.12f, 0.14f, 0.22f };

        // The environment the DXR stable preview shades with. It is the
        // RENDERER'S EnvironmentIBL - the same cube and the same nine SH
        // coefficients basic_ps.hlsl uses - not a second bake. That is what makes
        // the raster/DXR agreement structural rather than something a test has to
        // police.
        render::RTEnvironmentInputs rtEnvironment = {};
        rtEnvironment.ibl        = &m_renderer.Environment();
        rtEnvironment.intensity  = m_renderer.IBLIntensity();
        rtEnvironment.disabled   = probeRTIBLControl;
        rtEnvironment.probeWrite = probeRTIBLControl || probeRTIBLConsume;

        // Zeroing the probe block has to happen BEFORE the dispatch that writes
        // it, on the same command list. See PathTracer::PrepareIBLProbe for why
        // an uncleared block would make the control frame report the live
        // frame's answer.
        if (rtEnvironment.probeWrite &&
            !m_scene.GetPathTracer()->PrepareIBLProbe(m_device))
            return false;

        m_scene.PathTraceEntities(
            m_device, m_camera, lightDirection, lightColor, ambientColor,
            m_rtQualityMode, rtEnvironment);

        if (rtEnvironment.probeWrite &&
            !m_scene.GetPathTracer()->RecordIBLProbeReadback(m_device))
        {
            core::Log::Error("DXR IBL probe readback could not be recorded");
            return false;
        }

        m_scene.CopyPathTraceToBackBuffer(m_device);

        if (m_debugOverlayReady && m_showDebugOverlay)
        {
            m_device.TransitionResource(
                m_device.CurrentBackBuffer(),
                D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            const render::DebugOverlayState overlayState = BuildOverlayState(timeStep);
            m_debugOverlay.Draw(m_device, overlayState);
            m_device.TransitionResource(
                m_device.CurrentBackBuffer(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT);
        }
    }
    else
    {
        // Light selection for the raster frame, set UNCONDITIONALLY (so no override
        // leaks across a mode/focus change) BEFORE the shadow pass (which builds its
        // cascades from the light) and BeginFrame (which uploads lightDir):
        //  - a true-scale star-system view focused on a PLANET -> light FROM THE STAR
        //    (frame origin) toward that planet, so the terminator and the atmosphere
        //    shell agree and both are physically correct;
        //  - otherwise (the orrery, the demo scene, or focus ON the star itself, whose
        //    direction would be a degenerate zero) -> the fixed default light.
        core::Vec3f lightDir   = core::Vec3f(0.5f, 0.8f, 0.3f).Normalized();
        core::Vec3f lightColor = { 1.0f, 0.97f, 0.92f };
        core::Vec3f ambient    = { 0.12f, 0.14f, 0.22f };
        if (m_options.starSystem && m_cameraMode != CameraMode::Orrery)
        {
            const uint64_t focusId = m_focusBodyId
                ? m_focusBodyId : (scene::kStarSystemBodyIdBase + 10);
            const auto& reg = m_scene.GetRegistry();
            if (auto* pool = reg.GetPool<ecs::GravitationalBody>())
                for (uint32_t i = 0; i < pool->Count(); ++i)
                    if (pool->DataAt(i).bodyId == focusId)
                    {
                        const ecs::Entity e = reg.EntityAtIndex(pool->EntityAt(i));
                        if (const auto* t = reg.TryGet<ecs::Transform>(e))
                        {
                            // The star holds the frame origin: focusing IT gives a zero
                            // direction (which would zero the light and degenerate the
                            // shadow cascades), so only realign for a body off the origin.
                            const core::Vec3d delta = core::Vec3d{ 0.0, 0.0, 0.0 } - t->position;
                            if (delta.LengthSq() > 1.0)
                            {
                                lightDir = delta.Normalized().ToFloat();
                                ambient  = { 0.02f, 0.03f, 0.05f }; // near-vacuum
                            }
                        }
                        break;
                    }
        }
        m_renderer.SetDirectionalLight(lightDir, lightColor, ambient);

        // Shadow depth first, into its own target. Must precede BeginScenePass:
        // it rebinds render targets, viewport and scissor, so running it after
        // would leave the scene pass pointing at a 2048x2048 depth-only target.
        if (m_renderer.ShadowsAvailable())
        {
            // One Begin/End pair around the whole set, one BeginShadowCascade per
            // slice. The casters are walked once per cascade; per-cascade frustum
            // culling would avoid that, but Mesh carries no bounding volume yet,
            // so it is pure waste rather than an artifact and is deferred.
            m_renderer.BeginShadowPass(m_device, m_camera.Position());
            for (uint32_t cascade = 0; cascade < core::kShadowCascadeCount; ++cascade)
            {
                m_renderer.BeginShadowCascade(m_device, cascade);
                m_scene.RenderShadowCasters(m_device, m_renderer, m_camera.Position());
            }
            m_renderer.EndShadowPass(m_device);
        }

        // Depth prepass + SSAO: render camera depth into a renderer-owned target and
        // derive the ambient-occlusion factor the opaque pass samples. Runs after the
        // shadow pass and before the scene pass (both rebind targets/viewport). Uses
        // the SAME camera view-projection and camera-relative matrices as the opaque
        // pass, so the prepass depth equals what that pass produces. Skipped if SSAO
        // resources are unavailable (the AO texture then stays white = no occlusion).
        if (m_renderer.SSAOAvailable())
        {
            const float w = static_cast<float>(m_device.Width());
            const float h = static_cast<float>(m_device.Height());
            D3D12_VIEWPORT ppVp = { 0.0f, 0.0f, w, h, 0.0f, 1.0f };
            D3D12_RECT ppSc = { 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
            commandList->RSSetViewports(1, &ppVp);
            commandList->RSSetScissorRects(1, &ppSc);

            const float aspect = w / h;
            m_renderer.BeginDepthPrepass(m_device, m_camera.ViewProjectionMatrix(aspect));
            m_scene.RenderDepthPrepass(m_device, m_renderer, m_camera.Position());
            m_renderer.EndDepthPrepass(m_device);

            const float tanHalfFovY =
                std::tan(m_camera.GetFOV() * 0.5f * 3.14159265358979f / 180.0f);
            m_renderer.RenderSSAO(m_device, m_camera.GetNearZ(), tanHalfFovY, aspect);
        }

        // Scene renders into the linear HDR target, not the back buffer. The
        // back buffer is not touched until ResolveToBackBuffer tone-maps into it,
        // which is also what transitions it out of PRESENT.
        m_renderer.BeginScenePass(m_device);

        D3D12_VIEWPORT viewport = {
            0.0f, 0.0f,
            static_cast<float>(m_device.Width()),
            static_cast<float>(m_device.Height()),
            0.0f, 1.0f
        };
        commandList->RSSetViewports(1, &viewport);

        D3D12_RECT scissor = {
            0, 0,
            static_cast<LONG>(m_device.Width()),
            static_cast<LONG>(m_device.Height())
        };
        commandList->RSSetScissorRects(1, &scissor);

        m_renderer.BeginFrame(m_device, m_camera);
        // Deep-space starfield background in the star-system views; the grey sky
        // gradient in the demo sandbox. Only the visible background differs — the IBL
        // environment is baked from the sky and is untouched either way.
        if (m_options.starSystem)
            m_renderer.DrawSpace(m_device);
        else
            m_renderer.DrawSky(m_device);
        m_scene.RenderEntities(m_device, m_renderer, m_camera.Position());
        if (m_cameraMode == CameraMode::Surface)
            RenderTerrainPreview();

        // Planetary atmospheres: analytic single-scattering shells over the just-drawn
        // planets (blue limb glow, day/night terminator). Drawn before the orbit/marker
        // overlays so those read on top, and only in the true-scale views — the Scene
        // self-gates on K == 1. Raster-only (composites into the HDR target).
        if (m_cameraMode != CameraMode::Orrery && !m_usePathTracing)
        {
            const float aspect = static_cast<float>(m_device.Width()) /
                                 static_cast<float>(m_device.Height());
            m_scene.RenderAtmospheres(m_device, m_renderer, m_camera.Position(),
                                      m_camera.ViewProjectionMatrix(aspect));
        }

        // Orbit-trace overlay: in the orrery/map view, draw each seeded body's
        // Keplerian orbit as translucent lines over the raster scene. Raster-only
        // (the line pass writes the HDR raster target); path tracing has no
        // rasterized geometry to overlay, so it is skipped there.
        if (m_cameraMode == CameraMode::Orrery && !m_usePathTracing)
        {
            const uint64_t focusId = m_focusBodyId
                ? m_focusBodyId : (scene::kStarSystemBodyIdBase + 10);
            const std::vector<render::Renderer::LineVertex> orbitVerts =
                m_scene.BuildOrbitTraceVertices(m_camera.Position(), focusId);
            if (!orbitVerts.empty())
            {
                const float aspect = static_cast<float>(m_device.Width()) /
                                     static_cast<float>(m_device.Height());
                m_renderer.DrawLines(m_device, orbitVerts.data(),
                                     static_cast<uint32_t>(orbitVerts.size()),
                                     m_camera.ViewProjectionMatrix(aspect));
            }
        }

        // Body-marker overlay: in the true-scale views (near-body / ship / free) a
        // distant planet or the star is sub-pixel, so draw each seeded body as a
        // constant-pixel dot at its real depth. Reversed-Z tested, so the body the
        // camera sits on hides its own center marker while far bodies stay visible.
        // Raster-only for the same reason the orbit lines are (the pass writes the
        // HDR raster target). The orrery already draws exaggerated body spheres, so
        // it is excluded to avoid doubling a marker onto a marker.
        if (m_cameraMode != CameraMode::Orrery && !m_usePathTracing)
        {
            const std::vector<render::Renderer::BillboardVertex> markerVerts =
                m_scene.BuildBodyMarkerVertices(m_camera.Position());
            if (!markerVerts.empty())
            {
                const float w = static_cast<float>(m_device.Width());
                const float h = static_cast<float>(m_device.Height());
                m_renderer.DrawBillboards(m_device, markerVerts.data(),
                                          static_cast<uint32_t>(markerVerts.size()),
                                          m_camera.ViewProjectionMatrix(w / h), w, h);
            }
        }

        // Ship-orbit trace: the player ship's own predicted path (green), through the
        // same reversed-Z line pass as the body orbits. K self-scales it — sub-pixel
        // and invisible in the compressed orrery, framed nicely around the primary in
        // the near-body / ship views. Raster-only, and only once the ship is flying
        // an orbit in the system.
        if (m_shipOrbit.valid && !m_usePathTracing)
        {
            const std::vector<render::Renderer::LineVertex> shipVerts =
                m_scene.BuildDerivedOrbitTraceVertices(
                    m_camera.Position(), m_shipOrbit, 0.30f, 1.00f, 0.55f, 0.85f);
            if (!shipVerts.empty())
            {
                const float aspect = static_cast<float>(m_device.Width()) /
                                     static_cast<float>(m_device.Height());
                m_renderer.DrawLines(m_device, shipVerts.data(),
                                     static_cast<uint32_t>(shipVerts.size()),
                                     m_camera.ViewProjectionMatrix(aspect));
            }
        }

        // Apsis markers: periapsis (cyan) / apoapsis (magenta) billboards on the
        // ship's orbit and the focused body's orbit — the KSP-style map markers.
        // Constant-pixel like the body markers; self-scales with K, so useful in the
        // orrery (focus apsides) and the ship/near-body views (ship apsides) alike.
        if (!m_usePathTracing)
        {
            const uint64_t focusId = m_focusBodyId
                ? m_focusBodyId : (scene::kStarSystemBodyIdBase + 10);
            const std::vector<render::Renderer::BillboardVertex> apsisVerts =
                m_scene.BuildApsisMarkerVertices(m_camera.Position(), m_shipOrbit, focusId);
            if (!apsisVerts.empty())
            {
                const float w = static_cast<float>(m_device.Width());
                const float h = static_cast<float>(m_device.Height());
                m_renderer.DrawBillboards(m_device, apsisVerts.data(),
                                          static_cast<uint32_t>(apsisVerts.size()),
                                          m_camera.ViewProjectionMatrix(w / h), w, h);
            }
        }

        // Maneuver-node preview: when toggled (M), overlay in amber the orbit a
        // prograde burn would produce, so the player can plan before committing. The
        // preview delta-v scales with the current speed so it reads on any orbit.
        if (m_showManeuverPreview && m_shipOrbit.valid && !m_usePathTracing)
        {
            const scene::OsculatingOrbit preview =
                m_scene.PreviewProgradeBurn(m_shipOrbit, 0.15 * m_shipOrbit.speed);
            const std::vector<render::Renderer::LineVertex> previewVerts =
                m_scene.BuildDerivedOrbitTraceVertices(
                    m_camera.Position(), preview, 1.00f, 0.70f, 0.15f, 0.90f);
            if (!previewVerts.empty())
            {
                const float aspect = static_cast<float>(m_device.Width()) /
                                     static_cast<float>(m_device.Height());
                m_renderer.DrawLines(m_device, previewVerts.data(),
                                     static_cast<uint32_t>(previewVerts.size()),
                                     m_camera.ViewProjectionMatrix(aspect));
            }
        }

        // Flight/targeting HUD: reticle, velocity vectors, target bracket + lead
        // pip, drawn over the scene in the HDR target (on top of the orbit/marker
        // overlays), raster-only. Screen-space, so it sits above everything.
        if (!m_usePathTracing) RenderHud();

        // Tone-map the linear scene into the back buffer. Post-process passes
        // (bloom, exposure, TAA) belong between the line above and this one,
        // which is the whole reason the HDR intermediate exists.
        m_renderer.ResolveToBackBuffer(m_device);

        // Overlay draws after the resolve, directly in display space, so it is
        // not tone-mapped. The back buffer is already RENDER_TARGET here.
        if (m_debugOverlayReady && m_showDebugOverlay)
        {
            const render::DebugOverlayState overlayState = BuildOverlayState(timeStep);
            m_debugOverlay.Draw(m_device, overlayState);
        }

        m_device.TransitionResource(
            m_device.CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
    }

    if (m_captureThisFrame && !m_device.RecordBackBufferReadback())
        m_captureThisFrame = false;

    // Probe the shadow map on the last RASTER frame - see smokeRasterVerifyFrame
    // in App::Run. Only in smoke mode; it costs a copy and a GPU-visible
    // readback.
    //
    // Gated on !renderedPathTracing, the SAME predicate that chose the render
    // branch above, not on !m_usePathTracing. Those differ when path tracing is
    // requested but unavailable: the else branch runs, the shadow pass renders,
    // and the map is perfectly readable, yet the old gate would have declined to
    // probe it. Using the branch's own predicate means the probe runs exactly
    // when there is a shadow pass to observe.
    const bool probeShadow = m_verifyShadowThisFrame &&
                             !renderedPathTracing &&
                             m_renderer.ShadowsAvailable();
    if (probeShadow && !m_renderer.RecordShadowMapReadback(m_device))
        m_verifyShadowThisFrame = false;
    if (probeDrawRecords && !m_renderer.RecordDrawProbeReadback(m_device))
        m_verifyDrawRecordsThisFrame = false;
    // Recorded on BOTH probe frames - the control and the live one - because the
    // pair is the assertion. Reading back only the live frame would leave the
    // control arming a probe nobody looks at.
    if ((probeIBLControl || probeIBLConsume) &&
        !m_renderer.RecordIBLConsumeProbeReadback(m_device))
    {
        m_verifyIBLControlThisFrame = false;
        m_verifyIBLConsumeThisFrame = false;
    }

    // Everything that records into this frame's arena has now done so. Clearing
    // the guard HERE is what makes it able to catch a pass added above
    // BeginFrameResources on a LATER frame; a flag set once and never cleared
    // would be true forever after frame 0 and would catch nothing.
    m_renderer.EndFrameResources();

    const bool vsync = !(m_options.smoke && m_options.smokeUnlocked);
    if (!m_device.ExecuteAndPresent(vsync))
        return false;

    if (m_options.smoke)
    {
        m_smokeMaxOutstandingSubmissions = (std::max)(
            m_smokeMaxOutstandingSubmissions,
            m_device.OutstandingSubmissionCount());
    }

    bool gpuRetiredForReadback = false;
    if (m_captureThisFrame)
    {
        m_captureThisFrame = false;
        if (!m_device.WriteBackBufferCapture(kSmokeCaptureFile))
            return false;
        gpuRetiredForReadback = true;
    }

    if ((probeShadow || probeDrawRecords || probeIBLControl || probeIBLConsume ||
         probeRTIBLControl || probeRTIBLConsume) &&
        !gpuRetiredForReadback)
    {
        if (!m_device.WaitForGpu())
            return false;
        gpuRetiredForReadback = true;
    }

    // The DXR IBL consumption probe, both frames. Reduced by the SAME SHIPPED
    // ReduceIBLConsumeProbe the raster probe uses, so the two paths' markers are
    // the same quantities computed the same way and can be read side by side.
    //
    // The marker keys are DISTINCT from the raster ones and from each other, and
    // that shape is mandatory rather than tidy: smoke_test.ps1 stores markers in
    // a hashtable, so a shared key would collapse a pair to whichever logged last
    // and the harness would assert one frame twice while believing it had checked
    // both.
    if (probeRTIBLControl || probeRTIBLConsume)
    {
        const bool live = probeRTIBLConsume;
        m_verifyRTIBLControlThisFrame = false;
        m_verifyRTIBLConsumeThisFrame = false;

        render::IBLConsumeValidation ibl = {};
        if (!m_scene.GetPathTracer()->ReadIBLProbe(ibl, live))
        {
            core::Log::Error("DXR IBL consumption probe readback failed");
            return false;
        }

        const char* prefix = live ? "rt_ibl_consume" : "rt_ibl_consume_control";
        core::Log::Infof(
            "[SMOKE] %s=%s %s_shaded_pixels=%u %s_cube_samples=%u "
            "%s_env_zero_pixels=%u",
            prefix, ibl.ok ? "ok" : "failed",
            prefix, ibl.shadedPixels,
            prefix, ibl.cubeSamples,
            prefix, ibl.envZeroPixels);
        core::Log::Infof(
            "[SMOKE] %s_spec_max=%.6f %s_diffuse_max=%.6f %s_in_final_max=%.6f "
            "%s_radiance_max=%.6f %s_mirror_max=%.6f %s_sky_rel_err=%.6f",
            prefix, ibl.envSpecularMax,
            prefix, ibl.envDiffuseMax,
            prefix, ibl.envInFinalMax,
            prefix, ibl.radianceMax,
            prefix, ibl.mirrorLuminanceMax,
            prefix, ibl.skyRelError);
        core::Log::Infof(
            "[SMOKE] %s_reached=%s %s_consumption=%s %s_identity=%s %s_occlusion=%s",
            prefix, ibl.reachedOk ? "ok" : "failed",
            prefix, ibl.consumptionOk ? "ok" : "failed",
            prefix, ibl.identityOk ? "ok" : "failed",
            prefix, ibl.occlusionOk ? "ok" : "failed");
        core::Log::Infof(
            "[SMOKE] %s_spec_occ_above_ao=%.6f %s_toksvig_rough_inc=%.6f",
            prefix, ibl.specOccAboveAo,
            prefix, ibl.toksvigRoughInc);

        if (!ibl.ok)
        {
            if (live)
                core::Log::Error(
                    "path_trace.hlsl's stable preview did not consume the environment "
                    "cube as shipped: the DXR path either skipped the IBL terms, "
                    "dropped them out of the radiance sum, sampled a resource that "
                    "is not the prefiltered sky at t0/space8, or dropped the specular "
                    "occlusion / Toksvig roughness fixes");
            else
                core::Log::Error(
                    "The DXR IBL probe's NEGATIVE CONTROL failed: with iblParams.z "
                    "forced to 0 the environment terms did not vanish from the stable "
                    "preview. That means the live assertion beside it proves nothing - "
                    "it would pass with the feature absent");
        }
    }

    // The IBL consumption probe, both frames. Reduced by the SHIPPED
    // ReduceIBLConsumeProbe (render/ibl_consume_probe.cpp), which
    // tests/test_ibl_consume_probe.cpp drives directly on a machine with no
    // device - so the verdict logic is covered by CPU cases and the GPU supplies
    // only the block.
    //
    // The two frames log under DIFFERENT marker keys. That shape is mandatory
    // rather than tidy: smoke_test.ps1 stores markers in a hashtable, so a shared
    // key would collapse the pair to whichever logged last and the harness would
    // assert one frame twice while believing it had checked both.
    if (probeIBLControl || probeIBLConsume)
    {
        const bool live = probeIBLConsume;
        m_verifyIBLControlThisFrame = false;
        m_verifyIBLConsumeThisFrame = false;

        render::IBLConsumeValidation ibl = {};
        if (!m_renderer.ReadIBLConsumeProbe(ibl, live))
        {
            core::Log::Error("GPU IBL consumption probe readback failed");
            return false;
        }

        const char* prefix = live ? "ibl_consume" : "ibl_consume_control";
        core::Log::Infof(
            "[SMOKE] %s=%s %s_shaded_pixels=%u %s_cube_samples=%u "
            "%s_env_zero_pixels=%u",
            prefix, ibl.ok ? "ok" : "failed",
            prefix, ibl.shadedPixels,
            prefix, ibl.cubeSamples,
            prefix, ibl.envZeroPixels);
        core::Log::Infof(
            "[SMOKE] %s_spec_max=%.6f %s_diffuse_max=%.6f %s_in_final_max=%.6f "
            "%s_radiance_max=%.6f %s_mirror_max=%.6f %s_sky_rel_err=%.6f",
            prefix, ibl.envSpecularMax,
            prefix, ibl.envDiffuseMax,
            prefix, ibl.envInFinalMax,
            prefix, ibl.radianceMax,
            prefix, ibl.mirrorLuminanceMax,
            prefix, ibl.skyRelError);
        core::Log::Infof(
            "[SMOKE] %s_reached=%s %s_consumption=%s %s_identity=%s %s_occlusion=%s",
            prefix, ibl.reachedOk ? "ok" : "failed",
            prefix, ibl.consumptionOk ? "ok" : "failed",
            prefix, ibl.identityOk ? "ok" : "failed",
            prefix, ibl.occlusionOk ? "ok" : "failed");
        core::Log::Infof(
            "[SMOKE] %s_spec_occ_above_ao=%.6f %s_toksvig_rough_inc=%.6f",
            prefix, ibl.specOccAboveAo,
            prefix, ibl.toksvigRoughInc);

        // These GPU probes assert the DEMO scene renders as shipped. Under
        // --star-system the scene is the solar system (camera at a planet / the
        // orrery), so the demo geometry is out of view and the probes legitimately
        // do not apply — downgrade their failure to non-fatal there. The default
        // smoke (no --star-system) keeps full teeth.
        if (!ibl.ok && !m_options.starSystem)
        {
            if (live)
                core::Log::Error(
                    "basic_ps.hlsl did not consume the environment cube as shipped: "
                    "the raster path either skipped the IBL terms, dropped them out "
                    "of finalColor, sampled a resource that is not the "
                    "prefiltered sky at t0/space6, or dropped the specular occlusion "
                    "/ Toksvig roughness fixes");
            else
                core::Log::Error(
                    "The IBL consumption probe's NEGATIVE CONTROL failed: with "
                    "iblParams.z forced to 0 the environment terms did not vanish. "
                    "That means the live assertion beside it proves nothing - it "
                    "would pass with the feature absent");
        }
    }

    if (probeDrawRecords && m_verifyDrawRecordsThisFrame)
    {
        m_verifyDrawRecordsThisFrame = false;
        render::DrawProbeValidation validation = {};
        if (!m_renderer.ReadDrawProbe(validation))
        {
            core::Log::Error("GPU draw-record probe readback failed");
            return false;
        }
        const bool blendValid = validation.shadowBlendRecords > 0 &&
                                validation.shadowBlendPixels > 0 &&
                                (validation.shadowBlendPairMask & 1u) != 0 &&
                                validation.shadowBlendSignalQ8 > 0 &&
                                validation.shadowBlendMismatchPixels == 0 &&
                                validation.shadowBlendExpectedQ8 ==
                                    validation.shadowBlendOutputQ8;
        const bool valid = validation.ObjectRecordsChecked() > 0 &&
                           validation.materialRecordsChecked > 0 &&
                           validation.ObjectMismatches() == 0 &&
                           validation.materialMismatches == 0 &&
                           blendValid;
        // Emitted per PASS, with the pass in the key. The harness stores markers
        // in a hashtable keyed by name, so a shared key would collapse the two
        // passes to whichever logged last - green while the other pass was
        // entirely wrong.
        core::Log::Infof(
            "[SMOKE] draw_probe=%s "
            "draw_probe_shadow_records=%u draw_probe_shadow_distinct=%u "
            "draw_probe_shadow_mismatches=%u "
            "draw_probe_main_records=%u draw_probe_main_distinct=%u "
            "draw_probe_main_mismatches=%u",
            valid ? "ok" : "failed",
            validation.shadowRecordsChecked,
            validation.shadowDistinctMarkers,
            validation.shadowMismatches,
            validation.mainRecordsChecked,
            validation.mainDistinctMarkers,
            validation.mainMismatches);
        core::Log::Infof(
            "[SMOKE] draw_probe_material_records=%u draw_probe_material_distinct=%u "
            "draw_probe_material_mismatches=%u draw_probe_material_unshaded=%u",
            validation.materialRecordsChecked,
            validation.materialDistinctMarkers,
            validation.materialMismatches,
            validation.materialRecordsUnshaded);
        core::Log::Infof(
            "[SMOKE] shadow_blend_probe=%s shadow_blend_records=%u "
            "shadow_blend_pair_mask=%u shadow_blend_pixels=%llu "
            "shadow_blend_expected_q8=%llu shadow_blend_output_q8=%llu "
            "shadow_blend_primary_q8=%llu shadow_blend_signal_q8=%llu "
            "shadow_blend_mismatch_pixels=%llu",
            blendValid ? "ok" : "failed",
            validation.shadowBlendRecords,
            validation.shadowBlendPairMask,
            static_cast<unsigned long long>(validation.shadowBlendPixels),
            static_cast<unsigned long long>(validation.shadowBlendExpectedQ8),
            static_cast<unsigned long long>(validation.shadowBlendOutputQ8),
            static_cast<unsigned long long>(validation.shadowBlendPrimaryQ8),
            static_cast<unsigned long long>(validation.shadowBlendSignalQ8),
            static_cast<unsigned long long>(validation.shadowBlendMismatchPixels));
        if (!valid && !m_options.starSystem) // demo-scene probe; N/A to the star scene
            core::Log::Error("GPU draw-record or cascade-blend consumption probe failed");
    }

    if (probeShadow && m_verifyShadowThisFrame)
    {
        m_verifyShadowThisFrame = false;

        // Per-cascade markers, with the INDEX IN THE KEY. That shape is
        // mandatory, not stylistic: smoke_test.ps1 stores markers in a hashtable
        // keyed by name, so a looped "cascade_written=yes" would OVERWRITE and
        // collapse to whichever cascade ran last - passing while cascades 0..N-2
        // were empty.
        bool  allRead     = true;
        float fraction[core::kShadowCascadeCount] = {};
        float minDepth[core::kShadowCascadeCount] = {};

        for (uint32_t c = 0; c < core::kShadowCascadeCount; ++c)
        {
            minDepth[c] = 1.0f;
            if (!m_renderer.ReadShadowMapCoverage(c, fraction[c], minDepth[c]))
            {
                allRead = false;
                break;
            }
            // The assertion is "this cascade rasterised something", not a
            // threshold on how much. Deleting the pass, culling everything, or
            // an inverted light matrix all land on zero.
            core::Log::Infof("[SMOKE] shadow_cascade_written_%u=%s",
                             c, fraction[c] > 0.0f ? "yes" : "no");
            core::Log::Infof("[SMOKE] shadow_cascade_fraction_%u=%.4f shadow_cascade_min_depth_%u=%.4f",
                             c, fraction[c], c, minDepth[c]);
        }

        if (allRead)
        {
            // Cascade 0's numbers keep the ORIGINAL key names so the existing
            // assertions stay true and need no edit.
            core::Log::Infof("[SMOKE] shadow_map_written=%s",
                             fraction[0] > 0.0f ? "yes" : "no");
            core::Log::Infof("[SMOKE] shadow_written_fraction=%.3f shadow_min_depth=%.4f",
                             fraction[0], minDepth[0]);

            // EACH CASCADE STORED A DIFFERENT DEPTH. This is the GPU-side
            // assertion with real discriminating power, and it rests on a
            // structural property rather than on scene layout: cascade c's
            // orthographic slab is ShadowCascadeDepthRange(c) deep - 120, 325,
            // 875, 2350 - so the SAME geometry necessarily normalises to a
            // different NDC depth in every slice. Four equal values mean the
            // slices did not each get their own matrix.
            //
            // What it catches: m_activeCascade never advancing (every slice
            // rasterised with cascade 0's matrix), the readback reading slice 0
            // four times, and every slice bound to the same DSV.
            //
            // What it does NOT catch, stated plainly: a PERMUTATION of matrices
            // across slices leaves all four distinct. Nothing on the GPU side
            // here detects that; the CPU cases in tests/test_shadow_cascades.cpp
            // are what constrain the matrices themselves.
            //
            // The coverage-fraction ordering the design called for is NOT
            // asserted, because it is vacuous in this scene: the probe window is
            // 1/8 of each footprint, so even cascade 3's is 117 world units
            // across and sits entirely on the 200x200 ground plane. All four
            // fractions are 1.0. That prediction assumed the old 10x10 plane,
            // which this branch itself replaced. The fractions are still logged
            // for diagnosis; they are simply not evidence of anything here.
            float minSeparation = 1.0f;
            for (uint32_t a = 0; a < core::kShadowCascadeCount; ++a)
                for (uint32_t b = a + 1; b < core::kShadowCascadeCount; ++b)
                {
                    const float d = std::fabs(minDepth[a] - minDepth[b]);
                    if (d < minSeparation) minSeparation = d;
                }
            core::Log::Infof("[SMOKE] shadow_cascade_depths_distinct=%s",
                             minSeparation > 1e-4f ? "yes" : "no");
            core::Log::Infof("[SMOKE] shadow_cascade_depth_separation=%.5f",
                             minSeparation);

            // How many cascades the pass actually began. NOT redundant with the
            // per-slice written markers, and that is not a guess: skipping the
            // last cascade was measured to leave shadow_cascade_written_3=yes,
            // because an uncleared, never-rendered depth slice holds arbitrary
            // values below 1.0 and reads exactly like a slice full of real
            // depth. This counter is what catches a loop that stops early.
            core::Log::Infof("[SMOKE] shadow_cascades_rendered=%u",
                             m_renderer.ShadowCascadesRendered());

            // Computed from the table actually in flight this frame, so it is a
            // check of the live fit rather than a mirror of the constants.
            core::Log::Infof("[SMOKE] shadow_cascade_texel_monotonic=%s",
                             m_renderer.ShadowCascadeTexelSizesAreMonotonic()
                                 ? "yes" : "no");
        }
        else
        {
            core::Log::Info("[SMOKE] shadow_map_written=unknown");
        }
    }

    if (m_device.IsDeviceLost())
    {
        core::Log::Error("GPU device lost - exiting");
        m_exitCode = 3;
        m_running = false;
    }

    return !m_device.IsDeviceLost();
}

void App::Shutdown()
{
    if (!m_windowReady && !m_deviceReady && !m_rendererReady && !m_sceneReady)
        return;

    core::Log::Info("=== Shutting down ===");

    if (m_deviceReady && !m_device.IsDeviceLost() && !m_device.WaitForGpu())
        core::Log::Error("GPU did not become idle before application resource shutdown");
    if (m_sceneReady)
    {
        if (!m_runtimeAssembly.Shutdown(m_scene, m_device, m_renderer))
            core::Log::Error("Runtime assembly teardown failed");
        m_scene.Shutdown(m_device, m_renderer);
        m_sceneReady = false;
        if (m_options.smoke)
        {
            core::Log::Infof("[SMOKE] descriptors_in_use_after_scene_shutdown=%u",
                             m_renderer.TextureDescriptorsInUse());
            core::Log::Infof("[SMOKE] descriptors_pending_after_scene_shutdown=%zu",
                             m_renderer.TextureDescriptorsPending());
        }
    }
    if (m_debugOverlayAttempted)
    {
        m_debugOverlay.Shutdown();
        m_debugOverlayAttempted = false;
        m_debugOverlayReady = false;
    }
    if (m_rendererReady)
    {
        m_renderer.Shutdown();
        m_rendererReady = false;
        if (m_options.smoke)
        {
            core::Log::Infof("[SMOKE] descriptors_pending_after_renderer_shutdown=%zu",
                             m_renderer.TextureDescriptorsPending());
        }
    }
    if (m_deviceReady)
    {
        m_device.Shutdown();
        m_deviceReady = false;
    }
    if (m_windowReady)
    {
        m_window.Shutdown();
        m_windowReady = false;
    }
}

} // namespace dawning
