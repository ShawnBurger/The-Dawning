# Runtime Integration Audit - 2026-07-21

## Scope

This audit traces the executable runtime from the application fixed-step loop
through `scene::Scene`, the ECS, simulation kernels, save codec, renderer, asset
tools, build graph, CI, and smoke harness. WS-019 established the simulation
orchestrator; WS-020 audited and connected it to the production host, supplied
the missing live snapshot transaction, and corrected boundary defects exposed
by that integration.

Two stale Claude worktrees remain untouched as recovery sources. Their old glTF
and per-object-buffer experiments are based far behind current `main` and have
already been superseded by merged implementations. They are not active locks on
the current scene, renderer, or asset code.

## Executive Result

The runtime now has one authoritative fixed-step path. `App` calls
`Scene::UpdateSystems`, which calls `sim::StepSimulation` exactly once and then
advances only legacy non-rigid render props. The scene owns reference frames,
coordinate time, simulation tick, one-shot FTL commands, persistent atmosphere
bindings, and persistent clock-primary bindings. FTL obligations reach the
timer and path tracer explicitly.

The playable ship is a complete force-integrated simulation body in the root
frame, so the production demo and smoke harness execute the same scheduler and
snapshot topology as future authored worlds. A live smoke witness builds,
validates, applies, and rebuilds a simulation snapshot without disturbing render
or gameplay components.

WS-021 through WS-026 also complete the first production assembly path. A
reviewed source manifest cooks to an immutable assembly, its locators resolve
through a leased catalog, WS-025 stages an all-or-nothing ECS graph, and the
WS-026 runtime host publishes that graph only after cooked model uploads retire.

## Production Call Graph

Current host path:

```text
App fixed-step accumulator
  -> App::AdvanceSimulation(fixedDt)
     -> Scene::UpdateSystems(fixedDt)
        -> StepSimulation(fixedDt, config)
           -> FTL commands
           -> atmospheric interactions and ownership promotion
           -> passive N-body/collision and Kepler rails
           -> force-integrated gravity accumulation
           -> flight control, thrust, rigid-body and relativistic momentum
           -> relativistic proper-time clocks
        -> legacy non-rigid Velocity/RotationSpeed props
     -> render-history invalidation when requested
     -> fixed-accumulator drain when requested
```

The order is load-bearing. Atmosphere and FTL can promote a body to
`ForceIntegrated`, so both must run before passive ownership dispatch. Gravity
must accumulate before flight consumes and clears force. Clock updates consume
the final velocity for the coordinate-time step.

## Subsystem Wiring Matrix

| System | Kernel | ECS adapter | Scheduler | Production host | Automated witness |
| --- | --- | --- | --- | --- | --- |
| Fixed-step timing | Existing | Existing | Input contract | Connected | Smoke `timeline=fixed` + timer tests |
| Flight control/thrusters | Existing | `StepFlightPhysics` | Connected | Scheduler | CPU suite + flight smoke |
| Force gravity | Existing | `AccumulateForceIntegratedGravity` | Connected | Scheduler | Analytic and cross-frame CPU tests |
| Passive N-body | Existing | `StepPassiveOrbits` | Connected | Scheduler | Cross-frame and deterministic CPU tests |
| Kepler rails | Existing | `StepPassiveOrbits` | Connected | Scheduler | Primary-resolution CPU test |
| Collision policy | Existing | Reconciled by `StepPassiveOrbits` | Connected | Scheduler | Merge/destruction/binding-retirement tests |
| Atmosphere | Existing | Existing, corrected | Connected | Persistent scene bindings | Frame/drag/momentum CPU tests |
| FTL | Existing | Existing | Connected | One-shot scene queue | Atomic transition + host obligation tests |
| Relativistic momentum | Existing | Corrected in flight adapter | Connected | Scheduler | Exact-force CPU test |
| Proper time | Existing | `StepRelativisticClocks` | Connected | Persistent scene bindings | SR/GR and atomic CPU tests |
| Save codec | Existing | `snapshot_system` | N/A | Scene build/apply API | Atomic CPU tests + live smoke round trip |
| Raster rendering | Existing | Scene traversal | N/A | Connected | Raster smoke |
| DXR rendering | Existing | Scene/path tracer | N/A | Connected | Stable/full smoke |
| Asset cooking/loading | Cooked model + assembly + `.tdcontent` | `AssemblyRuntimeHost` | WS-025 transaction | Connected | CPU contracts + runtime prepare/commit smoke markers |

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

### 8. The production host bypassed the scheduler

`Scene::UpdateSystems` called flight physics directly, leaving gravity, passive
orbits, collisions, atmosphere, FTL, and clocks disconnected from the app.

Correction: `Scene` now owns the complete scheduler state and returns the stage
result to `App`. The app fails closed on a rejected step and consumes explicit
FTL history-reset and accumulator-drain obligations. Smoke requires the
`simulation_scheduler=ok` marker.

### 9. The save codec had no live ECS transaction

The codec could round-trip hand-authored records, but no runtime path mapped
stable body IDs and sidecars to live entities. A successful codec test therefore
did not prove the game could save or restore simulation.

