# CLAUDE.md — The Dawning V3 V3 Engine (Layer-by-Layer Rebuild)

## PROJECT

The Dawning is a D3D12 space game engine built from scratch in C++20.
It is a fresh rebuild taking a layer-by-layer approach: each layer is brought
up far enough for the next one to stand on, then revisited as needed.

Verification is a unit-test target covering core math and ECS invariants plus
one end-to-end smoke test (`tools/smoke_test.ps1`). That is not comprehensive
coverage — most of the render and DXR layers are exercised only by the smoke
test, and there is no CI. Read "DONE" in the roadmap below as "runs, and later
layers depend on it", not as "finished, hardened, or fully tested".

## CURRENT STATE: Layer 3+RT (ECS + Full Path Tracing)

Layer 3 provides ECS architecture; the RT extension adds full DXR path tracing:
- Sparse-set ECS with generational entity IDs, type-indexed component pools
- Handle-based ResourceManager for meshes and materials
- Scene class with phase-based update/render systems
- DXR 1.1 ray tracing pipeline:
  - BLAS built per unique mesh (PREFER_FAST_TRACE, opaque geometry)
  - TLAS rebuilt every frame from ECS entity transforms
  - State object: ray generation + primary closest-hit + shadow hit + 2 miss shaders
  - Shader Binding Table with proper 64/32-byte alignment
  - Global root signature, 9 parameters (rt_pipeline.cpp:55-131): TLAS SRV (t0),
    output UAV table (u0-u1), per-frame CB (b0), material StructuredBuffer (t1),
    triangle normals (t2), instance metadata (t3), triangle UVs (t4), triangle
    positions (t5), and an SRV table for albedo (t0/space4) + normal (t0/space5)
    textures
  - Per-instance material lookup: global material StructuredBuffer indexed by
    InstanceID(). This is a root SRV, not SM 6.6 bindless — no
    ResourceDescriptorHeap anywhere in the project
- Path tracing shader (path_trace.hlsl):
  - Multi-bounce iterative path tracing (max bounces driven by the quality mode)
  - Cook-Torrance BRDF (GGX NDF, Smith geometry, Fresnel-Schlick) for direct
    lighting under NEE
  - Next Event Estimation with shadow rays (ACCEPT_FIRST_HIT optimization)
  - Cosine-weighted hemisphere sampling for diffuse bounces
  - Russian roulette path termination. The estimator is still not unbiased:
    throughput is hard-clamped to 4.0 (path_trace.hlsl) and samples are
    firefly-clamped to a fixed luminance ceiling before accumulation
  - PCG hash RNG, each dimension hashed in sequence. Seeded from a wall-clock
    dispatch counter (`seedIndex`), NOT the accumulation index — the two are
    separate fields precisely because the accumulation index resets on camera
    motion and would otherwise pin every moving frame to one random sequence
  - Temporal accumulation: progressive running mean weighted 1/(n+1) over the
    accumulation index, which the CPU resets to 0 on any camera or quality
    change. Equal weight per frame, so variance falls as 1/n while the view is
    still. No reprojection — a moving camera restarts accumulation
  - Firefly clamping against a fixed per-sample luminance ceiling, applied before
    the sample enters the mean. NaN/Inf samples are dropped rather than
    propagated into the history buffer
  - Indirect specular uses GGX VNDF importance sampling; with a separable Smith
    G the Monte Carlo weight reduces to F * G1(NdotL). Rays sampled below the
    horizon terminate the path
- DXC shader compiler integration (dxcompiler.dll loaded on-demand for lib_6_3+)
- Dual render mode: F1 toggles between rasterization and path tracing
- RT output copied to back buffer (future: DLSS Ray Reconstruction)
- Title bar shows render mode + FPS + entity count

## BUILD

All commands run from the repository root. Do not hardcode an absolute path —
the repo is relocatable.

```batch
SETUP_AND_BUILD.bat
```

`SETUP_AND_BUILD.bat` locates `cmake` on `PATH`, then falls back to the common
Visual Studio 2022 bundled locations, and runs the two steps below. Manual
equivalent:

