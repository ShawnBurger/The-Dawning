# Physics research reference — implementation notes for the sim

Companion to `RELATIVISTIC_SIM_ARCHITECTURE.md`. Produced by a deep-research pass
(5 search angles, 29 authoritative sources, 117 extracted claims). It confirms
most of the architecture's locked decisions and surfaces one real tension worth
reconsidering (patched-conics vs N-body).

## Epistemic status — read this first

The research harness completed **search, fetch, and claim-extraction** but its
**adversarial verification and synthesis did not run** — the session usage limit
was hit mid-verification (resets 8 pm CDT). So confidence is tagged per item:

- **[VERIFIED 3-0]** — survived the harness's three-vote adversarial check. Exactly
  one claim reached this: the Markley Kepler solver.
- **[SOURCED]** — a direct quote from a named authoritative source (NASA NTRS,
  MNRAS, arXiv, *Class. Quantum Grav.*, GDC, or a shipped-game postmortem) AND
  standard textbook physics. High confidence, but not adversarially challenged.
- **[DISPUTED]** — the sources themselves disagree; presented as a live dispute,
  never as settled. All the warp positive-energy claims are here.

The raw gathered material (all 117 claims with quotes and sources) is preserved in
`PHYSICS_RESEARCH_MATERIAL.md`. The harness can be resumed to complete verification
(`resumeFromRunId` on the deep-research workflow) once the limit resets.

---

## 1. Orbital mechanics — Kepler solvers and patched conics

**Kepler's equation across eccentricity** is the load-bearing numerical primitive.
The elliptic form `M = E − e·sin E` and hyperbolic `M = e·sinh E − E`.

- **[VERIFIED 3-0]** Markley (NASA, 1995) solves the elliptic range `0 ≤ e < 1`
  **non-iteratively** — a fifth-order refinement of a cubic starter, four
  transcendental evals total (sqrt, cbrt, two trig), reaching machine precision in
  one pass. Source: ntrs.nasa.gov/citations/19950021346. This is the recommended
  elliptic solver: fixed cost, no iterate-to-tolerance.
- **[SOURCED]** Naive `M = E − e·sin E` suffers **catastrophic cancellation** for
  `e > 0.75, E < 45°` (errors > 5e-14). The fix Markley gives: reformulate the
  derivative as `f'(E) = 1 − e + 2e·sin²(E/2)` and use a Padé approximant for `M*`
  in that region → error < 4e-16 across the range. This is the concrete stable form.
- **[SOURCED]** For **near-parabolic (e→1) and hyperbolic (e>1)**, use the
  **universal-variable formulation** (single anomaly, Sundman `ds/dt = 1/r`), which
  classical `e`-specific solvers cannot handle — Newton on eccentric anomaly "may
  become unusably sluggish, or fail to converge, for e ≥ 1". Wisdom & Hernandez
  (MNRAS 2015) give a Stumpff-free universal solver using half-angle formulas,
  ~2× faster and ~6.5× more accurate than the Stumpff reference, with a
  **Newton → Laguerre-Conway → recursive-subdivision** fallback ladder.
- **[SOURCED]** The (e→1, M→0) critical region: Markley has exactly one singularity
  at `e=1 ∧ M=0`, harmless because `E=0` when `M=0` (no solve needed). This is the
  region that defeats some iterative solvers.

**Patched conics / sphere-of-influence:**
- **[SOURCED]** Reduces the n-body problem to a sequence of two-body problems, each
  a closed-form conic — the basis for on-rails propagation that *cannot drift*.
- **[SOURCED]** Body selection is by SOI membership; **continuity at an SOI boundary
  is enforced by matching state across the patch, `r₋ = r₊, v₋ = v₊`** — exactly the
  continuity requirement the architecture specified.
- **[SOURCED]** Known limits: patched conics "does not model Lagrangian points" and
  is insufficiently accurate for some missions.

→ **Confirms** the architecture's Kepler-robustness and SOI-continuity decisions.
See the tension in §11 on patched-conics vs N-body.

## 2. Integrators and conservation

