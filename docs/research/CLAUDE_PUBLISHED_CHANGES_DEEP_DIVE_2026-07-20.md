# Claude Published Changes Deep Dive - 2026-07-20

## Verdict

Claude's 14 published commits from `66d1c2d` through `b197701` are present on
`origin/main`. The rendering work is coherent, live-tested, and appropriately
instrumented. The flight work is a strong GPU-free foundation, but it is not yet
a playable ship implementation. The physics research and N-body revision are
useful design inputs, not implemented systems. This follow-up fixes two code
defects and corrects two research/design overstatements found during review.

Claude independently checked the final finding summary against the code and
reported no factual disagreements. That cross-check made no file edits.

## Published Range

| Commit | Area | Result |
|---|---|---|
| `66d1c2d`, `a37c67b` | Stable-DXR IBL implementation and merge | Accepted |
| `cde0d95`, `62d56e5` | Specular occlusion and Toksvig roughness | Accepted |
| `7236733` | Session log | Accepted |
| `ea8e639` | Flight/physics design | Accepted with scope notes |
| `d2be0a1`, `1c1bb68` | Rigid body and thrusters | Accepted |
| `d851e75`, `d5b0d42`, `8c10891` | Flight control, assist, and ECS wiring | Accepted after fix |
| `c3272f9` | Relativistic simulation architecture | Design only |
| `c7bf6b6` | Physics research material and reference | Accepted after provenance correction |
| `b197701` | Active-system N-body decision revision | Accepted after integrator-boundary correction |

The range adds 6,101 lines across 40 files. GitHub synchronization was verified
at `b197701b793e83650bfa434e832123962a405d9a` before the corrective commit.

## Findings

### High - fixed: multiple systems could integrate one pose

`Scene::UpdateSystems` ran legacy velocity, decorative rotation, and rigid-body
physics in sequence. The generic registry allowed an entity to carry both
`Velocity` and `RigidBody`, or both `RotationSpeed` and `RigidBody`, despite the
architecture requiring one pose owner. Such an entity translated or rotated
twice per fixed step.

The fix makes `RigidBody` authoritative: `IntegrateVelocities` and the extracted
`IntegrateRotations` system skip rigid-body entities. The regression test creates
both a kinematic entity and an intentionally conflicting dynamic entity, proving
the former still moves and the latter is untouched until flight physics owns it.

### High - corrected: research claims were not individually traceable

`PHYSICS_RESEARCH_MATERIAL.md` listed 29 URLs and then 117 extracted claims, but
did not retain a claim-to-source identifier. `PHYSICS_RESEARCH_REFERENCE.md`
nevertheless described all 29 as authoritative and every `[SOURCED]` claim as a
direct quote from a named source. That level of provenance was not present in the
repository and could not be audited.

The correction labels the corpus provisional, distinguishes search relevance
from source authority, and requires source-map recovery before any constant,
performance ratio, or disputed-physics statement becomes a build requirement.
Primary-source spot checks support the broad Markley, universal-variable,
OpenRelativity, Godot precision, and symplectic N-body directions, but do not
repair the missing mapping for the other claims.

### Medium - corrected: Forest-Ruth was assigned beyond its valid evidence

The N-body revision assigned one fourth-order symplectic integrator to planets,
stations, and ships without separating conservative gravity from thrust, drag,
collisions, or time-warp stepping. Symplectic bounded-energy arguments apply to
the conservative fixed-step subsystem; they are not valid acceptance criteria for
nonconservative ship motion. Fixed-step symplectic schemes also need an explicit
close-encounter policy.

The architecture now treats Forest-Ruth as the baseline candidate for conservative
active-system gravity, requires explicit splitting or a separate integrator for
nonconservative forces, requires deterministic pairwise equal/opposite updates,
and adds close-encounter and block-synchronized time-acceleration gates.

### Low - fixed: shutdown retained retired shader tables

`RTPipeline::BuildShaderTable` correctly parks replaced tables for the
frames-in-flight window, but `Shutdown` reset only the active table. The parked
COM references survived a shutdown/reinitialize cycle. The app already waits for
GPU idle before scene shutdown, so the follow-up safely resets every retired
slot and the ring cursor.

### Medium - open: Stage 2 is not wired to player input

