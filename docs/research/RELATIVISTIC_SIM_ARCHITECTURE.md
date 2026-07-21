# RELATIVISTIC_SIM_ARCHITECTURE — The Dawning V3

Foundation architecture for a realistic space simulation: orbital mechanics, time
dilation (SR velocity + GR gravitational), Newtonian↔relativistic dynamics, atmospheric
flight, and a consistent FTL/warp/wormhole fiction — built so the load-bearing
coordinate decision is validated FIRST and the fiction is built LAST on a proven base.

**Status:** design only. No source, shader, or build file is modified by this document.
Flight Stage 2 is concurrently wiring `IntegrateRigidBody` into the scene loop; this
design is written to sit *around* that wiring, not to rewrite it.

**Provenance:** synthesized from three candidate architectures and four adversarial
review lenses (physical coherence, numerical robustness, architectural fit, verifiability).
The relativistic-dynamics core, orbital ownership, and staging follow the design ranked
first on every lens (momentum-space dynamics); the complete atomic-teleport state set is
grafted from the second; the KDK-Verlet-as-primary-orbital-integrator commitment that a
lens flagged as a coherence/double-integration liability is dropped.

Throughout, **VERIFIED** marks a claim checked against the code at the cited location on
2026-07-20; **ASSUMED** marks a design choice or an unread dependency.

---

## Executive summary

The spine is **Design 2 — Hierarchical Reference Frames + Proper-Time Sidecar + Momentum-Space
Dynamics**, ranked first by all three adversarial lenses (architectural fit & verifiability,
numerical robustness, physical coherence). It wins because it *generalizes the shipped
camera-relative primitive* rather than fighting it, and because its relativistic core is robust
**by construction** rather than **by clamp**. Grafted onto that spine: the complete
atomic-teleport retained-state set from the second-ranked design, and universal-variable Kepler
for hyperbolic/parabolic escape states. Dropped: the third design's commitment to KDK-Verlet as
the *primary* orbital integrator — a lens flagged it as both a physical-coherence error (KDK's
symplectic property is void where non-conservative thrust/drag live) and a double-integration
hazard on the shared linear half.

**Real physics vs fiction.** Orbital mechanics, gravity, atmosphere, and SR+GR time dilation are
**real physics** here (with weak-field / single-primary approximations named where they are made).
FTL, warp, and wormhole are **internally-consistent fiction** — not real physics — engineered so
their only hard invariant is a clean, deterministic, bit-exact frame/position discontinuity.

**The seven locked consensus decisions** — each converged on by all three lenses, each stated with
its watched-failure verification:

1. **Momentum-space relativistic dynamics** (§3). Advance `p += F·dt`; recover
   `v = p/√(m² + (|p|/c)²)`. `|v| < c` becomes a *structural property of the stored state*, not a
   clamp on a derived γ, so the near-c NaN is impossible by construction. *Watched:* an enormous
   force over many steps asymptotes `v → c` with no NaN, while the naive γ-denominator form
   overflows on the same input. A β clamp survives only as defense-in-depth for the dilation
   clock's `√(1−β²)`.
2. **Deviation-accumulated dilation** (§2). Compute `1−β²` as `(1−β)(1+β)`, use the series
   `1−β²/2` near β=0, and accumulate the deficit `Σ(dt−dτ)` in a separate double — never
   `τ += factor·dt` — so the ~1e-9 dilation signal is not drowned in coordinate-time magnitude.
   *Watched:* at `v=0, r=∞` the factor is `1.0` bit-exact and the deficit increment is exactly `0.0`.
3. **One shared Plummer softening** (§5.4, §2.6). `g = −μ·r/(r²+ε²)^1.5` and the GR clock
   `√(1−2GM/(r c²))` share ONE `ε`, floored at `max(body radius, r_s+ε0)`; the deep-dive's
   `if(dist<1) continue` is explicitly rejected. *Watched:* the softened force stays finite as
   `r → 0`, with the discontinuous-zeroing form as the anti-test.
4. **Exactly one owner per body per step** (§5.1). OnRails XOR Forced, debug-asserted; on-rails
   owns every passive orbit (analytic, cannot drift) and receives NO primary gravity; thrust forces
   an on-rails→force state-vector transition; the shipped `IntegrateRigidBody` is reused
   **unchanged** (no KDK on its linear half). Also enforced: an entity carries `Vec3f Velocity` XOR
   `RigidBody`, never both. *Watched:* a body given both an on-rails write and its primary's gravity
   diverges, and the one-owner guard fires.
5. **Robust Kepler solver** (§4.3, §4.5). Seed `E0 = M + e·sinM` (Danby/Markley), not `E=M`; capped
   Newton with a bisection fallback; universal variables near `e=1` (slingshot/escape produce
   near-parabolic and hyperbolic state). *Watched:* Kepler **closure** over one period + **dt-
   convergence** of integrated drift — never exact energy/momentum conservation (Stage 1's
   fantasy-tolerance lesson); `E=M` fails to converge at `e=0.85` where the robust seed succeeds.
6. **Atomic FTL teleport inside the RULE-6 fixed step** (§7.3). Rotate world-frame `linearVelocity`,
   authoritative `RelativisticBody.momentum` when present, AND orientation by the mouth-to-mouth
   rotation; flush `forceAccum`/`torqueAccum`, set
   `prevPosition`/`prevRotation` to the post-teleport pose (zero interpolation smear), drain the
   fixed-step accumulator, reset `m_accumFrameIndex` but NOT `m_seedFrameCounter`. *Watched:* the
   identity-mouth round-trip returns bit-exact retained state, while rotated mouths meet the
   documented quaternion/coordinate tolerance; a teleport that forgets to rotate either
   world-frame velocity or momentum is watched failing. Warp cruise's invariant is deterministic,
   bounded stepping (same route → hash-identical arrival).
7. **Stage 0 is the load-bearing validation, first** (§1, §9). Hierarchical relative-to-parent
   `Vec3d` keeps intra-frame coords under ~1e13 m (~2 mm ULP); every force/separation/relative-
   velocity is computed WITHIN one frame, because catastrophic cancellation — not stored jitter —
   is the real killer. *Watched:* the `test_math.cpp:613` negative control replicated at every new
   narrowing site (1e7 m, 1 AU, a rebased far origin) — the naive absolute narrow LOSES a sub-metre
   offset; subtract-first preserves it — driven through the real matrix builder.

**The single biggest open problem** (§1.6, §11): the interstellar top-level frame retains a
galaxy-scale precision hole. A star ~1 ly from the active sector origin, stored relative to a
sector frame spanning 9.5e15 m, is only ~2 m resolvable in double. Hierarchical framing solves
intra-*system* precision (planet SOIs ~5e10 m are luxurious) but NOT distant frame *origins*. It
is bounded here (the imprecision lives only in static catalog placement, restored to mm on arrival
via rebase) but not eliminated; candidate directions are interstellar sub-framing and integer
sector coordinates + double intra-sector.

---

## 0. What was verified against the code

| Claim | Location | Result |
|---|---|---|
| `Transform.position` is `Vec3d`; `ToCameraRelativeMatrix` subtracts camera in double, narrows the residual | `src/ecs/components.h:21`, `:25-32` | VERIFIED |
| `RigidBody` carries `Vec3d linearVelocity`, `Vec3d forceAccum`, `Vec3f torqueAccum`, `Vec3d prevPosition`, `Quatf prevRotation`, `double invMass`, `Vec3f invInertiaDiag` | `src/ecs/components.h:149-181` | VERIFIED |
| Linear half is semi-implicit Euler: `v += a*dt; x += v*dt`; `a = totalForce * invMass` | `src/sim/rigid_body.cpp:125-128` | VERIFIED |
| `externalForce` is a per-call input, not stored, not cleared ("e.g. gravity, later") | `src/sim/rigid_body.h:73`, `.cpp:125` | VERIFIED |
| Angular gyroscopic coupling solved IMPLICITLY (Cramer 3×3); explicit form measured diverging to NaN in ~1e4 steps | `src/sim/rigid_body.cpp:30-86` | VERIFIED |
| House guard: `if (!(dt > 0.0) || !std::isfinite(dt)) return;` leaves accumulators intact | `src/sim/rigid_body.cpp:118` | VERIFIED |
| Accumulators zeroed at end of step | `src/sim/rigid_body.cpp:164-165` | VERIFIED |
| `core::types.h` has `Quatf` only — **no `Quatd`, no `Mat3x3`** | `src/core/types.h:238-338` | VERIFIED |
| Fixed timestep: `m_fixedDt = 1/60`, `kMaxDt = 0.25`, `ConsumeFixedStep()` drains one `m_fixedDt` at a time | `src/core/timer.h:51-53`, `.cpp:40,67-71` | VERIFIED |
| Path-trace reset compares full-double camera pos (`HasPositionChanged`, eps `1e-5` in double); `m_accumFrameIndex=0` on change; `m_seedFrameCounter` NEVER resets | `src/render/path_tracer.cpp:90,844,859`, `.h:280-281` | VERIFIED |
| GPU camera pinned at local origin (`ViewMatrix` uses `origin={0,0,0}`) | `src/render/camera.cpp:77-83` | VERIFIED |
| Shadow snap floors in double; exact while `\|lx\|/t < 2^53` (~3.8e14 m at cascade 0) | `src/core/shadow_cascades.cpp:87-94` | VERIFIED |
| Negative-control test exists: naive narrow LOSES a 0.25 m offset at 1e7 m; subtract-first preserves it | `tests/test_math.cpp:613-625` | VERIFIED |
| Fixed-step site: `while (m_timer.ConsumeFixedStep()) m_scene.UpdateSystems(GetFixedDt());`, with a drain form `while(ConsumeFixedStep()){}` | `src/app.cpp:1033,1042-1048` | VERIFIED |
| Orbital deep-dive uses `if (dist < 1.0) continue;` softening and labels leapfrog "energy-conserving" | `docs/research/TheDawning_Batch1_Orbital_Mechanics_Deep_Dive.md:235,262` | VERIFIED (both rejected below) |

