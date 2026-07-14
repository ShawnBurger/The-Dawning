# CONVERSATION_CONTEXT.md — The Dawning V3 Engine: Complete DC Build Guide

This document carries full project context for Claude Code (DC) sessions. Read this entirely before making any changes. It contains architecture decisions, research-backed design rationale, code conventions, and layer-by-layer build specifications for the complete engine.

---

## PROJECT OVERVIEW

**The Dawning** is a custom D3D12 space game engine written from scratch in C++20 for Windows. Target hardware: NVIDIA RTX 4090 (Ada Lovelace, 24GB VRAM). The engine is built layer-by-layer — each layer must be complete, compilable, and correct before the next begins.

**Owner:** Shawn — experienced developer, direct communication style, appreciates concise and technically precise work.

**Build target:** Visual Studio 2022, CMake 3.20+, x64, C++20. Windows SDK with D3D12.

---

## CURRENT STATE (as of this handoff)

### Completed Layers

| Layer | Status | Lines | Files | Description |
|-------|--------|-------|-------|-------------|
| 1: Kernel | DONE | ~2,300 | 13 | Window, D3D12 device, timer, input, math library, DRED, GPU caps |
| 2: Geometry Pipeline | DONE | ~3,900 | 22 | Shaders, PSO, mesh upload, camera, PBR-lite lit scene |
| 3: Core Architecture | DONE | ~5,000 | 30 | ECS, resource manager, entity-driven rendering |
| 3+RT: Path Tracing | DONE | ~7,200 | 37 | DXR pipeline, BLAS/TLAS, multi-bounce path tracer, DXC integration |

### File Structure
```
dawning_v3/
├── CMakeLists.txt
├── SETUP_AND_BUILD.bat
├── CLAUDE.md                     ← DC auto-reads this file
├── CONVERSATION_CONTEXT.md       ← This file (full context)
├── shaders/
│   ├── basic_vs.hlsl             ← SM 5.1 vertex shader (rasterization)
│   ├── basic_ps.hlsl             ← SM 5.1 pixel shader (PBR-lite, rasterization)
│   └── path_trace.hlsl           ← SM 6.3 DXR library (full path tracer)
└── src/
    ├── main.cpp                  ← Entry point, dual render mode (F1 toggles raster/RT)
    ├── core/
    │   ├── types.h               ← Vec2f, Vec3f, Vec3d, Vec4f, Color, Quatf, Mat4x4
    │   ├── log.h/.cpp            ← Timestamped logger (NOT thread-safe)
    │   ├── input.h/.cpp          ← Keyboard/mouse via raw input
    │   ├── timer.h/.cpp          ← QPC timer + fixed timestep accumulator
    │   └── window.h/.cpp         ← Win32 window, raw input, mouse capture
    ├── ecs/
    │   ├── entity.h              ← Generational entity ID (20-bit index + 12-bit gen)
    │   ├── component_pool.h      ← Sparse-set storage per component type
    │   ├── components.h          ← Transform, Velocity, MeshInstance, Material, RotationSpeed, etc.
    │   └── registry.h            ← Central ECS coordinator with type-indexed pools
    ├── render/
    │   ├── d3d12_device.h/.cpp   ← D3D12 init, swap chain, DRED, caps probe, resize
    │   ├── shader_utils.h/.cpp   ← Dual backend: D3DCompile (SM 5.1) + DXC (SM 6.0+)
    │   ├── mesh.h/.cpp           ← Vertex format, GPU upload, primitive generators, 16/32-bit indices
    │   ├── camera.h/.cpp         ← FPS camera with mouse look, WASD, sprint
    │   ├── renderer.h/.cpp       ← Rasterization renderer (root sig v1.1, PSO, CB ring)
    │   ├── rt_acceleration.h/.cpp ← BLAS/TLAS management
    │   ├── rt_pipeline.h/.cpp    ← DXR state object, shader table builder
    │   └── path_tracer.h/.cpp    ← Path tracer orchestrator (dispatch, output texture)
    └── scene/
        ├── resource_manager.h/.cpp ← Handle-based slot-map pools for meshes/materials
        └── scene.h/.cpp          ← Owns ECS registry + resources, runs systems, dual render
```

