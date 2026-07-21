# Runtime Integration Audit - 2026-07-21

## Scope

This audit traces the executable runtime from the application fixed-step loop
through `scene::Scene`, the ECS, simulation kernels, save codec, renderer, asset
tools, build graph, CI, and smoke harness. It records observed source state at
the WS-019 base (`220417e`) plus the corrections made in this lane.

The audit does not edit Claude's active, uncommitted `app.cpp`, `scene.cpp`,
renderer, shader, or asset-import worktrees. Those shared callsites must be
reconciled before the host can adopt the new scheduler without overwriting live
work.

## Executive Result

The repository has strong GPU-free kernel coverage and an unusually thorough
GPU smoke harness. The main missing connection was not another physics law; it
was a deterministic ECS adapter and phase-order boundary. WS-019 supplies that
boundary as `sim::StepSimulation` and supplies the passive-orbit and proper-time
adapters it composes.

The production host still calls `sim::StepFlightPhysics` directly from
`Scene::UpdateSystems`. Consequently, gravity, passive N-body, rails,
atmosphere, FTL transitions, and proper clocks are compiled into the executable
but are not yet driven by the demo scene. This is an explicit follow-on host
integration item, not an implicit claim of completion.

## Production Call Graph

Current host path:

```text
App fixed-step accumulator
  -> Scene::UpdateSystems(fixedDt)
     -> IntegrateVelocities
     -> IntegrateRotations
     -> StepFlightPhysics
```

WS-019 simulation path:

```text
StepSimulation(fixedDt, config)
  -> FTL commands
  -> atmospheric interactions and ownership promotion
  -> passive N-body/collision and Kepler rails
  -> force-integrated gravity accumulation
  -> flight control, thrust, rigid-body and relativistic momentum integration
  -> relativistic proper-time clocks
```

The order is load-bearing. Atmosphere and FTL can promote a body to
`ForceIntegrated`, so both must run before passive ownership dispatch. Gravity
must accumulate before flight consumes and clears force. Clock updates consume
the final velocity for the coordinate-time step.

## Subsystem Wiring Matrix

| System | Kernel | ECS adapter | Scheduler | Production host | Automated witness |
| --- | --- | --- | --- | --- | --- |
| Fixed-step timing | Existing | Existing | Input contract | Connected | Smoke `timeline=fixed` |
| Flight control/thrusters | Existing | `StepFlightPhysics` | Connected | Connected directly | CPU suite + flight smoke |
| Force gravity | Existing | `AccumulateForceIntegratedGravity` | Connected | Not yet scheduled | Analytic and cross-frame CPU tests |
| Passive N-body | Existing | `StepPassiveOrbits` | Connected | Not yet scheduled | Cross-frame and deterministic CPU tests |
| Kepler rails | Existing | `StepPassiveOrbits` | Connected | Not yet scheduled | Primary-resolution CPU test |
| Collision policy | Existing | Reconciled by `StepPassiveOrbits` | Connected | Not yet scheduled | Merge/destruction CPU test |
| Atmosphere | Existing | Existing, corrected | Connected | No authored bindings | Frame/drag/momentum CPU tests |
| FTL | Existing | Existing | Connected | No command queue | Atomic transition CPU tests |
| Relativistic momentum | Existing | Corrected in flight adapter | Connected | Available through flight | Exact-force CPU test |
| Proper time | Existing | `StepRelativisticClocks` | Connected | No clock bindings | SR/GR and atomic CPU tests |
| Save codec | Existing | Not implemented | N/A | Not connected | Codec CPU tests only |
| Raster rendering | Existing | Scene traversal | N/A | Connected | Raster smoke |
| DXR rendering | Existing | Scene/path tracer | N/A | Connected | Stable/full smoke |
| Asset cooking/loading | Existing | Model loader | N/A | Connected | CPU tests + smoke markers |

## Correctness Findings And Corrections

### 1. Passive simulation kernels had no real ECS owner

`nbody`, Kepler rails, and collision policy existed as tested kernels but no
system gathered current ECS bodies, transformed frames, enforced the three-way
motion owner, or reconciled collision destruction.

Correction: `orbit_system` stages a complete passive update, advances the
collision-aware N-body set, resolves rail dependencies recursively, preserves
same-frame precision, updates survivors, and destroys absorbed entities using
their current generational handles. Invalid input rejects before registry
mutation.

### 2. Relativistic bodies could consume force through the Newtonian path

The ECS flight adapter did not make momentum authoritative. Applying force in
both a relativistic momentum step and the ordinary linear integrator would
double-apply acceleration or allow stale velocity to overwrite momentum.

Correction: relativistic bodies seed velocity from momentum, consume linear
force exactly once through `RelativisticMomentumStep`, clear that force before
the angular/pose integration half, and validate mass agreement atomically.

### 3. Atmospheric drag could be undone by authoritative momentum

Atmosphere correctly updated velocity semi-implicitly, but an attached
`RelativisticBody` retained its old momentum. The next flight step could restore
the pre-drag velocity.

