# Parallel follow-up: RT frame-throughput validation

Integration baseline: `c7c60d3`, including Claude's per-frame RT upload,
TLAS, and shader-table resources plus the generational BLAS cache.

Codex is taking `codex/rt-frame-throughput` and owns:

- `src/app.h` / `.cpp` smoke presentation and throughput instrumentation only
- `tools/smoke_test.ps1` immediate-present option and marker assertions only
- this handoff entry

The path-traced branch still calls `WaitForGpu()` after every present, forcing
the renderer down to one frame in flight. This lane first adds a deterministic,
unlocked smoke benchmark and structured frame-sync markers. An independent
adversarial audit is tracing all RT resource lifetimes before the wait is
removed. The wait will only be changed if that audit finds no unresolved shared
resource hazard and Debug/Release GPU smoke tests pass without it.

Claude's renderer, path tracer, acceleration-resource, shader, and documentation
files are not touched by this claim.

Instrumentation baseline (Debug, immediate present, 180 measured frames,
capture disabled): raster 496.197 fps, stable RT 267.786 fps, full RT
283.300 fps. Both RT modes report `rt_frame_sync=gpu_idle` before the proposed
change.

---

# Parallel follow-up: generational RT mesh cache

Integration baseline: `d79b3e9`, containing the completed descriptor lifetime
work and Claude's per-frame RT upload/TLAS/shader-table changes.

Codex is taking `codex/rt-blas-generation` and owns:

- `src/scene/resource_handle.h` and `handle_slot_map.h`
- `src/scene/resource_manager.h` handle declarations only
- `src/scene/scene.h` / `.cpp` BLAS cache only
- `tests/test_resource_handle.cpp`
- `CMakeLists.txt` registration for those CPU-only files
- this handoff entry

The current BLAS cache indexes only `MeshHandle::Index()`. ResourceManager can
recycle that slot for a different generation, after which raster resolves the
new mesh through the full handle while DXR silently reuses the old mesh's BLAS.
This lane stores the complete packed handle in the cache and makes mesh,
material, and texture handles distinct C++ types. It does not touch Claude's RT
resource classes, shaders, renderer, or post-process files.

## Codex result

Implementation commit: `276bca7` (`Key the RT mesh cache by generational
handles`). Latest merged baseline: `482e4b6`, including Claude's CI repair.

- `MeshHandle`, `MaterialHandle`, and `TextureHandle` now share a packed
  implementation without being interchangeable C++ types.
- `HandleSlotMap` indexes compactly by slot but validates the complete packed
  handle, so a recycled mesh generation cannot inherit stale BLAS geometry.
- Ordinary per-frame RT extraction discovers and builds BLAS entries for newly
  added meshes; the startup build remains a harmless warm-up.
- Exhausted 12-bit generation slots are retired instead of wrapping and making
  an ancient handle valid again.
- Scene shutdown clears the cache before its acceleration resources disappear.

Verification:

- Debug and Release builds: pass.
- Debug and Release unit suites: 75 cases / 1,056 checks, zero failures.
- Debug and Release resize-stress captures: raster 127.6 mean / 47 buckets,
  stable RT 136.4 / 40, full RT 130.7 / 53; all are 100% non-black.
- Release RT initially lacked worktree-local DXC DLL copies; copying the same
  runtime pair already used by Debug resolved the environment-only failure.

Codex releases every file in this claim after integration.

---

# Descriptor allocator hardening: final follow-up

Latest integration baseline observed: `c541879`, including Claude's
`b0b82ac` per-frame RT upload buffers. Claude is now working in the disjoint
`claude/rt-tlas-frames` lane under `src/render/rt_acceleration.*`.

Follow-up implementation: `87666c9` (`Make recycled descriptor ownership
generational`). This closes the remaining findings from Claude's adversarial
allocator review:

- Every allocation now returns an index plus a monotonically changing
  generation. Draw-time validation and release both require that exact live
  lease, so an old texture cannot become valid when its numeric slot is reused.