- **[SOURCED]** Children of a Dead Earth — the most physically-serious orbital game
  — propagates with a **fourth-order symplectic (Forest–Ruth)** integrator, chosen
  for energy/momentum conservation over standard integrators.
- **[SOURCED]** Symplectic methods (leapfrog / velocity-Verlet / KDK, Forest–Ruth)
  bound energy error over long runs; non-symplectic (RK4) drifts secularly — the
  reason on-rails analytic Kepler is preferred for passive orbits (analytic cannot
  drift *at all*).
- The shipped engine already reflects this: `IntegrateRigidBody` is **semi-implicit
  (symplectic) Euler**, and Stage 1 found the naive **explicit gyroscopic** term
  secularly unstable (NaN in ~1e4 steps) and switched to an **implicit solve** — a
  robustness lesson the literature corroborates. The architecture reuses this
  unchanged and delegates long-term orbital stability to on-rails.

## 3. Special relativity for simulation

- **[SOURCED]** **OpenRelativity** (MIT Game Lab, MIT-licensed, AJP 2016) is the
  engineering precedent. It makes `|v| < c` **structural** by advancing velocity
  through **relativistic velocity addition** of a per-step increment — "automatically
  guarantees the player will not move faster than the speed of light." The
  architecture's **momentum-space** recovery `v = p/√(m² + (|p|/c)²)` achieves the
  same invariant in momentum space; both are valid, and the precedent confirms the
  *strategy* (make sub-c a property of the state, not a clamp).
- **[SOURCED]** Rapidity `φ = ∫ a dτ` is the natural additive variable; `v = c·tanh φ`
  structurally bounds speed. A momentum/rapidity state is what serious SR sims
  advance, not raw velocity.
- **[SOURCED]** Acceleration handled per-step as constant-velocity in a **momentarily
  co-moving inertial frame (MCRF)**; proper vs world time relate by
  `Δt_world = Δt_ship / √(1 − v²/c²)`. Constant proper acceleration cannot be
  assumed across a step — this validates accumulating proper time as a per-step
  deviation (architecture §2).

## 4. General relativity, weak field

- **[SOURCED]** Schwarzschild clock factor `t₀ = t_far·√(1 − 2GM/(rc²))` — proper
  time deep in the well vs coordinate time at infinity. Real only for `r > r_s`,
  `r_s = 2GM/c²` (breaks down at the horizon). This is exactly the GR clock the
  architecture's dilation model uses.
- **[SOURCED]** Weak-field linear approximation `T = 1 + gh/c²` when `g` ~ constant
  and `gh ≪ c²` — the cheap form for near-surface play.
- **[SOURCED]** Gravitational dilation in a well equals the SR velocity dilation for
  that well's escape speed — a useful cross-check for the implementation.
- The r→0 singularity is bounded by the **shared Plummer softening** the architecture
  specified (one constant feeding both the `1/r²` force and the GR clock).

## 5. Gravity and softening

