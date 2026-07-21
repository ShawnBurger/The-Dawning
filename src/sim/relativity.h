#pragma once
// =============================================================================
// sim/relativity.h — Relativistic dynamics (momentum-space) + time dilation
//                    (SR velocity + GR gravitational), CPU-only.
// =============================================================================
// SIM STAGE 2. Builds ON the proven coordinate + N-body foundation and REUSES
// its shared constants and softening: this header includes sim/nbody.h and
// takes kSpeedOfLight, kGravitationalConstant and SofteningLength FROM there —
// it never redefines c or the softening. GPU-free by construction (nbody.h,
// reference_frame.h and ecs/components.h are all device-free), so it links into
// BOTH TheDawningV3 and TheDawningTests and tests/test_relativity.cpp drives the
// SHIPPED functions, not a paraphrase. sim includes ecs; ecs never includes sim
// — no cycle.
//
// This implements RELATIVISTIC_SIM_ARCHITECTURE.md §3 (momentum-space dynamics,
// singularity-free) and §2 (proper time — deviation-accumulated, SR·GR weak
// field), plus §2.3 (the frame-relative-velocity subtlety — β measured in a
// master frame). PHYSICS_RESEARCH_REFERENCE.md §3/§4 are the physics sources.
//
// -----------------------------------------------------------------------------
// WHY |v| < c IS STRUCTURAL, NOT CLAMPED (§3.1)
// -----------------------------------------------------------------------------
// The dynamical state is MOMENTUM p. Velocity is RECOVERED:
//     v = p / sqrt(m² + (|p|/c)²)          (exact inverse of p = γ m v)
// The radicand m² + (|p|/c)² is strictly positive for m > 0 and grows without
// bound with |p|, so |v| = |p| / sqrt(m² + (|p|/c)²) < |p| / (|p|/c) = c for
// EVERY finite p — a structural property of the stored state, with NO γ to blow
// up and NO clamp. γ itself is taken as sqrt(1 + (|p|/(mc))²), which never
// overflows for finite p (computed via std::hypot to dodge the intermediate
// square's overflow at absurd |p|). This is the whole reason momentum-space beat
// the velocity-space form on every review lens: robustness by construction, not
// by band-aid over a form that can NaN.
//
// -----------------------------------------------------------------------------
// THE NEWTONIAN <-> RELATIVISTIC BOUNDARY IS CONTINUOUS (§3.2)
// -----------------------------------------------------------------------------
// The momentum-space form IS the Newtonian form at low speed: p ≈ m v and
// v ≈ p/m as β → 0, and the recovery, γ, KE and the force step all reduce to
// their Newtonian limits smoothly — there is NO discontinuous regime switch.
// Below β ~ 1e-3 the relativistic and Newtonian results agree to tolerance;
// above, they diverge correctly. The Newtonian path stays valid where it is
// valid, and the relativistic corrections only matter near c or deep in a well.
// =============================================================================

#include "../core/types.h"
#include "../ecs/components.h"
#include "nbody.h"            // kSpeedOfLight, kGravitationalConstant, SofteningLength
#include "reference_frame.h"  // FrameGraph, Body, FrameId — the §2.3 master frame

#include <cstdint>

