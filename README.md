# The Dawning V3

The Dawning V3 is the canonical C++20/D3D12 engine baseline for this repository.
Older `SpaceEngineD3D12`, V1, and V2 archives are historical references only.
Do not merge old snapshots directly into this source tree.

## Current Baseline

- Windows-only, Visual Studio 2022, x64.
- CMake project targeting C++20.
- D3D12 raster renderer with ECS-driven scene rendering.
- Optional DXR path tracing path when the GPU and runtime DLLs support it.
- Deterministic 60 Hz simulation scheduler covering flight, passive orbits,
  gravity, atmosphere, collisions, FTL transitions, and relativistic clocks.
- Canonical simulation snapshots with validated, atomic ECS application.
- Deterministic `.tdmodel` runtime loading with a real PBR corridor asset shipped
  through Git LFS; raw glTF/GLB remains an offline import/cooking format.
- Versioned `.tdcontent` deployment manifests that bind exact cooked assembly
  locators to immutable model/contract owners and publish the complete ECS graph
  transactionally after GPU upload retirement.
- Deterministic authored interior interactions with reversible moving parts,
  camera-relative nearest use, portal passability, and topology-bound snapshots.
- Layer 4 material work is partially landed: albedo/normal textures,
  Cook-Torrance GGX shading, packed ORM (occlusion/roughness/metallic) maps, a
  linear HDR scene target, bloom, and a separate tone-map resolve pass exist;
  emissive maps, and a directional shadow map for the raster path; SM 6.6
  bindless does not. See "Material System Status" below and `CLAUDE.md`.
- Source lives in `src/`; runtime shaders live in `shaders/`.

## Build

From the repository root:

```bat
git lfs pull
SETUP_AND_BUILD.bat
```

The script first tries `cmake` from `PATH`, then common Visual Studio bundled
CMake locations. `git lfs pull` materializes the required cooked runtime asset;
an LFS pointer is intentionally rejected by the model loader.

Manual equivalent:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

If `cmake` is not on `PATH`, the Visual Studio bundled executable is commonly:

