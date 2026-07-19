# Claude Code lane claim: deferred GPU resource release

Branch `claude/deferred-release` from `main`. Claimed per the Communication
Contract in `AGENT_COORDINATION.md`.

## Why this task and not HDR

I intended to take the HDR render target and tone-map pass. I am not, because
`codex/camera-relative` has **uncommitted** changes in its worktree to
`src/app.cpp`, `src/render/renderer.cpp`, `src/render/camera.*`,
`src/render/path_tracer.*`, `src/scene/scene.*`, `src/ecs/components.h` and
`src/render/debug_overlay.h`. HDR needs `renderer.cpp` (PSO render-target
formats) and `app.cpp` (a new pass in the frame structure), so it would collide
head-on. Per the contract those uncommitted changes are owner-held and I have
not touched them.

HDR remains unclaimed and is the natural follow-on once camera-relative lands.

## Files this branch owns

- `src/render/d3d12_device.h` / `.cpp`
- `src/scene/resource_manager.h` / `.cpp`
- `tests/**`

All are outside Codex's current working set. I am **not** touching
`renderer.cpp`, `app.cpp`, `camera.*`, `path_tracer.*`, `scene.*`,
`components.h` or `debug_overlay.h`.

## What it fixes

`resource_manager.h` documents a HAZARD: `RemoveMesh` and `RemoveTexture` drop
their ComPtrs synchronously while up to `kFrameCount` frames are still in flight,
so a resource can be destroyed while the GPU is still reading it. The header used
to advertise a fence-guarded deferred-release queue that did not exist; that
false claim was corrected earlier, and this branch makes the claim true instead.

`docs/ANALYSIS.md` section 5 item 4 covers the general problem.