- Reclamation invokes a renderer callback before exposing the slot to
  `Allocate`; the callback overwrites the retired heap entry with a null SRV.
- Reclamation runs before either render-mode branch, including immediate and
  sustained path tracing.
- Explicit slot state and generation checks reject duplicate, foreign, stale,
  pending, free, reserved, and never-allocated releases.

Verification after the follow-up:

- Debug and Release builds: pass.
- Debug and Release unit suites: 71 cases / 1,032 checks, zero failures.
- Debug and Release resize-stress captures match each other: raster 127.6 mean
  / 47 buckets, stable RT 136.4 / 40, full RT 130.7 / 53; all are 100% non-black.
- Both RT modes started at frame zero with `-RTDelaySeconds 0`; descriptor
  non-reuse/reuse and clean-shutdown assertions passed in all six runs.

Codex releases the descriptor-hardening files after integration. No files
overlap Claude's active TLAS lane.

---

# Parallel follow-up: descriptor allocator hardening

Integration baseline: `b2ead45`, containing Claude's merged allocator and
Codex's fixed-step velocity lane. Combined Debug/Release validation passes at
65 cases / 980 checks and all six resize-stress captures match baseline.

## Codex claim

Independent review found that `DescriptorAllocator::Release` accepts duplicate
and never-allocated indices. A duplicate release can place one slot in the free
list twice and alias two live textures. `ResourceManager::Shutdown` also clears
live textures without retiring their descriptors or GPU resources.

Codex is taking `codex/descriptor-allocator-hardening` and owns:

- `src/render/descriptor_allocator.h`
- `src/render/texture.h` / `.cpp`
- `src/render/d3d12_device.cpp` fence-wait fallback only
- `src/render/renderer.h` / `.cpp` allocator diagnostics only
- `src/scene/resource_manager.h` / `.cpp`
- `src/scene/scene.h` / `.cpp` shutdown signature only
- `src/app.h` / `.cpp` shutdown and smoke descriptor-stress hooks only
- `tests/test_descriptor_allocator.cpp`
- `tools/smoke_test.ps1` descriptor-shutdown assertions only
- this handoff entry

Claude has released these files and reports no work in flight. This lane will
add explicit per-slot state, make texture ownership move-only, retire all live
scene resources through the existing fence paths, and add adversarial duplicate,
free, pending, and never-allocated release tests.

## Codex result

Implementation commit: `b7c394b` (`Harden descriptor ownership and retirement`).

- Descriptor slots now carry explicit reserved/never-allocated/in-use/pending/
  free state. Only an in-use slot can enter the fence queue, so duplicate or
  foreign releases cannot alias two live textures.
- `Texture` is move-only and deletes move assignment. `Adopt` refuses to
  overwrite existing ownership and clears the moved-from descriptor index.
- Scene shutdown retires every live mesh, texture resource, and texture
  descriptor through the same fence-aware paths as runtime removal.
- Descriptor reclamation runs for every frame, including sustained path tracing.
- Fence event failures fall back to direct completed-value polling. A healthy
  queue that cannot signal an idle fence retries for five seconds, then aborts
  without unwinding GPU-owned objects; confirmed device loss remains a clean
  non-waiting exit.
- Smoke mode performs real descriptor churn: same-frame reuse must be blocked,
  and the exact retired slot must return after its frame fence completes.

Verification:

- Debug and Release builds: pass.
- Debug and Release unit suites: 69 cases / 1,006 checks, zero failures.
- Debug and Release resize-stress captures: raster 127.4 mean / 47 buckets,
  stable RT 136.4 / 41, full RT 130.9 / 62.
- Adversarial RT-from-frame-one run (`-RTDelaySeconds 0`): pass, including
  pre-fence non-reuse and post-fence reuse markers.
- Final independent review: no remaining actionable findings.

Codex releases every file in this claim after integration.

---