```text
C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

Build output:

```text
build\Debug\TheDawningV3.exe
```

Runtime log:

```text
build\Debug\TheDawning.log
```

## Smoke Test

After building, run a short automated render test:

```powershell
.\tools\smoke_test.cmd
```

The smoke test launches `build\Debug\TheDawningV3.exe --smoke --smoke-rt`,
waits for the engine to enter path tracing, and exits cleanly. It fails if:

- the process returns a nonzero exit code. `main.cpp:775-780` latches
  `core::Log::ErrorCount()` and forces exit code 4 when anything logged an
  error during the run, even if the frame loop otherwise completed. This
  catches the paths that log an error and skip their work while rendering
  continues (`PathTracer::Dispatch` has several).
- the log contains an error line. `tools/smoke_test.ps1:59` matches
  `\[ERR\s*\]`, which is the prefix `core::Log` actually writes
  (`log.cpp:82` emits `[ERR ]`).
- an expected `[SMOKE] key=value` marker is missing or has the wrong value.
  Assertions match these structured markers rather than human-readable log
  prose, so rewording a log line cannot silently disarm a check.
- the captured frame fails its pixel assertions (see below).

### Pixel assertions

`--smoke-capture` reads the final frame's back buffer back to the CPU and writes
`build\<Config>\smoke_capture.ppm` (binary P6). The harness then checks the image
is the expected size, is not essentially black, is not blown out, has a
reasonable fraction of non-black pixels, and contains more than a handful of
distinct colours.

For a deterministic playable-ship capture, launch the executable with
`--smoke --smoke-flight --smoke-capture`. The opt-in profile switches to
decoupled flight and holds forward thrust over the final twelve fixed steps, so
the capture and `[SMOKE] playable_ship=ok` marker exercise physical nozzle
throttle, ship motion, chase-camera ownership, and exhaust feedback together.

In raster mode the harness additionally probes the shadow map itself, reading
back a 256x256 window from its centre and asserting that the depth pass
rasterised something. `shadow_map=ok` only proves the resource was created;
deleting the shadow pass outright leaves the map at its cleared value, every
pixel reads fully lit, and the frame still looks entirely plausible. The probe
was verified by deleting the caster draw and confirming the marker flips to
`no`, which is the only way to know an assertion has teeth.

This is what separates the harness from a liveness check. Everything else it
verifies proves the engine did not crash; only this proves it drew something. A
black frame, inverted culling, a shader emitting nothing, or NaN-poisoned output
would pass every other assertion.

### The GPU draw-record probe

In raster mode the harness asserts that every draw consumed the exact
per-object and per-material records uploaded for it, via the `draw_probe_*`
markers.

The two record buffers are root SRVs: bare GPU virtual addresses without
descriptors or `StructureByteStride`, so neither D3D12 nor the debug layer can
detect a wrong index or CPU/HLSL layout drift. The CPU-side record counters only
measure what was written and cannot prove what a shader read.

On the final raster smoke frame, both vertex shaders hash every field of the
`ObjectData` they loaded into a root UAV. The main vertex shader also hashes the
loaded `MaterialData`. Index-plus-one markers distinguish an unwritten slot
from a valid zero hash. After queue retirement the renderer reads the UAV back
and compares each value with the mapped upload record. The shared
`shaders/gpu_draw_records.hlsli` declaration prevents the raster stages from
drifting apart, while the live GPU comparison covers the CPU-to-HLSL boundary.

The probe is gated by the third `b3` root constant, so ordinary frames carry no
UAV writes and `ObjectData` remains 96 bytes. It was verified by watching three
mutations fail independently: `basic_vs` reading object record 0, `shadow_vs`
reading object record 0, and `basic_vs` reading material record 0.

This replaced a golden-value gate on the capture's mean luminance and
colour-bucket count. That gate was measuring the same invariant *through the
rendered image*, which made it depend on which assets happened to sit in
`build\<Config>` — build output, not tracked source — and it was mis-calibrated
twice in one round on exactly that. It also could not do the job: pinning
`shadow_vs.hlsl` to record 0 moved the bucket count by one and the mean by 0.7,
less than the legitimate drift between two checkouts of the same commit. The
probe is independent of the scene's appearance, assets, and lighting.

The thresholds are deliberately loose — they catch catastrophic failure, not
appearance regressions, because pinning down appearance would flake on any
legitimate lighting change. Pass `-NoCapture` to skip this section.

**Captures are not guaranteed to be byte-reproducible.** Smoke simulation and
legacy scene animation now advance on a fixed synthetic timestep, and the
harness removes known stale test-scene textures before launch. That makes the
scene inputs reproducible, but GPU scheduling, driver behavior, and stochastic
path-tracing samples can still produce small per-pixel differences.

Historical measurements from before fixed-step smoke animation showed:

| mode | channels differing | max delta |
|---|---|---|
| raster | 0.55% | **109** |
| rt-stable | 0.21% | **1** |

The practical consequences:

- The aggregate thresholds above are intentionally loose: mean luminance and
  counts are stable to 0.1 across runs.
- Byte-level comparison remains unsuitable as a cross-machine gate without a
  characterized per-adapter tolerance. The structured GPU probes are the
  authoritative correctness checks; pixels are a catastrophic-output check.
- The harness deletes the known test-scene texture names from the build output,
  forcing deterministic procedural fallbacks. Other intentionally loaded assets
  must be pinned before adding any future reference-image comparison.

Use `-RasterOnly` for the raster path, `-FullQuality` for the higher
path-tracing quality mode, and `-Config Release` to test a Release build.

## Path Tracing Runtime

Raster mode builds and runs without extra DLLs. For DXR/path tracing shader
compilation, CMake discovers the newest x64 Windows SDK runtime and copies these
next to every built `TheDawningV3.exe` configuration:

- `dxcompiler.dll`
- `dxil.dll`

For a standalone DirectX Shader Compiler release or a
Microsoft.Direct3D.DXC NuGet layout, configure with
`-DDAWNING_DXC_RUNTIME_DIR=<directory-containing-both-dlls>`. CMake warns when
no valid pair is available; raster remains usable but DXR is unavailable.

## Material System Status

Layer 4 is partially implemented. What exists is described below; SM 6.6
bindless does not exist yet. See `CLAUDE.md` for the full done/not-done split.

Packed ORM maps (occlusion in R, roughness in G, metallic in B - the glTF
convention) are sampled by both the raster and DXR paths and MODULATE the
material scalars rather than replacing them, so those stay usable as
per-instance tints. Occlusion is applied to ambient and environment terms only,
never to direct light, because occluding next-event estimation would
double-count the shadow ray.

Emissive maps are sampled by both paths. The texture is a greyscale mask; the
colour and strength come from the material, so one mask can drive many
differently coloured emitters. Emission is added after lighting and is
unaffected by occlusion or the Fresnel split, because it is radiance the surface
produces rather than radiance it reflects.

**Emissive surfaces are not light sources.** Neither path samples them. The path
tracer sees emission when a ray happens to land on an emitter, but there is no
next-event estimation of emitters, so a bright panel lights only itself. Making
emitters illuminate the scene needs light sampling, which is separate work.

### Shadows

The raster path renders a 2048x2048 depth-only pass from the light's point of
view before the scene pass, and samples it with a 3x3 grid of hardware
comparison taps - 9 taps, each already 2x2 filtered by the comparison sampler,
so the effective kernel is 4x4 texels.

The light matrices are built in CAMERA-RELATIVE space, like every other matrix
the raster path uses (see RULES #1 in `CLAUDE.md`). The camera is therefore at
the origin of that space and every footprint follows the viewer. Absolute camera
position enters only in double precision to compute a sub-texel residual that
anchors the shadow lattice to the world; no absolute position is narrowed.

Two separate biases fight shadow acne, because they address different errors:
slope-scaled rasteriser depth bias acts along the light direction, while a
normal offset in the pixel shader pushes the sampling point across the surface.
Neither alone keeps the ground plane clean at grazing angles.

Four 2048x2048 slices share one `Texture2DArray`, with half-extents 24, 65, 175,
and 470 world units. Radial selection is rotation-invariant, and the world-fixed
texel snap prevents translation shimmer. The outer 15% of each footprint uses a
smoothstep partition-of-unity cross-fade: the first three bands blend adjacent
cascade samples, while the last fades continuously to lit coverage. Ordinary
pixels keep the existing 9-tap PCF cost; only transition pixels sample twice.
The comparison sampler uses a white border, so out-of-footprint taps remain lit
rather than creating a hard black square.

Shadowing multiplies DIRECT light only. Ambient and emission are untouched -
ambient stands in for everything the single directional light does not carry, so
occluding it too would make shadowed regions pure black. This matches the path
tracer, where the shadow ray gates the next-event-estimation term and nothing
else.

The DXR path does not use the shadow map at all; it traces shadow rays, which is
strictly better and needs no bias tuning. The two paths agree on where shadows
fall, which is the property worth checking after any change to either.

Raster geometry and sky render into a linear `R16G16B16A16_FLOAT` scene target,
and a fullscreen pass tone-maps that into the back buffer at the end of the
frame. Tone mapping therefore happens exactly once, in `shaders/tonemap_ps.hlsl`,
rather than inside each material shader. Post-process passes (bloom, exposure,
TAA) belong between `Scene::RenderEntities` and `Renderer::ResolveToBackBuffer`;
that insertion point is the reason the HDR intermediate exists. The debug overlay
draws after the resolve, in display space, so it is deliberately not tone-mapped.

Raster materials bind textures through descriptor indices stored on material
constants. The demo scene resolves each texture from `assets\textures\`
relative to the process working directory — which is the executable directory
in normal use, so this is `build\Debug\assets\textures\`, not the
`assets/textures/` folder at the repository root (that folder ships only a
README). For albedo it tries KTX v1, then PNG via Windows Imaging Component,
then DDS; if the starter DDS files are absent it first writes generated checker
DDS files and loads those, and if even that fails it falls back to an in-memory
procedural checker. Because no textures are committed, a clean clone always
takes the generated-checker path and the KTX and WIC loaders go unexercised.

RGBA/WIC sources generate CPU mip chains; KTX and DDS files upload the mip
levels declared in their headers. Raster and DXR path-traced materials both
sample albedo through indexed shader-visible texture tables, and both can bind
normal maps: the demo scene loads `ground_normal` / `cube_normal` KTX, PNG, or
DDS when present and otherwise generates procedural wave normal textures for
the floor and cube. These tables are fixed-size and allocated from a monotonic
counter with no reclamation, so they are sized for demo scale only.

Raster lighting uses Cook-Torrance/GGX direct lighting so material roughness and
metallic response track more closely between preview and path tracing.

Raster mode draws its sky through the SAME evaluation the DXR path uses. Both
include `shaders/sky_common.hlsli` and both call `DawningSkyRadiance(direction)`
on a world-space view ray. The raster shader reconstructs that ray per pixel from
the camera basis and projection extents carried in `CBPerFrame`, matching how the
path tracer builds primary rays.

This was previously a real divergence: `sky_ps.hlsl` used a screen-space blend,
which nailed the gradient to the framebuffer so it did not rotate with the camera
or respond to pitch or FOV, and the horizon visibly jumped when toggling `F1`.
The camera basis exists in `CBPerFrame` specifically to close that gap. Toggling
`F1` at any camera orientation is the check that it stays closed.

## Runtime Controls

- `F1`: toggle raster/path tracing.
- `F2`: toggle path tracing quality.
- `F3`: toggle the debug overlay.
- `F`: use the nearest authored interaction in the camera view cone.
- `WASD` or arrow keys: forward/back thrust and left/right strafe.
- `Space` / `Ctrl`: lift up/down.
- Mouse: pitch and yaw after clicking into the window.
- `I/J/K/L`: keyboard pitch/yaw fallback.
- `Q` / `E`: roll left/right.
- `V`: toggle coupled flight assist / decoupled Newtonian flight.
- `ESC`: release mouse capture, then quit.

## Development Rule

Keep V3 compile-clean before adding features. Prefer small, verified changes:
build after each meaningful edit, then move to the next layer.

## Parallel Agent Workflow

Codex and Claude Code should work from separate Git worktrees instead of the
same physical checkout. See `AGENT_COORDINATION.md` and the `tools\agent_*.ps1`
scripts for the shared branch, build, smoke-test, and merge workflow. Use
`tools\claude.cmd` to launch Claude Code even when the desktop app's bundled
CLI is not on the current shell's `PATH`.
