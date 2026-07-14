# DC_HANDOFF.md — The Dawning V3 Complete Layer-by-Layer Instructions

> **Purpose**: This file gives Claude Code (DC) full context to continue building
> The Dawning V3 engine. Read this before making any changes. It covers what was
> built, why, what the known issues are, and what comes next.

---

## PROJECT OVERVIEW

**The Dawning** is a custom D3D12 space game engine written from scratch in C++20
for Windows. It uses a proprietary ECS architecture and targets an NVIDIA RTX 4090
(Ada Lovelace, 24GB VRAM, DXR 1.1, 3rd-gen RT cores).

This is the **V3 rebuild** — a layer-by-layer approach where each layer is complete
and hardened before the next begins. V1 was 287 files/60K lines (header-only, had
naming issues). V2 was a 3000-step DC blitz that produced 337 files/137K lines but
had critical gaps (checkerboard textures, only 5 shaders, no audio, no sockets).
V3 is the "do it right" rebuild.

**Build target**: Windows, MSVC (VS 2022), CMake, x64 only.
**Hardware target**: NVIDIA RTX 4090, but rasterization path works on any D3D12 GPU.

---

## WHAT HAS BEEN BUILT (Layers 1-3+RT)

### Layer 1: Kernel (DONE) — 2,311 lines, 13 files

The foundation. Everything that needs to exist before a single pixel is drawn.

**Files:**
- `src/core/types.h` — Vec2f, Vec3f, Vec3d, Vec4f, Color, Quatf, Mat4x4 (with
  LookAt LH, PerspectiveFovLH, InverseTranspose3x3, FromQuaternion)
- `src/core/log.h/.cpp` — Timestamped logger. NOT thread-safe (by design until
  a job system exists). Uses OutputDebugStringA.
- `src/core/input.h/.cpp` — Keyboard + mouse state tracking via raw input.
  Per-frame delta tracking. FlushAll() on focus loss.
- `src/core/timer.h/.cpp` — QPC high-resolution timer with fixed timestep
  accumulator pattern (default 60Hz for future physics).
- `src/core/window.h/.cpp` — Win32 window creation. Raw input registration.
  Mouse capture with ClipCursor (refreshes clip rect on resize). Minimized/
  resize state tracking. Focus loss handling.
- `src/render/d3d12_device.h/.cpp` — Full D3D12 initialization. Debug layer.
  DRED (configured BEFORE device creation). Triple buffering with per-frame
  command allocators and fence sync. GpuCapabilities struct probing shader
  model, mesh shaders, DXR tier, VRS, tearing, HDR, enhanced barriers.
  Device-lost detection with graceful exit.
- `src/main.cpp` — Game loop: input → timer → camera → render → present.

**Key decisions:**
- LEFT-HANDED coordinate system everywhere. +Z forward. CW winding = front face.
  `FrontCounterClockwise = FALSE`. This is the D3D12 default.
- Double precision Vec3d exists for future large-world positions. Currently unused —
  all rendering is float-based. The large-world camera-relative rendering is a
  Layer 5 concern.
- DRED auto-breadcrumbs and page-fault reporting are ON in debug builds.
- All GPU objects are named via SetName() for DRED readability.

**Known limitation:** Logger is single-threaded. If you add a job system, you must
add a thread-safe logging path first.

### Layer 2: Geometry Pipeline (DONE) — 3,934 lines, 22 files

First visible rendering. Shaders, mesh upload, camera, lit scene.

**New files:**
- `src/render/shader_utils.h/.cpp` — Dual compiler: D3DCompile for SM 5.1,
  DXC (dxcompiler.dll, loaded on demand) for SM 6.0+ and DXR libraries.
  Automatically routes based on target profile string.
- `src/render/mesh.h/.cpp` — Vertex format: position(f3) + normal(f3) +
  color(f4) + uv(f2) = 48 bytes. Upload-to-VRAM staging pattern: create
  DEFAULT heap buffer, create UPLOAD heap staging, copy, transition. Supports
  both 16-bit (`CreateMesh`) and 32-bit (`CreateMesh32`) index formats.
  Primitive generators: cube (CW winding), plane (subdivided), sphere (UV).
  Input validation on all generators.
