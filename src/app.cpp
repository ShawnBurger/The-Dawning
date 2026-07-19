#include "app.h"

#include "core/input.h"
#include "core/log.h"
#include "render/mesh.h"
#include "render/texture.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace
{

constexpr const char* kSmokeCaptureFile = "smoke_capture.ppm";

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
        core::Log::Infof("Smoke mode enabled (rt=%s, full=%s, seconds=%.2f)",
                         m_options.smokeRT ? "yes" : "no",
                         m_options.smokeFullQuality ? "yes" : "no",
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

    if (!m_device.Init(m_window.GetHWND(), m_window.GetWidth(), m_window.GetHeight(), enableDebug))
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
    InitializeScene();
    InitializePathTracingState();

    m_timer.Init();
    return true;
}

void App::InitializeScene()
{
    m_device.WaitForCurrentFrame();
    m_device.ResetCommandList();

    ComPtr<ID3D12Resource> cubeVBUp, cubeIBUp;
    ComPtr<ID3D12Resource> planeVBUp, planeIBUp;
    ComPtr<ID3D12Resource> sphereVBUp, sphereIBUp;
    ComPtr<ID3D12Resource> groundTexUp, cubeTexUp;
    ComPtr<ID3D12Resource> groundNormalTexUp, cubeNormalTexUp;

    auto cubeData = render::GenerateCube(core::Color::White());
    auto planeData = render::GeneratePlane(10.0f, 10.0f, 10, 10, core::Color::White());
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
        groundTexture = render::CreateTexture2DFromKTXFile(
            m_device.Device(), m_device.CmdList(),
            groundKTXPath, groundTexUp, L"GroundAlbedoTexture");
    }
    if (!groundTexture.IsValid() && FileExists(groundPNGPath))
    {
        groundTexture = render::CreateTexture2DFromWICFile(
            m_device.Device(), m_device.CmdList(),
            groundPNGPath, groundTexUp, L"GroundAlbedoTexture");
    }
    if (!groundTexture.IsValid())
    {
        groundTexture = render::CreateTexture2DFromDDSFile(
            m_device.Device(), m_device.CmdList(),
            groundDDSPath, groundTexUp, L"GroundAlbedoTexture");
    }
    if (!groundTexture.IsValid())
    {
        auto pixels = render::GenerateCheckerTextureRGBA8(
            512, 512, 32,
            { 0.82f, 0.86f, 0.91f, 1.0f },
            { 0.48f, 0.53f, 0.60f, 1.0f });
        groundTexture = render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 512, 512, groundTexUp, L"GroundAlbedoTexture");
    }
    groundTexture.descriptorIndex = m_renderer.RegisterTexture(m_device.Device(), groundTexture);

    render::Texture cubeTexture;
    if (FileExists(cubeKTXPath))
    {
        cubeTexture = render::CreateTexture2DFromKTXFile(
            m_device.Device(), m_device.CmdList(),
            cubeKTXPath, cubeTexUp, L"BluePanelAlbedoTexture");
    }
    if (!cubeTexture.IsValid() && FileExists(cubePNGPath))
    {
        cubeTexture = render::CreateTexture2DFromWICFile(
            m_device.Device(), m_device.CmdList(),
            cubePNGPath, cubeTexUp, L"BluePanelAlbedoTexture");
    }
    if (!cubeTexture.IsValid())
    {
        cubeTexture = render::CreateTexture2DFromDDSFile(
            m_device.Device(), m_device.CmdList(),
            cubeDDSPath, cubeTexUp, L"BluePanelAlbedoTexture");
    }
    if (!cubeTexture.IsValid())
    {
        auto pixels = render::GenerateCheckerTextureRGBA8(
            256, 256, 32,
            { 0.20f, 0.42f, 0.92f, 1.0f },
            { 0.03f, 0.08f, 0.20f, 1.0f });
        cubeTexture = render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 256, 256, cubeTexUp, L"BluePanelAlbedoTexture");
    }
    cubeTexture.descriptorIndex = m_renderer.RegisterTexture(m_device.Device(), cubeTexture);

    render::Texture groundNormalTexture;
    if (FileExists(groundNormalKTXPath))
    {
        groundNormalTexture = render::CreateTexture2DFromKTXFile(
            m_device.Device(), m_device.CmdList(),
            groundNormalKTXPath, groundNormalTexUp, L"GroundNormalTexture");
    }
    if (!groundNormalTexture.IsValid() && FileExists(groundNormalPNGPath))
    {
        groundNormalTexture = render::CreateTexture2DFromWICFile(
            m_device.Device(), m_device.CmdList(),
            groundNormalPNGPath, groundNormalTexUp, L"GroundNormalTexture");
    }
    if (!groundNormalTexture.IsValid() && FileExists(groundNormalDDSPath))
    {
        groundNormalTexture = render::CreateTexture2DFromDDSFile(
            m_device.Device(), m_device.CmdList(),
            groundNormalDDSPath, groundNormalTexUp, L"GroundNormalTexture");
    }
    if (!groundNormalTexture.IsValid())
    {
        auto pixels = render::GenerateWaveNormalTextureRGBA8(512, 512, 8.0f, 0.008f);
        groundNormalTexture = render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 512, 512, groundNormalTexUp, L"GroundNormalTexture");
    }
    groundNormalTexture.descriptorIndex =
        m_renderer.RegisterTexture(m_device.Device(), groundNormalTexture);

    render::Texture cubeNormalTexture;
    if (FileExists(cubeNormalKTXPath))
    {
        cubeNormalTexture = render::CreateTexture2DFromKTXFile(
            m_device.Device(), m_device.CmdList(),
            cubeNormalKTXPath, cubeNormalTexUp, L"CubeNormalTexture");
    }
    if (!cubeNormalTexture.IsValid() && FileExists(cubeNormalPNGPath))
    {
        cubeNormalTexture = render::CreateTexture2DFromWICFile(
            m_device.Device(), m_device.CmdList(),
            cubeNormalPNGPath, cubeNormalTexUp, L"CubeNormalTexture");
    }
    if (!cubeNormalTexture.IsValid() && FileExists(cubeNormalDDSPath))
    {
        cubeNormalTexture = render::CreateTexture2DFromDDSFile(
            m_device.Device(), m_device.CmdList(),
            cubeNormalDDSPath, cubeNormalTexUp, L"CubeNormalTexture");
    }
    if (!cubeNormalTexture.IsValid())
    {
        auto pixels = render::GenerateWaveNormalTextureRGBA8(256, 256, 6.0f, 0.012f);
        cubeNormalTexture = render::CreateTexture2DFromRGBA8(
            m_device.Device(), m_device.CmdList(),
            pixels.data(), 256, 256, cubeNormalTexUp, L"CubeNormalTexture");
    }
    cubeNormalTexture.descriptorIndex =
        m_renderer.RegisterTexture(m_device.Device(), cubeNormalTexture);

    m_device.CmdList()->Close();
    ID3D12CommandList* uploadLists[] = { m_device.CmdList() };
    m_device.CmdQueue()->ExecuteCommandLists(1, uploadLists);
    m_device.WaitForGpu();

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

    core::Log::Infof("Meshes registered: cube=%u plane=%u sphere=%u",
                     cube.Index(), plane.Index(), sphere.Index());
    core::Log::Infof("Textures registered: ground=%u cube=%u groundNormal=%u cubeNormal=%u",
                     groundAlbedo.Index(), cubeAlbedo.Index(),
                     groundNormal.Index(), cubeNormal.Index());

    m_scene.CreateRenderable(
        "GroundPlane", plane,
        ecs::Material{ { 0.54f, 0.57f, 0.62f, 1.0f }, 0.9f, 0.0f,
                       groundAlbedo.value, groundNormal.value },
        ecs::Transform{ { 0, 0, 0 }, core::Quatf::Identity(), { 1, 1, 1 } });

    m_scene.CreateSpinner(
        "BlueCube", cube,
        ecs::Material{ { 0.6f, 0.8f, 1.0f, 1.0f }, 0.3f, 0.9f,
                       cubeAlbedo.value, cubeNormal.value },
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

    m_device.WaitForCurrentFrame();
    m_device.ResetCommandList();
    m_scene.EnsureBLAS(m_device);

    m_device.CmdList()->Close();
    ID3D12CommandList* blasLists[] = { m_device.CmdList() };
    m_device.CmdQueue()->ExecuteCommandLists(1, blasLists);
    m_device.WaitForGpu();

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
    m_smokeRTStarted = false;
    m_captureThisFrame = false;
    m_frameCount = 0;
    m_exitCode = 0;
    m_titleTimer = 0.0f;

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

        const core::TimeStep timeStep = m_timer.Tick();

        if (m_options.smoke)
        {
            if (m_options.smokeRT && !m_smokeRTStarted &&
                timeStep.totalTime >= m_options.smokeRTDelaySeconds)
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

            if (timeStep.totalTime >= m_options.smokeSeconds)
            {
                m_captureThisFrame = m_options.smokeCapture;
                core::Log::Infof("[SMOKE] mode=%s", m_options.smokeRT ? "rt" : "raster");
                core::Log::Infof("[SMOKE] rt_available=%s", m_rtAvailable ? "yes" : "no");
                core::Log::Infof("[SMOKE] rt_active=%s",
                                 (m_usePathTracing && m_rtAvailable) ? "yes" : "no");
                core::Log::Infof("[SMOKE] rt_quality=%s",
                                 m_rtQualityMode == render::RTQualityMode::FullPathTrace
                                     ? "full"
                                     : "stable");
                core::Log::Infof("[SMOKE] overlay=%s",
                                 m_debugOverlayReady ? "ok" : "unavailable");
                core::Log::Infof("[SMOKE] frames=%llu",
                                 static_cast<unsigned long long>(m_frameCount));
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
            Sleep(10);
            continue;
        }

        while (m_timer.ConsumeFixedStep())
        {
            // Future fixed-step simulation.
        }

        m_scene.UpdateSystems(static_cast<float>(timeStep.dt));
        RenderFrame(timeStep);
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
        core::Log::Error("Swap chain resize failed; skipping frame and retrying");
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

void App::RenderFrame(const core::TimeStep& timeStep)
{
    static constexpr float clearColor[4] = { 0.50f, 0.55f, 0.62f, 1.0f };

    m_device.WaitForCurrentFrame();
    m_device.ResetCommandList();
    auto* commandList = m_device.CmdList();
    const bool renderedPathTracing = m_usePathTracing && m_rtAvailable;

    if (renderedPathTracing)
    {
        m_device.TransitionResource(
            m_device.CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        m_scene.BuildAccelerationStructures(m_device);

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
        m_device.TransitionResource(
            m_device.CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        auto renderTarget = m_device.CurrentRTV();
        auto depthStencil = m_device.DSV();
        commandList->ClearRenderTargetView(renderTarget, clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(
            depthStencil, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commandList->OMSetRenderTargets(1, &renderTarget, FALSE, &depthStencil);

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
        m_scene.RenderEntities(m_device, m_renderer);

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

    m_device.ExecuteAndPresent(true);

    if (m_captureThisFrame)
    {
        m_captureThisFrame = false;
        m_device.WriteBackBufferCapture(kSmokeCaptureFile);
    }

    if (m_device.IsDeviceLost())
    {
        core::Log::Error("GPU device lost - exiting");
        m_exitCode = 3;
        m_running = false;
    }

    if (renderedPathTracing && !m_device.IsDeviceLost())
        m_device.WaitForGpu();
}

void App::Shutdown()
{
    if (!m_windowReady && !m_deviceReady && !m_rendererReady && !m_sceneReady)
        return;

    core::Log::Info("=== Shutting down ===");

    if (m_deviceReady)
        m_device.WaitForGpu();
    if (m_sceneReady)
    {
        m_scene.Shutdown();
        m_sceneReady = false;
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