---

## ABSOLUTE RULES (never violate these)

1. **LEFT-HANDED coordinate system everywhere.** +X right, +Y up, +Z forward (into screen). LookAt is LH. PerspectiveFovLH maps Z to [0,1]. Back-face culling expects CW winding as front-face (`FrontCounterClockwise = FALSE`).

2. **Double precision (Vec3d) for world positions.** Vec3f only after camera subtraction for GPU upload. Not yet implemented in the render path — aspirational for Layer 5+.

3. **Proper .h/.cpp split.** No header-only implementations except small inlines, templates, and the ECS headers which are template-heavy by nature.

4. **No copyrighted terms.** Rename map:
   - Starfleet → Vanguard
   - LCARS → HELIX
   - Phaser → Arc Lance
   - Photon Torpedo → Fusion Torpedo
   - Dilithium → Helion
   - Federation → Commonwealth
   - Klingon → Xenoes
   - Romulan → Vesk
   - Warp → Foldspace

5. **Always `#include <cstdint>`** when using `uint32_t`/`uint64_t`.

6. **Fixed timestep for physics:** accumulator pattern, default 60Hz. Already in timer.h.

7. **Logger is NOT thread-safe.** Main thread only until a job system exists.

8. **Compile after every change.** Never leave the codebase in a broken state.

9. **Name all GPU objects** via `SetName()` for DRED breadcrumb readability.

10. **256-byte CBV alignment** — `D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT`. Every CBV address must be 256-byte aligned.

---

## LAYER 1: KERNEL — Detailed Specification

### Purpose
Bare-metal foundation: a window that initializes D3D12, clears to a color, runs a game loop with a timer, and handles input.

### Components

**`core/types.h`** — Math library, all inline/constexpr where possible:
- `Vec2f`, `Vec3f`, `Vec3d`, `Vec4f` — with operators, Dot, Cross, Length, Normalized
- `Color` — float4 RGBA, with named constructors (White, Red, etc.)
- `Quatf` — quaternion with Identity, FromAxisAngle, operator*, Normalized
- `Mat4x4` — 4x4 float matrix, row-major storage:
  - Identity (default constructor)
  - Translation, Scaling, RotationX/Y/Z, FromQuaternion
  - LookAt (LEFT-HANDED: forward = target - eye, not eye - target)
  - PerspectiveFovLH (maps Z to [0,1])
  - operator* (matrix multiply)
  - TransformPoint, TransformDirection
  - InverseTranspose3x3 (for correct normal transformation under non-uniform scale)
  - Data() returns const float* to m[0][0]
- Constants: PI, DEG_TO_RAD, RAD_TO_DEG

**`core/log.h/.cpp`** — Simple timestamped logger:
- `Log::Init()`, `Log::Shutdown()`, `Log::Info()`, `Log::Warn()`, `Log::Error()`, `Log::Infof()`, `Log::Errorf()`
- Uses OutputDebugStringA on Windows
- NOT thread-safe (documented)

**`core/input.h/.cpp`** — Keyboard + mouse input:
- `input::BeginFrame()` — reset per-frame deltas
- `input::ProcessKeyDown/Up()`, `input::ProcessRawMouse()`
- `InputState` with `KeyDown()`, `KeyPressed()`, mouse deltaX/deltaY, button states
- `FlushAll()` on focus loss

**`core/timer.h/.cpp`** — QueryPerformanceCounter timer:
- `TimeStep` struct: dt (double seconds), totalTime, fps
- Fixed timestep accumulator: `ConsumeFixedStep()` returns true for each 1/60s tick
- Protected against negative dt and large spikes