- `src/render/camera.h/.cpp` — FPS fly camera. WASD + mouse look + sprint.
  Pitch clamped [-89, 89]. Diagonal movement normalized. Produces LH
  view/projection matrices.
- `src/render/renderer.h/.cpp` — Root signature v1.1 (actual v1.1 with
  DATA_VOLATILE flags, fallback to v1.0). 3 root CBVs (per-object b0,
  per-frame b1, material b2) + 1 static sampler. 6 DWORDs used of 64 max.
  Classic PSO (stream upgrade deferred). Per-frame constant buffer upload
  ring: 256KB per frame, persistently mapped, 256-byte aligned sub-allocation.
  Real InverseTranspose3x3 for correct normals under non-uniform scale.
- `shaders/basic_vs.hlsl` — Transforms position, outputs world-space position
  + normal for lighting.
- `shaders/basic_ps.hlsl` — PBR-lite: Lambert diffuse + Blinn-Phong specular
  with Fresnel-Schlick, hemisphere ambient, roughness/metallic parameters.

**Key decisions:**
- Root parameters ordered by frequency: per-object (hottest) first, then
  per-frame, then material. This matches AMD RDNA SGPR budget advice.
- Static sampler is free (0 DWORDs). Always use static samplers for immutable
  samplers.
- DENY_HULL/DOMAIN/GEOMETRY_SHADER_ROOT_ACCESS flags set.
- Shader compilation uses D3DCompile for rasterization shaders (SM 5.1) and
  DXC for ray tracing (SM 6.3+). DXC is loaded lazily.

**Known limitations:**
- No texture support yet — vertex colors only.
- No HDR / tone mapping / post-processing pipeline.
- Upload path is synchronous (WaitForGpu after upload). Fine for startup,
  needs async for streaming.

### Layer 3: Core Architecture (DONE) — 5,034 lines, 30 files

ECS, resource management, entity-driven rendering.

**New files:**
- `src/ecs/entity.h` — 32-bit generational entity ID (20-bit index + 12-bit
  generation). EntityManager with intrusive free list. O(1) create/destroy/
  is_alive. Max ~1M concurrent entities, 4096 generations per slot.
- `src/ecs/component_pool.h` — Sparse-set storage per component type. O(1)
  add/remove/get via swap-and-pop. Dense arrays for cache-friendly iteration.
  Type-erased IComponentPool base for heterogeneous storage in the registry.
- `src/ecs/components.h` — Transform (pos/rot/scale + ToMatrix()), Velocity,
  MeshInstance (handle + visible flag), Material (albedo/roughness/metallic),
  RotationSpeed (axis + rate), Parent (entity index), Name (48-char debug).
- `src/ecs/registry.h` — Central coordinator. Owns EntityManager + type-indexed
  pools via std::unordered_map<type_index, unique_ptr<IComponentPool>>.
  Template methods: Assign, Remove, Get, Has. Multi-component iteration via
  Each<A,B>(func) and Each<A,B,C>(func) — iterates smaller pool, cross-checks
  larger. GetByIndex() for systems iterating raw pool indices.
- `src/scene/resource_manager.h/.cpp` — Slot-map pools for meshes and materials
  with generational handles (ResourceHandle = 20-bit index + 12-bit gen).
  Multiple entities can share same mesh handle. AddMesh takes ownership via
  std::move. Free list recycling with generation bumping.
- `src/scene/scene.h/.cpp` — Owns Registry + ResourceManager (+ PathTracer
  after RT integration). Helper methods: CreateRenderable(), CreateSpinner().
  Phase execution: UpdateSystems(dt) runs RotationSystem. RenderEntities()
  iterates MeshInstance pool, resolves handles, calls renderer.DrawMesh().

**Key decisions:**
- Sparse-set over archetype. Simpler implementation, excellent for small-medium
  entity counts. Component add/remove is O(1) with no migration cost. The
  research showed archetypes win at 100K+ entities for bulk iteration, but
  sparse sets are better for the dynamic composition patterns in gameplay.
- Entities store mesh handles (uint32_t), never raw GPU pointers. This decouples
  entity lifetime from GPU resource lifetime.
- Scene graph is NOT a separate data structure. Transform hierarchy will use
  Parent/Children ECS components when needed (currently flat).