No production entity receives `RigidBody`, `ThrusterSet`, or `FlightControl`, and
no app/input code writes `linearDemand`, `angularDemand`, or `FlightMode`.
`StepFlightPhysics` is in the fixed-step scene loop, but currently has no ship to
step. The implementation therefore proves the laws and ECS pipeline, not a
playable flight loop. This is the next user-facing vertical slice after the
coordinate/rebase foundation.

### Medium - accepted limitation: coupled mode is actuator-unlimited

Coupled mode applies an ideal force/torque from `ComputeFlightAssist` directly to
the body and does not allocate through `ThrusterSet`. Acceleration can exceed the
installed thrusters and throttle state does not represent the assist wrench. The
source calls this a follow-on; it should be resolved before thruster VFX, fuel,
damage, or ship-to-ship balance depend on actuator output.

### Low - open: ray-cone CPU tests are a manual mirror

The CPU test suite pins the ray-cone formulas but does not consume the HLSL
implementation. A shader-only sign, area, or logarithm edit could leave those
tests green. The live path compiles and renders in all smoke modes, which catches
large failures, but a future GPU readback of selected hit LODs would close this
remaining evidence gap.

### Collaboration incident - corrected: staged files crossed agent ownership

Claude's `c7bf6b6` commit included two Codex review documents that were already
staged in the shared checkout. The code changes were not swept into that commit,
but the event proves that path-level lane ownership is insufficient when two
agents share one Git index. Parallel work should use separate worktrees. Any agent
committing from a shared worktree must inspect `git diff --cached --name-only`
immediately before the commit.

## Rendering Analysis

### Stable DXR IBL

The raster and stable DXR paths call the same `ibl_common.hlsli` functions for L2
SH diffuse irradiance, roughness-aware Fresnel, prefiltered radiance, and the
split-sum BRDF fit. `RTPerFrameConstants` has static size/offset assertions, and
the environment descriptor's table-relative offset matches the heap layout.

The probe design is materially stronger than a startup math check. Each path
runs a disabled control frame and a live frame, writes at the actual consumption
site, verifies that all shaded invocations took the environment branch, compares
the sampled cube with procedural sky radiance, and proves that the environment
term reached the final color/radiance sum. Raster and stable DXR report identical
SH diffuse contribution (`0.287125`, delta zero).

Full DXR intentionally does not use the split-sum approximation. Escaping BSDF
rays collect `DawningSkyRadiance` in the miss shader, avoiding a second ambient
term and environment double counting.

### Specular fidelity

Toksvig widening is applied once to the material roughness used by direct GGX,
stable IBL mip selection, and the full tracer's sampled lobe. The variance signal
comes from the filtered tangent normal before normalization. Specular occlusion
uses the Lagarde remap for environment specular only; diffuse keeps raw AO and
direct light remains governed by shadow visibility.

The live probes report nonzero departures from the old behavior in both raster
and stable DXR, while the disabled controls require those words to remain zero.
This demonstrates that the fixes are not merely compiled helpers.

### Ray-cone texture LOD

Primary cone spread derives from the active camera FOV and render height. Closest
hit computes UV density from world-transformed triangle positions, including
instance scale, and uses the geometric normal for grazing projection. Albedo,
normal, ORM, and emissive textures all use computed mips. Cone width and spread
propagate across bounces, with conservative widening for rough lobes.

No descriptor-range, constant-layout, resource-state, or per-frame upload race
was found in this pass. Shader tables and mutable upload buffers are retained or
instanced for the frames-in-flight window.

## Flight Analysis

### Rigid body

The translational half is semi-implicit Euler over `Vec3d` world position and
velocity. Rotation stores body-frame angular velocity and diagonal inverse
inertia. The gyroscopic term uses an implicit Newton step, avoiding the measured
energy growth of the explicit asymmetric-top update. Orientation advances by a
right-multiplied exponential-map quaternion and is normalized.

Tests cover constant thrust, zero-force drift, torque impulse, long-run angular
momentum behavior, quaternion normalization, invalid timesteps, and deterministic
replay. The math and frame conventions are internally consistent.

### Thrusters and control