**`core/window.h/.cpp`** — Win32 window:
- `WindowDesc` struct for title, width, height
- Creates window, registers raw input devices (keyboard + mouse)
- Mouse capture/release with `ClipCursor`, `ShowCursor`
- `WM_KILLFOCUS` flushes input and releases mouse
- `WM_SIZE` handles resize, refreshes clip rect if captured
- `ProcessMessages()` returns false on WM_QUIT

**`render/d3d12_device.h/.cpp`** — Complete D3D12 initialization:
- Debug layer enabled in Debug builds
- DRED configured BEFORE device creation (auto-breadcrumbs + page fault reporting)
- Factory → adapter selection (prefer discrete GPU) → device creation
- Direct command queue, 3 command allocators (triple buffering), 1 command list
- Swap chain (DXGI_FORMAT_R8G8B8A8_UNORM, 3 buffers, FLIP_DISCARD)
- RTV descriptor heap (3 descriptors for back buffers)
- DSV descriptor heap (1 descriptor, D32_FLOAT depth buffer)
- Fence for per-frame synchronization: `WaitForCurrentFrame()`, `WaitForGpu()`
- `GpuCapabilities` struct probed at startup: adapter name, VRAM, feature level, shader model, mesh shaders, raytracing tier, VRS, sampler feedback, enhanced barriers, resource binding tier, tearing support, HDR support, DRED support
- `IsDeviceLost()` flag set on DXGI_ERROR_DEVICE_REMOVED/RESET
- `Resize()` for swap chain recreation
- `TransitionResource()` helper for resource barriers
- All objects named via SetName()

### Build Configuration
```cmake
cmake_minimum_required(VERSION 3.20)
project(TheDawningV3 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
target_link_libraries(TheDawningV3 PRIVATE d3d12 dxgi d3dcompiler dxguid)
```

### Expected Result
Window opens at 1920x1080, clears to dark background, FPS in title bar, ESC quits.

---

## LAYER 2: GEOMETRY PIPELINE — Detailed Specification

### Purpose
First rendered geometry: shaders, vertex buffers in VRAM, a camera, and a lit scene with multiple objects and materials.

### Components

**`shaders/basic_vs.hlsl`** — Vertex shader (SM 5.1):
- CBPerObject at b0: worldViewProj (float4x4), world (float4x4), worldInvTranspose (float4x4)
- Input: POSITION (float3), NORMAL (float3), COLOR (float4), TEXCOORD0 (float2)
- Output: SV_POSITION (clip space), world position, world normal, color, UV

**`shaders/basic_ps.hlsl`** — Pixel shader (SM 5.1):
- CBPerFrame at b1: lightDir, lightColor, ambientColor, eyePos (all float3 + padding)
- CBMaterial at b2: albedo (float4), roughness (float), metallic (float)
- PBR-lite: Lambert diffuse + Blinn-Phong specular, Fresnel-Schlick approximation, hemisphere ambient, energy-conserving diffuse (reduced by metallic)

**`render/shader_utils.h/.cpp`** — Dual shader compilation:
- `CompileShaderFromFile()` auto-routes: SM 5.1 → D3DCompile, SM 6.0+ → DXC
- DXC loaded on-demand via `LoadLibraryW(L"dxcompiler.dll")`
- Debug builds: `-Zi -Od`, Release: `-O3`
- Requires `dxcompiler.dll` + `dxil.dll` for RT shaders

**`render/mesh.h/.cpp`** — Geometry management:
- Vertex struct: position (Vec3f), normal (Vec3f), color (Color), uv (Vec2f) = 48 bytes
- Input layout description matching the vertex struct
- `Mesh` struct: vertex/index buffer ComPtrs, buffer views, counts, index format (R16 or R32)
- `CreateMesh()` — 16-bit indices, upload-to-VRAM staging pattern:
  1. Create DEFAULT heap destination buffers
  2. Create UPLOAD heap staging buffers (with null check!)
  3. Map staging, memcpy, unmap
  4. CopyBufferRegion → destination
  5. Transition to final usage state
  6. Caller must execute command list + wait before releasing staging buffers