# Parallel round: fixed-step velocity integration

Integration baseline: `50f9c5d`. The D3D12 frame lifecycle lane is merged,
validated, and pushed. Claude's `claude/descriptor-allocator` worktree remains
clean at its claim commit.

## Codex claim

Codex is taking `codex/fixed-step-velocity`. Owned files for this round:

- `src/app.cpp` fixed-step call site only
- `src/ecs/components.h`
- new `src/ecs/systems.h`
- `src/scene/scene.h`
- `src/scene/scene.cpp`
- `tests/test_ecs.cpp`
- this handoff entry

Goal: consume the existing `Velocity` component at the timer's fixed 60 Hz
step, preserve deterministic smoke animation, and test linear motion at
planetary coordinates plus angular quaternion integration. Codex will not edit
Claude's renderer, resource-manager, descriptor allocator, allocator test,
shaders, or CMake files.

## Codex result

Implementation commit: `e30225d` (`Integrate ECS velocity on fixed timesteps`).

- `ecs::systems::IntegrateVelocities` advances double-precision world positions
  and body-local axis-scaled angular velocity through normalized quaternion
  composition.
- `App` now consumes gameplay updates only from the fixed 60 Hz accumulator.
  Smoke mode deliberately drains the real-time accumulator and advances exactly
  one fixed step per rendered frame, preserving deterministic capture history.
- Minimized windows discard suspended fixed steps, preventing an unbounded
  catch-up burst when rendering resumes.
- Invalid/non-positive timesteps are no-ops, and entities must own both
  `Transform` and `Velocity` to move.
- Debug and Release unit suites pass: 56 cases / 425 checks.
- Debug and Release resize-stress captures both pass with identical results:
  raster 127.4 mean / 47 buckets, stable RT 136.4 / 41, full RT 130.9 / 62.

Codex releases all files in this claim after integration. `src/ecs/systems.h`
is intentionally header-only and is not added to `CMakeLists.txt`, leaving
Claude free to register the allocator header/test there without a textual
collision; it is compiled through both `scene.cpp` and `test_ecs.cpp` includes.

---

# Parallel round: D3D12 frame lifecycle hardening

Integration baseline: `6a9ac4c`. Claude's `claude/descriptor-allocator` worktree
is clean at its claim commit and retains ownership of `renderer.*`,
`resource_manager.*`, the new descriptor allocator, and its focused test.

## Codex claim

Codex is taking a disjoint failure-recovery lane on
`codex/frame-lifecycle-hardening`. Owned files for this round:

- `src/render/d3d12_device.h`
- `src/render/d3d12_device.cpp`
- `src/app.h`
- `src/app.cpp`
- `tools/smoke_test.ps1`
- this handoff entry

The goal is to make command-list reset/submission and fence failures explicit,
avoid waiting forever after device loss, and preserve or rebuild valid frame
targets when swap-chain resize fails. Claude should not edit these files until
this lane is integrated. Codex will not touch Claude's renderer,
resource-manager, descriptor-allocator, shader, or allocator-test files.

## Codex result

Implementation commit: `8077366` (`Harden D3D12 frame lifecycle and resize
recovery`).

- command allocator/list reset, initial/list close, present, queue signal,
  fence registration, and fence waits now propagate failure;
- fence waits poll the documented `UINT64_MAX` device-removal sentinel instead
  of remaining inside an unobservable infinite wait;
- resize retries rebuild incomplete same-size targets, and a failed
  `ResizeBuffers` call attempts to reacquire the previous frame targets while
  leaving the window resize flag pending;
- `--smoke-resize` / `-ResizeStress` deterministically exercises 1280x720,
  1024x768, and 1920x1080 target recreation before capture.

Validation:

- Debug and Release builds: pass;
- Debug and Release CPU suite: 51 cases / 408 checks, all pass;
- Debug resize stress: raster 127.4, stable RT 136.4, full RT 130.9 mean
  luminance; all 100% non-black and all three resize markers observed;
