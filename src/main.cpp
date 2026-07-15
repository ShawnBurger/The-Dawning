// =============================================================================
// main.cpp — The Dawning V3 Engine Entry Point
// =============================================================================
// Layer 3+RT: Core Architecture + Full Path Tracing
// Dual-mode renderer: rasterization (default) and DXR path tracing (F1 toggle).
// Path tracer: multi-bounce with NEE, Cook-Torrance BRDF, shadow rays,
// temporal accumulation. Architectured for future ReSTIR/SER/DLSS upgrade.
// =============================================================================

#include <windows.h>
#include "core/types.h"
#include "core/log.h"
#include "core/input.h"
#include "core/timer.h"
#include "core/window.h"
#include "render/d3d12_device.h"
#include "render/renderer.h"
#include "render/camera.h"
#include "render/mesh.h"
#include "render/path_tracer.h"
#include "scene/scene.h"

int WINAPI WinMain(HINSTANCE /*hInstance*/, HINSTANCE, LPSTR, int)
{
    // =========================================================================
    // Initialize core systems
    // =========================================================================
    core::Log::Init();
    core::Log::Info("=== The Dawning V3 Engine Starting (Layer 3: ECS) ===");

    // Set working directory to executable directory
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) *lastSlash = '\0';
        SetCurrentDirectoryA(exePath);
        core::Log::Infof("Working directory: %s", exePath);
    }

    core::Window window;
    core::WindowDesc windowDesc;
    windowDesc.title  = "The Dawning V3";
    windowDesc.width  = 1920;
    windowDesc.height = 1080;

    if (!window.Init(windowDesc))
    {
        core::Log::Error("Failed to create window");
        core::Log::Shutdown();
        return 1;
    }

    // =========================================================================
    // Initialize D3D12
    // =========================================================================
    render::D3D12Device device;

#ifdef NDEBUG
    bool enableDebug = false;
#else
    bool enableDebug = true;
