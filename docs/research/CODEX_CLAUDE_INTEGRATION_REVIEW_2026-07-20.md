# Codex and Claude Integration Review - 2026-07-20

## Scope

This document records the reconciled per-draw structured-buffer work, the
conflicts resolved between the Claude and Codex implementations, and the current
validation evidence. The runtime measurements below were taken against the code
baseline at `c3272f9`; later commits `c7bf6b6` and `b197701` add research and design
documents only. Review fixes are documented in
`CLAUDE_PUBLISHED_CHANGES_DEEP_DIVE_2026-07-20.md`.

Raster object and material records no longer consume the 256-KiB constant-buffer
ring. Frame-local structured buffers hold those records and root constants select
the element for each draw. Per-frame and per-pass constants remain in the ring.

## Work Integrated

Claude's lanes supplied:

- frame-local object and material buffers, renderer bindings, and explicit
  initial capacity floors;
- real smoke-scene churn: 80 renderables added at frame 8 and removed at frame
  16 in raster and RT modes;
- total and in-flight reallocation counters;
- the original GPU-side record-index witness;
- ray-cone texture LOD for DXR, merged separately at `5d59432`.

Codex's integration supplied:

- one merged GPU probe that combines Claude's self-identifying record marker
  with a hash of every `ObjectData` and `MaterialData` word consumed by the
  shader;
- a shared `gpu_draw_records.hlsli` declaration and matched CPU layout checks;
- per-frame UAV storage, zeroing, readback after GPU retirement, and comparison
  against the exact CPU upload records;
- exact first allocation followed by geometric repeated growth;
- CPU tests for layouts, hashes, record identities, range separation, and growth
  invariants.

## Conflicts Resolved

### Real churn and sizing stress

The final implementation keeps both mechanisms because they test different
things. Real entity churn proves that changing scene topology drives actual
record demand. The sizing hint also ramps by four draws per smoke frame so the
default path repeatedly replaces buffers. `-ForceGrow` steepens that ramp to
sixteen draws per frame and remains the dedicated heavy test. Removing either
mechanism would reduce coverage.

### Additive and geometric growth

The merged policy never shrinks, performs an exact first allocation, and then
grows to at least:

`max(requested, current + max(current / 2, 64))`

This preserves small explicit floors while avoiding a fixed-cadence recreate as
real scene size grows.

### Index witness and field hash

Neither original probe was sufficient alone. A record ID catches a shader stuck
on element zero even when two records contain identical rendering fields. A full
field hash catches layout, stride, padding, and upload corruption even when the
right element index was selected. The merged 16-byte `DrawProbeRecord` carries
both pieces of evidence in one UAV/readback path.

The object witness is written where `basic_vs` and `shadow_vs` consume the
object. The material witness is written in `basic_ps`, where the material is
actually shaded. Unshaded records are counted separately rather than treated as
false mismatches.

### Probe scheduling

The draw probe runs on the last raster frame before RT takes over, so default RT
smoke cannot skip it. The IBL probes use adjacent control/live frames and distinct
marker keys. Reallocation assertions run in every mode and require replacements
after submitted work exists.

### Shared-checkout staging crossover

Commit `c7bf6b6` unintentionally included two Codex documents that were staged in
the shared checkout while Claude committed the physics research. Git's index is
worktree-global, so file ownership conventions do not protect staged content. The
documents are corrected by the follow-up review commit. Future parallel work must
use separate worktrees; in any shared checkout, each agent must inspect
`git diff --cached --name-only` immediately before every commit.

## Current Validation

The full matrix was rerun after the flight ownership fix. The later
shutdown-only shader-table cleanup was followed by fresh Debug and Release
builds, both CPU suites, and a stable-DXR smoke rerun.

| Gate | Result |
|---|---|
| Debug build | Pass |
| Release build | Pass |
| Debug unit tests | 193 cases, 4,175 checks, 0 failures |
| Release unit tests | 193 cases, 4,175 checks, 0 failures |
| Debug raster with GPU validation | Pass |
| Debug stable DXR | Pass |
| Debug full DXR | Pass |
| Release raster | Pass |
| Release stable DXR | Pass |
| Release full DXR | Pass |
| Debug raster with `-ForceGrow` | Pass |

Measured default smoke invariants:

- constant-ring peak: 2,048 of 262,144 bytes;
- flat ring budget: 2,304 bytes;
- structured-buffer reallocations: 13 total, 11 with frames in flight;
- first in-flight replacement: frame 5;
- raster draw probe: all 18 visible object and material records distinct;
- RT-transition draw probe: 98 object records distinct, 18 shaded material
  records distinct, 80 deliberately off-camera records classified unshaded.

The heavy `-ForceGrow` run reported 20 replacements, 18 with frames in flight,
and its first in-flight replacement at frame 2.

Raster and stable DXR IBL probes both pass their negative control and live frame.
The measured SH diffuse contribution is `0.287125` in both paths, with reported
delta zero. Full DXR correctly does not arm the split-sum IBL probe because it
collects environment radiance through miss rays instead.

## Remaining Risks

- The root-signature 1.0 fallback remains a low-frequency path not forced by
  this matrix.
- CPU ray-cone tests mirror HLSL arithmetic; they do not bind the shader to that
  mirror. Shader compilation and live smoke provide execution coverage, but
  there is no dedicated GPU LOD-value readback.
- The field probe runs only on its scheduled smoke frame, not production frames.
- Generated Meshy assets in build output remain outside the tracked source-asset
  policy. Smoke no longer trusts exact image statistics from those files.
- Historical sections in this handoff and older research files retain obsolete
  intermediate measurements for provenance. This review and the runtime tests
  are the current authority.

## Next Runtime Work

The graphics foundation is substantially ahead of the playable simulation.
Stage 0 coordinate/rebase validation is the next simulation foundation because
active-system gravity and relativistic motion both depend on it. The next
user-facing slice should then create one production ship entity with `Transform`,
`RigidBody`, `ThrusterSet`, and `FlightControl`; map an input snapshot to six-axis
demand; expose coupled/decoupled mode; and render feedback from actual thruster
state. Collision, gravity, orbital regimes, interiors, and relativistic adapters
remain later milestones.