Everything the three candidate designs asserted about the code that is *relevant to this
synthesis* was checked and holds. The line numbers a couple of candidates cited for the
path tracer were off by a few (the predicate is at `:844`, not `:90-95`); the substance is
correct. `RULE 1`, `RULE 6`, and the camera-relative contract are exactly as described.

---

## 1. Coordinate architecture — the load-bearing decision

### 1.1 Decision

**Hierarchical reference frames**, with plain floating-origin as the degenerate
single-frame first step. Every world position stays `Vec3d` but is stored **relative to
its containing frame**, not absolute. This is a *generalization of the primitive that
already ships*, not a new mechanism.

Rejected alternatives, with numbers:

- **Raw absolute `Vec3d`.** ULP ≈ `value · 2.22e-16`. At 1 AU (1.5e11 m): ~3e-5 m — fine.
  At 1 ly (9.5e15 m): ~2 m. At 1000 ly: ~2 km. At the galaxy edge (~1e21 m): ~city-scale.
  The killer is not stored jitter but **catastrophic cancellation**: two ships 10 m apart
  stored as ly-scale absolutes know their *separation* to ~2 m — a 20% force error from a
  `1/r²` law — long before either stored position looks wrong. Rejected as primary storage.
- **int64 fixed-point (`Vec3i64`).** The ONLY option that makes position addition exact,
  associative, and therefore *cross-machine deterministic*. But it replaces `Vec3d`
  engine-wide, forces a double conversion at every integrator/force step, and cannot
  natively express rotating/orbiting frames. **Rejected as primary storage; retained as
  the documented escape hatch** if networking later demands cross-machine determinism
  (§8, §11).

### 1.2 Why hierarchical extends what exists (VERIFIED mechanism)

`Transform::ToCameraRelativeMatrix` (`components.h:27`) is already the whole floating-origin
operation: `(position - cameraPosition).ToFloat()` subtracts a `Vec3d` reference **in
double**, then narrows only the small residual to float. `Camera::ViewMatrix`
(`camera.cpp:77-83`) pins the GPU camera at the local origin. Generalizing the subtracted
reference from *"the camera"* to *"the active-frame origin (chosen near the player)"* turns
that one method into frame→camera composition **with zero change to the GPU contract**.

### 1.3 Structure

A **FrameGraph**: nodes are reference frames, edges are parent links.

```
FrameId  (opaque handle; 0 = root)
Frame {
    FrameId  parent;            // kInvalidFrame for the root
    Vec3d    originInParent;    // this frame's origin, expressed in the parent frame
    Quatd    orientationInParent; // NEW TYPE REQUIRED — see §1.7
    double   mu;                // GM of the body this frame is anchored to (0 for pure sectors)
    double   epoch;             // coordinate time at which this frame's elements are defined
}
```

Frame boundaries **are** sphere-of-influence boundaries, so the frame tree is
`root → sector → star-system → planet-SOI → moon-SOI`, 1:1 with patched conics (§4). Each
body carries a `FrameId` naming its containing frame; `Transform.position` is its offset
within that frame.

