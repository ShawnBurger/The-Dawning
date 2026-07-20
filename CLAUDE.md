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
    positions (t5), and an SRV table for albedo (t0/space4), normal (t0/space5)
    ORM (t0/space6) and emissive (t0/space7) textures
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
           raster and DXR paths, packed occlusion/roughness/metallic (ORM) maps
           in the glTF channel convention modulating the material scalars in
           both paths, emissive maps in both paths, bloom with a parameterised
           exposure, and a linear
           R16G16B16A16_FLOAT scene target resolved to the back buffer by a
           dedicated tone-map pass
           (shaders/tonemap_ps.hlsl). Tone mapping happens exactly once, not per
           material shader; post-process passes insert between
           Scene::RenderEntities and Renderer::ResolveToBackBuffer
           Directional shadows are CASCADED: 4 slices of one 2048x2048
           Texture2DArray (64 MiB), fixed half-extents {24, 65, 175, 470},
           selected radially in the pixel shader, with a world-anchored
           double-precision texel snap so distant edges do not crawl as the
           camera translates. Reach is ~448 world units, up from 24. Cascade 0 is
           bit-identical to the old single cascade, so the near field is
           unchanged. Cascades cost NOTHING per entity in the constant ring:
           per-object and per-material data live in structured buffers indexed
           by a root constant, and the light view-projection is a per-PASS
           cbuffer, so each cascade costs one flat 256-byte upload and the four
           cascades share one set of object records. Ring peak is 1792 bytes,
           flat, in both smoke modes
           Raster smoke validates draw records ON THE GPU through one merged
           probe at u0/space4, 16 bytes per object record. Each slot carries two
           independent claims: a HASH of every field of the record the shader
           loaded, which catches CPU/HLSL layout drift, and a MARKER read out of
           the record's own recordId field, which catches wrong-element
           indexing. Neither covers the other - shifted garbage still comes from
           the right element, and two records with equal contents hash equal.
           The CPU reads the buffer back after queue retirement, compares
           against the mapped upload records, and reduces to per-pass record,
           distinct-index and mismatch counts so a failure names a shader.
           WHICH STAGE WRITES WHAT IS THE LOAD-BEARING PART. Both vertex shaders
           write the object words, because the vertex stage is where
           objectBuffer is consumed; basic_ps writes the material words, beside
           the `mat` it shades with, because the pixel stage is where
           materialBuffer is consumed. Hashing the material record from the
           vertex shader - which reads it for no other purpose - witnesses the
           probe's own load and stays green with basic_ps hardcoded to
           materialBuffer[0]. All four mutations are watched failing: object
           indexing in each vertex shader, material indexing in the pixel
           shader, and the deferred-release fence tagging.
           A third b3 constant gates the writes to the final smoke frame, so
           ordinary frames pay no per-invocation witness cost. The UAV
           DECLARATION in basic_ps is not gated and cannot be - it defeats
           early-Z for that PSO in every configuration, permanently. That is a
           real cost, accepted because the alternative is a material index that
           nothing checks
  Not done: SM 6.6 bindless (raster still compiles vs_5_1/ps_5_1 through FXC)
           and any real mesh file loading. Emissive surfaces shade
           themselves but are NOT light sources - nothing samples them, so a
           bright panel does not illuminate anything around it.
           `assets/textures/` ships only a README, so a clean clone always takes
           the procedural fallback path
Layer 5: World Foundation — terrain, atmosphere shader, sky dome, camera-relative

## RT UPGRADE PATH (future)

Phase 1 (current): Path tracing with NEE, VNDF specular sampling, and a
         progressive running-mean accumulator that resets on camera motion.
         Not yet done in this phase: reprojection, so accumulation restarts
         whenever the view changes rather than reusing history. Accumulation
         also does not reset on OBJECT motion, only camera motion, so animated
         geometry ghosts across the history buffer. That is deliberate - the
         demo spins continuously, and resetting per object would disable
         accumulation outright - but it is why moving objects look smeared in
         path tracing. Reprojection is the actual fix
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
   ONE SANCTIONED EXCEPTION: `core::BuildShadowCascadeMatrix` takes the absolute
   `Vec3d` camera position. It uses it only to quantise the shadow texel lattice
   onto a grid fixed to the world origin, entirely in double, and narrows only a
   sub-texel RESIDUAL - never a position. The exception is necessary because
   quantising a camera-relative coordinate quantises nothing: the lattice would
   move with the thing being quantised, giving a snap that compiles, does not
   crash, and shimmers exactly as much as no snap at all. Two unit cases pin
   this, `Cascades_MatricesAreCameraPositionInvariant` (probed at 1e7) and
   `Cascades_SnapIsStableUnderSubTexelCameraMotion`.
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