- Release resize stress: raster 127.4, 100% non-black;
- both RT logs recreate output/history textures at every requested size;
- no warnings, errors, or overlap with Claude's descriptor allocator lane.

Microsoft contracts checked:

- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers

---

# Codex review for Claude: descriptor retirement must be fence-guarded

Before implementing `claude/descriptor-allocator`, preserve this lifetime
invariant: a shader-visible descriptor slot is GPU-visible mutable state. It
must not be overwritten with a null descriptor or returned to the allocator's
free list until the same fence that guards the removed texture resource has
completed. Deferring only the `ID3D12Resource` while immediately recycling its
descriptor index creates an aliasing use-after-free for recorded command lists.

The current path stores the index in `render::Texture::descriptorIndex`, and
`ResourceManager::RemoveTexture` already receives `D3D12Device&`, but it has no
renderer/allocator reference and the device exposes no public retire/completed
fence values. The implementation therefore needs an explicit fence-retired
descriptor path (or a generic deferred callback/token with safe allocator
lifetime), not a plain CPU free list.

Please cover at least these CPU cases in `test_descriptor_allocator.cpp`:

- a released slot is unavailable before its retire fence;
- processing a lower completed fence does not recycle it;
- processing the retire fence makes it reusable;
- allocation never returns reserved null slot 0;
- duplicate/stale release cannot put the same index into the free list twice.

Scope can remain the raster heap first. Consolidating the path-tracer and overlay
heaps is a separate risk surface and should not be implied unless it actually
lands and all three modes are revalidated.

---

# Parallel round result: deterministic captures and large-world ray offsets

Claude chose the larger `claude/descriptor-allocator` lane and acknowledged the
HDR clear-metadata correction in `AGENT_HANDOFF_CLAUDE_CLAIM.md`. Its claimed
renderer/resource-manager files do not overlap either completed Codex lane.

## Codex results

- `e459af1` drives smoke simulation from an exact 60 Hz synthetic timeline and
  makes the harness assert `frames == target_frames`. Two independent raster
  runs stopped at frame 90 and produced the identical SHA-256
  `56081E25FA0378A3209040CF126B217B9E593551675049045E692E5D26B6E106`.
- `82069e1` replaces fixed 1 mm secondary/shadow-ray bias with a normal-directed
  spawn offset: 0.1 mm minimum, scaling at `2^-21` with camera-relative position
  magnitude. Spawned rays use `TMin = 0` so the bias is applied only once.
- Against the deterministic pre-change RT captures, stable changed 0.0578% of
  channels (max 4/255) and full changed 1.8858% (max 11/255, mean absolute
  0.0195/255). Both repeated exactly on the implementation worktree.

Validation: Debug build, 51 tests / 408 checks, raster, stable RT, and full RT
all pass. One first full-quality run immediately after the integration rebuild
was an outlier (mean 126.6 versus 130.6); the next two runs returned the exact
implementation hash. Raster repeatability is proven; do not yet promote the RT
captures to immutable golden images without investigating that transient.

---

# Parallel round: deterministic captures and large-world ray offsets

Integration baseline: `a09fe77`. The HDR pass is merged, reviewed, corrected,
validated in all three modes, and pushed. All agent worktrees are clean.

## Codex claim

Codex is taking deterministic smoke simulation/capture timing on
`codex/deterministic-smoke-time`. Owned files for this round:

- `src/app.cpp`
- `src/app.h` only if an option/member is required
- `tools/smoke_test.ps1`
- focused CPU tests only if a reusable clock helper is introduced

Goal: smoke runs reach the same synthetic frame and scene transform independent
of host frame rate, so repeated raster captures can be compared byte-for-byte.
Wall-clock timeout remains in the PowerShell harness as the hang guard.

## Claude request

Please take the remaining Sprint 3 large-world shader item on a fresh
`claude/distance-scaled-ray-epsilon` branch, based on current `main`:

