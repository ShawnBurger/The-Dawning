# Codex and Claude Integration Review - 2026-07-20

## Scope

This document records the per-draw structured-buffer work integrated from the
Claude and Codex branches, the conflicts resolved during the merge, the evidence
used to validate the result, and the remaining independent work.

The integrated change removes per-object and per-material uploads from the
256-KiB constant-buffer ring. Raster draws now index frame-local structured
buffers through root constants. Per-frame and per-pass constants remain in the
ring, whose measured peak is now flat at 1,792 bytes.

## Claude Work Integrated

- Moved object and material records into frame-local structured buffers.
- Added explicit small initial capacity floors and allocation during renderer
  initialization so the default scene crosses the replacement path.
- Added real scene churn: 80 off-camera renderables are created at smoke frame 8
  and destroyed at frame 16 in both raster and RT smoke modes.
- Added separate total-reallocation and in-flight-reallocation counters. The
  latter distinguishes a replacement that can exercise deferred-release safety
  from a startup replacement with no submitted frame referencing the old buffer.
- Kept the growth churn useful to RT smoke as a TLAS topology mutation while
  making it unconditional for raster buffer coverage.

## Codex Work Integrated

- Replaced the permanent object-record ID witness with a smoke-only GPU field
  probe that hashes the actual `ObjectData` and `MaterialData` words consumed by
  the vertex shaders.
- Added `shaders/gpu_draw_records.hlsli` as the shared HLSL record declaration so
  the raster shaders do not maintain duplicate layouts.
- Added per-frame UAV probe storage, immutable zero initialization, readback
  after GPU retirement, and exact comparison with the CPU upload records.
- Kept normal frames free of probe writes through an explicit root constant.
- Changed repeated capacity growth to a geometric policy while preserving exact
  first allocation at the explicit floors.
- Removed the synthetic draw-count sizing ramp and the retired `-ForceGrow`
  smoke option. Capacity is now driven by the real scene size.
- Added CPU tests for record layout, field hashing, exact first allocation,
  geometric growth, non-shrinking capacity, disjoint pass ranges, and the smoke
  scene's two-growth invariant.

## Merge Conflicts Resolved

### Scene growth versus sizing-hint ramp

Claude forced replacement by changing the actual scene. Codex had forced it by
ramping `maxDrawsHint`. The merged implementation keeps the real entity churn
and deletes the synthetic ramp. `maxDrawsHint` remains a truthful reflection of
`MeshInstanceCount()` in normal and smoke execution.

### Additive versus geometric growth

Claude's branch grew an existing allocation by a fixed 64-element headroom.
Codex's branch used geometric growth to avoid recreating all frame-slot buffers
at a fixed cadence as a real scene expands. The merged rule is:

- no shrink when demand falls;
- exact allocation when current capacity is zero;
- otherwise grow to at least `max(requested, current + max(current / 2, 64))`.

This preserves Claude's coverage invariant: object capacity starts at 4, grows
to 68 for the 17-renderable demo scene, then the 97-renderable churn requires
194 records and forces a second replacement with frames in flight.

### Draw-index witness versus field-hash probe

Claude's witness proved that shaders selected distinct record indices by storing
a permanent `recordId` in every object record. The field-hash probe is stronger:
it detects an incorrect object index, incorrect material index, or mismatch in
any uploaded word without increasing the production record layout. The merged
implementation therefore removes `recordId` and keeps the field-hash probe.

### Smoke diagnostics

The old branch reported a first replacement frame and conditioned its assertion
on raster smoke. The merged harness asserts both reallocation markers in every
mode. It requires at least one total replacement and at least one replacement
after frames have begun. This directly covers the deferred-release hazard and
does not depend on a mode-specific flag.

### Documentation

Earlier sections of `AGENT_HANDOFF_CLAUDE.md` preserve historical blockers and
measurements for provenance. Its integration-closure section is authoritative
for the merged result. `PER_OBJECT_BUFFER_DESIGN.md` is a research/design record
and intentionally contains superseded alternatives; it is not the runtime
contract.

## Validation Evidence

The canonical merge was validated on Windows with the checked-in CMake/MSBuild
configuration and local D3D12 runtime dependencies:

| Gate | Result |
|---|---|
| Debug build | Pass |
| Release build | Pass |
| Debug unit tests | 134 cases, 3,256 checks, 0 failures |
| Release unit tests | 134 cases, 3,256 checks, 0 failures |
| Debug raster plus GPU validation | Pass |
| Debug stable RT | Pass |
| Debug full RT | Pass |
| Release raster | Pass |
| Release stable RT | Pass |
| Release full RT | Pass |

All six smoke modes reported:

- `cb_ring_peak=1792` of 262,144 bytes;
- `structured_buffer_reallocations=4`;
- `structured_buffer_reallocations_in_flight=2`.

Raster smoke also required `draw_probe=ok` and zero object/material field-hash
mismatches. Before the canonical merge, three deliberate shader mutations were
tested independently: pinning the main object index, shadow object index, or
main material index to zero. Each mutation failed the probe as intended and was
reverted.

## Remaining Conflicts and Risks

No source-level merge conflicts remain in this integration.

- Claude's RT texture-LOD work is an independent, dirty worktree and is not part
  of this merge. It needs its own review, tests, commit, and integration.
- The D3D12 root-signature 1.0 fallback remains a low-frequency runtime path. Its
  layout was kept synchronized with 1.1, but this pass did not force an adapter
  onto the fallback path.
- The GPU field probe intentionally runs only on the final raster smoke frame.
  It verifies record transfer and shader consumption, not every production
  frame.
- The local Meshy corridor asset is ignored build content. Smoke correctness no
  longer depends on its exact image statistics, but publishing that asset still
  requires a deliberate source-asset policy.
- Historical research text contains superseded proposed layouts and growth
  formulas. Runtime code, unit tests, the current README, and the integration
  closure are the implementation authority.

## Next Work

1. Deep-review and finish Claude's RT texture-LOD lane without touching this
   structured-buffer contract.
2. Integrate the cooked `.tdmodel` runtime-to-GPU bridge so authored assets use
   the hardened renderer path.
3. Add an explicit root-signature 1.0 fallback test hook when that can be done
   without changing normal adapter selection.
