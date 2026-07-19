#include "app.h"

#include "core/input.h"
#include "core/log.h"
#include "render/mesh.h"
#include "render/texture.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>   // std::size, for the pillar distance table
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace
{

constexpr const char* kSmokeCaptureFile = "smoke_capture.ppm";
constexpr double kSmokeFixedDeltaSeconds = 1.0 / 60.0;

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
    options.gpuValidation = HasOption(args, "--gpu-validation");
    options.showOverlay = !HasOption(args, "--no-overlay");
    options.smokeSeconds = ReadDoubleOption(args, "--smoke-seconds=", options.smokeSeconds);
    options.smokeRTDelaySeconds = ReadDoubleOption(args, "--smoke-rt-delay=", options.smokeRTDelaySeconds);

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
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
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
        core::Log::Infof("Smoke mode enabled (rt=%s, full=%s, unlocked=%s, seconds=%.2f)",
                         m_options.smokeRT ? "yes" : "no",
                         m_options.smokeFullQuality ? "yes" : "no",
                         m_options.smokeUnlocked ? "yes" : "no",
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
    auto planeData = render::GeneratePlane(200.0f, 200.0f, 40, 40, core::Color::White());
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
    m_smokeTextureEntity = m_scene.CreateSpinner(
        "BlueCube", cube,
        m_smokeGrowthMaterial,
        ecs::Transform{ { 0, 0.5f, 0 }, core::Quatf::Identity(), { 1, 1, 1 } },
        0.5f);

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

    core::Log::Infof("Scene populated: %u entities", m_scene.EntityCount());
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
    core::Log::Info("Controls: F1 toggles raster/path tracing, F2 toggles RT quality, F3 toggles overlay");
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
    core::Log::Info("=== Entering main loop (WASD+Mouse, click to capture, ESC to release) ===");
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

        core::TimeStep timeStep = m_timer.Tick();
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
                }
            }

            if (m_frameCount >= smokeEndFrame)
            {
                m_captureThisFrame = m_options.smokeCapture;
                m_verifyShadowThisFrame = true;
                core::Log::Infof("[SMOKE] mode=%s", m_options.smokeRT ? "rt" : "raster");
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
                core::Log::Info("Smoke mode complete");
                m_running = false;
            }
        }

        UpdateWindowTitle(timeStep);
        UpdateCamera(timeStep);

        if (!HandleResize())
            continue;
        if (m_window.IsMinimized())
        {
            // Simulation is suspended with rendering. Discard elapsed fixed
            // steps so restoring the window cannot trigger an unbounded catch-up.
            while (m_timer.ConsumeFixedStep()) {}
            Sleep(10);
            continue;
        }

        if (m_options.smoke)
        {
            // The smoke timeline is frame-driven so capture hashes do not depend
            // on how quickly this machine happens to render the test.
            while (m_timer.ConsumeFixedStep()) {}
            m_scene.UpdateSystems(kSmokeFixedDeltaSeconds);
        }
        else
        {
            while (m_timer.ConsumeFixedStep())
                m_scene.UpdateSystems(m_timer.GetFixedDt());
        }

        if (!RenderFrame(timeStep))
        {
            m_exitCode = m_device.IsDeviceLost() ? 3 : 5;
            m_running = false;
        }
    }

    return m_exitCode;
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

    char title[160] = {};
    std::snprintf(title, sizeof(title),
                  "The Dawning V3 | %.1f fps | %u entities | %s [F1 render | F2 quality | F3 overlay]",
                  timeStep.fps, m_scene.EntityCount(), modeText);
    SetWindowTextA(m_window.GetHWND(), title);
}

void App::UpdateCamera(const core::TimeStep& timeStep)
{
    const auto& input = core::input::GetState();
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    if (m_window.IsCaptured())
    {
        mouseDeltaX = static_cast<float>(input.mouse.deltaX);
        mouseDeltaY = static_cast<float>(input.mouse.deltaY);
    }

    m_camera.Update(
        static_cast<float>(timeStep.dt), mouseDeltaX, mouseDeltaY,
        input.KeyDown('W'), input.KeyDown('S'),
        input.KeyDown('A'), input.KeyDown('D'),
        input.KeyDown('E') || input.KeyDown(VK_SPACE),
        input.KeyDown('Q') || input.KeyDown(VK_CONTROL),
        input.KeyDown(VK_SHIFT));
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
    else if (m_frameCount == 8)
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

    return true;
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
    return state;
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
    const bool renderedPathTracing = m_usePathTracing && m_rtAvailable;

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
        m_scene.PathTraceEntities(
            m_device, m_camera, lightDirection, lightColor, ambientColor, m_rtQualityMode);
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
        // Shadow depth first, into its own target. Must precede BeginScenePass:
        // it rebinds render targets, viewport and scissor, so running it after
        // would leave the scene pass pointing at a 2048x2048 depth-only target.
        if (m_renderer.ShadowsAvailable())
        {
            m_renderer.BeginShadowPass(m_device);
            m_scene.RenderShadowCasters(m_device, m_renderer, m_camera.Position());
            m_renderer.EndShadowPass(m_device);
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
        m_renderer.DrawSky(m_device);
        m_scene.RenderEntities(m_device, m_renderer, m_camera.Position());

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

    // Probe the shadow map on the same frame the capture is taken, and only in
    // smoke mode - it costs a copy and a GPU-visible readback.
    const bool probeShadow = m_verifyShadowThisFrame &&
                             !m_usePathTracing &&
                             m_renderer.ShadowsAvailable();
    if (probeShadow && !m_renderer.RecordShadowMapReadback(m_device))
        m_verifyShadowThisFrame = false;

    const bool vsync = !(m_options.smoke && m_options.smokeUnlocked);
    if (!m_device.ExecuteAndPresent(vsync))
        return false;

    if (m_options.smoke)
    {
        m_smokeMaxOutstandingSubmissions = (std::max)(
            m_smokeMaxOutstandingSubmissions,
            m_device.OutstandingSubmissionCount());
    }

    if (m_captureThisFrame)
    {
        m_captureThisFrame = false;
        if (!m_device.WriteBackBufferCapture(kSmokeCaptureFile))
            return false;
    }

    if (probeShadow && m_verifyShadowThisFrame)
    {
        m_verifyShadowThisFrame = false;
        float writtenFraction = 0.0f;
        float minDepth        = 1.0f;
        if (m_renderer.ReadShadowMapCoverage(writtenFraction, minDepth))
        {
            // The assertion is "the depth pass rasterised something", not a
            // threshold on how much. Deleting the pass, culling everything, or
            // an inverted light matrix all land on zero; anything above it means
            // geometry reached the map.
            core::Log::Infof("[SMOKE] shadow_map_written=%s",
                             writtenFraction > 0.0f ? "yes" : "no");
            core::Log::Infof("[SMOKE] shadow_written_fraction=%.3f shadow_min_depth=%.4f",
                             writtenFraction, minDepth);
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