**Frame-extent budget.** Keep intra-frame local coords under **~1e13 m**, where double ULP
is ~2 mm — luxuriously inside gameplay tolerance. Planet SOIs are tiny by comparison
(Jupiter's Hill sphere ~5.3e10 m), so intra-system precision is a solved problem. The
interstellar frame is the exception and is treated explicitly in §1.6.

**The invariant that defeats cancellation:** every force, separation, relative velocity,
and dilation term is computed **between two positions in the same frame**, where both are
small. Cross-frame math is never done in raw coordinates — it is done by composing offsets
up the tree (§1.4), which keeps every intermediate small.

### 1.4 Active frame and render handoff

The **active frame** is the one containing the camera, re-selected each fixed step. Per
render frame, for each visible body:

1. Compose its `Vec3d` position from its own frame up the parent chain to the active frame,
   accumulating relative `Vec3d` offsets **in double** (each step small, so the sum stays
   small and precise). Frame orientations compose in `Quatd` (§1.7).
2. `ToCameraRelativeMatrix` subtracts the camera position (also expressed in the active
   frame) and narrows the residual — **the identical GPU contract**.

The DXR TLAS is already rebuilt camera-relative every frame (`scene.cpp:331`, ASSUMED from
candidate citations — not re-read here, but consistent with the camera-at-origin contract
VERIFIED at `camera.cpp:77`), so a rebase is nearly free for ray tracing.

### 1.5 Rebase

When the camera crosses an SOI, the active frame changes. The camera is re-expressed
relative to the new frame origin by one double subtraction; distant bodies are never stored
in active-frame coords, only composed on demand. This is what actually solves *global*
scale — floating origin alone keeps the active region precise but still stores every
distant star in one `Vec3d` list at ~2 km resolution at 1000 ly. Relative-per-frame storage
is the fix.

Two consumers must track the active-frame origin in lockstep:

- **Shadow-cascade snap** (the one sanctioned absolute-`Vec3d` consumer, RULE 1's
  exception, VERIFIED `shadow_cascades.cpp`). Anchor its world-origin texel lattice to the
  **active-frame origin**, kept near the camera so `floor(lx/t)` stays exact (`|lx|/t < 2^53`,
  VERIFIED `:87`). A frame switch reseats the lattice once; the matrix is rebuilt each frame
  so it is at most a one-frame reseat. RISK: needs empirical check that the reseat is
  imperceptible (§11).
- **Path-trace accumulation** (VERIFIED `path_tracer.cpp:844,859`). A rebase that perturbs
  camera-relative coords by *rounding* MUST force `m_accumFrameIndex = 0`. A bit-identical
  rebase MAY keep accumulating. `m_seedFrameCounter` is NEVER reset (VERIFIED `.h:281`).

### 1.6 The interstellar frame-extent gap — named, not hidden

Hierarchical framing solves intra-*system* precision. It does NOT by itself fix distant
frame *origins*: a star ~1 ly from the active sector origin, stored relative to a sector
frame spanning 9.5e15 m, is only ~2 m resolvable in double. This is the sharpest shared gap
across all three candidate designs.

**Resolution adopted here:** interstellar space is sub-framed. A `sector` frame spans at
most ~1e13 m of *precise* extent; beyond that, star systems are children of the sector at
`originInParent` magnitudes up to ~1 ly, and the ~2 m origin imprecision is **accepted as a
non-gameplay-relevant constant** because (a) no gameplay physics is ever computed across a
sector boundary — you are always inside some system's or the active sector's small local
frame — and (b) a 2 m error on a star's *placement* 1 ly away is far below any rendered or
navigational tolerance at that range. When the player warps to that star, the destination
frame is rebased to a local origin (§7) and precision is restored to mm before any local
physics runs. So the imprecision exists only in the *static catalog placement* of distant
frame origins, never in any dynamics. This is the honest boundary: mm everywhere physics
happens, ~2 m in the interstellar map, restored to mm on arrival.

### 1.7 New type required: `Quatd`

Deep frame hierarchies compose orientations across many levels. `Transform.rotation` is
`Quatf` (VERIFIED `types.h:241`) — fine for a single body's attitude, but composing float
quaternions across a 4–5-level frame chain compounds error in a distant frame's basis.
`core::types.h` has **no double quaternion and deliberately no `Mat3x3`** (VERIFIED). Frame
`orientationInParent` therefore requires a new `Quatd` type (double `x,y,z,w`, the same
operations as `Quatf`). `Transform.rotation` stays `Quatf`. This is the one new math type
the coordinate architecture forces; it is additive and does not touch the GPU contract.

If most frames turn out to be translation-only (a real possibility — orbits translate, they
do not force the *frame basis* to rotate), `Quatd` composition collapses to identity and the
type is a cheap insurance. Prefer translation-only frames wherever a use case does not force
rotation (§11 open question).

### 1.8 RULE 1 preserved, not weakened

No absolute position is ever narrowed before its frame subtraction. Every **new narrowing
site** (frame origin instead of camera) gets the same negative-control test as
`test_math.cpp:613` (VERIFIED): assert the naive narrow LOSES the sub-metre offset and
subtract-first preserves it. A regression here is otherwise silent.

### 1.9 Trace through subsystems

- **Orbital integration** runs in the body's own frame — small coords, no cancellation.
- **Time dilation** reads velocity/potential composed to a designated master frame (§2.3).
- **Gravity/atmosphere forces** are computed in the body's frame.
- **SOI / atmosphere / FTL transitions** are all the *same* frame-rebase + state-vector
  operation (§4.4, §6.3, §7).
- **Save/load** stores `FrameId` + `originInParent` per object and the whole FrameGraph (§8).
- **Camera-relative rendering** is mechanically unchanged; only *what is subtracted* changes.

---

## 2. Time — coordinate time vs proper time

### 2.1 Coordinate time (unchanged)

Coordinate time is the single global RULE 6 accumulator (VERIFIED `timer.h:51-53`). It stays
exactly as shipped: `dt` decides only HOW MANY fixed steps run; the step itself consumes
only `m_fixedDt = 1/60 s` (VERIFIED the loop consumes `GetFixedDt()`, `app.cpp:1048`), which
is what makes it deterministic. All position/orientation/orbital propagation advances at the
fixed coordinate `dt`. **`dt` is NEVER made per-object** — that would break the single-
accumulator contract and determinism.

`kMaxDt = 0.25` (VERIFIED `timer.cpp:40`) lets up to ~15 fixed steps run in one catch-up
frame after a stall. Each is a fixed coordinate step accumulating its own dilation — correct,
and it specifically avoids ever taking one giant dilated step.

### 2.2 Proper time — a per-object accumulated scalar side-channel

A new POD component, added the way `prevPosition` was pre-reserved:

```
Clock {
    double properTimeDeficit;   // Σ (dt − dτ), accumulated
}
```

Each fixed step, per object:

```
factor = sqrt(1 − beta^2) · sqrt(1 − 2·G·M / (r · c^2))     // SR · GR, weak field
dtau   = dt · factor
properTimeDeficit += (dt − dtau)                            // accumulate the DEVIATION
```

Proper time is reported as `properTime = totalCoordinateTime − properTimeDeficit`.

**Why the deficit, not `tau += dt·factor`.** Near `beta=0` and `r=∞` the factor is ~1.0 and
the dilation signal is a tiny residual (~1e-9 of the step) that would drown in coordinate-
time magnitude if summed onto a large running total. Accumulating the *deviation* separately
is the same double-accumulator/residual discipline the shadow texel snap and SH irradiance
already use. This is a must-have, applied.

**SR·GR multiply is first-order.** The rates multiply only to first order in the weak field.
This is **labeled as an approximation**, not presented as exact GR — see §2.4.

### 2.3 The frame-invariance fix (shared fatal flaw, resolved)

All three candidate designs wrote `beta = |linearVelocity| / c`. But `linearVelocity` is
stored *relative to the body's frame*. SR proper time is only well-defined against a single
coordinate frame. A ship at rest planetocentrically, on a planet orbiting its star at 30
km/s, has `beta ≈ 0` in its own frame but `beta ≈ 1e-4` against the star — so a naive
per-frame `beta` makes accumulated proper time **frame-dependent and physically incoherent**.

**Resolution (must-have, applied):** define ONE designated **master coordinate frame** per
gravitational context — the top of the current star-system frame (the barycentric/heliocentric
frame). Before computing `beta`, compose the body's velocity up the frame tree to that master
frame:

```
v_master = velocity_of_body_in_its_frame
         + Σ (velocity of each intermediate frame origin in its parent)   // frame drift velocities
beta     = |v_master| / c
```

The frame drift velocities are exactly the orbital velocities the FrameGraph already tracks
(a planet-SOI frame's origin moves at the planet's orbital velocity in the star frame). So
the composition reuses machinery that exists for orbits. **`beta` is measured against the
star-system master frame, never against raw frame-relative `linearVelocity`.** The same
master frame is the reference for which potential terms enter the GR factor (§2.4).

For interstellar travel the master frame is the sector frame; the choice of master frame is
itself part of saved state (it changes on a warp/SOI transition, and the proper-time
accumulation is continuous across the change because both sides agree on `v_master` at the
instant — the same state-vector-continuity argument as §4.4).

### 2.4 The GR single-primary truncation — named, not hidden

The GR factor `sqrt(1 − 2GM/(r c²))` uses only the **single active primary's** potential.
Within a star system, the *star's* potential at a planet's surface is comparable in
magnitude to the planet's own, so this factor under-counts gravitational dilation. This is
acceptable as a weak-field approximation but must be **labeled** — it is a patched-conic-
style single-primary truncation, not complete physics. The honest statement in the displayed
clock and the docs: *"gravitational dilation is computed against the dominant local mass;
multi-body potential summation is deferred."* If a scenario ever needs it, the potential is a
scalar sum `Φ = −Σ GMᵢ/rᵢ` over the bodies in the master frame — the same softened `rᵢ` as
gravity (§5) — added with no architectural change, only more terms.

### 2.5 Cosmetic vs gameplay

**Cosmetic by default.** Dilation is a displayed clock / differential-aging readout. It does
NOT touch position integration, so it costs one scalar field and a few multiplies per body
and CANNOT break determinism or the fixed step.

**Gameplay-affecting differential aging is available at zero extra cost** because
`properTimeDeficit` is real saved state — the twin who flew near c returns younger, and that
is a persisted number. The only architectural cost is in systems that *read* proper time and
in the save set.

**Explicit non-goal:** making dilation change a ship's *action rate* (a dilated ship running
fewer AI/physics ticks). That requires per-object sub-stepping and destroys the single-
accumulator model. Differential *aging* is free; differential *simulation rate* is out of
scope (§11).

### 2.6 NaN bounds for the clock

- `beta → 1`: clamp `beta` to `beta_max = 1 − 1e-12` **before** the sqrt. Because dynamics
  uses momentum recovery (§3), `beta` from `v_master` is always `< 1` by construction, so
  this clamp is defense-in-depth that never bites in normal play.
- `1 − beta²` cancellation near c: compute as `(1−beta)(1+beta)`, never `1.0 − beta*beta`.
- Near `beta=0`: use the series `sqrt(1−beta²) ≈ 1 − beta²/2` so precision is not wasted
  forming `1.0 − tiny`.
- GR term imaginary for `r < r_s = 2GM/c²`: floor `r` at `max(body radius, r_s + eps)` using
  the SAME softened `r` as gravity (§5). Weak-field GR is valid only `r ≫ r_s`.
- Assert every step: both factors finite, in `(0, 1]`. A non-finite factor degrades to
  `deficit += 0` (the house guard pattern, VERIFIED `rigid_body.cpp:118`), never propagates.

---

## 3. Relativistic dynamics — momentum-space, singularity-free

### 3.1 Decision: integrate momentum, not velocity

Store and advance **relativistic momentum** in the force-integrated regime. Recover velocity
for the position update:

```
p += F · dt                                  // semi-implicit, matches the shipped v-first structure
v  = p / sqrt(m^2 + (|p|/c)^2)               // exact inverse of p = γ m v
position += v · dt
```

This is algebraically the relativistic relation `p = γmv` (since
`γm = sqrt(m² + p²/c²)`), so it is simultaneously **physically correct** and **structurally
singularity-free**:

- `|v| < c` is a **structural invariant of the stored state** for every finite `p` — not a
  clamp on a derived quantity.
- There is **no `1/sqrt(1−β²)` anywhere in the dynamics path**, so no term can divide by zero
  or take a negative sqrt as the ship approaches c.