**Main loop change:** Scene creation is now entity-driven. main.cpp calls
`CreateRenderable()` / `CreateSpinner()` to populate the scene. The render loop
calls `gameScene.RenderEntities()` which generically iterates all renderable
entities. No per-object draw calls in main.cpp.

### Layer 3+RT: Path Tracing (DONE) — 7,173 lines, 37 files

Full DXR path tracing integrated with the ECS pipeline.

**New files:**
- `src/render/rt_acceleration.h/.cpp` — BLASEntry pool + per-frame TLAS.
  BuildBLAS() creates BLAS from Mesh vertex/index buffers with
  PREFER_FAST_TRACE + GEOMETRY_FLAG_OPAQUE. BuildTLAS() creates/grows instance
  upload buffer, fills D3D12_RAYTRACING_INSTANCE_DESC from TLASInstance structs,
  builds with PREFER_FAST_BUILD. UAV barriers after each build.
- `src/render/rt_pipeline.h/.cpp` — DXR state object creation. 5 shader exports:
  RayGen, ClosestHit, ShadowClosestHit, Miss, ShadowMiss. 2 hit groups (primary
  + shadow). Global root signature: [0] TLAS SRV, [1] output UAV table, [2]
  per-frame CBV, [3] material StructuredBuffer SRV. Shader table builder with
  correct alignment (64-byte table start, 32-byte record stride). 2 ray types
  per instance (primary + shadow).
- `src/render/path_tracer.h/.cpp` — Orchestrator. Creates output UAV texture
  (R8G8B8A8_UNORM), descriptor heap, per-frame CBs (triple-buffered), material
  structured buffer. Dispatch() uploads constants + materials, binds root params,
  issues DispatchRays. CopyToBackBuffer() handles resource transitions and copy.
- `shaders/path_trace.hlsl` — Megakernel path tracer:
  - Iterative multi-bounce loop (MaxTraceRecursionDepth=2, loop is iterative)
  - Cook-Torrance BRDF: GGX NDF, Smith geometry, Fresnel-Schlick
  - Next Event Estimation: directional light sampling + shadow ray
  - Shadow rays: ACCEPT_FIRST_HIT | SKIP_CLOSEST_HIT | FORCE_OPAQUE flags
  - BSDF sampling: probability split (diffuse cosine-hemisphere vs specular
    roughness-perturbed reflection)
  - Russian Roulette after bounce 1 with throughput compensation
  - PCG hash RNG seeded per-pixel per-frame
  - Temporal accumulation: 10% new + 90% history (converges when static)
  - Firefly clamping at 10.0
  - Sky gradient on miss

**Scene integration:**
- `scene.h/cpp` updated: InitPathTracer(), EnsureBLAS(), BuildAccelerationStructures(),
  PathTraceEntities(), CopyPathTraceToBackBuffer(). EnsureBLAS() maps mesh handles
  to BLAS indices (built once per unique mesh). BuildAccelerationStructures()
  collects TLAS instances from all renderable entities each frame.
  PathTraceEntities() collects materials in instance order (matching InstanceID
  indexing in the shader).
- `main.cpp` updated: F1 toggles between rasterization and path tracing. Title bar
  shows current mode. RT init builds BLAS on startup. In RT mode, TLAS rebuild +
  dispatch happens each frame.

**Key decisions:**
- Megakernel architecture (single DispatchRays, iterative bounces). This is
  what Cyberpunk 2077, Portal RTX, and Indiana Jones use. SER (future) fixes
  the coherence problem without needing wavefront decomposition.
- Global root signature with bindless material access. No local root signatures.
  Materials indexed by InstanceID() into a global StructuredBuffer. This
  eliminates SBT complexity and matches modern practice.