Thruster forces are body-local, torque is `r x F`, and world force is produced by
the body orientation. Decoupled mode uses the documented greedy allocator;
coupled mode is a proportional target-velocity controller with an exact discrete
closed form under the test conditions. The same fixed timestep used by legacy
simulation now drives `StepFlightPhysics`.

The allocator is intentionally directional rather than a constrained wrench
solver. It can saturate shared axes and does not balance redundant nozzles. That
is acceptable for this stage but insufficient for the final IFCS described by
the ship design.

## Architecture Reality

`RELATIVISTIC_SIM_ARCHITECTURE.md` is a design and sequencing artifact, not an
implemented relativistic simulation. Its current decision is active-system
N-body gravity with inactive-system Kepler LOD, while Stage 0 remains coordinate
and rebase validation. Coordinate frames, proper time, momentum-space dynamics,
gravity, orbital LOD transitions, atmospheres, FTL transitions, and save/replay
schemas remain future stages. The earlier patched-conics sections remain as
historical rationale and are explicitly superseded by the revision. The existing
motion components now enforce the one-pose-owner invariant through the review fix.

## Research Evidence Check

- NASA's Markley report supports a non-iterative elliptic Kepler solver with a
  cubic starter and fifth-order refinement.
- Wisdom and Hernandez support a universal-variable solver without Stumpff series;
  their published speed/error ratios depend on elliptic vs hyperbolic test cases,
  so those numbers must stay attached to the precise case.
- The Children of a Dead Earth developer states that the game uses N-body gravity
  with Forest-Ruth. That is strong implementation precedent, not proof that the
  same integrator is optimal for this engine's thrust, collision, and time-warp
  requirements.
- Chambers documents the fixed-step and close-encounter limits of symplectic
  planetary integrators. Nonconservative-integrator literature independently
  confirms that ordinary symplectic guarantees do not automatically survive drag
  and similar forces.
- Godot's official large-world write-up supports two-float translation on the GPU
  while keeping rotation and scale separate, consistent with this engine's narrow
  camera-relative rendering strategy.

Primary spot-check sources:

- [NASA Markley Kepler solver](https://ntrs.nasa.gov/api/citations/19950021346/downloads/19950021346.pdf)
- [Wisdom and Hernandez universal-variable solver](https://academic.oup.com/mnras/article/453/3/3015/1752673)
- [Children of a Dead Earth orbital implementation](https://childrenofadeadearth.wordpress.com/2016/05/17/fun-with-orbital-mechanics/)
- [Chambers hybrid symplectic integrator](https://academic.oup.com/mnras/article/304/4/793/1047461)
- [Tsang et al. nonconservative integrators](https://doi.org/10.1088/2041-8205/809/1/L9)
- [Godot large-world GPU precision](https://godotengine.org/article/emulating-double-precision-gpu-render-large-worlds/)

## Validation

The complete matrix ran after the pose-ownership fix. After the later
shutdown-only resource cleanup, both configurations were rebuilt, both CPU
suites were rerun, and Debug stable DXR was rerun to exercise shutdown.

- Debug and Release build successfully.
- Debug and Release each pass 193 tests and 4,175 checks.
- Debug and Release raster, stable DXR, and full DXR smoke pass.
- Debug raster also passes with D3D12 GPU validation.
- Debug raster `-ForceGrow` passes with 20 reallocations, 18 in flight.
- Default smoke reports 13 reallocations, 11 in flight, and a 2,048-byte constant
  ring peak against the 2,304-byte flat budget.
- Stable raster/DXR IBL probes pass control, consumption, identity, and specular
  fidelity checks.

## Recommended Next Order

1. Implement relativistic Stage 0 coordinate/rebase validation before orbital or
   FTL systems depend on frame identity.
2. Prototype the conservative N-body kernel CPU-only: stable pair order,
   equal/opposite updates, two-body closure, bounded long-run energy error,
   timestep convergence, and a watched close-encounter failure.
3. Build the playable ship slice: production entity, input snapshot, six-axis
   demand, coupled/decoupled toggle, and camera ownership.
4. Route coupled assist through actuator limits and define the split between
   gravitational stepping and thrust/drag/collision integration.
5. Add broad-phase collision and a close-encounter policy before enabling
   active-system gravity in production.
6. Add a GPU ray-cone LOD witness when texture-quality work resumes.