- `c` is an unreachable asymptote under any finite thrust — no hard rail, no clamp needed for
  the dynamics.

This is why momentum-space beat the velocity-space form
`a = (F − (v·F)v/c²)/(γm)` on every review lens: that form is *correct* and C∞-continuous but
keeps the γ singularity *inside the formula*, so it depends on a `beta_max` clamp never being
bypassed — robustness-by-band-aid over a form that can NaN, versus robustness-by-construction.
The candidate that stated *both* an acceleration form and a "near c" momentum form was
rejected on this point: a "near c" switch reintroduces exactly the seam it claims to avoid.

### 3.2 The Newtonian limit is continuous by construction

Below `beta ≈ 0.01` (`γ − 1 < 5e-5`), the relativistic and Newtonian velocity recovery are
numerically indistinguishable (`< 0.005%`). An implementation MAY skip the sqrt correction for
a fast Newtonian path, but this is a **performance branch producing the same bits within
tolerance**, not a model change. There is no discontinuous regime switch. On-rails Kepler
bodies (§4) are non-relativistic by construction (planets do not move near c), so momentum-
space dynamics only ever engages in the force-integrated regime.

### 3.3 Where it plugs in

The **linear half** of `IntegrateRigidBody` (VERIFIED `rigid_body.cpp:125-128`), as a thin
momentum↔velocity adapter:

- Map `forceAccum + externalForce` to `p += F·dt`.
- Rewrite `linearVelocity` from `p` via the recovery formula.
- The position update `x += v·dt` is unchanged in form.

`RigidBody.linearVelocity` stays a `Vec3d` world/frame velocity (VERIFIED `components.h:154`),
because position integration and the orbital invariants (`½v² − μ/r`, `r×v`) are written over
velocity; `p` is derived/stored alongside it. The **angular half is untouched** (VERIFIED the
implicit gyroscopic solve, `rigid_body.cpp:30-86`) — a ship's attitude dynamics are not
relativistic at these scales, and orientation dynamics are orthogonal to translation. This
respects the Stage-1 robustness lesson (the explicit gyro form was measured diverging to NaN;
do not disturb the implicit fix).

Because Flight Stage 2 is concurrently wiring the integrator into the scene, the adapter is
proposed as a wrapper around the existing Newtonian update, not a rewrite of it (§11 risk).

### 3.4 NaN bounds

- The recovery `sqrt(m² + (|p|/c)²)` has a strictly positive radicand (`m > 0`) — it cannot
  go imaginary and its denominator cannot vanish. This is the whole point.
- `beta` for the *clock* is derived from `v_master` (§2.3) and clamped defense-in-depth (§2.6).
- Negative control (§10): feed an enormous force for many steps; `|v|` asymptotes to c, never
  NaN, never `≥ c`. Fed the same input, the naive γ-denominator form overflows — watched, to
  prove the momentum form's advantage is real, not asserted.

---

## 4. Orbital mechanics — patched conics on-rails, force-integration only where perturbed

### 4.1 Decision

**Primary = patched-conics, on-rails, analytic two-body per sphere-of-influence.** Force-
integrated Newtonian dynamics is used ONLY for the body/bodies the player is actively
perturbing inside the current SOI. This is the orbital deep-dive's Part-4 split, with its
gaps corrected.

### 4.2 Justification (engaging the deep-dive)

- An analytic propagator **cannot blow up**: no integrator, no timestep, energy `½v²−μ/r` and
  angular momentum `r×v` exact by construction, fully deterministic, and hash-identical under
  both smoke and real stepping (it reads coordinate time `t`, not an accumulated integrator
  state). Time-acceleration is free: evaluate `KeplerPosition(elements, t)` at any `t`.
- The shipped semi-implicit Euler (VERIFIED `rigid_body.cpp:127`) visibly **precesses** a
  Kepler ellipse over long arcs — it is first-order and the wrong tool for a passive orbit
  that must stay stable for hours. **Resolution: on-rails Kepler OWNS every passive/stable
  orbit**, so a passive orbit is never numerically integrated and the precession never bites.
  Force-integration runs only transiently while thrust/atmosphere/perturbation act, over
  seconds-to-minutes, where Euler's bounded, dt-convergent drift is negligible.
- **The KDK-Verlet-as-primary-orbital-integrator commitment is dropped.** One candidate
  committed leapfrog KDK for the force-integrated orbital path in an early stage. A lens
  showed this is incoherent: KDK's symplectic energy-boundedness holds only for conservative,
  velocity-independent forces, but the force-integrated regime is *by definition* where
  non-conservative thrust and drag live — exactly where KDK gives no benefit and is not
  symplectic — and that candidate self-flagged an Euler+KDK double-integration hazard on the
  shared linear half. So it buys integrator complexity and a double-integration risk for a
  benefit its own on-rails architecture makes moot. If gameplay later needs *genuine* long
  force-integrated n-body arcs (emergent Lagrange points, resonances), a bounded leapfrog KDK
  region is added THEN, as a separate integrator, with tests that assert convergence and
  boundedness — never exact conservation (§4.5). The shipped Euler is not silently reused for
  that; it is inadequate and the doc says so.

The orbital deep-dive labels leapfrog "energy-conserving" (VERIFIED `:235`). That is wrong —
it is energy-**bounded**. Asserting exact conservation would repeat Stage 1's fantasy-
tolerance mistake (§4.5).

### 4.3 Solver robustness (do not inherit the deep-dive's gaps)

The deep-dive seeds Kepler's equation `E = M`, which fails to converge at `e > ~0.8`. Use:

- A robust starter: `E0 = M + e·sin M` (or a Markley/Danby cubic initializer).
- Newton iteration with a **hard iteration cap** and a **bisection fallback**.
- A **universal-variable formulation** near `e = 1` so parabolic and hyperbolic arcs do not
  hit the `e→1` singularity. This is required, not optional: a warp/slingshot ejection or a
  hyperbolic flyby produces `e ≥ 1` state that a seed-only elliptic solver cannot represent.
  (Two candidates included universal variables; the third did not — this synthesis takes the
  version that does.)

### 4.4 SOI transition — a frame rebase, continuous in position AND velocity

The on-rails↔force handoff writes the SAME shared carriers — `Transform.position` (`Vec3d`)
and `RigidBody.linearVelocity` (`Vec3d`, VERIFIED `components.h:154`) — so it is continuous
by construction:

- **on-rails → force:** evaluate `(pos, vel)` from elements at coordinate time `t`, seed the
  two fields, begin integrating.
- **force → on-rails:** `StateVectorToElements(position, linearVelocity, mu)`, then propagate
  analytically.

The genuine discontinuity is the **frame change** at the SOI crossing: position and velocity
must be re-expressed relative to the NEW primary (heliocentric↔planetocentric) using the same
`mu` and origin on both sides of the instant — `v_planetocentric = v_heliocentric − v_planet`.
In the hierarchical-frame architecture **an SOI IS a frame**, so an SOI crossing is exactly a
frame rebase (§1.5). One mechanism serves the coordinate architecture, patched conics,
atmosphere entry (§6), and FTL (§7). Continuity breaks only if the wrong `mu` or origin is
used — which the shared-carrier + matching-`mu` construction prevents.

### 4.5 Verification honesty

Do NOT assert exact energy/momentum conservation — a tolerance the method cannot meet. Assert:

- **Kepler closure:** the orbit returns to its start over one period to the tolerance the
  analytic propagator actually achieves.
- **dt-convergence** of any *integrated* drift: halving `dt` halves the drift.

---

## 5. Gravity — one owner per body per step

### 5.1 The invariant (must be ADDED — nothing enforces it today)

**EXACTLY ONE OWNER PER BODY PER STEP.** A body is moved by EITHER an on-rails analytic write
OR force-integration, never both in the same step. Violating it applies the primary's pull
twice, because an on-rails Kepler orbit **already is** the analytic two-body gravity solution.

An owner token:

```
OrbitState {
    enum { OnRails, Forced, Warp } owner;
    OrbitalElements elements;   // valid when OnRails
    FrameId         primary;    // the frame whose mu this body orbits
}
```

A debug assertion verifies no body is stepped by both movers in one frame. This also enforces
the older two-mover hazard: an entity must never carry both the legacy `Vec3f Velocity`
(VERIFIED `components.h:38`, the kinematic mover) and `RigidBody`, and never be simultaneously
on-rails and force-integrated.

### 5.2 Ownership rules

- **On-rails** owns a body by WRITING `Transform.position` and `RigidBody.linearVelocity`
  directly from `KeplerPosition/Velocity(elements, t)`. The integrator MUST NOT run for it,
  and it receives NO gravitational `externalForce` from its primary (the pull is already in
  the ellipse).
