#include "app.h"
#include "scene/model_loader.h"

#include "core/input.h"
#include "core/log.h"
#include "render/mesh.h"
#include "render/texture.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>   // optional generated-asset load
#include <iterator>     // std::size, for the pillar distance table
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
    options.smokeForceGrow = HasOption(args, "--smoke-force-grow");
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

    // Optionally load a generated glTF asset into the scene. The corridor GLB is
    // gitignored (it is ~9 MB of generated binary), so it is present on the
    // machine that generated it and absent on a clean clone or CI - exactly like
    // the texture assets, and handled the same way: load it when it exists, skip
    // silently when it does not. This exercises the whole import->GPU->entity
    // bridge with a real asset locally without making the build depend on a file
    // that is not in the repository.
    //
    // Its upload is recorded onto the SAME command list as the meshes above and
    // flushed by the Close/Execute/Wait immediately below, so loadedModel - which
    // owns the upload buffers - must outlive that wait. It does: it is a local of
    // this function, which returns well after both GPU waits.
    scene::LoadedModel loadedModel;
    {
        const std::filesystem::path modelPath =
            "assets/generated/corridor_section_05445d3a804f4d42/model.glb";
        if (std::filesystem::exists(modelPath))
        {
            loadedModel = scene::LoadModelIntoScene(
                m_scene, m_device, m_renderer, modelPath,
                ecs::Transform{ { -4.5, 0.85, 3.0 }, core::Quatf::Identity(), { 1, 1, 1 } });
            if (!loadedModel.ok)
                core::Log::Errorf("Generated asset present but failed to load: %s",
                                  loadedModel.error.c_str());
        }
        else
        {
            core::Log::Infof("No generated asset at %s; skipping (clean-clone path)",
                             modelPath.string().c_str());
        }
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

    if ((probeShadow || probeDrawRecords || probeIBLControl || probeIBLConsume) &&
        !gpuRetiredForReadback)
    {
        if (!m_device.WaitForGpu())
            return false;
        gpuRetiredForReadback = true;
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
            "[SMOKE] %s_reached=%s %s_consumption=%s %s_identity=%s",
            prefix, ibl.reachedOk ? "ok" : "failed",
            prefix, ibl.consumptionOk ? "ok" : "failed",
            prefix, ibl.identityOk ? "ok" : "failed");

        if (!ibl.ok)
        {
            if (live)
                core::Log::Error(
                    "basic_ps.hlsl did not consume the environment cube as shipped: "
                    "the raster path either skipped the IBL terms, dropped them out "
                    "of finalColor, or sampled a resource that is not the "
                    "prefiltered sky at t0/space6");
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
        const bool valid = validation.ObjectRecordsChecked() > 0 &&
                           validation.materialRecordsChecked > 0 &&
                           validation.ObjectMismatches() == 0 &&
                           validation.materialMismatches == 0;
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
        if (!valid)
            core::Log::Error("GPU consumed per-draw records that differ from the CPU upload contract");
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