Correction: accepted drag stages matching momentum from the new local velocity.
The regression test applies atmosphere and then flight, proving drag persists.

### 4. Save files rejected the live third motion owner

The codec accepted only owner values 0 and 1 after runtime introduced
`ForceIntegrated=2`.

Correction: validation accepts all three production owner values. A complete
round trip covers owner 2. Deserializing a non-empty null buffer now returns
`ShortBuffer` instead of entering byte operations with an invalid pointer.

### 5. Entity generation wrap could revive stale handles

The entity manager wrapped generation 4095 to zero and recycled the slot. An
old generation-zero handle could then alias an unrelated live entity.

Correction: terminal-generation slots are retired permanently. The regression
test runs the complete generation lifecycle and proves allocation advances to a
fresh slot while the oldest handle remains dead. Assigning through a dead handle
also no longer creates an empty component pool as a side effect.

The manager intentionally reserves index `0xFFFFF`: combined with generation
`0xFFF`, that packed value is the all-ones `NullEntity` sentinel. Allocated slots
therefore retain all 4096 usable generation values.

### 6. Separation across disconnected frame roots indexed an invalid frame

`NearestCommonAncestor` correctly returned `kInvalidFrame`, but
`SeparationBetween` passed that value to `ExpressInFrame`, which indexed the
frame vector out of bounds.

Correction: disconnected trees fall back to sector-aware world-space
separation. A two-root regression test watches the exact result.

### 7. Release builds did not receive the DXC runtime

The worktree helper happened to seed `dxcompiler.dll` and `dxil.dll` into the
Debug output only. Release raster worked, but Release DXR failed immediately at
runtime.

Correction: CMake now supports `DAWNING_DXC_RUNTIME_DIR` and otherwise discovers
the newest x64 Windows SDK pair. A post-build step copies both DLLs beside every
`TheDawningV3` configuration. The failure was reproduced before the fix and
Release stable/full DXR both passed afterward.

## Determinism And Atomicity Boundaries

- Duplicate FTL commands or atmosphere bindings for one entity reject before
  the first write.
- Passive bodies are gathered and sorted by stable body ID before integration.
- Invalid passive sets, frame references, collision configuration, gravity
  sources, relativistic masses, and clock bindings are staged as atomic no-ops.
- `ForceIntegrated`, `NBodyActive`, and `OnRails` are mutually exclusive movers.
- FTL reset-history and fixed-accumulator-drain obligations are returned to the
  host; they are not hidden global side effects.

`StepSimulation` is phase-atomic only within each subsystem. A later subsystem
failure can occur after an earlier accepted subsystem committed. That is an
intentional fixed-step transaction boundary for now and must be visible in the
returned `completedStage`. A future rollback architecture would require a full
snapshot transaction, not scattered reverse mutations.

## Build And Validation State

- Every repository `.cpp` has an intended CMake target assignment.
- New simulation sources are linked into both the application and CPU tests.
- CTest registers `TheDawningTests` and CI builds all targets before running it.
- Debug and Release app, tests, asset compiler, and asset inspector build.
- Debug and Release CPU suites pass 350 cases and 16,971 checks.
- Debug raster, stable DXR, and full-quality DXR smoke pass.
- Release raster, stable DXR, and full-quality DXR smoke pass.
- Captures are 1920x1080, 99.9-100% nonblack, with structured renderer, shadow,
  IBL, descriptor lifetime, and draw-record probes passing.

## Required Follow-On Integration

1. Reconcile Claude's active scene/app work and replace the direct flight call
   with one `StepSimulation` call per approved fixed step.
2. Give `Scene` or a dedicated simulation host ownership of `FrameGraph`, active
   frame, master frame, coordinate time, FTL command queue, atmosphere bindings,
   and clock-primary bindings.
3. Consume `resetRenderHistory` and `drainFixedAccumulator` after successful FTL
   transitions.
4. Add an ECS snapshot bridge that builds `SimSnapshot` from live components and
   applies a validated snapshot atomically. The codec alone is not a save game.
5. Author real planetary/atmospheric entities and attach the playable ship to a
   valid spatial frame before enabling those bindings in the demo.
6. Add host-level integration tests for scheduler invocation and save/load
   reconstruction; kernel tests cannot prove a callsite exists.

## Residual Risks

- The host integration remains deferred because its exact files contain active
  uncommitted work owned by Claude. Editing a separate copy now would create a
  predictable merge conflict and risk losing that work.
- The save format persists the simulation subset only. Rendering assets,
  gameplay identity, command queues, and higher-level world state need a
  versioned save-game layer around it.
- Atmosphere and GR clock operation require authored dominant-primary bindings.
  Automatic nearest-body selection is intentionally not inferred because it can
  introduce discontinuities and nondeterminism.
- GPU smoke validates this machine's D3D12/DXR path. CI remains GPU-free by
  design and cannot replace that local hardware gate.
