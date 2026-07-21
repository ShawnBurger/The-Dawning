# Flight & Physics Design — First Vertical Slice (Phase 4)

Status: DESIGN ONLY. No source, shader, or build file was modified producing
this. It grounds the Phase 4 ("Ship combat vertical slice") flight/physics
pillar in the code that exists today and in the three Batch 1 deep-dives
(Ship Flight Model, Physics & Collision, Orbital Mechanics).

The house convention in this repo is that a claim is worth more when it names
the file and line it was checked against, and a test is worth more when it has
been watched to FAIL with the feature absent. Both conventions are followed
below. Where a statement is an assumption rather than a verified fact it says
so.

---

## 0. What was verified against the code (not assumed)

- `ecs::Transform` (`src/ecs/components.h:19-33`) is already `{ Vec3d position;
  Quatf rotation; Vec3f scale; }` and already carries
  `ToCameraRelativeMatrix(const Vec3d& cameraPosition)`, which subtracts the
  camera in double and narrows only the small residual: `(position -
  cameraPosition).ToFloat()`. This is the render hand-off the design reuses
  unchanged.
- Both render paths already consume that method. Raster: `scene.cpp:221-222`.
  DXR TLAS: `scene.cpp:329-340` (it further repacks the row-vector matrix into
  DXR's 3x4). So a ship pose written into `Transform` renders identically in
  raster and path tracing with no new render code.
- `Camera::m_position` is `Vec3d` (`src/render/camera.h:54`) and the GPU camera
  is local-zero, per RULE 1.
- The fixed-timestep accumulator exists and is wired: `Timer` holds
  `m_fixedDt = 1/60`, `m_accumulator`, and a `kMaxDt = 0.25` spiral-of-death
  clamp (`src/core/timer.h:51-53`, `timer.cpp:67-75`). The main loop drains it:
  `while (m_timer.ConsumeFixedStep()) m_scene.UpdateSystems(m_timer.GetFixedDt());`
  (`src/app.cpp:1047-1049`). `Scene::UpdateSystems(double dt)` is the single
  insertion point (`scene.cpp:110-115`, comment already reads
  "// Future systems: physics, AI, etc.").
- A `Velocity` component already exists (`components.h:38-42`) with **Vec3f**
  linear + **Vec3f** angular, integrated by header-only
  `ecs::systems::IntegrateVelocity` (`src/ecs/systems.h:14-29`). That integrator
  right-multiplies the orientation delta: `transform.rotation * delta`
  (`systems.h:28`) — the correct body-frame convention (see §2.4). This is a
  toy mover; it is **not** reused for the flight model, because its velocity is
  `Vec3f` (see §3).
- The ECS is sparse-set, type-indexed pools; components are POD; the registry
  offers `Each<A,B>` / `Each<A,B,C>` and pool iteration (`registry.h`). Adding a
  component type is: define a POD struct, `Assign<T>` it, iterate it. Nothing
  else.
- The math library has `Vec3f`, `Vec3d`, `Quatf`, `Mat4x4`. It has **no**
  `Quatd`, **no** `Mat3x3` (`src/core/types.h`). The deep-dives assume all
  three; this design deliberately needs none of them for the first slice (§2.5,
  §3).
- The unit-test target `TheDawningTests` links **no** graphics libraries and
  pulls in a hand-picked set of GPU-free source files (`CMakeLists.txt:305-350`,
  and the rationale block at 283-302). `shadow_cascades.cpp`, `sky_radiance.cpp`,
  `sh_irradiance.cpp`, `gpu_draw_records.cpp`, `rt_texture_lod.cpp`,
  `ibl_consume_probe.cpp` are all compiled into the tests precisely so the
  SHIPPED code is exercised, not a copy. **This is the model the physics module
  must fit**: the integrator must be GPU-free so it joins that target the same
  way (§8, §9).
- The test framework is the hand-rolled `tests/test_framework.h`
  (`CHECK`, `CHECK_APPROX`, `CHECK_APPROX_EPS`, `CHECK_KNOWN_FAILING`). There is
  no screen-space here; physics tests are pure CPU unit tests.
- Precedent that the precision story already works:
  `test_math.cpp:351-362` (`Transform_ToCameraRelativeMatrix_PreservesPlanetaryOffset`)
  and `613-625` (`Vec3d_CameraRelativeSurvivesPlanetaryDistance`) already prove
  a 0.25 m offset survives at 1e7 m when subtracted in double and dies when
  narrowed first. A ship at 1e7 m inherits this exactly.
- Historical trap, now disarmed and directly relevant here: `Quatf::FromEuler`
  was mis-transcribed so every axis was wrong; `test_math.cpp:219-236` records it
  as "a trap armed for whoever first wrote ship orientation code." It is fixed
  and guarded. Ship control (§2.4) uses `FromAxisAngle` / `Rotate`, whose LH
  sign convention is pinned by `Quatf_FromAxisAngle_IsCorrect` (90° about +Y
  sends +X to −Z, `test_math.cpp:287-291`).

---

## 1. Scope of the first vertical slice

The smallest thing that is a real flight model, not a toy:

**In scope**
- Six-DOF rigid body: linear (position, velocity) and angular (orientation,
  angular velocity) state, mass, and a **diagonal** inertia tensor.
- Thrust and torque produced by discrete thrusters mounted at body-local
  offsets, exactly as the Ship deep-dive PART 2 describes: force is
  `dir * maxForce * throttle`; torque is `leverArm × force` with
  `leverArm = position - centreOfMass`.
- Linear and angular damping (a velocity-proportional decel), plus a minimal
  flight-assist ("coupled" mode) that auto-brakes toward zero when the stick is
  centred. This is the difference between a rigid body and a flyable ship.
- A control mapping that relates ship control to the existing WASD+mouse
  scheme (§2.4).
- Newtonian throughout: forces and torques integrate; nothing is on rails.

**Simplifications the first slice makes (each is a later stage, not a hole)**
- Inertia is diagonal and constant (box formula from the Ship deep-dive
  PART 1). No off-diagonal inertia, no fuel-mass-driven CoM/inertia shift. A
  diagonal-but-unequal tensor is still enough to exercise the full gyroscopic
  term and its hardest test (§9), so this simplification costs no test coverage.
- Decoupled thruster allocation is **direct**: normalized pilot demand maps to
  per-nozzle throttle by directional alignment. Coupled assist computes a desired
  physical wrench, then a deterministic bounded projected allocator maps it onto
  the installed nozzle bank. Both modes realize force only through
  `ComputeWrench`; there is no reactionless net-wrench shortcut. A globally
  optimal constrained IFCS allocator with explicit redundancy and damage policy
  remains later scope.
- No atmosphere (drag/lift/reentry heating). The slice is zero-g / vacuum plus
  optional uniform gravity. Atmospheric flight is a later stage.
- No orbital mechanics (see §4). Near a planet the slice uses at most a single
  uniform or point-mass gravity term fed to the integrator as one more external
  acceleration.
- Collision is bounding-sphere only (see §5).

The vertical-slice arena is one local region: a station or planet-adjacent
volume a few km across, a handful of ships (2–50 bodies), projectiles. That is
what "board ship → fly → fight" needs and nothing larger.

---

## 2. The rigid-body integrator

### 2.1 Recommendation: semi-implicit (symplectic) Euler

Integrate at the fixed 60 Hz step with **semi-implicit Euler** (a.k.a.
symplectic Euler): update velocity from the current force, then update position
from the *new* velocity.

```
a      = F / m
v_{n+1} = v_n + a * dt          // velocity first
x_{n+1} = x_n + v_{n+1} * dt    // position from the updated velocity
```

Rotational half, in the body frame, with a diagonal inertia I:

```
Iω        = (Ixx ωx, Iyy ωy, Izz ωz)                 // component-wise, I diagonal
gyro      = ω × Iω                                    // Euler's equation coupling
α         = Iinv · (τ_body − gyro)                    // component-wise with Iinv diagonal
ω_{n+1}   = ω_n + α * dt
// exponential map, exact for constant ω over the step:
θ         = |ω_{n+1}| * dt
q_delta   = FromAxisAngle(ω_{n+1} / |ω_{n+1}|, θ)     // identity if |ω| ~ 0
q_{n+1}   = normalize(q_n * q_delta)                  // RIGHT-multiply: body frame
```

### 2.2 Why semi-implicit Euler and not RK4

- **Fixed timestep is the whole point.** RULE 6 mandates a fixed-dt accumulator,
  already present. With a fixed step, a symplectic first-order method has
  *bounded* energy error — it oscillates around the true energy rather than
  drifting secularly. RK4 is fourth-order accurate per step but **not
  symplectic**: over many periods it bleeds energy monotonically. For the two
  regimes that matter to this project — a damped, thrusting ship, and (later) a
  closed orbit — bounded energy beats high local accuracy.
- **Cost.** One force/torque evaluation per step vs four for RK4. Physics runs
  every fixed step for every body; the flight slice does not need RK4's
  accuracy, and the orbital stage specifically does not *want* RK4's energy
  drift.
- **It is what the Ship and Physics deep-dives already specify** (Ship PART 1
  "symplectic Euler for stability"; Physics PART 1 step order "integrate
  velocities → positions"). Matching them keeps this design reconcilable with
  those docs.
- **It matches the shipped mover.** `IntegrateVelocity` already advances
  `position += v*dt` and right-multiplies a `FromAxisAngle` delta. Semi-implicit
  Euler is the same shape with the velocity update added ahead of it.

**Stated trade:** semi-implicit Euler is first-order, so per-step position error
is O(dt²) local / O(dt) global. Concretely (§9) this means a body under constant
thrust does **not** reproduce the continuous `x = x₀ + v₀t + ½at²` to tight
tolerance — the discrete scheme adds a known `½·a·dt·t` bias. The velocity
`v = v₀ + at`, however, is reproduced **exactly** (velocity is linear in t and
both Euler variants integrate it exactly for constant force). The verification
plan is written around this reality rather than against it (§9.1) — this is the
exact place a naive "match ½at² to 1e-5" test would fail a *correct* integrator.

### 2.3 The orbital-stage upgrade is in the same family (drop-in)

When the orbital stage (§4) arrives, promote the *translational* half to
**velocity-Verlet / leapfrog (kick–drift–kick)**, which the Orbital deep-dive
PART 4 already specifies for the near-field n-body path. It is second-order,
still symplectic, still one *new* force eval per step, and for a pure
`GM/r²` central force it closes orbits far better than plain Euler. It is a
drop-in because it consumes the same `RigidBody` state. **RK4 is explicitly not
adopted for orbits**, for the energy-drift reason above.

### 2.4 Orientation integration and the frame convention (a real correction)

The Ship deep-dive's snippet integrates orientation as
`ship.orientation += spin * ship.orientation * dt` — a *linearized* update that
**left**-multiplies, i.e. treats `ω` as a world-frame rate, while the same
struct comments that `angularVelocity` is "in body frame." That is
self-contradictory, and it disagrees with the shipped, correct code: the
existing `IntegrateVelocity` **right**-multiplies (`rotation * delta`,
`systems.h:28`), which is the correct body-frame exponential update.

This design follows the **shipped** convention, not the deep-dive snippet:

- `ω` is the body-frame angular velocity.
- The orientation update is the **exponential map** `q ← normalize(q · Δq)` with
  `Δq = FromAxisAngle(ω̂, |ω| dt)`, right-multiplied. This is exact for constant
  `ω` over the step (no first-order linearization error, no reliance on
  renormalization to stay a rotation), and it reuses the exact `FromAxisAngle`
  the render path is tested against.

RULE 7 (left-handed, +Z forward, +Y up) is respected by construction: torque
`τ = r × F` uses the shipped `Vec3f::Cross`, and the exponential map uses the
shipped `FromAxisAngle`/`Rotate`, whose LH sense is pinned by the existing test
(+Y 90° sends +X→−Z). Torque and spin therefore share the renderer's handedness;
there is no second sign convention to keep in step.

### 2.5 Inertia representation: diagonal, no Mat3x3

The first slice stores inverse inertia as a **diagonal** (three reciprocals),
not a `Mat3x3`. The box inertia from the Ship deep-dive is diagonal in body
axes:

```
Ixx = m/12 (W² + H²),  Iyy = m/12 (L² + H²),  Izz = m/12 (L² + W²)
```

With `I` diagonal, `Iω` and `Iinv·τ` are component-wise, and the gyroscopic term
`ω × Iω` is a single cross product. **No `Mat3x3` type is needed** — not even
for the asymmetric-inertia stress test (§9.2), because distinct
`Ixx ≠ Iyy ≠ Izz` already produce the full non-commuting tumble (the
tennis-racket / Dzhanibekov behaviour). A full `Mat3x3` inertia (off-diagonal
products of inertia, mesh-derived) is a later stage and adds `Mat3x3` to
`core/types.h` at that point.

---

## 3. Double-precision boundary — exactly which state is Vec3d

The rule, stated once and applied uniformly:

> **Vec3d** for the integrated world state that must survive long propagation —
> a quantity that is either an absolute world position or accumulates directly
> into one. **Vec3f** for body-local geometry and bounded angular quantities.
> **Quatf** for orientation.

Applied to the rigid body:

| Quantity | Type | Why |
|---|---|---|
| `position` (the ship's world position) | **Vec3d** | RULE 1. Absolute world coordinate. Float spacing is ~1 m at 1e7 m; a ship in orbit lives out there. Stored in `Transform.position`, which is already `Vec3d`. |
| `linearVelocity` | **Vec3d** | Two reasons. (1) It is added to a `Vec3d` position every step — a `Vec3f` velocity would inject float rounding into the double accumulator each step. (2) Orbital conservation laws are written over it: LEO speed ≈ 7.8 km/s, and specific energy `½v² − μ/r` and angular momentum `r × v` must stay constant over an orbit; that needs double. |
| `orientation` | **Quatf** (float) | A **unit** quaternion has components bounded in [−1, 1] regardless of where the ship is in the universe. Float resolution there is ~1e-7 — finer than any dynamically or visually meaningful orientation. Normalizing each step caps drift. Crucially this is the **same type as `Transform.rotation`**, so physics→render is a copy, not a conversion. Double would buy nothing and add a seam. |
| `angularVelocity` | **Vec3f** | Body-frame, O(1–10) rad/s, never large, does not accumulate a world position. Matches the existing `Velocity.angular` (Vec3f) and the deep-dive's `Thruster` fields. |
| `invMass` (scalar), `invInertiaDiag` | `double` scalar / **Vec3f** | Per-body constants. `invMass` is a scalar double for uniform arithmetic with the Vec3d velocity update; the inertia diagonal is small and body-local, Vec3f is fine. |
| Thruster `localPosition`, `localDirection`, `maxForce` | **Vec3f** / float | Body-local offsets in metres (ship is tens of metres) and a unit direction. Well inside float. Matches Ship deep-dive PART 2. |

So exactly **two** members are Vec3d by necessity: `position` and
`linearVelocity`. Everything else is Vec3f/Quatf. The `RigidBody` component
(§6) therefore carries the Vec3d `linearVelocity`; the world `position` and
`orientation` live in `Transform` (already the right types) and are not
duplicated — see §6 for why the pose stays single-sourced in `Transform`.

### 3.1 How a ship at 1e7 m renders without jitter

Unchanged from what already works. The integrator advances
`Transform.position` (Vec3d) and `Transform.rotation` (Quatf) in absolute world
space. The renderer calls `Transform::ToCameraRelativeMatrix(cameraPos)`, which
computes `(position − cameraPos).ToFloat()` — the subtraction is done in double,
and only the small camera-relative residual is narrowed to float for the GPU.
This is precisely the path `Transform_ToCameraRelativeMatrix_PreservesPlanetaryOffset`
already proves at 1e7 m. **The physics integrates in absolute Vec3d and hands
camera-relative Vec3f to the renderer exactly as the existing Transform does.**
No new render code, no per-object rebasing, no floating-origin shift is required
for the first slice — the camera-relative narrow already is the floating origin.

---

## 4. Orbital mechanics: deferred, and how the flight slice behaves near a planet

**Decision: orbital mechanics is NOT in the first slice.**

Engaging with the Orbital deep-dive's own position: it (PART 4) explicitly
splits the problem into **on-rails Kepler propagation** for distant bodies
(analytic `KeplerPosition`, Newton-Raphson solve of Kepler's equation) and
**numerical n-body (leapfrog)** for the player and nearby objects, with a
**patched-conics / SOI switch** between reference frames. That is a large,
multi-part subsystem (orbital elements, `SolveKeplersEquation`, SOI detection,
frame transforms, maneuver nodes, time-warp). None of it is needed to *fly and
fight* in a local arena, which is the Phase 4 milestone.

What the flight slice does near a planet **without** orbital mechanics: gravity
enters the integrator as **one external acceleration term**, nothing more.

- Simplest: a uniform `g` vector in a defined gravity volume (the Physics
  deep-dive PART 5 "gravity zones" with a linear falloff band). Good enough to
  land on a pad and to give the fight a "down."
- One step richer, still no orbital machinery: a single **point-mass** central
  body, `a = −μ · r̂ / |r|²` toward the planet centre, where `r` is the Vec3d
  vector from planet to ship and `μ = GM`. This is a real central force and lets
  a ship arc and fall realistically, but it needs **no** orbital elements, no
  SOI, no Kepler solve — it is just another summand in the force accumulator.

Because gravity is structured as an external acceleration from day one, the
orbital stage is additive, not a rewrite: point-mass gravity → n-body leapfrog
(KDK, §2.3) for the player + nearby bodies → on-rails Kepler for distant bodies
→ SOI/patched-conics. The strongest tests in this whole document (orbit closes
over one period; vis-viva; energy and `r×v` conservation, §9.4) belong to that
stage and are written now so the stage lands against ground truth rather than
"the ship curved."

**Two-body Keplerian vs n-body when the stage does arrive:** follow the
deep-dive — Kepler **on-rails** for everything distant (exact, cheap, no drift),
numerical **leapfrog n-body** only inside the player's SOI. The first slice's
point-mass gravity is the degenerate one-body case of the n-body path, so it is
the natural first rung.

---

## 5. Collision: broad phase and narrow phase for the first slice

Do not over-scope. The Physics deep-dive's SAP + GJK/EPA + sequential-impulse
solver is sized for 10k bodies and rigid stacking; the vertical slice has tens
of bodies in vacuum and needs none of that yet.

**First slice**
- **Shapes:** one bounding **sphere** per ship (radius from the ship class), and
  **AABB or sphere** for static geometry (station hull sections, asteroids).
  Projectiles are rays or small spheres.
- **Broad phase:** brute-force O(N²) sphere-sphere for the active set (2–50
  bodies). Honest and correct at this count; a uniform spatial grid is the first
  upgrade when N grows, SAP much later. Do not build SAP now.
- **Narrow phase:** analytic. Sphere-sphere: compare centre distance to radius
  sum (all in Vec3d, since centres are world positions). Sphere-AABB: closest
  point on the box. Projectile: ray-sphere (`t` of nearest approach) — this is
  the combat hit test.
- **Response:** impulse along the contact normal with a restitution coefficient
  (Physics deep-dive PART 3 values), applied to `linearVelocity`; optional
  angular impulse from the contact lever arm. For ship-vs-static, reflect/stop.
  Penetration is corrected by positional push-out (a fraction of overlap) to
  avoid sticking — no Baumgarte/constraint solver yet.
- **Layers:** adopt the deep-dive's `CollisionLayer` bitmask minimally
  (STATIC, DYNAMIC/PLAYER/NPC ship, PROJECTILE) so the matrix is explicit from
  the start; skip DEBRIS/RAGDOLL/SENSOR until they exist.

**Explicitly later (station interior, not first slice):** capsule character
controller, triangle-mesh collision against interior geometry, GJK/EPA for
convex hulls, the sequential-impulse solver with friction and slop, joints,
sleeping. These belong to the "walk through station" pillar, not the "fly →
fight" one. Flag: the character controller is kinematic and separate from the
rigid-body system (Physics deep-dive step 151-160), so it does not perturb this
design.

---

## 6. Coupling to the existing scene (the physics → transform hand-off)

**Verified path:** `Scene::UpdateSystems(double dt)` (`scene.cpp:110-115`) is
called once per fixed step from the main loop
(`app.cpp:1047-1049`). It writes `Transform` (Vec3d position + Quatf rotation),
which the render walks (`RenderEntities` raster `scene.cpp:221`, DXR TLAS
`scene.cpp:329-340`) turn into camera-relative matrices for both paths. **That
path exists and is what the flight model plugs into.** No renderer change.

**Single source of truth for the pose.** The ship's world `position` and
`orientation` live in `Transform` (which is already Vec3d + Quatf — the correct
types). The new `RigidBody` component adds the *dynamics* state
(`linearVelocity` Vec3d, `angularVelocity` Vec3f, `invMass`, `invInertiaDiag`)
and per-step force/torque accumulators. The integrator is
`Each<Transform, RigidBody>`: it reads the pose from `Transform`, integrates,
and writes the pose back to `Transform`. Position is therefore **not**
duplicated between `Transform` and `RigidBody` — a duplicate would be a second
copy free to diverge, and this ECS already has the right field in the right
type. This mirrors exactly how `Velocity + Transform` works today.

**Hand-off sequence inside `Scene::UpdateSystems(dt)`** (fixed step):
1. `SystemFlightControl(dt)` — read the input snapshot. Decoupled mode maps the
   six normalized axes directly to throttle; coupled mode computes a desired
   physical wrench and maps it through bounded per-nozzle allocation. Missing or
   damaged banks therefore reduce authority rather than gaining ideal force.
2. `SystemThrusters(dt)` — convert the resulting throttles to the realized body
   wrench via `Σ dir·F·throttle` and `Σ leverArm × force` (Ship deep-dive PART 2),
   then bridge body force to world force and add both to the accumulators. The
   same throttle state feeds physics, exhaust visuals, and future damage systems.
3. Add external accelerations (gravity §4, later).
4. `SystemIntegrateRigidBodies(dt)` — semi-implicit Euler (§2), write
   `Transform.position` / `Transform.rotation`, zero the accumulators.
5. `SystemCollision(dt)` — broad+narrow, apply impulses to velocities and
   push-out to `Transform.position` (§5).

Existing `SystemVelocity` / `SystemRotation` stay for kinematic movers
(spinners, debris) and are untouched.

**Interpolation for rendering (optional, recommended once frame-rate ≠ 60):**
the accumulator leaves a residual `alpha = accumulator / fixedDt` (the Physics
deep-dive PART 1 shows it). To avoid render stutter when display Hz ≠ 60, store
the previous fixed-step pose and render `lerp(prev, current, alpha)`. This is a
render-only read; it must not feed back into the simulation (that would break
determinism). The first slice can ship without it (the demo is effectively
frame-locked) and add it when it visibly matters — but the design reserves the
`prevPosition`/`prevRotation` fields so adding it later is not a component
change.

---

## 7. Determinism and save/load

**Fixed step gives reproducibility if inputs are ordered.** The step is a fixed
1/60 s; the same sequence of `(fixed-step index, input snapshot)` pairs produces
the same trajectory. Two things must hold:
- Physics is driven by an **ordered fixed-step count**, not wall-clock. Note the
  existing pause/minimize path deliberately *discards* pending fixed steps
  (`app.cpp:1032-1035`, `while (m_timer.ConsumeFixedStep()) {}`) to avoid an
  unbounded catch-up burst — so a replay must be driven by the recorded step
  index, exactly as smoke mode already drives a frame-locked timeline
  (`app.cpp:1042-1043`). Determinism is a property of the step sequence, not the
  clock.
- Inputs are applied at a well-defined point in the step (§6 step 1), and body
  iteration order is stable (the ECS pools iterate in a deterministic dense
  order).

**What a save needs — the full rigid-body state per body:**
- `Transform.position` (Vec3d ×3), `Transform.rotation` (Quatf ×4) — the pose.
- `RigidBody.linearVelocity` (Vec3d ×3), `RigidBody.angularVelocity`
  (Vec3f ×3) — the momentum state.
- Static per-body constants (`invMass`, `invInertiaDiag`) are derived from the
  **ship-class id** (+ fuel-driven mass later), so the save stores the class id
  and any mutable mass, not the derived tensor.
- The **fixed-step index** (and, if mid-frame saves are allowed, the accumulator
  residual). The Save/Load deep-dive's `SHIP_STATE` chunk already lists
  "position, orientation, velocity" and its reader already uses `ReadVec3d()`
  for position — consistent with the Vec3d boundary here.

A save/load round-trip that restores those fields and replays the same input
sequence must reproduce the same trajectory **on the same binary** — that is a
testable invariant (§9.5), and it reuses the existing scene-hash idea
(`scene.cpp:20-34` already FNV-hashes transform doubles) as a replay/save
checksum.

**Float determinism across machines: NON-GOAL for the first slice.** It is
single-player; nothing needs lockstep. Same-binary/same-machine determinism
(replay, save/load, the smoke timeline) **is** a goal and is achievable with
the fixed step and ordered inputs under MSVC `/fp:precise`. Cross-vendor,
cross-compiler bit-identical results are **not** promised — FMA contraction and
transcendental (`sin`/`cos`/`sqrt`) implementations differ across
toolchains/CPUs. When networked lockstep arrives it will need a deliberate
determinism pass (controlled FP model or fixed-point for the authoritative
sim); **flag it now** so no one assumes today's float sim is lockstep-ready.

---

## 8. Module structure and staged plan

### 8.1 Where the code lives (RULE 2, no cycle)

- **Components** (POD) are **appended** to `src/ecs/components.h` — RULE:
  append-only, do not reorder existing components. New: `RigidBody`,
  `Thruster` (+ a small fixed-capacity `ThrusterSet`), `FlightControl`. Data
  only; no methods beyond trivial inline helpers.
- **Systems / math** live in a **new `src/sim/`** with a proper .h/.cpp split
  (RULE 2):
  - `src/sim/rigid_body.{h,cpp}` — the pure integrator and force/torque math.
    Includes **only** `core/types.h` + `<cmath>`/`<cstdint>`. **No d3d12, no
    registry.** This is the GPU-free core that joins `TheDawningTests`
    (`CMakeLists.txt`) the same way `shadow_cascades.cpp` does, so the shipped
    integrator is what the analytic tests drive.
  - `src/sim/thrusters.{h,cpp}` — thruster wrench accumulation (also GPU-free).
  - `src/sim/collision.{h,cpp}` — broad/narrow sphere tests + impulse
    (GPU-free).
  - `src/sim/physics_system.{h,cpp}` — the ECS glue: iterates the registry,
    calls the pure functions, writes `Transform`. Includes `ecs/registry.h` +
    `ecs/components.h` + the `sim` headers. `registry.h` is itself GPU-free
    (STL only), so this layer is *also* eligible for the test target, which
    lets §9.5 run an end-to-end deterministic-replay test over the real ECS.
- **Dependency direction:** `core/` ← `sim/` ← `scene/`. `sim` includes `core`
  and `ecs` headers; **`core` and `ecs` never include `sim`**, so there is no
  cycle. `Scene::UpdateSystems` (in `scene/`, which already depends on `ecs`)
  calls into `physics_system`. This is the same shape as `scene.cpp` already
  calling `ecs::systems::IntegrateVelocities`.
- **CMake:** add the four `.cpp`/`.h` pairs to a new `SIM_SOURCES`/`SIM_HEADERS`
  group linked into `TheDawningV3`, and add `rigid_body.{h,cpp}`,
  `thrusters.{h,cpp}`, `collision.{h,cpp}`, `physics_system.{h,cpp}` to the
  `TheDawningTests` target (they are GPU-free by construction — do **not** add
  anything from `src/render` or `src/scene`).

### 8.2 Staged plan (each stage independently buildable and testable)

- **Stage 0 — RigidBody + integrator.** Append `RigidBody` to `components.h`.
  Write `src/sim/rigid_body.{h,cpp}` with a pure
  `IntegrateRigidBody(Vec3d& pos, Quatf& orn, RigidBody& body, Vec3d extForce,
  Vec3f extTorque, double dt)`. External wrench is a parameter; no thrusters, no
  ECS yet. Buildable (library compiles), testable (§9.1–9.3 run against it in
  `TheDawningTests`).
- **Stage 1 — Thrusters.** Append `Thruster`/`ThrusterSet`. Write
  `src/sim/thrusters.{h,cpp}`: `ComputeWrench(thrusters, com) → (force,
  torque)`. Testable (§9 thruster cases) with no ECS.
- **Stage 2 — Control mapping + ECS wiring.** Append `FlightControl`. Write
  `physics_system.{h,cpp}`; call it from `Scene::UpdateSystems`. Map WASD+mouse
  to the ship's six axes (§2.4 conventions). Buildable to a flyable ship in the
  app; testable end-to-end (§9.5) and by hand (fly it).
- **Stage 3 — Damping + flight assist.** Add linear/angular damping and a
  "coupled" auto-brake (a P-controller toward zero velocity when input is
  centred), plus a decoupled mode toggle. Allocate the desired wrench through
  installed nozzles so saturation and damage reduce authority. Testable: pure
  damping law (§9.3), bounded allocation, and coupled/decoupled ECS behavior.
- **Stage 4 — Collision.** `src/sim/collision.{h,cpp}` now provides the N-body
  close-encounter policy: bounded global subdivision, swept sphere contact,
  central impulse, and deterministic accretion merge. Its CPU contract is tested
  for anti-tunneling, momentum/energy bounds, determinism, hostile configuration,
  and depth saturation. ECS destruction/reconciliation and detailed ship/interior
  colliders remain follow-on work; projectile ray-sphere is also still pending.
- **Stage 5 — Gravity / orbital (next milestone, deferred).** External-accel
  gravity → point-mass central body → leapfrog n-body → on-rails Kepler + SOI.
  Testable: the orbital ground-truth suite (§9.4).

---

## 9. Verification plan

Ground rules taken from this repo: physics is CPU-only and has **analytic
ground truth**, so tests are pure `TheDawningTests` unit tests, not smoke
captures. For each item: the **assertion**, the **failure mode** it catches, and
the **negative test** that stops it passing while the feature is absent — because
this project's recurring defect is an assertion that is green when the feature it
"tests" is not there. Prefer laws with closed-form answers over "the ship moved."

### 9.1 Constant thrust — translational law
- **Assertion (tight):** under constant force, after N steps,
  `v_N == v₀ + a·(N·dt)` to float epsilon. Semi-implicit Euler integrates
  velocity **exactly** for constant force, so this is an equality, not a bound.
- **Assertion (position):** `x_N` equals the **exact discrete sum**
  `x₀ + v₀·t + a·dt²·N(N+1)/2` to float epsilon; **or** assert first-order
  convergence — halving `dt` halves `|x_N − (x₀+v₀t+½at²)|`. Do **not** assert
  the continuous `½at²` to a tight tolerance; a correct symplectic integrator
  fails that (the `½·a·dt·t` bias, §2.2).
- **Failure modes:** `F`/`m` swapped; `dt` applied twice or not at all; position
  updated from the old velocity (that would be *forward* Euler and changes the
  discrete sum).
- **Negative test:** with `F = 0`, assert velocity and position are **exactly
  unchanged** over N steps. An integrator that drifts from stale state (or that
  "always moves the ship") fails this. This is the direct antidote to
  "passes when the feature is absent": the law test alone would look fine for a
  mover that always adds a fixed drift; the `F=0` no-op control kills it.

### 9.2 Torque-free rotation — angular-momentum conservation (asymmetric body)
- **Assertion:** with **distinct** `Ixx ≠ Iyy ≠ Izz` and zero torque, the
  **world-frame** angular momentum `L = R · (I ω_body)` is conserved:
  `|L_N − L_0| / |L_0| < 1e-6` over 10⁴+ steps. (`R` from the current
  orientation.) Kinetic energy `½ ω·Iω` stays within a tight band (bounded, not
  drifting).
- **Failure modes:** dropping the gyroscopic `ω × Iω` term; integrating `ω` in
  the world frame instead of body; left- vs right-multiplying the orientation
  delta (the §2.4 deep-dive/shipped-code contradiction). Each produces a
  plausible tumble that is *wrong* and that a symmetric-inertia test cannot see.
- **Negative test:** apply a **known** torque impulse `J = ∫τ dt` and assert `L`
  changes by exactly `J` (world frame, over the interval). This proves the
  conservation test is not vacuously green because `L` never changes — a body
  that is simply frozen would pass "L constant."

### 9.3 Damping — decay law
- **Assertion:** with linear damping `a = −c·v` and no thrust, speed follows the
  discrete analogue of `v(t) = v₀ e^{−c t}` (assert the exact per-step geometric
  factor, or first-order convergence to the exponential). Angular damping the
  same for `ω`.
- **Failure mode:** damping sign flipped (anti-damping → blow-up); damping
  applied to position instead of velocity.
- **Negative test:** with `c = 0`, velocity is **exactly constant** — a test
  that always sees decay (e.g. because it silently drains energy) fails here.

### 9.4 Orbital ground truth (Stage 5 — written now, run when the stage lands)
- **Circular orbit maintains radius:** seed `v = √(μ/r)` perpendicular to `r`;
  assert `|r|` stays within a tight band over many steps.
- **Orbit closes over one period:** after `T = 2π√(a³/μ)`, position and velocity
  return to initial within bound (leapfrog, §2.3). RK4 visibly fails this over
  many periods (energy drift) — which is *why* leapfrog is chosen.
- **Vis-viva:** at radius `r` on an orbit of semi-major axis `a`,
  `|v|² == μ(2/r − 1/a)` (Orbital deep-dive PART 1).
- **Conserved quantities:** specific energy `½|v|² − μ/|r| == −μ/2a` and angular
  momentum `|r × v|` constant to bound.
- **Negative test:** perturb `μ` by 1% and assert the orbit does **not** close /
  vis-viva does **not** hold — proving the test discriminates the physics rather
  than passing for any smooth curve.

### 9.5 Determinism / save-load round-trip (end-to-end over the ECS)
- **Assertion:** run the ECS physics for N fixed steps with a scripted input
  sequence; snapshot the full state; reset; replay the same inputs; the
  trajectories are **bit-identical** (same binary). A save→load→replay reproduces
  the pre-save future exactly. Reuse the existing FNV state-hash
  (`scene.cpp:20-34`) as the checksum.
- **Failure mode:** unordered iteration; input applied at an inconsistent point;
  hidden wall-clock dependence; uninitialized accumulator on load.
- **Negative test:** change one input in the replay and assert the hash
  **differs** — so "identical" is not trivially true because nothing depends on
  input.

### 9.6 Collision (Stage 4)
- **Assertion (elastic, closed form):** two equal masses, head-on, restitution
  1 → velocities **exchange**; momentum and kinetic energy conserved. Unequal
  masses → the 1-D elastic-collision formula. Restitution 0 → common final
  velocity, momentum conserved.
- **Assertion (ray-sphere):** a projectile ray hits iff its perpendicular
  distance ≤ radius; assert hit/miss exactly at the analytic boundary and the
  impact `t`.
- **Negative test:** two bodies that are **not** overlapping produce **no**
  impulse (velocities unchanged) — an "always resolve" bug that nudges
  non-contacting bodies fails this.

**The two assertions I would trust most** (they exercise the most machinery and
have airtight ground truth + a real negative control):
1. **§9.2 torque-free angular-momentum conservation with an asymmetric inertia
   tensor**, paired with its known-impulse negative control. It simultaneously
   exercises the gyroscopic term, the body↔world frame handling, and the
   quaternion exponential map — the three places rotational dynamics goes subtly
   wrong — and `L = const` is exact ground truth. The negative control
   (`L` changes by exactly `∫τ`) closes the vacuity hole.
2. **§9.1 constant-thrust velocity `v = v₀ + at` (exact) with the `F = 0`
   no-drift control.** It is a *tight* equality (not a convergence bound),
   pins `F = ma` and the `dt` scaling, and the `F=0` control is the direct
   counter to this codebase's signature failure — an assertion that stays green
   when the feature is absent.

---

## 10. Risks and what could not be resolved from the code

- **Biggest risk — a rotational frame-convention error that only asymmetric
  ships reveal.** The Ship deep-dive's orientation update contradicts the
  shipped `IntegrateVelocity` on left- vs right-multiply and world- vs
  body-frame `ω` (§2.4). A wrong choice renders *fine* for symmetric ships and a
  symmetric-inertia test, and only manifests as a wrong tumble on asymmetric
  bodies. Mitigation is exactly §9.2 (asymmetric inertia + angular-momentum
  conservation + known-impulse control); it must be a hard `CHECK`, not skipped
  because "the ship rotates." This is the one place I would not trust visual
  inspection at all.
- **Flight *feel* has no analytic ground truth.** Damping constants, assist PID
  gains, and per-class thrust/inertia are tuning. Tuning presented as
  correctness is this repo's documented failure mode. Keep the tunables in data
  (ship-class presets, Ship deep-dive PART 5) and out of the tested laws; the
  tests assert *conservation and closed-form kinematics*, never "feels right."
- **Interpolation feedback.** If render interpolation (§6) is ever read back
  into the sim, determinism breaks silently. Reserve the `prev*` fields but keep
  interpolation strictly render-side; a determinism test (§9.5) is the guard.
- **Could not resolve from the code:**
  - There is no existing physics integrator to reconcile against beyond the toy
    `IntegrateVelocity`; the deep-dives are the only prior art, and they assume
    `Quatd`/`Mat3x3`/`Quatd`-orientation that the codebase does not have. This
    design chooses to *not* add those types for the first slice (§2.5, §3); if a
    later stage needs a full inertia tensor it adds `Mat3x3` then. Flagged so no
    one adds them speculatively.
  - The exact input-sampling point for deterministic replay is a design choice
    here, not something the current loop fixes: input is presently sampled once
    per rendered frame (`app.cpp` `UpdateCamera`), while physics may run 0..n
    fixed steps per frame. For a flyable ship at 60 Hz this is 1:1 and fine; a
    rigorous replay system will need input tied to the fixed-step index. Not
    resolvable without deciding the input model — called out, not silently
    assumed.
  - Whether interpolation is needed at ship velocities in the demo could not be
    judged from static code (it depends on the shipped display Hz vs 60). Hence
    it is reserved-but-deferred rather than mandated.

---

## Coordination note (READ BEFORE IMPLEMENTING)

Per `AGENT_COORDINATION.md` and the RULES: simulation was assigned to **Codex**
in an earlier lane split, but **Codex is currently out of usage.** This design is
written so implementation can proceed by whoever picks it up.

- **If Codex returns, this is the lane to reconcile against.** Do not integrate a
  physics implementation without checking it against this document and against
  any Codex work in progress; use separate worktrees/branches and check file
  overlap before integration (the parallel-agent rules).
- **Files this lane touches:**
  - `src/sim/**` — **new** (`rigid_body`, `thrusters`, `collision`,
    `physics_system`). No existing file conflict.
  - `src/ecs/components.h` — **append-only**: add `RigidBody`, `Thruster`/
    `ThrusterSet`, `FlightControl` at the end. **Do not reorder** existing
    components (layout/order is relied on elsewhere).
  - `src/scene/scene.*` — the physics→transform hand-off: `Scene::UpdateSystems`
    gains calls into `physics_system`. This is the one shared file with the rest
    of the engine; keep the change to the update-phase wiring.
  - `CMakeLists.txt` — new `SIM_SOURCES`/`SIM_HEADERS`; add the GPU-free `sim`
    `.cpp`s to `TheDawningTests`.

This file is intentionally left **untracked** in the main checkout at
`D:/The Dawning (new)/The Dawning`; it is a design artifact, not a committed
part of the tree.