- `CreateMesh32()` — 32-bit index variant
- Primitive generators: `GenerateCube()` (CW winding), `GeneratePlane()` (subdivided), `GenerateSphere()` (UV). All validate parameters with sane clamping.

**`render/camera.h/.cpp`** — FPS camera:
- Stores position (Vec3f), yaw, pitch (degrees)
- Forward() computed from spherical coordinates: sin(yaw)*cos(pitch), sin(pitch), cos(yaw)*cos(pitch)
- Right = Up × Forward (LH cross product)
- Update: mouse look (yaw += deltaX * sensitivity, pitch -= deltaY * sensitivity), WASD movement, sprint multiplier, diagonal normalization, vertical movement
- ViewMatrix: LookAt(position, position + Forward(), up)
- ProjectionMatrix: PerspectiveFovLH

**`render/renderer.h/.cpp`** — Rasterization renderer:
- Root signature v1.1 (actual v1.1, not v1.0 with a v1.1 comment):
  - Uses `D3D12_ROOT_PARAMETER1` with `DATA_VOLATILE` flags
  - `D3D12SerializeVersionedRootSignature()` for v1.1, fallback to v1.0
  - Slot 0: Per-object root CBV (b0, VERTEX visibility) — hottest, changes every draw
  - Slot 1: Per-frame root CBV (b1, PIXEL) — changes once per frame
  - Slot 2: Material root CBV (b2, PIXEL) — changes per material
  - 1 static sampler (free, doesn't count toward 64-DWORD limit)
  - DENY flags for unused stages (hull, domain, geometry)
  - Total: 6 DWORDs used
- Graphics PSO (classic API — stream upgrade deferred):
  - CW front face, back-face cull, depth test on, solid fill
  - DXGI_FORMAT_R8G8B8A8_UNORM RTV, DXGI_FORMAT_D32_FLOAT DSV
  - Triangle list topology
- Per-frame constant buffer upload ring:
  - 256KB per frame, 3 frames in flight
  - Persistently mapped (never unmap during normal operation)
  - 256-byte aligned sub-allocation via UploadCB()
- DrawMesh(): computes WVP + InverseTranspose3x3, uploads per-object + material CBs, binds vertex/index buffers, DrawIndexedInstanced

### Scene Composition
- Ground plane (dark grey, rough dielectric)
- Center cube (spinning, blue metal)
- Red sphere (left, rough dielectric)
- Gold sphere (right, smooth metal)
- 7 spinning white mini-cubes in a row

### Expected Result
Lit 3D scene with PBR materials, FPS camera with WASD + mouse, all objects spinning.

---

## LAYER 3: CORE ARCHITECTURE (ECS) — Detailed Specification

### Purpose
Replace hardcoded scene setup with a data-driven Entity-Component-System. Same visual output, completely different architecture.

### Design Decisions (research-backed)

**Storage model: Sparse sets (EnTT-style)**
- Each component type gets its own `ComponentPool<T>` with sparse/dense/data arrays
- O(1) add/remove/get via swap-and-pop
- Multi-component iteration: iterate smaller pool, cross-check larger
- Chosen over archetypes for simplicity and because our entity count is low (~20-100 initially)
- Eurographics 2025 benchmark showed differences are ~0.N ms at 1M entities

**Entity IDs: Generational indices**
- 32-bit packed: 20-bit index (max ~1M entities) + 12-bit generation (4096 recycling cycles)
- EntityManager with intrusive free list for O(1) allocation/recycling
- `IsAlive()` validates both index bounds and generation match

**System execution: Phase-based**
- UpdateSystems(dt) runs all gameplay systems (rotation, future: physics, AI)
- RenderEntities() iterates the MeshInstance pool and issues draw calls
- Systems are plain functions/lambdas, not registered objects

**Resource management: Handle-based slot maps**
- MeshHandle/MaterialHandle: 32-bit packed (20-bit index + 12-bit generation)
- ECS components store handles, never raw GPU pointers
- Multiple entities can share the same mesh/material
- ResourceManager owns all GPU mesh resources

### Components
```cpp
Transform      { Vec3f position, Quatf rotation, Vec3f scale } + ToMatrix()
Velocity       { Vec3f linear, Vec3f angular }
MeshInstance   { uint32_t meshHandle, bool visible }
Material       { Color albedo, float roughness, float metallic }
RotationSpeed  { float radiansPerSecond, Vec3f axis }
Parent         { uint32_t entityIndex }
Name           { char text[48] }
ActiveTag      { uint8_t _pad }
```

### Scene Class
- Owns Registry + ResourceManager
- `CreateRenderable()` / `CreateSpinner()` — entity creation helpers
- `UpdateSystems(dt)` — runs SystemRotation
- `RenderEntities()` — iterates MeshInstance pool, resolves handles, calls renderer

### Expected Result
Identical visual output to Layer 2. Title bar shows entity count. Architecture is now data-driven — adding 1000 entities requires zero rendering code changes.

---

## LAYER 3+RT: PATH TRACING — Detailed Specification

### Purpose
Full DXR path tracing integrated alongside the rasterization renderer, toggled with F1.

### Design Decisions (research-backed for RTX 4090)

**Architecture: Megakernel with iterative bounces**
- Single `DispatchRays` call, bounce loop in ray generation shader
- `MaxTraceRecursionDepth = 2` (primary + shadow, bounces are iterative)
- This is the proven production pattern (Cyberpunk 2077, Portal RTX, Indiana Jones)
- Architectured for future SER upgrade (NvHitObject/NvReorderThread)

**Light sampling: Next Event Estimation (NEE)**
- At each bounce, explicitly sample the directional light
- Shadow ray with `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER` (2-5x cheaper)
- Future: ReSTIR DI for thousands of lights, ReSTIR GI for indirect

**Material evaluation: Cook-Torrance BRDF**
- GGX Normal Distribution Function
- Smith height-correlated geometry function
- Fresnel-Schlick approximation
- F0 = lerp(0.04, baseColor, metallic)
- Diffuse lobe: cosine-weighted hemisphere sampling
- Specular lobe: roughness-perturbed reflection
- Probability split: specProb = 0.5 + 0.5 * metallic

**Denoising: Simple temporal accumulation**
- 10% new frame, 90% history (converges when camera is still)
- Future: DLSS 3.5 Ray Reconstruction replaces this entirely

**Material binding: Bindless via global StructuredBuffer**
- Materials in a StructuredBuffer indexed by InstanceID()
- No local root arguments — global root signature only
- This eliminates SBT complexity and is the modern pattern

### RT Pipeline Components

**`render/rt_acceleration.h/.cpp`** — BLAS/TLAS management:
- `BuildBLAS()`: from Mesh vertex/index buffers, PREFER_FAST_TRACE, GEOMETRY_FLAG_OPAQUE
- `BuildTLAS()`: rebuilt every frame from TLASInstance descriptors with PREFER_FAST_BUILD
- Auto-growing upload heap instance buffer with persistent mapping
- Proper UAV barriers between builds and before TraceRay

**`render/rt_pipeline.h/.cpp`** — DXR state object:
- DXIL library compiled from path_trace.hlsl via DXC (lib_6_3)
- 2 hit groups: PrimaryHitGroup (ClosestHit) + ShadowHitGroup (ShadowClosestHit)
- Global root signature: [0] TLAS SRV, [1] Output UAV table, [2] Per-frame CBV, [3] Material SRV
- Shader table: 64-byte aligned section starts, 32-byte aligned record strides
- 2 miss entries (primary + shadow), N×2 hit group entries (primary + shadow per instance)
- Shader identifiers retrieved via ID3D12StateObjectProperties

**`render/path_tracer.h/.cpp`** — Orchestrator:
- Output UAV texture (R8G8B8A8_UNORM, resizable)
- SRV/UAV descriptor heap (shader-visible)
- Per-frame constant buffers (triple-buffered, persistently mapped)
- Material StructuredBuffer (upload heap, updated per frame)
- `Dispatch()`: uploads CB + materials, binds root params, DispatchRays
- `CopyToBackBuffer()`: transitions + copies RT output to swap chain

**`shaders/path_trace.hlsl`** — The megakernel:
- `[shader("raygeneration")] RayGen` — iterative bounce loop, jittered ray generation, temporal accumulation
- `[shader("closesthit")] ClosestHit` — returns hit distance, instance ID, approximate normal
- `[shader("closesthit")] ShadowClosestHit` — hit = shadow
- `[shader("miss")] Miss` — sky gradient
- `[shader("miss")] ShadowMiss` — not occluded
- PCG hash RNG with per-pixel per-frame seed
- Firefly clamping at 10.0 per channel

### SBT Indexing Formula
```
HitGroupRecordAddress = HitGroupTable.Start + Stride × (
    RayContributionToHitGroupIndex          // 0=primary, 1=shadow
  + MultiplierForGeometryContribution × GeometryIndex  // multiplier=2 (2 ray types)
  + InstanceContributionToHitGroupIndex     // instanceID * 2
)
```

### Runtime Dependencies
- `dxcompiler.dll` + `dxil.dll` must be in the executable directory
- Get from: https://github.com/microsoft/DirectXShaderCompiler/releases

### Expected Result
F1 toggles between rasterization (default) and path tracing. Path-traced view shows the same scene with ray-traced shadows and multi-bounce lighting. Image is noisy at 1 SPP but converges when camera is still. Title bar shows current render mode.

---

## FUTURE LAYERS (not yet built)

### Layer 4: Material System
- Upgrade pixel shader to full Cook-Torrance GGX (already in path_trace.hlsl)
- Texture loading (DDS/PNG), normal maps, roughness/metallic maps
- DXC SM 6.6 migration for bindless `ResourceDescriptorHeap[]` access
- Stream-based PSO creation via `ID3D12Device2::CreatePipelineState`
- Shadow mapping for the rasterization path (cascaded shadow maps)

### Layer 5: World Foundation
- Procedural terrain (compute shader heightmap generation)
- Atmosphere shader (Rayleigh/Mie scattering)
- Sky dome / environment cubemap
- Camera-relative rendering: double-precision world positions, float after camera subtraction
- Star field rendering

### RT Upgrade Path
1. **ReSTIR DI** (NVIDIA RTXDI SDK) — efficient sampling from thousands of lights
2. **ReSTIR GI** — indirect illumination resampling, 9-166x MSE improvement over brute force
3. **SER** — Shader Execution Reordering via NvHitObject/NvReorderThread, 24-47% perf gain
4. **DLSS 3.5 Ray Reconstruction** — replaces temporal accumulation + upscaling in one AI pass
5. **DLSS Frame Generation** — doubles effective FPS via Optical Flow Accelerator
6. **Opacity Micro-Maps** — hardware alpha-test resolution without any-hit shaders

### Performance Budget (RTX 4090, 1080p internal → 4K DLSS)
| Pass | Target (ms) |
|------|-------------|
| TLAS rebuild | 0.3–1.0 |
| Path tracing (2-4 bounces) | 8–15 |
| DLSS Ray Reconstruction | 1–2 |
| Post-processing (4K) | 0.5–1.5 |
| DLSS Frame Generation | ~2.77 |
| **Total rendered frame** | **~12–20 (50-80 FPS base, 100-160 with FG)** |

---

## CODE CONVENTIONS

### Naming
- Classes: `PascalCase` (e.g., `D3D12Device`, `ComponentPool`)
- Functions/methods: `PascalCase` (e.g., `BuildBLAS`, `CreateMesh`)
- Variables: `camelCase` with `m_` prefix for members (e.g., `m_device`, `m_frameIndex`)
- Constants: `k` prefix + PascalCase (e.g., `kFrameCount`, `kVertexLayout`)
- Namespaces: lowercase (e.g., `core`, `render`, `ecs`, `scene`)

### Headers
- Every .h file starts with `#pragma once`
- Block comment with filename, description, and design notes
- `using Microsoft::WRL::ComPtr;` in render headers

### Error Handling
- All D3D12 calls check HRESULT and log on failure
- All buffer allocations check for null before use
- GPU object creation failures are logged with hex error codes
- Device-lost detection via DXGI_ERROR_DEVICE_REMOVED/RESET

### GPU Resource Patterns
- Static geometry: upload heap staging → DEFAULT heap copy → release staging after fence
- Per-frame constants: persistently mapped UPLOAD heap buffers, one per frame in flight
- Acceleration structures: UAV-capable DEFAULT heap buffers, scratch buffers reusable

### Shader Compilation
- SM 5.1 targets → `D3DCompile` (d3dcompiler_47.dll, always available)
- SM 6.0+ targets → DXC (`dxcompiler.dll`, loaded on demand)
- RT shader libraries → `lib_6_3` target, no entry point
- Auto-detection in `CompileShaderFromFile()` based on target string

---

## KNOWN LIMITATIONS & TECHNICAL DEBT

1. **Normal computation in closest-hit shader is approximate.** Uses instance transform matrix extraction instead of vertex normal lookup. Fix requires bindless vertex buffer access in the ray tracing shader (addressed in Layer 4 with SM 6.6).

2. **Ray direction reconstruction in RayGen is approximate.** Extracts camera basis from view-projection matrix columns. Should pass explicit inverse VP matrix or camera basis vectors.

3. **No proper denoiser.** Simple 10% temporal blend. DLSS Ray Reconstruction eliminates this.

4. **worldInvTranspose is correct but the rasterizer path doesn't test non-uniform scale rendering.** The math is right (cofactor matrix / determinant), but no scene objects exercise it.

5. **TLAS instance-to-hit-group mapping assumes all geometry is opaque.** Alpha-tested geometry needs any-hit shaders and OMMs.

6. **Shader table is rebuilt every frame.** Should only rebuild when instance count changes.

7. **BLAS compaction not implemented.** Would save ~45-50% BLAS memory. Requires multi-frame workflow: build → query compacted size → read back → copy.

8. **No async compute overlap.** TLAS builds and AS operations should run on async compute queue to overlap with other rendering work.

9. **The D3D12Device stores `ComPtr<ID3D12Device>` (base interface).** RT code casts to `ID3D12Device5*`. Should store the highest available interface directly.

10. **Logger has no file output.** Only OutputDebugStringA. Add file logging for release builds.

---

## BUILD INSTRUCTIONS

```batch
cd D:\SCIFI_game\TheDawningV3
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Debug

REM For RT: copy dxcompiler.dll + dxil.dll to build\Debug\ next to the .exe
```

Or use `SETUP_AND_BUILD.bat` which automates this.

---

## TESTING CHECKLIST (for any DC session)

After any changes, verify:
1. **Compiles cleanly** with MSVC /W4 (warnings as errors is off but should be clean)
2. **Runs without crash** — window opens, scene renders
3. **Rasterization mode works** — lit scene with spinning objects
4. **F1 toggles to path tracing** — noisy but converging image (if DXC DLLs present)
5. **Camera controls work** — click to capture, WASD + mouse, ESC to release
6. **Window resize works** — back buffer and RT output texture resize correctly
7. **No GPU validation errors** — check Output window in VS with debug layer enabled
8. **Title bar updates** — shows FPS, entity count, and render mode
9. **Clean shutdown** — no crashes on exit, all resources released in reverse order