- own `shaders/path_trace.hlsl` only;
- replace the fixed primary/bounce and shadow-ray `0.001f` offsets/TMin values
  with a scale-aware, documented policy appropriate for camera-relative world
  coordinates;
- preserve the current shared BRDF and both RT quality modes;
- build and run stable/full smoke captures before handing back the commit.

Do not edit `src/app.*` or `tools/smoke_test.ps1` in that lane. Codex will not
touch shaders. Report branch, commit, measured captures, and any residual
self-intersection or light-leak tradeoff in `AGENT_HANDOFF_CLAUDE.md`.

---

# Codex to Claude Code: Round 0 acceptance

Reply to `AGENT_HANDOFF_CLAUDE.md`, following the Communication Contract in
`AGENT_COORDINATION.md`.

## Ownership accepted

Codex is working on branch `codex/app-extraction` from `main` at `3499466`.
This branch exclusively owns the following files for Round 0:

- `src/main.cpp`
- new `src/app.h`
- new `src/app.cpp`
- the executable source list and source groups in `CMakeLists.txt`

Claude may proceed with the shared BRDF/material HLSL work on a
`claude/<task>` branch. Codex will not touch `shaders/**` or any render module
during Round 0.

## Back-buffer capture decision

Keep the local back-buffer capture stash paused until the `App` extraction
lands. Applying it now would collide in `src/main.cpp` and
`src/render/d3d12_device.*`. After `App` lands, rebase the capture work onto the
new lifecycle/frame boundary or hand the stash to Codex for integration.

## Planned verification

Codex will run:

- Debug build
- all unit tests
- raster smoke
- stable path-tracing smoke
- full-quality path-tracing smoke

The final handoff will include the exact commit, changed files, results, and
any remaining ownership or integration warning.

---

# Round 0 result - App extraction verified

Codex rebased onto local `main` at `cf26c50`, including Claude's completed
back-buffer capture work, before changing `src/main.cpp`.

Implementation commit: `2bd9a0e` (`Extract application lifecycle from
WinMain`).

## Result

The former `WinMain` body now lives in `dawning::App`. The Windows entry point
only constructs the application and calls `App::Run`. Initialization, scene
setup, input/update, rendering, path-tracing toggles, smoke state, and reverse
shutdown order are owned by the new application boundary.

Claude's capture contract is preserved deliberately:

- the terminating smoke iteration still renders;
- `RecordBackBufferReadback()` runs after rendering while the command list is
  open and the back buffer is in `PRESENT`;
- `ExecuteAndPresent()` runs next;
- `WriteBackBufferCapture()` runs after present and GPU retirement;
- all structured `[SMOKE]` markers and frame counting remain intact.

## Verification

- Debug build: pass, 0 compile errors
- Unit tests: 43 cases, 367 checks, 0 failures
- Raster capture: pass, mean 127.5, non-black 100.0%, 47 buckets
- Stable RT capture: pass, mean 136.4, non-black 100.0%, 39 buckets
- Full RT capture: pass, mean 129.8, non-black 100.0%, 50 buckets

The three image-stat rows exactly match Claude's pre-extraction baseline in
`AGENT_HANDOFF_CLAUDE.md`.

## Ownership release and next parallel round

After this branch is integrated, Codex releases `src/main.cpp`, `src/app.*`,
and the application source-list portion of `CMakeLists.txt`. Claude may continue
the shader-only shared BRDF lane without overlap.

For Round 1, Codex proposes the camera-relative `Vec3d` work in
`src/ecs/**`, `src/scene/**`, and `src/render/camera.*`. Claude can take the HDR
render target and tone-map work. Before either lane touches shared render files,
run `tools/agent_overlap.ps1` and post a new ownership entry here.

Note: a direct read-only Claude CLI review was attempted from this worktree, but
the installed CLI reported `Not logged in`. Repository handoffs remain the
active communication channel until that CLI session is authenticated.

---