```batch
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

Output: `build\Debug\TheDawningV3.exe`. Runtime log: `build\Debug\TheDawning.log`.

Smoke test (after building, from the repository root):

```batch
tools\smoke_test.cmd
```

## LAYER ROADMAP

Layer 1: Kernel (DONE) — window, D3D12, timer, input, math, DRED, capabilities
Layer 2: Geometry Pipeline (DONE) — shaders, PSO, mesh upload, camera, lit scene
Layer 3: Core Architecture (DONE) — ECS, resource manager, entity-driven rendering
Layer 3+RT: Path Tracing (DONE) — DXR pipeline, BLAS/TLAS, multi-bounce path tracer
Layer 4: Material System (PARTIAL) — see below. README.md's "Layer 4 material
         path" refers to this partial state, not a finished layer.
  Done:    KTX v1 / PNG (WIC) / DDS loaders with procedural fallbacks, CPU mip
           generation for RGBA sources, albedo + normal maps, Cook-Torrance GGX
           raster shading, indexed shader-visible texture tables in both the
           raster and DXR paths
  Not done: SM 6.6 bindless (raster still compiles vs_5_1/ps_5_1 through FXC at
           renderer.cpp:218-219, 287-288), HDR render target and a real tone-map
           pass (both raster PSOs hardcode R8G8B8A8_UNORM and tone map in the
           pixel shader), metallic/roughness/AO/emissive maps, shadow maps, and
           any real mesh file loading. `assets/textures/` ships only a README,
           so a clean clone always takes the procedural fallback path
Layer 5: World Foundation — terrain, atmosphere shader, sky dome, camera-relative

## RT UPGRADE PATH (future)

Phase 1 (current): Path tracing with NEE, VNDF specular sampling, and a
         progressive running-mean accumulator that resets on camera motion.
         Not yet done in this phase: reprojection, so accumulation restarts
         whenever the view changes rather than reusing history
Phase 2: NVIDIA RTXDI (ReSTIR DI) for efficient direct light sampling
Phase 3: ReSTIR GI for indirect illumination resampling
Phase 4: SER (Shader Execution Reordering) via NvHitObject/NvReorderThread
Phase 5: DLSS 3.5 Ray Reconstruction (replaces manual denoising + upscaling)
Phase 6: DLSS Frame Generation for 2x effective FPS
Phase 7: Opacity Micro-Maps for alpha-tested geometry (foliage, fences)

## RUNTIME DEPENDENCIES

- dxcompiler.dll + dxil.dll must be in the executable directory for RT shaders.
  Get from: https://github.com/microsoft/DirectXShaderCompiler/releases
  Or from the Microsoft.Direct3D.DXC NuGet package.

## RULES

1. ENFORCED — double precision (`Vec3d`) for world positions, `Vec3f` only after
   camera subtraction. `ecs::Transform::position` and `Camera::m_position` are
   `Vec3d`; `Scene` produces camera-relative float matrices for both raster and
   DXR. GPU camera position is local zero, while path-tracing history compares
   the full double-precision position. New world-space quantities must use
   `Vec3d`; never narrow an absolute world position before camera subtraction.
2. Proper .h/.cpp split. No header-only implementations except tiny inlines/templates.
3. No copyrighted terms. Rename map: Starfleet→Vanguard, LCARS→HELIX, Phaser→Arc Lance,
   Photon Torpedo→Fusion Torpedo, Dilithium→Helion, Federation→Commonwealth, etc.
4. Always include <cstdint> when using uint32_t/uint64_t.
5. Compile after every change.
6. Fixed timestep for physics (accumulator pattern, default 60Hz).
7. LEFT-HANDED coordinate system everywhere. +Z is forward (into screen).
   LookAt is LH. PerspectiveFovLH maps Z to [0,1]. Back-face culling is CW.
8. Logger is NOT thread-safe. Call only from main thread until job system exists.

## PARALLEL AGENT RULES

Read `AGENT_COORDINATION.md` before editing. Codex and Claude Code must use
separate Git worktrees and named branches, inspect live repository state before
each pass, report branch/commit/files/tests at handoff, and check file overlap
before integration.
