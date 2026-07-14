# CLAUDE.md — The Dawning V3 V3 Engine (Layer-by-Layer Rebuild)

## PROJECT

The Dawning is a D3D12 space game engine built from scratch in C++20.
This is a fresh rebuild taking a layer-by-layer approach — each layer is
complete, tested, and AAA quality before the next begins.

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
  - Global root signature: TLAS SRV, output UAV, per-frame CB, material StructuredBuffer
  - Bindless material access via InstanceID() indexing into global buffer
- Path tracing shader (path_trace.hlsl):
  - Multi-bounce iterative path tracing (configurable max bounces)
  - Cook-Torrance BRDF (GGX NDF, Smith geometry, Fresnel-Schlick)
  - Next Event Estimation with shadow rays (ACCEPT_FIRST_HIT optimization)
  - Cosine-weighted hemisphere sampling for diffuse bounces
  - Roughness-perturbed reflection for specular bounces
  - Russian Roulette for unbiased path termination
  - PCG hash RNG with per-pixel temporal variation
  - Simple temporal accumulation (10% blend per frame)
  - Firefly clamping
- DXC shader compiler integration (dxcompiler.dll loaded on-demand for lib_6_3+)
- Dual render mode: F1 toggles between rasterization and path tracing
- RT output copied to back buffer (future: DLSS Ray Reconstruction)
- Title bar shows render mode + FPS + entity count

## BUILD

```batch
cd D:\SCIFI_game\TheDawning
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Debug
```

## LAYER ROADMAP

Layer 1: Kernel (DONE) — window, D3D12, timer, input, math, DRED, capabilities
Layer 2: Geometry Pipeline (DONE) — shaders, PSO, mesh upload, camera, lit scene
Layer 3: Core Architecture (DONE) — ECS, resource manager, entity-driven rendering
Layer 3+RT: Path Tracing (DONE) — DXR pipeline, BLAS/TLAS, multi-bounce path tracer
Layer 4: Material System — PBR textures, normal maps, DXC SM 6.6 bindless
Layer 5: World Foundation — terrain, atmosphere shader, sky dome, camera-relative

## RT UPGRADE PATH (future)

Phase 1 (current): Basic path tracing with NEE + temporal accumulation
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

1. Double precision (Vec3d) for world positions. Vec3f only after camera subtraction.
2. Proper .h/.cpp split. No header-only implementations except tiny inlines/templates.
3. No copyrighted terms. Rename map: Starfleet→Vanguard, LCARS→HELIX, Phaser→Arc Lance,
   Photon Torpedo→Fusion Torpedo, Dilithium→Helion, Federation→Commonwealth, etc.
4. Always include <cstdint> when using uint32_t/uint64_t.
5. Compile after every change.
6. Fixed timestep for physics (accumulator pattern, default 60Hz).
7. LEFT-HANDED coordinate system everywhere. +Z is forward (into screen).
   LookAt is LH. PerspectiveFovLH maps Z to [0,1]. Back-face culling is CW.
8. Logger is NOT thread-safe. Call only from main thread until job system exists.