#endif

    if (!device.Init(window.GetHWND(), window.GetWidth(), window.GetHeight(), enableDebug))
    {
        core::Log::Error("Failed to initialize D3D12");
        window.Shutdown();
        core::Log::Shutdown();
        return 1;
    }

    // =========================================================================
    // Initialize renderer
    // =========================================================================
    render::Renderer renderer;
    if (!renderer.Init(device))
    {
        core::Log::Error("Failed to initialize renderer");
        device.Shutdown();
        window.Shutdown();
        core::Log::Shutdown();
        return 1;
    }

    renderer.SetDirectionalLight(
        core::Vec3f(0.5f, 0.8f, 0.3f).Normalized(),
        { 1.0f, 0.97f, 0.92f },
        { 0.12f, 0.14f, 0.22f }
    );

    // =========================================================================
    // Initialize camera
    // =========================================================================
    render::Camera camera;
    camera.Init({ 0.0f, 2.0f, -5.0f }, 0.0f, -10.0f);
    camera.SetMoveSpeed(5.0f);
    camera.SetFOV(70.0f);

    // =========================================================================
    // Initialize scene (ECS + resource manager)
    // =========================================================================
    scene::Scene gameScene;
    gameScene.Init();

    // =========================================================================
    // Upload meshes to GPU and register with resource manager
    // =========================================================================
    device.WaitForCurrentFrame();
    device.ResetCommandList();

    // Staging buffers (must outlive the GPU copy)
    ComPtr<ID3D12Resource> cubeVBUp, cubeIBUp;
    ComPtr<ID3D12Resource> planeVBUp, planeIBUp;
    ComPtr<ID3D12Resource> sphereVBUp, sphereIBUp;

    auto cubeData   = render::GenerateCube(core::Color::White());
    auto planeData  = render::GeneratePlane(10.0f, 10.0f, 10, 10, core::Color::White());
    auto sphereData = render::GenerateSphere(0.5f, 32, 16, core::Color::White());

    auto cubeMesh = render::CreateMesh(device.Device(), device.CmdList(),
        cubeData.vertices.data(), static_cast<uint32_t>(cubeData.vertices.size()),
        cubeData.indices.data(), static_cast<uint32_t>(cubeData.indices.size()),
        cubeVBUp, cubeIBUp);

    auto planeMesh = render::CreateMesh(device.Device(), device.CmdList(),
        planeData.vertices.data(), static_cast<uint32_t>(planeData.vertices.size()),
        planeData.indices.data(), static_cast<uint32_t>(planeData.indices.size()),
        planeVBUp, planeIBUp);

    auto sphereMesh = render::CreateMesh(device.Device(), device.CmdList(),
        sphereData.vertices.data(), static_cast<uint32_t>(sphereData.vertices.size()),
        sphereData.indices.data(), static_cast<uint32_t>(sphereData.indices.size()),
        sphereVBUp, sphereIBUp);

    // Execute uploads
    device.CmdList()->Close();
    ID3D12CommandList* uploadLists[] = { device.CmdList() };
    device.CmdQueue()->ExecuteCommandLists(1, uploadLists);
    device.WaitForGpu();

    // Register meshes with resource manager (transfers GPU resource ownership)
    auto& res = gameScene.GetResources();
    auto hCube   = res.AddMesh(std::move(cubeMesh), "Cube");
    auto hPlane  = res.AddMesh(std::move(planeMesh), "Plane");
    auto hSphere = res.AddMesh(std::move(sphereMesh), "Sphere");

    core::Log::Infof("Meshes registered: cube=%u plane=%u sphere=%u",
                     hCube.Index(), hPlane.Index(), hSphere.Index());

    // =========================================================================
    // Create scene entities via ECS
    // =========================================================================

    // Ground plane (static, dark grey, rough)
    gameScene.CreateRenderable("GroundPlane", hPlane,
        ecs::Material{ { 0.3f, 0.3f, 0.3f, 1.0f }, 0.9f, 0.0f },
        ecs::Transform{ { 0, 0, 0 }, core::Quatf::Identity(), { 1, 1, 1 } });

    // Center cube (spinning, blue metal)
    gameScene.CreateSpinner("BlueCube", hCube,
        ecs::Material{ { 0.2f, 0.4f, 0.8f, 1.0f }, 0.3f, 0.9f },
        ecs::Transform{ { 0, 0.5f, 0 }, core::Quatf::Identity(), { 1, 1, 1 } },
        0.5f);

    // Red sphere (left, rough dielectric)
    gameScene.CreateRenderable("RedSphere", hSphere,
        ecs::Material{ { 0.8f, 0.15f, 0.1f, 1.0f }, 0.4f, 0.1f },
        ecs::Transform{ { -2.5f, 0.75f, 0 }, core::Quatf::Identity(), { 1.5f, 1.5f, 1.5f } });

    // Gold sphere (right, smooth metal)
    gameScene.CreateRenderable("GoldSphere", hSphere,
        ecs::Material{ { 0.9f, 0.7f, 0.2f, 1.0f }, 0.25f, 1.0f },
        ecs::Transform{ { 2.5f, 0.75f, 0 }, core::Quatf::Identity(), { 1.5f, 1.5f, 1.5f } });

    // Row of spinning white cubes
    for (int i = -3; i <= 3; i++)
    {
        char name[32];
        snprintf(name, sizeof(name), "SmallCube_%d", i + 3);
        float x = static_cast<float>(i) * 1.5f;

        gameScene.CreateSpinner(name, hCube,
            ecs::Material{ { 0.9f, 0.9f, 0.9f, 1.0f }, 0.7f, 0.0f },
            ecs::Transform{ { x, 0.15f, 3.0f }, core::Quatf::Identity(), { 0.3f, 0.3f, 0.3f } },
            2.0f + static_cast<float>(i) * 0.3f);
    }

    core::Log::Infof("Scene populated: %u entities", gameScene.EntityCount());

    // =========================================================================
    // Path tracer state (DXR initializes lazily on first F1 press)
    // =========================================================================
    bool usePathTracing = false;
    bool rtAvailable = false;
    bool rtInitAttempted = false;
    render::RTQualityMode rtQualityMode = render::RTQualityMode::StablePreview;

    core::Log::Info("Path tracing will initialize on first F1 press");

    // =========================================================================
    // Timer
    // =========================================================================
    core::Timer timer;
    timer.Init();

    const float clearColor[4] = { 0.1f, 0.1f, 0.15f, 1.0f };

    core::Log::Info("=== Entering main loop (WASD+Mouse, click to capture, ESC to release) ===");

    // =========================================================================
    // Main loop
    // =========================================================================
    bool running = true;
    while (running)
    {
        // --- Input ---
        core::input::BeginFrame();

        if (!window.ProcessMessages()) { running = false; break; }

        const auto& input = core::input::GetState();

        if (input.KeyPressed(VK_ESCAPE))
        {
            if (window.IsCaptured()) window.ReleaseMouse();
            else running = false;
        }

        if (input.mouse.buttonPressed[0] && !window.IsCaptured())
            window.CaptureMouse();

        if (input.KeyPressed(VK_F2))
        {
            rtQualityMode = (rtQualityMode == render::RTQualityMode::StablePreview)
                ? render::RTQualityMode::FullPathTrace
                : render::RTQualityMode::StablePreview;
            core::Log::Infof("RT quality mode: %s",
                rtQualityMode == render::RTQualityMode::StablePreview ? "STABLE PREVIEW" : "FULL PATH TRACE");
        }

        // F1: initialize/toggle path tracing
        if (input.KeyPressed(VK_F1))
        {
            if (!rtInitAttempted)
            {
                rtInitAttempted = true;

                if (gameScene.InitPathTracer(device))
                {
                    device.WaitForCurrentFrame();
                    device.ResetCommandList();

                    gameScene.EnsureBLAS(device);

                    device.CmdList()->Close();
                    ID3D12CommandList* blasLists[] = { device.CmdList() };
                    device.CmdQueue()->ExecuteCommandLists(1, blasLists);
                    device.WaitForGpu();

                    rtAvailable = true;
                    core::Log::Info("Path tracing initialized");
                }
                else
                {
                    core::Log::Warn("Path tracing not available - DXR initialization failed");
                }
            }

            if (rtAvailable)
            {
                usePathTracing = !usePathTracing;
                core::Log::Infof("Render mode: %s", usePathTracing ? "PATH TRACING" : "RASTERIZATION");
            }
        }

        // --- Timer ---
        core::TimeStep ts = timer.Tick();

        static float titleTimer = 0.0f;
        titleTimer += static_cast<float>(ts.dt);
        if (titleTimer >= 0.5f)
        {
            titleTimer = 0.0f;
            const char* qualityName = rtQualityMode == render::RTQualityMode::StablePreview
                ? "STABLE" : "FULL";
            char title[160];
            snprintf(title, sizeof(title), "The Dawning V3 | %.1f fps | %u entities | %s%s%s [F1 render | F2 quality]",
                     ts.fps, gameScene.EntityCount(),
                     usePathTracing ? "PATH TRACING" : "RASTERIZED",
                     usePathTracing ? " " : "",
                     usePathTracing ? qualityName : "");
            SetWindowTextA(window.GetHWND(), title);
        }

        // --- Camera ---
        float mdx = 0.0f, mdy = 0.0f;
        if (window.IsCaptured())
        {
            mdx = static_cast<float>(input.mouse.deltaX);
            mdy = static_cast<float>(input.mouse.deltaY);
        }

        camera.Update(static_cast<float>(ts.dt), mdx, mdy,
                      input.KeyDown('W'), input.KeyDown('S'),
                      input.KeyDown('A'), input.KeyDown('D'),
                      input.KeyDown('E') || input.KeyDown(VK_SPACE),
                      input.KeyDown('Q') || input.KeyDown(VK_CONTROL),
                      input.KeyDown(VK_SHIFT));

        // --- Resize ---
        if (window.WasResized())
        {
            device.Resize(window.GetWidth(), window.GetHeight());
            if (rtAvailable)
            {
                gameScene.ResizePathTracer(device, device.Width(), device.Height());
            }
            window.ClearResizeFlag();
        }

        if (window.IsMinimized()) { Sleep(10); continue; }

        // --- Fixed timestep ---
        while (timer.ConsumeFixedStep()) { /* future physics */ }

        // =================================================================
        // Phase 1: Update gameplay systems (ECS-driven)
        // =================================================================
        gameScene.UpdateSystems(static_cast<float>(ts.dt));

        // =================================================================
        // Phase 2: Render (dual mode: rasterization or path tracing)
        // =================================================================
        device.WaitForCurrentFrame();
        device.ResetCommandList();

        auto* cmd = device.CmdList();

        bool renderedPathTracing = usePathTracing && rtAvailable;

        if (renderedPathTracing)
        {
            // --- Path Tracing Mode ---

            // Transition back buffer: PRESENT → RENDER_TARGET (needed for copy dest later)
            device.TransitionResource(device.CurrentBackBuffer(),
                                       D3D12_RESOURCE_STATE_PRESENT,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);

            // Build TLAS from current entity transforms
            gameScene.BuildAccelerationStructures(device);

            // Dispatch path tracing rays
            core::Vec3f lightDir = core::Vec3f(0.5f, 0.8f, 0.3f).Normalized();
            core::Vec3f lightColor = { 1.0f, 0.97f, 0.92f };
            core::Vec3f ambientColor = { 0.12f, 0.14f, 0.22f };
            gameScene.PathTraceEntities(device, camera, lightDir, lightColor, ambientColor, rtQualityMode);

            // Copy RT output to back buffer and transition to PRESENT
            gameScene.CopyPathTraceToBackBuffer(device);
        }
        else
        {
            // --- Rasterization Mode ---
            device.TransitionResource(device.CurrentBackBuffer(),
                                       D3D12_RESOURCE_STATE_PRESENT,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);

            auto rtv = device.CurrentRTV();
            auto dsv = device.DSV();
            cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
            cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

            D3D12_VIEWPORT viewport = { 0, 0,
                static_cast<float>(device.Width()), static_cast<float>(device.Height()),
                0, 1 };
            cmd->RSSetViewports(1, &viewport);
            D3D12_RECT scissor = { 0, 0,
                static_cast<LONG>(device.Width()), static_cast<LONG>(device.Height()) };
            cmd->RSSetScissorRects(1, &scissor);

            renderer.BeginFrame(device, camera);
            gameScene.RenderEntities(device, renderer);

            device.TransitionResource(device.CurrentBackBuffer(),
                                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_PRESENT);
        }

        device.ExecuteAndPresent(true);

        if (device.IsDeviceLost())
        {
            core::Log::Error("GPU device lost — exiting");
            running = false;
        }

        if (renderedPathTracing && !device.IsDeviceLost())
            device.WaitForGpu();
    }

    // =========================================================================
    // Shutdown (reverse order of init)
    // =========================================================================
    core::Log::Info("=== Shutting down ===");

    device.WaitForGpu();
    gameScene.Shutdown();
    renderer.Shutdown();
    device.Shutdown();
    window.Shutdown();

    core::Log::Info("=== The Dawning V3 Engine stopped ===");
    core::Log::Shutdown();

    return 0;
}