namespace sim
{

using core::Vec3d;

// =============================================================================
// 1. MOMENTUM-SPACE RELATIVISTIC DYNAMICS (§3)
// =============================================================================

// γ from momentum: γ = sqrt(1 + (|p|/(mc))²). NEVER overflows for finite p
// (unlike 1/sqrt(1−β²), which divides by zero at v=c). Computed with hypot so
// the intermediate square cannot overflow even at astronomically large |p|.
// restMass must be > 0; a non-positive mass returns 1.0 (the house-guard limit).
double GammaFromMomentum(const Vec3d& momentum, double restMass);

// Recover velocity from momentum: v = p / sqrt(m² + (|p|/c)²) = p / (m·γ). The
// STRUCTURAL sub-c guarantee lives here — |v| < c for every finite p.
Vec3d VelocityFromMomentum(const Vec3d& momentum, double restMass);

// Momentum from velocity: p = γ m v = m v / sqrt((1−β)(1+β)). The INVERSE
// direction is NOT structurally bounded (it blows up as |v| → c), so β is
// clamped to kBetaMax as defense-in-depth for this direction only (used to SEED
// p from an initial sub-c velocity, never in the dynamics path). 1−β² is formed
// as (1−β)(1+β) to stay precise near c.
Vec3d MomentumFromVelocity(const Vec3d& velocity, double restMass);

// Relativistic kinetic energy KE = (γ−1) m c², computed stably as
// |p|² / (m (γ+1)) so the (γ−1) cancellation near β=0 is avoided. Reduces to the
// Newtonian ½ m v² = |p|²/(2m) as β → 0.
double RelativisticKineticEnergy(const Vec3d& momentum, double restMass);

// Rapidity φ (additive SR convenience, §3 research note): p = m c sinh φ, so
// φ = asinh(|p|/(mc)); the speed is v = c tanh φ. Momentum stays the state —
// these are provided because rapidity is the natural additive variable, not
// because anything advances φ.
double RapidityFromMomentum(const Vec3d& momentum, double restMass);
double SpeedFromRapidity(double rapidity);

// One relativistic force step, momentum-space (§3.1, §3.3). Advances momentum by
// dp = force·dt, then recovers the new velocity (the sub-c-by-construction one).
// Returns the new momentum and writes the recovered velocity into velocityOut;
// the caller does position += velocityOut·dt (the position update is unchanged
// in form — this is a thin momentum↔velocity adapter, NOT a new integrator).
// House guard: dt <= 0 or non-finite is a no-op (momentum unchanged, velocityOut
// = the current recovered velocity), matching rigid_body.cpp / nbody.cpp.
Vec3d RelativisticMomentumStep(const Vec3d& momentum, const Vec3d& force,
                               double restMass, double dt, Vec3d& velocityOut);

// The defense-in-depth β ceiling for the v→p direction and the clock's √(1−β²).
// Dynamics never needs it (momentum recovery keeps β < 1 structurally); it only
// bounds the two places a caller could hand in β ≥ 1 (a seed velocity at/over c,
// or a clock fed a bad β). See §2.6.
inline constexpr double kBetaMax = 1.0 - 1e-12;

// =============================================================================
// 2. TIME DILATION (§2) — SR velocity + GR gravitational, both robust
// =============================================================================
// The DILATION FACTOR is dτ/dt ∈ (0, 1]. It is returned two ways:
//   * FACTOR:        the value itself (for dτ = dt·factor display / composition).
//   * FACTOR MINUS 1: the cancellation-free (factor − 1), which is what the
//                     proper-time DEVIATION accumulator needs. Forming (factor−1)
//                     by subtracting 1 from a factor ≈ 1 would lose the ~1e-9
//                     signal to cancellation; the "MinusOne" forms compute it
//                     directly (conjugate identity / series) so the residual is
//                     exact. ALWAYS feed the accumulator a *MinusOne quantity.

// ---- SR velocity dilation ----------------------------------------------------

// 1 − β², computed as (1−β)(1+β) — NEVER 1.0 − β*β, which loses digits near β=1.
// This is the load-bearing near-c precision form (§2.6).
double OneMinusBetaSq(double beta);

// SR factor dτ/dt = sqrt(1 − β²) = sqrt((1−β)(1+β)). β is clamped to kBetaMax
// defense-in-depth so the sqrt is real even if a caller hands in β ≥ 1.
double SRDilationFactor(double beta);

// (SR factor − 1), cancellation-free: sqrt(1−β²) − 1 = −β² / (1 + sqrt(1−β²)).
// Near β=0 this is the series −β²/2 − …; near β=1 it is a clean −1. This is the
// quantity the proper-time deviation accumulator consumes.
double SRDilationFactorMinusOne(double beta);

// ---- GR gravitational dilation ----------------------------------------------

// Schwarzschild radius r_s = 2μ/c² (μ = GM). Same μ the N-body force uses.
double SchwarzschildRadius(double mu);

// r floored at the SHARED softening (nbody.h SofteningLength) so the clock stays
// finite at/inside the softened radius — no horizon NaN. `softening` MUST be the
// value the caller got from sim::SofteningLength(mu, radius): gravity and the
// clock then floor r at exactly the same place (§2.6, §5.4).
double FlooredRadius(double r, double softening);

// GR factor dτ/dt = sqrt(1 − r_s/r), with r floored at `softening`. Because the
// softening floor is ≥ r_s + kSofteningBase > r_s, the radicand 1 − r_s/r_floored
// is strictly in (0, 1] — real and finite, the unsoftened horizon NaN designed
// out. Weak field (r_s ≪ r) it approaches 1.
double GRDilationFactor(double mu, double r, double softening);

// (GR factor − 1), cancellation-free: sqrt(1 − r_s/r) − 1 = −(r_s/r)/(1+sqrt(...)).
double GRDilationFactorMinusOne(double mu, double r, double softening);

// STABLE WEAK-FIELD LINEAR GR factor: 1 − GM/(r c²) = 1 − (r_s/2)/r_floored.
// Avoids the sqrt cancellation when r_s ≪ r (the near-surface / weak-field case,
// PHYSICS_RESEARCH_REFERENCE §4). Agrees with GRDilationFactor to O((r_s/r)²).
double GRDilationFactorWeak(double mu, double r, double softening);
double GRDilationFactorWeakMinusOne(double mu, double r, double softening); // = −(r_s/2)/r_floored

// ---- Composition (§2.2 weak-field product) ----------------------------------

// Combined SR·GR factor for a body both moving AND deep in a well: the PRODUCT
// of the two factors (the standard weak-field first-order combination, labeled
// as such in §2.2 — the rates multiply only to first order). Equal to
// SRDilationFactor(beta) * GRDilationFactor(mu, r, softening).
double CombinedDilationFactor(double beta, double mu, double r, double softening);

// (Combined factor − 1), cancellation-free. With a = srMinus1, b = grMinus1:
// (1+a)(1+b) − 1 = a + b + a·b — no product-then-subtract-1 cancellation.
double CombinedDilationFactorMinusOne(double beta, double mu, double r, double softening);

// ---- Proper-time deviation accumulation (§2.2) ------------------------------

// Per-step deviation dτ − dt = dt·(factor − 1). Feed it a *MinusOne quantity
// (factorMinusOne = factor − 1), NOT a factor, so the tiny residual is exact.
double ProperTimeDeviationStep(double factorMinusOne, double dt);

// Advance a clock by one fixed step: coordinateTime += dt and
// properTimeDeviation += dt·factorMinusOne. NEVER τ += dτ — the deviation is
// accumulated in isolation so it is not drowned by coordinate-time magnitude
// over a long trip (the whole point of the sidecar). House guard: dt <= 0 or
// non-finite, or a non-finite factor, leaves the clock untouched (deficit += 0,
// §2.6). `factorMinusOne` is (dτ/dt − 1), e.g. from CombinedDilationFactorMinusOne.
void AdvanceClock(ecs::RelativisticClock& clock, double factorMinusOne, double dt);

// Accumulated proper time τ = coordinateTime + properTimeDeviation. This is a
// RECONSTRUCTION, not a running τ += dτ, so the deviation kept its precision the
// whole way and only meets the large magnitude once, here, on demand.
double ProperTime(const ecs::RelativisticClock& clock);

// =============================================================================
// 3. THE FRAME-RELATIVE-VELOCITY SUBTLETY (§2.3)
// =============================================================================
// β = |v|/c is FRAME-DEPENDENT. A ship at rest relative to a planet is NOT at
// rest relative to the star, so its SR dilation depends on which frame the
// velocity is measured in. SR proper time is only well-defined against a single
// coordinate frame, so the velocity MUST be composed to a designated MASTER
// frame before β is formed (§2.3). Under reference_frame.h's translation-only
// frames the body's velocity in the master frame is
//     v_master = ResolveWorldVel(body) − masterFrame.velocity
// (both are global velocities; the difference is the master-frame-relative one).
//
// CAVEAT (stated honestly, per the prompt and §2.3/§11 — this is a one-lens,
// design-time decision, unverified against a running sim):
//   * The master frame is a CONVENTION (the architecture picks the star-system
//     barycentric/top frame). Different master-frame choices give different
//     coordinate-time rates, hence different accumulated dilation. Proper time
//     along a worldline is invariant, but the dτ/dt_coordinate FACTOR is only
//     meaningful relative to the chosen master's coordinate time.
//   * The SR formula assumes the master frame is INERTIAL. A star-system
//     barycentric frame accelerates (it orbits the galaxy; its members orbit
//     it), so over a step this is a weak-field approximation — negligible at a
//     1/60 s step and orbital accelerations, but not exact.
//   * On a master-frame change (SOI / warp transition) the proper-time
//     accumulation is continuous only if both sides agree on v_master at the
//     instant — the same state-vector-continuity argument as §4.4. Handling that
//     handoff is the ship-slice lane's job; this module provides the composition.

// The body's velocity expressed in the master frame (translation-only frames):
// ResolveWorldVel(body) − masterFrame.velocity. This is the CORRECT velocity for
// SR dilation.
Vec3d VelocityInMasterFrame(const FrameGraph& graph, const Body& body, FrameId masterFrame);

// β = |v_master| / c — the CORRECT, frame-consistent β for SR dilation. Use this
// to drive the clock.
double BetaInMasterFrame(const FrameGraph& graph, const Body& body, FrameId masterFrame);

// β = |body.localVel| / c — β from the body's LOCAL (own-frame) velocity. This is
// the NAIVE, frame-DEPENDENT quantity the architecture warns against: it gives
// the WRONG dilation for a body at rest in a moving frame (β_local = 0 while
// β_master ≠ 0). Shipped as the labeled DISCRIMINATING NEGATIVE CONTROL for the
// §2.3 subtlety — the same pattern nbody.cpp ships StepNBodyExplicitEuler as the
// non-symplectic control. DO NOT use it to drive a clock.
double BetaLocalNaive(const Body& body);

} // namespace sim
