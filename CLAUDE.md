# CLAUDE.md - The Dawning V3 Engine

## PROJECT

The Dawning is a D3D12 space game engine built from scratch in C++20.
It is a fresh rebuild taking a layer-by-layer approach: each layer is brought
up far enough for the next one to stand on, then revisited as needed.

The current source tree is the canonical V3 engine. Older archives are
historical references only and must not be merged directly into this tree.

Read these files before editing:

- `README.md`
- `AGENT_COORDINATION.md`
- The relevant `src/`, `shaders/`, `tests/`, and `tools/` files for the task

Verification is a unit-test target covering core math and ECS invariants plus
one end-to-end smoke test (`tools/smoke_test.ps1`). That is not comprehensive
coverage: most of the render and DXR layers are exercised only by the smoke
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
    InstanceID(). This is a root SRV, not SM 6.6 bindless; there is no
    ResourceDescriptorHeap anywhere in the project
- Path tracing shader (path_trace.hlsl):
  - Multi-bounce iterative path tracing (max bounces driven by the quality mode)
  - Cook-Torrance BRDF (GGX NDF, Smith geometry, Fresnel-Schlick) for direct
    lighting under NEE. The indirect specular bounce (path_trace.hlsl:486-498)
    does NOT evaluate the BRDF or divide by a PDF; it multiplies throughput by
    Fresnel alone, which is only correct in the mirror limit
  - Next Event Estimation with shadow rays (ACCEPT_FIRST_HIT optimization)
  - Cosine-weighted hemisphere sampling for diffuse bounces
  - Roughness-perturbed reflection for specular bounces
  - Russian roulette path termination. Note the estimator is not unbiased:
    throughput is hard-clamped to 4.0 (path_trace.hlsl:502) and radiance is
    firefly-clamped against the previous frame
  - PCG hash RNG seeded per pixel and per frame index. The frame index supplied
    is the accumulation index (path_tracer.cpp:674), which resets to 0 on any
    camera or quality change, so while the camera is moving every frame draws
    the identical random sequence
  - NO temporal accumulation. `accumulatedRadiance` (path_trace.hlsl:525) is a
    pass-through alias for the current frame's radiance. The HDR history texture
    is read only as the firefly-clamp reference; it is never blended
  - Firefly clamping (references the previous frame's already-clamped output)
- DXC shader compiler integration (dxcompiler.dll loaded on-demand for lib_6_3+)
- Dual render mode: F1 toggles between rasterization and path tracing
- RT output copied to back buffer (future: DLSS Ray Reconstruction)
- Title bar shows render mode + FPS + entity count

## BUILD

All commands run from the repository root. Do not hardcode an absolute path;
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

Smoke tests (after building, from the repository root):

```powershell
.\tools\smoke_test.cmd -RasterOnly -Seconds 1.5 -TimeoutSeconds 8
.\tools\smoke_test.cmd -Seconds 3 -TimeoutSeconds 12
.\tools\smoke_test.cmd -FullQuality -Seconds 3 -TimeoutSeconds 12
```

Run smoke tests serially within one checkout because they share one log file.
If path tracing fails to initialize, confirm `dxcompiler.dll` and `dxil.dll`
are next to the executable.

## LAYER ROADMAP

Layer 1: Kernel (DONE) - window, D3D12, timer, input, math, DRED, capabilities
Layer 2: Geometry Pipeline (DONE) - shaders, PSO, mesh upload, camera, lit scene
Layer 3: Core Architecture (DONE) - ECS, resource manager, entity-driven rendering
Layer 3+RT: Path Tracing (DONE) - DXR pipeline, BLAS/TLAS, multi-bounce path tracer
Layer 4: Material System (PARTIAL) - see below. README.md's "Layer 4 material
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
Layer 5: World Foundation - terrain, atmosphere shader, sky dome, camera-relative

## RT UPGRADE PATH (future)

Phase 1 (current): Basic path tracing with NEE. Temporal accumulation is NOT
         implemented; the CPU-side apparatus (accumulation index, camera-change
         detection, HDR history texture) exists but the shader never blends.
         Implementing it is the first item in this phase, not a finished one
Phase 2: NVIDIA RTXDI (ReSTIR DI) for efficient direct light sampling
Phase 3: ReSTIR GI for indirect illumination resampling
Phase 4: SER (Shader Execution Reordering) via NvHitObject/NvReorderThread
Phase 5: DLSS 3.5 Ray Reconstruction (replaces manual denoising + upscaling)
Phase 6: DLSS Frame Generation for 2x effective FPS
Phase 7: Opacity Micro-Maps for alpha-tested geometry (foliage, fences)

## PARALLEL AGENT RULES

Use Git worktrees for parallel work. Do not have Codex and Claude Code edit the
same physical checkout at the same time.

- Codex branches use `codex/<task-slug>`.
- Claude branches use `claude/<task-slug>`.
- Local agent worktrees live under `.agents\worktrees\` and are ignored by Git.
- Use `tools\agent_worktree.ps1` to create an isolated checkout.
- Use `tools\agent_status.ps1` and `tools\agent_overlap.ps1` before merging.
- Build and test each branch before merging it into `main`.

## RULES

1. TARGET, NOT YET ENFORCED - double precision (Vec3d) for world positions,
   Vec3f only after camera subtraction. This is aspirational for Layer 5+ and
   matches CONVERSATION_CONTEXT.md:72. Reality today: `Vec3d` has zero
   references anywhere outside its own definition in `core/types.h`.
   `ecs::Transform::position` and `Camera::m_position` are both `Vec3f`, and
   `Vec3d` (types.h:126-160) lacks `Cross`, `operator*=`, `operator/=`,
   scalar-left `operator*`, and `Lerp`, all of which `Vec3f` has, so it cannot
   receive the conversion as written. Do not write new code that assumes
   double-precision world positions are available.
2. Proper .h/.cpp split. No header-only implementations except tiny inlines/templates.
3. No copyrighted terms. Rename map: Starfleet to Vanguard, LCARS to HELIX,
   Phaser to Arc Lance, Photon Torpedo to Fusion Torpedo, Dilithium to Helion,
   Federation to Commonwealth, etc.
4. Always include <cstdint> when using uint32_t/uint64_t.
5. Compile after every change.
6. Fixed timestep for physics (accumulator pattern, default 60Hz).
7. LEFT-HANDED coordinate system everywhere. +Z is forward (into screen).
   LookAt is LH. PerspectiveFovLH maps Z to [0,1]. Back-face culling is CW.
8. Logger is NOT thread-safe. Call only from main thread until job system exists.