- **Force-integrated** owns a body by running `IntegrateRigidBody` with the primary's point-
  mass gravity passed as `externalForce` — the exact per-call hook the header documents
  (VERIFIED `rigid_body.h:73`, "e.g. gravity, later"; not stored, not double-cleared). Its
  position is touched ONLY through the integrator. Thrust (`forceAccum`, VERIFIED
  `components.h:172`), atmosphere, and third-body perturbations are added here and only here.

### 5.3 Transition trigger

Thrust feeds `forceAccum`, which only the force-integrated path consumes. So the instant a
thruster fires, an on-rails body MUST first convert (state-vector → seed the two fields →
begin integrating). Coasting = on-rails; maneuvering = force-integrated. Thrust cannot be
applied on rails.

### 5.4 Point-mass field and shared softening

```
g = −mu · r / (r^2 + eps^2)^(3/2)        // Plummer softening
```

`eps` is floored at `max(body physical radius, r_s + eps0)`. This **explicitly rejects** the
deep-dive's `if (dist < 1.0) continue` (VERIFIED `:262`), which zeroes gravity inside 1 m — a
discontinuity and a blow-up source.

**One shared softening constant** feeds BOTH the `1/r²` gravity force AND the
`sqrt(1 − 2GM/(r c²))` GR clock term (§2), so the two subsystems agree on exactly where the
`1/r` singularity is bounded. `G = 6.674e-11`, one canonical value. Gravity is computed in the
body's frame (small coords, no cancellation).

---

## 6. Atmosphere — force-integrated flight, continuous entry

### 6.1 Density

Exponential isothermal, computed in the planet's SOI frame (small local coords):

```
h   = |localPos| − R_planet                         // altitude, floored at the surface
rho = rho0 · exp(−h / H)                             // H = scale height per body
```

The exponent is floored at deep-negative `h` to avoid overflow. At the top, density and
pressure are multiplied by a terminal smoothstep before becoming **exactly 0.0 at and above
the cutoff**. The taper, rather than the zero branch alone, makes the atmosphere-top force
boundary genuinely C0.

### 6.2 Forces (fed via the existing hooks)

- **Drag:** `F_d = −0.5 · rho · |v_rel|² · Cd · A · v̂_rel`, with
  `v_rel = v_body − v_atmosphere`, `v_atmosphere = ω_planet × r` (the atmosphere co-rotates),
  computed in the planetocentric frame.
- **Lift:** the component of the aerodynamic force perpendicular to `v_rel` from angle of
  attack — this couples orientation (`Quatf`) into translation, which is *why* atmosphere is
  force-integrated.
- **Heating:** a Sutton-Graves form `q ≈ k · sqrt(rho) · |v_rel|³` driving a **cosmetic/
  gameplay scalar** (skin temperature, damage), accumulated like proper time. It **never
  feeds the integrator**, so it cannot destabilize it.

All aerodynamic force is summed into `externalForce` (world) and aerodynamic torque (from the
center of pressure offset, `r×F`) into `externalTorque` (body) — the SAME hooks gravity uses
(VERIFIED `rigid_body.h`). No new integrator, no new pose owner. Atmospheric flight simply IS
the force-integrated regime with extra force terms.

### 6.3 The boundary, made continuous

A body leaves on-rails and enters force-integration when it descends below the atmosphere
cutoff, because drag makes the trajectory non-Keplerian. This is the SAME on-rails→force seam
as SOI entry (§4.4), one layer deeper inside the planet's frame, continuous by the identical
state-vector construction. **Force continuity is automatic**: the terminal smoothstep makes
`rho` approach exactly 0.0 from below, so drag/lift magnitude starts at zero and grows smoothly
— crossing the shell produces no force step (C0). Leaving reverses it: once `rho` reaches 0 and
no thrust is applied, the body may convert back to on-rails. Atmospheric speeds are deep in a
gravity well and sub-orbital (`beta ~ 1e-5`), so no relativistic coupling. Drag remains
dissipative through its dedicated contractive frozen-speed substep even when the ordinary
fixed timestep exceeds the local drag timescale.

---

## 7. FTL / warp / wormhole — the consistent fiction, cleanest discontinuity handling

### 7.1 The fiction

- **Alcubierre warp-bubble** for continuous FTL cruise: locally sub-c (the bubble moves
  space), so nothing inside ever locally exceeds c and no in-frame relativistic blow-up
  occurs.
- **Wormhole** for instantaneous frame/position teleport between two mouths.

Neither is "correct"; both are internally consistent and, above all, **robust** — both reduce
to the same engineering object: a well-defined discontinuity in position and reference frame
that must not corrupt retained state.

### 7.2 Warp cruise — a dedicated warp-space frame (not "frozen")

Entering warp puts the ship on a **deterministic rail** (`OrbitState::Warp`), the same
ownership model as on-rails Kepler. It enters a dedicated **warp-space frame** (its own
`FrameId`) whose origin advances along the route at the superluminal *coordinate* speed,
deterministically, at fixed `dt`. Inside the bubble the ship is at rest / sub-c relative to
the bubble frame, so all LOCAL physics (rigid body, attitude) run normally with `beta ≪ 1` —
no relativistic overflow because local velocity is small. As the ship moves, the active frame
is rebased along the route so local coords stay small and precise the whole way — **warp is
literally a moving floating origin**. Real-space gravity and atmosphere are **suspended**
during cruise (the bubble isolates the interior); the ship's real-space position is the
bubble-frame origin. So warp is "a separate warp-space frame with external forces suspended,
local sim live" — not frozen, not force-integrated against the outside.

Each fixed-step origin advance is validated before coordinate arithmetic and is bounded to at
most one sector per axis. Longer route motion is split into deterministic fixed steps; invalid,
non-positive, non-finite, or unrepresentable advances are rejected as transactional no-ops.

**Proper time during warp** is defined by convention: the bubble interior is treated as flat,
`dτ = dt` (the local `beta ≪ 1` and no local potential make this consistent with §2 anyway).
This convention is flagged for owner sign-off (§11). It does NOT contradict the dilation model:
a warp rider ages normally; a near-c *sublight* traveler ages less — different worldlines, both
consistent.

### 7.3 The atomic teleport — the complete retained-state set (grafted from Design 1)

Wormhole teleport and warp enter/exit are ONE atomic operation over the full retained-state
set. This is the single most complete piece grafted from the second-ranked design, whose
teleport state analysis a lens called the most thorough of the three:

```
{ Transform.position, Transform.rotation,
  RigidBody.linearVelocity, RigidBody.angularVelocity,
  RigidBody.forceAccum, RigidBody.torqueAccum,
  RigidBody.prevPosition, RigidBody.prevRotation }
  PLUS RelativisticBody.momentum when that component is present
  PLUS OrbitState.owner and FrameId
```

The rigid-body fields are VERIFIED present in `components.h:149-181`; the authoritative
relativistic momentum is present in `RelativisticBody`. The pure FTL module uses `WorldPos` and
double vectors, so the future engine call site must explicitly adapt ECS local `Vec3d` +
`FrameId` into `WorldPos` and widen the ECS float angular/torque fields. Step by step:

1. **Runs INSIDE the RULE-6 fixed-step `UpdateSystems` path** (VERIFIED `app.cpp:1047-1048`),
   NEVER the variable-rate camera/render path — else the transit lands at a wall-clock-
   dependent sub-frame point and breaks replay/save-load.
2. **Position:** set to the destination mouth, expressed in the destination frame; then
   **rebase the active origin** so the destination sits near a local origin (far-jump
   precision; raw far-absolute `Vec3d` is out of spec, §1).
3. **linearVelocity** and **RelativisticBody.momentum** are WORLD/frame `Vec3d` values: **rotate
   both by the mouth-to-mouth rotation.** A wormhole that only rotates the mouth STILL must rotate
   both values — they are expressed in the frame the mouth rotation changes (the easy-to-miss
   hazard). Rotate the orientation quaternion by the same mouth rotation. `angularVelocity` is
   body-frame, so its co-rotating components remain unchanged even through an oriented mouth.
4. **forceAccum / torqueAccum:** FLUSH to zero — staged pre-jump wrench must not be consumed
   on the first post-jump step (VERIFIED they are consumed then zeroed, `rigid_body.cpp:125,
   164-165`) in the new frame.
5. **prevPosition / prevRotation:** set to the POST-teleport pose in the same step, so render
   interpolation's delta across the jump is zero (no source-to-destination smear). These are
   VERIFIED reserved-but-unread by the sim (`components.h:175-180`).