- Covered above: Newtonian point-mass, Plummer softening, and the explicit rejection
  of discontinuous cutoffs (the deep-dive's `if(dist<1) continue`) are in the
  architecture. The research confirms the N-body-vs-analytic tradeoff (§11).

## 6. Atmospheric flight

The searches under-covered this angle (subsumed into other angles). Standard forms
the implementation will need — to be verified on the harness resume:
`q = ½ρv²`, drag `F_d = q·C_d·A`, exponential density `ρ = ρ₀·e^(−h/H)` (scale
height H), and Sutton–Graves stagnation heating `q̇ ∝ √(ρ/R_n)·v³` for reentry.
US Standard Atmosphere 1976 is the reference density model. Flagged as the thinnest
research area; do not implement from memory without the harness pass.

## 7. Coordinate precision at scale

- **[SOURCED]** float32 degrades ~linearly: ~1 m spacing at ~1e7 units, ~6 cm at
  1000 km — the ULP-growth problem quantified.
- **[SOURCED]** **Star Citizen** (CryEngine-derived) added **64-bit positioning** as
  a fundamental change; range ~1e18 units; the double-precision switch cost was
  "marginal" at runtime, and the conversion was *selective* (physics/positioning
  only; AI stayed 32-bit).
- **[SOURCED]** **Godot** emulates near-double on the GPU via a **double-single
  (two-float)** split, applied **only to the MODELVIEW translation** — rotation/scale
  stay single. This is the same "narrow only the residual, only where needed"
  principle as the engine's camera-relative rendering.
- The architecture's **hierarchical frames + camera-relative narrowing** is a third,
  compatible approach. The **interstellar precision hole it flagged is real**: double
  gives ~metre resolution at 1 ly, so a top-level galactic frame needs sub-framing
  (integer sector + double intra-sector) — the research confirms this is unsolved by
  hierarchy alone and matches how 64-bit-world games cap their extent.

## 8–10. Fictional physics — Alcubierre, wormholes, FTL

**All internally-consistent fiction, not achievable physics. The research confirms
the constraints that keep the fiction honest:**

- **[SOURCED]** Alcubierre (1994): warp bubble, ship locally sub-c, requires
  **negative energy density / exotic matter**. Bobrick & Martire (2021) give a
  general classification of physical warp drives; Natário and Van Den Broeck are
  variants.
- **[DISPUTED]** Positive-energy warp claims — **Lentz (2022)** "hyper-fast positive
  energy" solitons and **Fell & Heisenberg (2021)** "positive energy warp from
  hidden geometric structures" — are contested. The harness gathered the critiques
  alongside them (**Pfenning & Ford 1997**, "the unphysical nature of warp drive",
  and later demystification work). **Do not present a positive-energy warp drive as
  established.** For the game it does not matter: warp is fiction; what matters is
  the design labels it so.
- **[SOURCED]** Morris–Thorne (1988) traversable wormhole: needs exotic matter
  violating the energy conditions; geometry a game would model is the throat radius
  and the two mouths. Time-machine/causality implications (Thorne) follow if the
  mouths move relativistically.
- **[SOURCED]** FTL under special relativity implies **causality violation** via
  relativity of simultaneity. The standard fictional dodge is a **preferred frame**
  (a "hyperspace"/warp-space frame in which the jump is defined), which is precisely
  the architecture's warp-space frame + atomic frame-teleport. The engineering
  problem is clean frame discontinuity, not physics — as the architecture states.

## 11. The one tension worth reconsidering — patched conics vs N-body

The architecture chose **on-rails patched conics** for passive orbits, justified by
robustness: analytic propagation *cannot* drift, is deterministic, and is cheap.

The research surfaces that the **most physically-serious game, Children of a Dead
Earth, deliberately chose the opposite** — a full N-body simulation with a
Forest–Ruth symplectic integrator — and calls patched conics "much less accurate,"
citing NASA's practice of patched-conics-for-estimates, N-body-for-precision. N-body
also reproduces **Lagrange points and multi-body perturbation**, which patched
conics structurally cannot.

This is a genuine design tension, not a defect in either choice:
- **Patched conics (architecture's choice):** robust, deterministic, drift-free,
  cheap, one-owner-per-body simple; but approximate, no Lagrange points, needs
  careful SOI-transition handling.
- **N-body symplectic (CoDE's choice):** accurate, emergent Lagrange points and
  perturbations; but drifts (bounded, symplectic), more expensive, and determinism
  needs care.

**Recommendation:** keep patched conics as the *default owner* per the architecture
— its robustness properties are exactly what the owner asked for — but the design
should explicitly note N-body symplectic as the accuracy-upgrade path for the
"actively perturbed" bodies the architecture already carves out for force
integration, and Lagrange-point gameplay as a feature patched conics forecloses.
Feed this back into `RELATIVISTIC_SIM_ARCHITECTURE.md` §4.

---

## Open follow-ups when the session limit resets

1. **Resume the harness** to complete adversarial verification of the 116
   still-unverified claims (especially the disputed warp items — the verification
   is where their contested status gets pinned).
2. **Atmospheric flight (§6)** is the thinnest area — re-run that angle specifically.
3. Fold the **patched-conics-vs-N-body tension (§11)** into the architecture doc.
