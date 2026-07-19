# The Dawning V3

The Dawning V3 is the canonical C++20/D3D12 engine baseline for this repository.
Older `SpaceEngineD3D12`, V1, and V2 archives are historical references only.
Do not merge old snapshots directly into this source tree.

## Current Baseline

- Windows-only, Visual Studio 2022, x64.
- CMake project targeting C++20.
- D3D12 raster renderer with ECS-driven scene rendering.
- Optional DXR path tracing path when the GPU and runtime DLLs support it.
- Layer 4 material work is partially landed: albedo/normal textures,
  Cook-Torrance GGX shading, packed ORM (occlusion/roughness/metallic) maps, a
  linear HDR scene target, bloom, and a separate tone-map resolve pass exist;
  SM 6.6 bindless and emissive maps do not. See "Material System Status" below and `CLAUDE.md`.
- Source lives in `src/`; runtime shaders live in `shaders/`.

## Build

From the repository root:

```bat
SETUP_AND_BUILD.bat
```

The script first tries `cmake` from `PATH`, then common Visual Studio bundled
CMake locations.

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

This is what separates the harness from a liveness check. Everything else it
verifies proves the engine did not crash; only this proves it drew something. A
black frame, inverted culling, a shader emitting nothing, or NaN-poisoned output
would pass every other assertion.

The thresholds are deliberately loose — they catch catastrophic failure, not
appearance regressions, because pinning down appearance would flake on any
legitimate lighting change. Pass `-NoCapture` to skip this section.

**Captures are not byte-reproducible, and raster is far worse than RT.** The
scene animates on wall-clock `dt` (`Scene::SystemRotation`), so the final
captured frame lands at a rotation angle that depends on exact elapsed time.
Measured on this machine, same build, two consecutive runs:

| mode | channels differing | max delta |
|---|---|---|
| raster | 0.55% | **109** |
| rt-stable | 0.21% | **1** |

Raster captures an instantaneous frame, so a fractional-degree rotation shifts
high-contrast silhouettes by a whole pixel. Path tracing accumulates over many
frames, which averages that away almost entirely.

The practical consequences:

- The aggregate thresholds above are unaffected — mean luminance and bucket
  counts are stable to 0.1 across runs.
- **A byte-level or reference-image comparison is not valid for the raster path
  as things stand.** A max delta of 109 is the noise floor, not a regression.
  Any such test needs the animation driven by a fixed synthetic timestep under
  `--smoke` rather than wall clock, so the captured frame is reproducible.
  That, plus pinning the texture set below, are the two prerequisites.

**Capture statistics are also not comparable across checkouts.** They depend on which
textures happen to be present in `build\<Config>\assets\textures\`, which is
build output rather than tracked source. `assets/textures/` in the repository
contains only a README, so a clean clone falls through to procedurally generated
checker textures — but a checkout whose build directory has accumulated real
PNG/DDS/KTX files will load those instead and render measurably differently. On
this machine the same commit measures mean luminance 127.5 in a fresh worktree
and 124.4 in a checkout carrying leftover PNGs.

That is fine for the loose thresholds above, which only ask "did it draw
something plausible". It is a hard blocker for the reference-image comparison
that would otherwise be the natural next step: such a test needs the texture set
pinned as tracked input first, otherwise it will flake on nothing but build-
directory history. Treat "commit real texture assets, or make the procedural
fallback deterministic and mandatory under `--smoke`" as its prerequisite.

Use `-RasterOnly` for the raster path, `-FullQuality` for the higher
path-tracing quality mode, and `-Config Release` to test a Release build.

## Path Tracing Runtime

Raster mode builds and runs without extra DLLs. For DXR/path tracing shader
compilation at runtime, copy these next to `TheDawningV3.exe`:

- `dxcompiler.dll`
- `dxil.dll`

They can come from the Microsoft DirectX Shader Compiler release or the
Microsoft.Direct3D.DXC NuGet package.

## Material System Status

Layer 4 is partially implemented. What exists is described below; SM 6.6
bindless and metallic/roughness/AO/emissive maps do not exist yet. See
`CLAUDE.md` for the full done/not-done split.

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

Raster mode also draws a tone-mapped sky gradient, but it is NOT the same
evaluation the DXR path uses. Both include `shaders/sky_common.hlsli` and share
the gradient ramp `DawningSkyRadianceFromBlend`, however they feed it different
inputs: `sky_ps.hlsl:16` uses a screen-space blend (`1.0 - saturate(input.uv.y)`),
while the path tracer calls `DawningSkyRadiance(direction)`
(`path_trace.hlsl:383, 448`), a function of world-space elevation. The raster sky
is therefore nailed to the framebuffer — it does not rotate with the camera and
does not respond to pitch or FOV. Pitching the camera and pressing `F1` visibly
moves the horizon.

## Runtime Controls

- `F1`: toggle raster/path tracing.
- `F2`: toggle path tracing quality.
- `F3`: toggle the debug overlay.
- `WASD` + mouse: move the camera after clicking into the window.
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