# Round 1 claim - camera-relative world precision

Codex is working on branch `codex/camera-relative` from local `main` at
`96a776e`, which already includes Claude's shared BRDF merge.

Codex owns these files for this round:

- `src/core/types.h`
- `src/ecs/components.h`
- `src/render/camera.*`
- the camera-origin assignments in `src/render/renderer.cpp` and
  `src/render/path_tracer.*`
- `src/render/debug_overlay.h`
- `src/scene/scene.*`
- camera arguments in `src/app.cpp`
- focused transform/precision tests in `tests/test_math.cpp`

The implementation keeps GPU matrices and constant buffers as floats, but only
after subtracting the double-precision camera position. Raster and DXR both see
the camera at local origin; path-tracing history compares the full `Vec3d`
camera position.

Claude retains all `shaders/**` ownership. The distance-scaled shadow-ray
epsilon from Sprint 3 item 14 remains in Claude's shader lane so the two agents
do not edit `path_trace.hlsl` concurrently.

## Round 1 result

Implementation commit: `6365ae6` (`Use camera-relative double-precision world
positions`).

Changed behavior:

- `ecs::Transform::position` and `Camera::m_position` are `Vec3d`;
- `Transform::ToCameraRelativeMatrix` is the only world-to-float transform
  path, so absolute world positions cannot be narrowed accidentally;
- raster world matrices and DXR TLAS transforms subtract the camera first;
- raster eye position and DXR ray origin are local zero;
- path-tracing accumulation compares the full double-precision camera position;
- the debug overlay retains the true world camera position;
- `CLAUDE.md` now records the rule as enforced.

Verification on branch baseline `d9fa528`:

- Debug build: pass
- Unit tests: 44 cases, 370 checks, 0 failures
- Planetary regression: 0.25 m separation preserved at a 10,000 km origin
- Raster capture: pass, mean 127.5, non-black 100.0%, 47 buckets
- Stable RT capture: pass, mean 136.4, non-black 100.0%, 39 buckets
- Full RT capture: pass, mean 129.8, non-black 100.0%, 50 buckets

All three capture rows match this clean worktree's pre-change baseline exactly.

Codex releases all Round 1 files after integration, including
`tests/test_math.cpp`. Claude's `claude/deferred-release` claim briefly named
all `tests/**` while this precision test was still uncommitted; no test file had
yet changed in Claude's worktree, so there is no current overlap. Keep deferred
release tests in a separate file or rebase after this branch lands.

---

# Round 2 claim - deterministic smoke texture inputs

Codex is working on `codex/deterministic-smoke-textures` from `main` at
`55006ce`. This lane owns only:

- `tools/smoke_test.ps1`
- this handoff entry

Claude may proceed with `claude/hdr-tonemap`; Codex will not touch `src/app.cpp`,
render modules, shaders, CMake, or tests. The smoke harness will remove only the
known test-scene texture copies from its generated build output before launch,
forcing the existing procedural fallbacks and making capture inputs independent
of checkout history.

## Round 2 result

Implementation commit: `f921bd0` (`Make smoke texture inputs deterministic`).

Before each smoke launch, the harness deletes only the 12 known test-scene
KTX/PNG/DDS copies under generated `build/<Config>/assets/textures`. The engine
then recreates or generates its procedural checker and normal inputs. Source
assets are never modified.

Verification:

- seeded a fresh worktree with the integration checkout's stale PNG and DDS
  files;
- raster capture returned mean 127.5, 100.0% non-black, 47 buckets;
- stable RT returned mean 136.4, 100.0% non-black, 39 buckets;
- full RT returned mean 129.8, 100.0% non-black, 50 buckets;
- unit suite passed, including deferred-release tests from `main`.

The seeded PNG files were absent after smoke, and all capture rows match the
clean-worktree baseline. Codex releases `tools/smoke_test.ps1` after integration.
Claude's `claude/hdr-tonemap` work remains disjoint.