- DXC loaded on-demand when lib_6_* target is requested. Falls back gracefully
  if dxcompiler.dll is not present (RT just won't initialize).

**Known issues / TODOs in the RT path:**
1. **Normals in closest-hit shader are approximate.** The shader doesn't read
   vertex normals from a ByteAddressBuffer — it computes a geometric normal
   from the ObjectToWorld transform. This produces flat shading. Fix: add
   vertex buffer SRVs to the global root signature, read vertex normals via
   PrimitiveIndex() + index buffer lookup.
2. **Ray direction reconstruction is approximate.** The shader extracts camera
   basis vectors from the view-projection matrix rows. Fix: pass explicit
   camera right/up/forward vectors in the constant buffer, or pass the actual
   inverse view-projection matrix (requires Mat4x4::Inverse()).
3. **No vertex buffer access in hit shaders.** Materials work via InstanceID()
   → StructuredBuffer, but vertex attributes (position, normal, UV) need
   bindless vertex/index buffer access. Fix: add SRV array of all vertex
   buffers to global root signature, indexed by InstanceID + GeometryIndex.
4. **Temporal accumulation is naive.** Fixed 10% blend with no motion vector
   reprojection. Moving the camera causes ghosting. Fix: generate motion
   vectors, reproject, use disocclusion detection.
5. **No denoiser.** Raw 1-SPP output is noisy. The upgrade path is:
   NRD (ReBLUR/ReLAX) → DLSS 3.5 Ray Reconstruction (replaces NRD entirely).
6. **Output is R8G8B8A8_UNORM.** Should be R16G16B16A16_FLOAT for HDR path
   tracing, then tone-mapped to LDR for display.
7. **Shader table rebuilt every frame.** Should only rebuild when instance count
   changes.
8. **BLAS not compacted.** Compaction saves ~45-50% memory. Implement the
   query-readback-copy workflow.
9. **No BSDF importance sampling for GGX.** Specular bounces use roughness-
   perturbed reflection instead of proper VNDF sampling. Fix: implement
   Dupuy-Benyoub 2023 spherical cap VNDF sampling.

---

## FILE STRUCTURE

```
dawning_v3/
├── CLAUDE.md               — Project rules (DC auto-reads this)
├── DC_HANDOFF.md           — This file
├── CMakeLists.txt          — Build config (VS 2022, x64, C++20)
├── SETUP_AND_BUILD.bat     — One-click build script
├── shaders/
│   ├── basic_vs.hlsl       — Rasterization vertex shader (SM 5.1)
│   ├── basic_ps.hlsl       — Rasterization pixel shader (SM 5.1)
│   └── path_trace.hlsl     — DXR path tracing library (SM 6.3)
└── src/
    ├── main.cpp            — Entry point, game loop, dual render mode
    ├── core/
    │   ├── types.h         — Math types (Vec2f/3f/3d/4f, Color, Quatf, Mat4x4)
    │   ├── log.h/.cpp      — Logger (NOT thread-safe)
    │   ├── input.h/.cpp    — Keyboard/mouse via raw input
    │   ├── timer.h/.cpp    — QPC timer + fixed timestep accumulator
    │   └── window.h/.cpp   — Win32 window + mouse capture
    ├── ecs/
    │   ├── entity.h        — Generational entity ID + EntityManager
    │   ├── component_pool.h — Sparse-set per-component-type storage
    │   ├── components.h    — All component types (Transform, Material, etc.)
    │   └── registry.h      — Central ECS coordinator
    ├── render/
    │   ├── d3d12_device.h/.cpp    — D3D12 init, DRED, caps, triple buffer
    │   ├── shader_utils.h/.cpp    — D3DCompile + DXC dual compiler
    │   ├── mesh.h/.cpp            — Vertex format, upload, generators
    │   ├── camera.h/.cpp          — FPS fly camera
    │   ├── renderer.h/.cpp        — Rasterization renderer (root sig, PSO, CB ring)
    │   ├── rt_acceleration.h/.cpp — BLAS/TLAS management
    │   ├── rt_pipeline.h/.cpp     — DXR state object + shader table
    │   └── path_tracer.h/.cpp     — RT output texture, dispatch, material upload
    └── scene/
        ├── resource_manager.h/.cpp — Handle-based mesh/material pools
        └── scene.h/.cpp            — ECS + ResourceManager + RT integration
```

---

## COORDINATE SYSTEM AND CONVENTIONS

- **Left-handed** everywhere. +X right, +Y up, +Z forward (into screen).
- **CW winding** = front face. `FrontCounterClockwise = FALSE`.
- **Row-major matrices** in CPU code. HLSL uses `mul(matrix, vector)`.
- **Depth range** [0, 1] (D3D12 default). Near plane maps to 0, far to 1.
- **PerspectiveFovLH** with `z / (farZ - nearZ)` mapping.
- **Back-face culling** enabled (CULL_MODE_BACK).
- **Depth test**: LESS (standard, not reverse-Z yet).

---

## BUILD INSTRUCTIONS

```batch
cd D:\SCIFI_game\TheDawningV3
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Debug
```

**For path tracing**: Copy `dxcompiler.dll` + `dxil.dll` into `build/Debug/`
(same directory as TheDawningV3.exe). Get from DXC GitHub releases or the
Microsoft.Direct3D.DXC NuGet package.

**For release**: Replace `Debug` with `Release` in the build command.

---

## ENGINE RULES (must follow)

1. **LEFT-HANDED coordinate system.** +Z forward, CW front, FrontCounterClockwise=FALSE.
2. **Double precision Vec3d** for world positions (when large-world is implemented).
   Vec3f only after camera subtraction for GPU data.
3. **Proper .h/.cpp split.** No header-only implementations except tiny inlines/templates.
   Exception: ECS component_pool.h and registry.h are header-only (templates).
4. **Always include `<cstdint>`** when using uint32_t/uint64_t.
5. **Compile after every change.** No batch-then-fix.
6. **Fixed timestep for physics** (accumulator pattern, 60Hz default).
7. **Logger is NOT thread-safe.** Main thread only until job system exists.
8. **No copyrighted terms.** Rename map: Starfleet→Vanguard, LCARS→HELIX,
   Phaser→Arc Lance, Photon Torpedo→Fusion Torpedo, Dilithium→Helion,
   Federation→Commonwealth, Borg→Xenoes, Klingon→Vesk, Warp→Foldspace.
9. **Name all GPU objects** via SetName() for DRED breadcrumb readability.
10. **256-byte alignment** for all constant buffer views.
11. **Resource barriers** between state transitions. UAV barriers between AS builds
    and TraceRay.
12. **Handle-based resource references** in ECS. Never store raw GPU pointers in
    components.

---

## LAYER 4: MATERIAL SYSTEM (NEXT)

What Layer 4 should deliver:

### 4.1 Texture Loading and Management
- Load PNG/DDS/KTX textures from disk into D3D12 textures
- Texture resource pool in ResourceManager with TextureHandle
- MipMap generation (compute shader or CPU-side)
- Upload via staging buffers (same pattern as mesh upload)
- BC7/BC5 compressed texture support for DDS

### 4.2 PBR Material Pipeline
- MaterialData struct: albedo texture, normal map, metallic-roughness map,
  emissive map, AO map
- Material component references texture handles
- Descriptor heap for all textures (bindless via SM 6.6 ResourceDescriptorHeap[])
- Update rasterization pixel shader for texture sampling
- Update path tracing closest-hit for textured materials

### 4.3 Normal Mapping
- Tangent-space normal map sampling in pixel shader and closest-hit
- Tangent vector in vertex format (adds 4 bytes: R8G8B8A8_SNORM with W=handedness)
- TBN matrix construction from vertex tangent + normal

### 4.4 DXC Migration for Rasterization Shaders
- Move rasterization shaders from SM 5.1 (D3DCompile) to SM 6.6 (DXC)
- Enable bindless textures via ResourceDescriptorHeap[]
- Remove D3DCompile dependency entirely

### 4.5 Fix RT Vertex Access
- Add vertex buffer and index buffer SRVs to the RT global root signature
- In closest-hit shader: read vertex normals and UVs via PrimitiveIndex()
  + LoadTriangleIndices() + interpolate with barycentrics
- Enable textured path tracing

### 4.6 HDR Rendering Pipeline
- Change RT output to R16G16B16A16_FLOAT
- Change rasterization render target to R16G16B16A16_FLOAT
- Add tone mapping pass (ACES or Khronos PBR Neutral)
- Add exposure control

---

## LAYER 5: WORLD FOUNDATION (FUTURE)

What Layer 5 should deliver:

### 5.1 Camera-Relative Rendering
- Subtract camera position (Vec3d) from all world positions before GPU upload
- All GPU-side positions are Vec3f relative to camera origin
- Eliminates floating-point jitter at large world coordinates
- This is the single most important change for a space game

### 5.2 Terrain System
- Procedural terrain generation (Perlin/Simplex noise)
- LOD system for terrain patches
- Terrain BLAS for ray tracing

### 5.3 Atmosphere Shader
- Rayleigh + Mie scattering for planetary atmospheres
- Sky dome rendering (compute or fullscreen pass)
- Integrate with path tracer miss shader for sky lighting

### 5.4 Star Field
- Procedural star rendering (point sprites or instanced quads)
- Brightness variation, color temperature
- Parallax based on camera movement

---

## RT UPGRADE PATH (Phases 2-7)

If Shawn wants to continue improving the path tracer toward Cyberpunk-tier:

### Phase 2: RTXDI (ReSTIR DI)
- Integrate NVIDIA RTXDI SDK (github.com/NVIDIA-RTX/RTXDI)
- Replace single directional light NEE with reservoir-based sampling
- Enables thousands of dynamic light sources (emissive meshes, point lights)

### Phase 3: ReSTIR GI
- Reservoir-based indirect illumination resampling
- Store visible point + secondary hit + radiance per reservoir
- Spatiotemporal reuse with Jacobian correction
- Massive variance reduction for 1-SPP indirect lighting

### Phase 4: SER (Shader Execution Reordering)
- Replace TraceRay() with NvTraceRayHitObject() + NvReorderThread() + NvInvokeHitObject()
- Requires NVAPI and SM 6.9 / DXR 1.2
- 24-47% performance improvement on RTX 40 series
- Minimize live state across ReorderThread calls

### Phase 5: DLSS 3.5 Ray Reconstruction
- Integrate via Streamline SDK (github.com/NVIDIA-RTX/Streamline)
- Replaces all separate denoisers with single AI pass
- Also replaces DLSS Super Resolution (combined denoise + upscale)
- Requires: noisy HDR color, diffuse/specular albedo, normals, roughness,
  motion vectors, depth

### Phase 6: DLSS Frame Generation
- Doubles effective FPS via Optical Flow Accelerator (RTX 40+ only)
- Requires mandatory Reflex integration
- ~2.77ms overhead, ~725MB VRAM

### Phase 7: Opacity Micro-Maps
- Pre-encode per-micro-triangle opacity for alpha-tested geometry
- Eliminates any-hit shader invocations in hardware
- Critical for foliage, fences, particle effects

---

## PERFORMANCE BUDGET (RTX 4090, 1080p internal → 4K DLSS)

| Pass                        | Target (ms) |
|-----------------------------|-------------|
| TLAS rebuild                | 0.3–1.0     |
| Path tracing (3 bounces)    | 8–15        |
| Denoising / DLSS RR         | 1–2         |
| Post-processing (4K)        | 0.5–1.5     |
| Frame generation             | ~2.77       |
| **Total rendered frame**    | **12–20**   |
| **Target FPS (with FG)**    | **100–160** |

---

## KNOWN GLOBAL ISSUES

1. **No Mat4x4::Inverse().** Only InverseTranspose3x3 exists. A general 4x4
   inverse is needed for camera inverse view-projection in the path tracer.
2. **No FromQuaternion() in Mat4x4.** RotationSpeed uses Quatf but Transform::
   ToMatrix() calls Mat4x4::FromQuaternion() which may not exist in types.h yet.
   Verify this compiles — if not, add the implementation.
3. **d3d12_device.h exposes ID3D12Device* not ID3D12Device5*.** The RT code casts
   via `static_cast<ID3D12Device5*>(device.Device())`. This works if the actual
   device is DXR-capable, but a proper QI would be safer.
4. **The D3D12Device class uses ID3D12GraphicsCommandList, not CommandList4.**
   RT code casts to CommandList4. Same concern as above.
5. **No async compute queue.** TLAS builds could overlap with other work on a
   separate queue. Currently everything runs on the direct queue.

---

## PREVIOUS SESSION TRANSCRIPTS

Full conversation history is available at `/mnt/transcripts/`. Key sessions:
- `2026-03-09-19-23-13` — 3000-step curriculum design
- `2026-03-11-00-03-28` — 24 research documents (D3D12, ECS, RT, materials, etc.)
- `2026-03-20-23-12-35` — V3 Layer 1 kernel build and 2 rounds of hardening

The master index of all research documents:
- `/mnt/user-data/outputs/TheDawning_MASTER_INDEX.md`