Correction: `snapshot_system` builds canonical snapshots only at fixed-step
boundaries and applies them onto an identical stable-ID topology. Every write is
staged before commit; render, material, name, input, and thruster components are
preserved. Orphan orbit/relativistic sidecars, pending wrenches, malformed data,
and topology mismatches reject atomically. Smoke requires a live
`snapshot_roundtrip=ok` marker.

### 10. Entity destruction left scheduler bindings stale

Scene-owned atmosphere and clock bindings outlived manually destroyed entities.
Collision merging was subtler: an absorbed clock owner was valid at preflight,
then disappeared before the clock phase and caused a late rejection after the
merge had committed.

Correction: direct scene destruction clears all associated queues/bindings,
accepted steps purge bindings for collision-absorbed entities, and the scheduler
filters clock bindings whose entities were intentionally absorbed during that
same step. If the bound gravity primary was absorbed instead, the scheduler
remaps its stable body ID to the terminal collision survivor and returns that
mapping so the scene can repair its persistent binding. Collision regressions
cover both cross-phase lifecycles.

### 11. Public timing, frame, and checksum boundaries were partial

`Timer::SetFixedDt(0)` could make every drain loop infinite. `FrameGraph` accepted
invalid parents and non-finite frames, `ValidSector(INT64_MIN)` used an unsafe
absolute value, and canonicalization could overflow a sector addition. The
public CRC helper dereferenced a null non-empty buffer.

Correction: invalid fixed deltas are rejected, suspended simulation has an
explicit accumulator discard, frame creation validates topology and numeric
state, coordinate arithmetic contains invalid extremes, and CRC null input is a
defined failure sentinel. Regression tests exercise each boundary.

### 12. Resource registration could alias or leak slots

Resource pools did not stop at the packed handle's index limit, failed texture
adoption consumed a free slot, materials had no removal path, and app/model
loaders did not fail when required resource or descriptor registration failed.

Correction: all pools enforce handle capacity, invalid mesh/texture inputs are
rejected, texture slot rollback is complete, material retirement mirrors the
generational policy, and both production loaders propagate registration failure.
The imported-model bridge now rolls back every entity, mesh, texture, descriptor,
and resource registered earlier in the same failed load.

### 13. Production assembly content stopped at an offline graph

The reviewed `.tdasset.json` contract could be compiled, loaded, leased, and
transactionally prepared, but the executable still spawned one cooked model
directly from `App`. That bypass left authored modules, moving parts, collision,
navigation, and walkable identities disconnected from the production scene.

Correction: the versioned `.tdcontent` manifest maps every authored typed
locator to one cooked model primitive or immutable contract owner. The runtime
host confines paths, verifies exact coverage, deduplicates model loads, seals
owner/catalog identity, prepares through WS-025, and commits only after startup
GPU uploads retire. Shutdown destroys entities before their model resources.
Smoke now requires the exact 21-binding preparation and six-entity commit.

## Determinism And Atomicity Boundaries

- Duplicate FTL, atmosphere, or clock bindings for one entity reject before the
  first write.
- Multi-entity FTL and atmosphere batches restore the entire phase if a later
  entity rejects.
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
- CTest registers `TheDawningTests`; CI now builds and tests both Debug and
  Release rather than Debug alone.
- Debug and Release app, tests, asset compiler, and asset inspector build.
- Debug and Release CPU suites pass 411 cases and 17,608 checks.
- Debug raster, stable DXR, and full-quality DXR smoke pass.
- Release raster, stable DXR, and full-quality DXR smoke pass.
- Debug stable DXR also passes with D3D12 GPU validation enabled.
- Captures are 1920x1080, 99.9-100% nonblack, with structured renderer, shadow,
  IBL, descriptor lifetime, and draw-record probes passing.

## Required Follow-On Product Work

1. Add a versioned game-save envelope and atomic file replacement around the
   in-memory simulation snapshot. Rendering assets, missions, inventory, and
   world-streaming state belong in that higher-level format.
2. Author real planetary/atmospheric source entities and bind them explicitly;
   the current demo ship is simulation-complete but intentionally experiences no
   gravity or atmosphere in the material test scene.
3. Add reference-frame creation/selection policy for streamed star systems and
   define when the active integration frame diverges from the master clock frame.
4. Build gameplay-facing FTL command authoring only after destination frame and
   world-stream ownership rules are fixed.
5. Replace the Stage 4 corridor-based assembly witness with the Stage 5 modular
   production ship kit, then publish real collision, navigation, pressure,
   interaction, moving-part, and runtime LOD systems against the existing typed
   identities.

## Residual Risks

- The save format persists the simulation subset only. Rendering assets,
  gameplay identity, command queues, and higher-level world state need a
  versioned save-game layer around it.
- Atmosphere and GR clock operation require authored dominant-primary bindings.
  Automatic nearest-body selection is intentionally not inferred because it can
  introduce discontinuities and nondeterminism.
- GPU smoke validates this machine's D3D12/DXR path. CI remains GPU-free by
  design and cannot replace that local hardware gate.
- `StepSimulation` remains subsystem-atomic rather than whole-step rollback.
  `completedStage` makes a late rejection explicit; a full transaction would
  require snapshot-and-rollback ownership at the host boundary.
- Claude's independent review was requested through a read-only background
  agent but could not start because its session limit was active. Per the agreed
  fallback, review remains manual debt rather than a false completed gate.