6. **Path-trace:** force `m_accumFrameIndex = 0` (the on-screen image changed, VERIFIED the
   reset at `:859`); do NOT reset `m_seedFrameCounter` (VERIFIED never-reset by design, `.h:281`
   — resetting it would re-lock every post-jump frame to one RNG sequence).
7. **DRAIN the accumulator** if the transition burned wall-clock (AS rebuild, rebase) using the
   existing pattern `while (ConsumeFixedStep()) {}` (VERIFIED `app.cpp:1033,1042`), so arrival
   does not replay a burst of steps. `kMaxDt` is a backstop, not a substitute.

Before any field is changed, validate canonical/addressable coordinates, finite retained state,
and a finite non-zero mouth quaternion. Normalize finite non-unit mouth rotations once at the
boundary. Any rejected operation returns the complete pre-jump state unchanged; it must not flush
the wrench or rewrite interpolation history on a partial failure.

### 7.4 Determinism across the jump

The teleport is a pure function of `{pre-jump state, mouth-pair transform}` — no wall-clock, no
RNG. **Round-trip invariant:** A→B→A with identity mouth rotations at sector origins returns
bit-exact position and retained vectors. Rotated mouths are tolerance-bounded by float-quaternion
and sector-offset ULPs, not bit-exact. This is well-defined ONLY because each world vector's
*frame* is saved alongside it (§8).

---

## 8. Save/load and determinism

### 8.1 Full save state

Components are POD, fixed-capacity, direct-memcpy (VERIFIED the pattern, `components.h:202-208`
`ThrusterSet`). Per object:

- `Transform.position` (`Vec3d`) — **meaningless without its `FrameId`**, the load-bearing new
  save item.
- `Transform.rotation` (`Quatf`).
- `RigidBody.linearVelocity` (world/frame `Vec3d`) — equally frame-dependent; **its frame is
  saved with it**, or a round-trip is not well-defined. Plus `angularVelocity`, `invMass`,
  `invInertiaDiag`. (`forceAccum`/`torqueAccum` are per-step transients, VERIFIED zeroed each
  step; saving zero is harmless.)
- `Clock { properTimeDeficit }`.
- `OrbitState { owner, elements, primary FrameId }`.
- Atmosphere heating scalar (if gameplay-affecting).

Global:

- The **FrameGraph**: every frame's `{ parent, originInParent (Vec3d), orientationInParent
  (Quatd), mu, epoch }`.
- The designated **master frame** id per context (§2.3).
- Warp-space frame state if mid-cruise.
- The coordinate-time epoch (`totalTime`), so on-rails Kepler propagation and the proper-time
  deficit (both functions of `t`) reproduce exactly.

`FrameId`, `Clock`, `OrbitState` are appended fields/components exactly as `prevPosition` was
pre-reserved (VERIFIED the pre-reservation pattern, `components.h:175-180`).

### 8.2 Not saved — regenerate deterministically

`m_accumFrameIndex`, `m_seedFrameCounter`, the timer accumulator, `sceneSignature`,
`prevPosition/prevRotation` (pure render transients, VERIFIED not read by the sim).

### 8.3 Determinism

**Same-binary determinism is the GOAL.** The step consumes only `m_fixedDt` (VERIFIED never
the wall-clock `timeStep.dt` in the fixed loop, `app.cpp:1048`), no RNG in the step; smoke mode
(VERIFIED frame-driven `UpdateSystems`, `app.cpp:1043`) is the existing deterministic pattern
to generalize into a save/replay harness. On-rails Kepler is analytic, which shrinks the
float-summation surface.

**Cross-machine float determinism is a NAMED NON-GOAL** under hierarchical/floating-origin
storage, because float addition is non-associative and summation order varies with
compiler/FPU. The ONLY way to make coordinate addition exact and associative — and thus
cross-machine deterministic — is int64 fixed-point storage (§1.1), which this design rejects as
primary storage. Cross-machine determinism is explicitly out of scope unless fixed-point is
later adopted (§11).

---

## 9. Staged plan — coordinate decision FIRST, fiction LAST

Each stage is independently buildable and testable. Ordered so the load-bearing coordinate
decision is validated before anything depends on it and the fiction lands only on a proven
foundation. Every gate is an analytic invariant + assertion + negative control (§10).

**Stage 0 — Coordinate architecture, validated FIRST (pure math, no render, no physics).**
`FrameId` + FrameGraph + `Quatd` + generalized `ToActiveFrameRelativeMatrix` (compose up the
chain in double, subtract camera in double, narrow the residual). Floating-origin single-frame
degenerate case first, then multi-frame. GATE: replicate the `test_math.cpp:613` negative
control at EVERY new narrowing site — naive narrow LOSES the sub-metre offset, subtract-first
preserves it — at 1e7 m, 1 AU, and a rebased far origin; composition across 5 frame levels
stays sub-mm within a star system; shadow snap anchored to the active-frame origin stays
bit-exact while in-frame. Nothing else starts until this is green.

**Stage 1 — Proper-time sidecar (cosmetic).** `Clock` component; SR·GR weak-field factor with
`beta` measured against the master frame (§2.3); deficit accumulator. NO dynamics change. GATE:
both factors finite and in `(0,1]` every step; at `v_master=0, r=∞` the factor is `1.0`
bit-exact and the deficit increment is exactly `0.0`; a twin round-trip's proper time is
path-dependent and `<` coordinate time.

**Stage 2 — Relativistic momentum-space dynamics.** `p += F·dt`; `v = p/sqrt(m²+(|p|/c)²)` as a
thin adapter around the shipped linear half. Coordinate with the concurrent Flight Stage 2
wiring. GATE: `|v| < c` for arbitrarily large `p` (no NaN); Newtonian and relativistic agree to
bits below `beta 0.01`; the naive γ form overflows on the same input (watched).

**Stage 3 — Gravity + one-owner invariant + force-integrated regime.** Softened point-mass
gravity as `externalForce` (shared Plummer softening); `OrbitState` owner token; debug
assertion that no body is stepped twice. GATE: force free-fall matches the analytic two-body
path; deliberately double-applying gravity to an on-rails body is watched diverging.

**Stage 4 — On-rails patched-conics + SOI transitions.** `OrbitalElements`,
`KeplerPosition/Velocity` with a robust seed + universal variables, `StateVectorToElements`;
SOI handoff as a frame rebase. GATE: Kepler closure over one period to the tolerance the
propagator meets; dt-convergence of integrated drift; state-vector continuity in position AND
velocity across an SOI crossing; solver converges at `e=0.9` where `E=M` fails (watched); a
hyperbolic `e>1` arc is representable.

**Stage 5 — Atmosphere.** Clamped exponential density, drag/lift as `externalForce`/
`externalTorque`, cosmetic heating; on-rails→force boundary at the cutoff. GATE: steady descent
reaches analytic terminal velocity; drag is dissipative; `rho` ramps from exactly 0.0 (C0
entry, no force step).

**Stage 6 — FTL / warp / wormhole (LAST).** Atomic teleport over the full retained-state set +
warp-space frame + accumulator drain + accum reset. Reuses Stage 0 rebase, Stage 4 rail
ownership, the fixed-step drain. GATE: identity-mouth round-trip returns bit-exact retained state;
rotated round-trip remains within quaternion/coordinate tolerances; teleport applied only inside
the fixed-step path; `prevPose == postPose` (zero smear); invalid transforms are transactional;
no physics burst on arrival.

**Stage 7 — Save/load + deterministic replay.** Frame identity + velocity's frame + clocks +
orbital state + FrameGraph + master-frame id + epoch. GATE: `load(save(state)) == state`;
same-binary replay from a save is hash-identical under the smoke frame-driven path; dropping
`FrameId` (or velocity's frame) makes a far body's position/velocity wrong (watched).

Frame identity is threaded into the save *schema* from Stage 0 even before it is populated, so
no save format has to migrate later.

---

## 10. Robustness verification strategy (the most important section)

For each subsystem: the analytic **invariant**, the **assertion** (what the test checks), and
the **negative control** (a deliberate break that must be watched failing — an assertion nobody
has seen fail is not evidence). Every NaN/precision/regime-transition case is named and bounded.

**Coordinates / frames.**
Invariant: subtract-in-double-then-narrow preserves sub-metre offsets at any scale.
Assertion: `(obj − camera).ToFloat()` recovers the offset to `1e-6` at 1e7 m, 1 AU, and a
rebased far origin, driven through the real matrix builder; 5-level composition stays sub-mm.
Negative control: the naive `obj.ToFloat() == camera.ToFloat()` LOSES the offset (the shipped
`test_math.cpp:613` pattern, replicated at every new narrowing site, or the regression is
silent).

**Time dilation.**
Invariant: `dτ ≤ dt` always; each factor in `(0,1]`.
Assertion: at `v_master=0, r=∞` the factor is `1.0` bit-exact and the deficit increment is
exactly `0.0` (cheap, always-on); both factors finite each step; a twin round-trip's proper
time is path-dependent and `<` coordinate time.
Negative controls: (a) at `beta` near 1 the factor is `≪1` and the deficit accumulates — a
factor `>1` or NaN is a bug. (b) **Stub the dilation factor to a constant `1.0` and assert the
twin round-trip test FAILS** — this proves the dilation code is actually *executed*, not that a
constant happens to pass a bounds check. (This stronger reachability control was missing from
all three candidates and is added here.)
Bounds: `beta` clamp before sqrt; `1−beta²` as `(1−beta)(1+beta)`; series near 0; `r` floored
at `max(radius, r_s+eps)`; non-finite factor → `deficit += 0`.

**Relativistic dynamics.**
Invariant: `|v| < c` for all finite `p` (structural, from `v = p/sqrt(m²+(|p|/c)²)`); `a → F/m`
as `beta → 0`.
Assertion: below `beta 0.01`, trajectory bit-identical to Newtonian within float tolerance;
`|a_rel − a_newton|/|a_newton| < 5e-5`.
Negative controls: feed an enormous force many steps — `v` asymptotes to c, never NaN, never
`≥c`; the naive γ-denominator form fed the same input overflows (watched, proving momentum-space
is the robust choice). At `beta=0.9`, dynamics differs measurably from Newtonian, proving the
relativistic term is really evaluated.

**Orbital.**
Invariant: a Kepler orbit closes over one period; energy `½v²−μ/r` and `r×v` are bounded.
Assertion: position returns to start within the tolerance the analytic propagator meets.
Negative controls: halving `dt` halves force-integrated drift (dt-convergence — NOT an exact-
conservation tolerance, which would repeat Stage 1's fantasy); the `E=M` seed fails to converge
at `e=0.85` while the robust seed succeeds (watched, proving the seed is load-bearing).

**Gravity / ownership.**
Invariant: exactly one mover per body per step.
Assertion: force free-fall matches the analytic two-body trajectory.
Negative control: deliberately double-apply the primary's gravity to an on-rails body — watched
diverging the orbit, proving the one-owner guard fires; softened force at `r→0` is finite
(assert no `if dist<1 continue` zeroing).

**Atmosphere.**
Invariant: drag removes kinetic energy (dissipative); terminal velocity where drag balances
gravity.
Assertion: steady descent converges to the analytic terminal velocity.
Negative control: `rho` at high altitude → 0 so drag → 0 — crossing the shell produces no force
step (C0 continuous entry); `exp` density does not overflow at deep-negative `h` nor produce
denormals above the cutoff (clamped to exact 0.0).

**FTL / wormhole.**
Invariant: an identity-mouth round-trip returns exact retained state; rotated mouths are bounded
by the documented float-quaternion and WorldPos tolerances.
Assertion: after out-and-back, `{position, linearVelocity, momentum, rotation}` meet that bound and
`prevPosition == post-teleport pose`; `forceAccum/torqueAccum` flushed; accumulator drained;
`m_seedFrameCounter` unchanged.
Negative controls: a jump that leaves `prevPosition` at the pre-jump pose yields a non-zero
interpolation delta (visible smear); a jump that skips rotating world-frame `linearVelocity`
yields a velocity discontinuity; one that skips authoritative momentum makes the next recovered
velocity disagree; non-finite or zero mouth transforms do not partially mutate state; a teleport
applied in the variable-rate path diverges under replay — all watched failing.

**Save/load / determinism.**
Invariant: `load(save(state)) == state`; same inputs → same bytes (same binary).
Assertion: same-binary replay from a save is hash-identical under the smoke frame-driven
stepping.
Negative controls: dropping `FrameId` (or velocity's frame) makes a far body's absolute
position/velocity wrong (watched, proving frame identity is load-bearing); injecting wall-clock
or RNG into the step breaks the replay hash.

**Named blow-ups and their bounds (consolidated).**

| Blow-up | Where | Bound |
|---|---|---|
| `γ → ∞`/NaN near c | dynamics | AVOIDED structurally: momentum recovery `v=p/sqrt(m²+(\|p\|/c)²)`, `\|v\|<c` for all finite `p`. Clock's `sqrt(1−β²)`: clamp `β` before sqrt (defense-in-depth). |
| `1−β²` cancellation near c | clock | compute `(1−β)(1+β)`; series `1−β²/2` near 0; accumulate the deficit, not `τ` |
| `1/r²` gravity at `r→0` | gravity | shared Plummer `−μr/(r²+ε²)^1.5`, `ε` floored at `max(radius, r_s+ε0)`; reject `if dist<1 continue` |
| GR `sqrt(1−2GM/rc²)` imaginary for `r<r_s` | clock | same softened `r` floor; weak-field valid only `r≫r_s`; assert real, in `(0,1]` |
| Kepler solver divergence `e>0.8` | orbital | robust seed + capped Newton + bisection + universal variables for `e→1` |
| Galaxy-scale coordinate blow-up | coordinates | hierarchical frames, local coords `<1e13 m`; never store/compute in far-absolute doubles; interstellar origins sub-framed with accepted ~2 m map imprecision (§1.6) |
| Catastrophic cancellation of separations | coordinates | every separation/force/relative-velocity computed WITHIN one frame |
| Frame-dependent `β` (SR incoherence) | time | compose velocity to the master frame before `β` (§2.3) |
| Single-primary GR truncation | time | labeled as approximation; sum over master-frame bodies if ever needed (§2.4) |
| FTL state corruption | FTL | atomic teleport over the full retained set, accumulator drain, `accumFrameIndex=0`, `prevPose=postPose`, `seedFrameCounter` preserved |
| NaN/non-finite `dt` or factor | all | house guard `!(dt>0)\|\|!isfinite(dt)` → no-op (VERIFIED `rigid_body.cpp:118`); non-finite factor → defined no-op |
| Aerodynamic heating `ρ·\|v\|³` overflow | atmosphere | floor `ρ`, use a scaled finite norm and log-domain finite saturation, reject non-finite `v`; heating never feeds the integrator |
| Collision subdivision runaway / swept tunneling | close encounters | global power-of-two subdivision is hard-capped at level 16; larger demand returns a level-17 saturation sentinel and sets `hitDepthCap`. Invalid divisors/response parameters are a no-op. Swept endpoint crossing uses a central fallback impulse rather than accepting a post-crossing "separating" state. |

Note: the path-trace history NaN/Inf guard self-heals a corrupt jump in one frame but fails
**silently** — robustness tests must check the accumulator/positions directly, never wait for a
visible NaN.

---

## 11. Risks and open questions (honest)

**Resolved-but-load-bearing:**

- **Frame-invariant `β` (§2.3)** and the **GR single-primary truncation label (§2.4)** were the
  deepest shared coherence holes across all three candidate designs (a lens rated them the
  fatal flaw common to every design). They are resolved here by composing velocity to a
  designated master frame and by explicitly labeling the potential truncation. This resolution
  is the single most important correction this synthesis makes over its inputs, and it is
  **unverified against a running sim** — it is a design decision that must be tested at Stage 1.

**Risks:**

- **Cross-machine float determinism is a stated non-goal.** If networking later needs it, only
  int64 fixed-point coordinate storage delivers it — a `Vec3d → Vec3i64` migration touching
  every integrator and force step. Decide early whether same-binary is enough; retrofitting is
  costly.
- **Flight Stage 2 is a moving target.** It is concurrently wiring `IntegrateRigidBody` into the
  scene loop. The one-owner invariant, the gravity `externalForce` hook, and the momentum-space
  adapter must be reconciled with exactly how Stage 2 lands the wiring and which mover the scene
  calls. Coordinate before Stage 3.
- **`Quatd` must be added** to `core/types.h` (VERIFIED absent). Additive, but a new type the
  coordinate architecture forces. If frames end up translation-only, it is cheap insurance.
- **Shadow-snap reseat under rebase** needs empirical validation that the one-frame lattice
  reseat is imperceptible; alternative is anchoring to a coarser, rarely-changing frame.
- **Path-trace reset on rebase** must distinguish a bit-identical rebase (keep accumulating)
  from a rounding rebase (force reset). Getting the double-precision comparison wrong blends
  stale screen-space history silently; tests must inspect `m_accumFrameIndex` and positions
  directly.

**Open questions (could not be resolved from the code alone):**

- **Exactly where interstellar sub-frames are placed** inside a ~1 ly star frame, and whether
  the accepted ~2 m map-placement imprecision (§1.6) is acceptable to the owner, or whether a
  finer sector grid is wanted.
- **Whether frames need rotation at all.** Translation-only frames avoid `Quatd` composition
  entirely. Prefer them unless a use case (a rotating reference frame, a spun station's local
  frame) forces rotation.
- **Warp-space cruise semantics** — whether a ship can be intercepted or affected mid-warp.
  "External forces suspended, local sim live" and "`dτ = dt` by fiat" are design choices, not
  derivations. Owner sign-off required.
- **Cosmetic vs gameplay dilation** — ship cosmetic first, gate gameplay-affecting differential
  aging behind an explicit switch. Differential *simulation rate* (a dilated ship ticking
  slower) stays out of scope; confirm that boundary with the owner.
- **Whether a genuine long-arc n-body integrator is ever needed.** If emergent Lagrange
  points/resonances become gameplay, a bounded leapfrog KDK region is added then (tests assert
  convergence/boundedness, never conservation). The shipped Euler is inadequate for that and
  must not be silently reused.
- **`scene.cpp:331` TLAS rebuild and the DXR camera-relative rebuild** were cited by candidates
  but not re-read here (ASSUMED). Confirm the rebase is as cheap for RT as claimed before Stage
  0 depends on it.

---

## DECISION REVISION (owner directive, supersedes §4/§5 orbital ownership)

The owner directed: use the more accurate physics like Children of a Dead Earth,
take no shortcuts. This changes the orbital core from patched-conics-default to
**N-body-default within the active system**, with a scale-aware hybrid so it stays
tractable across a galaxy.

### The N-body / on-rails hybrid by scale

- **Active gravitational system (where the player is): FULL N-BODY GRAVITY.** Every
  dynamic body receives the summed Newtonian gravity of the active massive bodies.
  Massive contributors are integrated pairwise; ships, stations, missiles, and
  debris are test particles unless their configured mass crosses an explicit
  contribution threshold. Evaluate each massive pair once and apply equal/opposite
  impulses in stable-ID order so momentum symmetry and deterministic summation are
  designed in. A **Forest-Ruth 4th-order symplectic integrator** is the baseline
  candidate for the fixed-step conservative gravity subsystem (the CoDE choice;
  `PHYSICS_RESEARCH_REFERENCE.md` §2/§11). It is not automatically the integrator
  for thrust, drag, collision impulses, or other nonconservative forces. A system
  with O(100) massive contributors requires roughly O(10^4) pair evaluations per
  substep, which is a reasonable starting budget but must be profiled. Softening
  uses the shared policy already specified in §5.
- **Inactive / distant systems: ON-RAILS KEPLER for LOD.** A planet in a system
  the player is not in cannot be meaningfully perturbed by the player, and its
  siblings' perturbations are cosmetic at that distance, so it propagates on
  analytic Kepler rails around its primary. This is a performance LOD, not a
  physics claim, and a system is promoted to full N-body when the player enters
  its SOI/frame. The promotion must be CONTINUOUS: seed the N-body state from the
  on-rails state vector (r, v) at the instant of promotion, and demote by fitting
  osculating elements from the current (r, v).
- **Interstellar: static catalog positions.** Stellar mutual gravity is irrelevant
  on gameplay timescales; stars are fixed in their sector frame. This is where the
  unsolved interstellar-precision problem (§11 / open problems) lives, unchanged.

### What this changes from the patched-conics design

- The "on-rails XOR force-integrated, exactly one owner per body per step"
  invariant (§5) is REPLACED by a level-of-detail state: a body is either
  N-BODY-ACTIVE or ON-RAILS, still exactly one per step, still debug-asserted, but
  the default flips to N-body in the active system. The double-count negative
  control still applies (an on-rails body must receive no separate primary-gravity
  force).
- Drift: N-body symplectic integration DOES drift (bounded, energy-oscillating),
  unlike analytic on-rails which cannot. This is accepted for the accuracy it buys,
  per CoDE. The verification changes accordingly and this is load-bearing: do NOT
  assert exact energy/momentum conservation over the N-body arc (the Stage-1
  fantasy-tolerance lesson). Assert instead: (a) bounded energy OSCILLATION with no
  secular trend over a long run (the defining property of a symplectic method), (b)
  a two-body N-body case CLOSES on the analytic Kepler orbit over one period to the
  integrator's actual tolerance, and (c) dt-CONVERGENCE of the drift. The Forest-Ruth
  coefficients themselves get a unit test against their published values.
- Integrator domain: the bounded-energy claim applies only to an isolated,
  conservative, fixed-step gravity test. Player ships under thrust, atmospheric
  drag, collisions, scripted forces, or relativistic adapters do not conserve that
  Hamiltonian. Apply those forces through an explicit operator split or a separate
  integrator, and never use total-energy conservation as their acceptance test.
- Close encounters: fixed-step symplectic methods lose accuracy when a pair becomes
  too close. Detect encounters from physical radii and a timestep-derived approach
  bound, then route the pair through a documented collision or high-accuracy hybrid
  path. Plummer softening is not permission to integrate through a collision.
- Time acceleration: changing a symplectic timestep ad hoc changes the modified
  Hamiltonian and can introduce secular error. Time-warp modes need deterministic,
  block-synchronized step schedules with convergence gates; 60 Hz real-time cost
  alone does not establish time-warp accuracy.
- Determinism: N-body is deterministic under fixed dt and fixed body iteration
  order (RULE 6). Same-binary determinism is a goal; the force-summation order must
  be fixed (sort by a stable body id) so floating-point non-associativity does not
  make it depend on iteration accident.
- Cost: the active-system force sum is O(N^2) in significant bodies. At O(100)
  bodies this is ~10^4 pairwise force evals per substep, negligible. A
  Barnes-Hut/tree upgrade is only needed if significant-body count ever reaches
  thousands, which it will not for a hand-authored system; noted, not built.

### Production close-encounter policy

`sim/collision.{h,cpp}` makes the close-encounter route concrete without changing
the conservative `StepNBody` leaf. It chooses one global power-of-two subdivision
level for the active set, runs the unmodified integrator at each micro-step, and
resolves swept sphere contacts at every boundary. True surface `radius` is stored
separately from Plummer `softening`; force regularization never becomes collision
geometry by accident.

The work budget is explicit. Level 16 (65,536 leaves) is the absolute public cap;
unrepresentable or larger finite demand returns the level-17 sentinel and the
step reports `hitDepthCap`. A caller may request a lower cap, including a negative
value normalized to level zero. Non-finite/non-positive subdivision denominators,
invalid contact scales, response coefficients outside `[0,1]`, duplicate stable
IDs, non-positive softening, invalid particle state, or non-positive solver passes
reject the operation as a no-op. Merge reduction is staged and rejects aggregate
overflow atomically. This keeps hostile authoring data from turning a fixed
simulation step into undefined integer conversion, partial topology changes, or
unbounded work.

Shallow approaching contacts use a central mass-fraction impulse. A fully swept
outside-to-outside crossing uses the opposite endpoint line-of-centres direction,
so it cannot be dismissed as already separating and the fallback impulse remains
central. Deep overlap or effectively zero restitution merges the connected contact
component in ascending stable-ID order, conserving `Σ μv` to floating-point
tolerance and combining radius by volume. ECS destruction, survivor rigid-body
spin, relativistic momentum reconciliation, and the production active-system
callsite remain a separate integration lane.

### What this does NOT change

The coordinate architecture (§1, hierarchical frames + camera-relative narrowing),
time and dilation (§2), relativistic momentum-space dynamics (§3), atmosphere (§6),
FTL/warp/wormhole frame handling (§7), and save/load (§8) are unchanged. N-body
makes the coordinate/precision story MORE important, not less, because every body's
force depends on relative positions computed WITHIN a single frame — the
catastrophic-cancellation concern of §1 now applies to every gravity pair.

Stage 0 (coordinate validation) remains the first thing built and is unaffected by
this revision. The orbital stage that follows is now N-body-first per this note.
This also supersedes the Stage 4 label in §9: Stage 3 establishes the pairwise
gravity kernel and ownership guard; revised Stage 4 adds active-system N-body,
inactive-system Kepler LOD, promotion/demotion continuity, close-encounter routing,
and time-warp convergence.
