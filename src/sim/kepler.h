#pragma once
// =============================================================================
// sim/kepler.h — Robust Kepler solver + orbital-elements <-> state conversion
// =============================================================================
// SIM STAGE 1 (orbital core), the ANALYTIC / on-rails half. GPU-free: this header
// includes only core/types.h and ecs/components.h (both device-free), exactly the
// way sim/rigid_body.h does, so tests/test_kepler.cpp drives the SHIPPED solver
// rather than a paraphrase. sim includes ecs; ecs never includes sim — no cycle.
//
// What lives here, per RELATIVISTIC_SIM_ARCHITECTURE.md §4.3 / §4.5 and
// PHYSICS_RESEARCH_REFERENCE.md §1:
//
//   * SolveKeplerElliptic  — Markley (NASA 1995) NON-ITERATIVE fifth-order solve
//     of M = E - e sinE for e in [0,1). Fixed cost, machine precision in one pass.
//     Uses the STABLE derivative form f'(E) = (1-e) + 2e sin^2(E/2), which is the
//     documented fix for the catastrophic cancellation of 1 - e cosE at e>0.75,
//     E<45deg. There is exactly one Markley singularity, at e=1 AND M=0, and it is
//     harmless because E=0 there (no solve needed).
//
//   * SolveKeplerHyperbolic — M = e sinhH - H for e>1, robust seeded Newton with a
//     bisection fallback so it converges across the whole hyperbolic range.
//
//   * PropagateUniversal — the UNIVERSAL-VARIABLE (Stumpff) propagator. Advances a
//     Cartesian state (r0,v0) by dt for ANY conic (ellipse, near-parabola, hyperbola)
//     without an e-specific branch, which is what makes the e->1 region well-behaved
//     where classical element propagation is ill-conditioned. This is the on-rails
//     propagator for near-parabolic and hyperbolic arcs (slingshot/escape).
//
//   * ElementsToState / StateToElements — classical osculating-elements <-> (r,v)
//     conversion, BOTH directions, for elliptic and hyperbolic conics. This is what
//     LOD demotion fits and LOD promotion seeds (sim/nbody.h).
//
//   * PropagateElements — elliptic on-rails propagation via mean-anomaly advance +
//     the Markley solve, returning updated elements (true anomaly advanced).
//
// State vectors here are RELATIVE TO THE PRIMARY (primary at the origin, primary at
// rest). The caller composes the primary's own world position/velocity back on;
// that is the frame-relative discipline §1/§4.4 insists on. mu is the PRIMARY's
// gravitational parameter G*M_primary.
// =============================================================================

#include "../core/types.h"
#include "../ecs/components.h"

namespace sim
{

using core::Vec3d;

// A Cartesian orbital state RELATIVE TO THE PRIMARY (primary at origin/rest).
struct StateVector
{
    Vec3d position; // r, metres, relative to the primary
    Vec3d velocity; // v, m/s,    relative to the primary
};

// -----------------------------------------------------------------------------
// Kepler's equation solvers
// -----------------------------------------------------------------------------

// Solve M = E - e*sinE for the eccentric anomaly E (radians), elliptic e in [0,1).
// Markley non-iterative fifth-order. M may be any real; it is reduced to [-pi,pi]
// and E returned in the matching branch. Machine precision in one evaluation.
double SolveKeplerElliptic(double meanAnomaly, double eccentricity);

// Solve M = e*sinhH - H for the hyperbolic anomaly H (radians), e > 1. Robust
// seeded Newton with a bisection safety net (RELATIVISTIC_SIM_ARCHITECTURE.md §4.3).
double SolveKeplerHyperbolic(double meanAnomaly, double eccentricity);

// A tolerance-iterating REFERENCE elliptic solve (Newton + bisection bracket),
// exposed so tests can validate Markley against an independent method and so the
// architecture's "capped Newton with a bisection fallback" robustness path is a
// real, shipped, callable function rather than a claim. Slower than Markley.
double SolveKeplerEllipticReference(double meanAnomaly, double eccentricity);

// -----------------------------------------------------------------------------
// Universal-variable propagation (Stumpff) — all conic types, one formulation
// -----------------------------------------------------------------------------

// Stumpff functions c2(psi), c3(psi), branch-selected for psi>0 / psi<0 / psi~0.
// Exposed for unit testing against their series limits.
double StumpffC2(double psi);
double StumpffC3(double psi);

// Advance a primary-relative Cartesian state by dt seconds using universal
// variables. Robust for ellipse, near-parabola (e->1) and hyperbola. `mu` is the
// primary gravitational parameter. `ok` (optional) is set false if the internal
// Newton iteration failed to converge (never expected in-domain).
StateVector PropagateUniversal(const StateVector& state, double mu, double dt,
                               bool* ok = nullptr);

// -----------------------------------------------------------------------------
// Osculating elements <-> primary-relative state vector
// -----------------------------------------------------------------------------

// Elements (defined at their epoch) + primary mu -> primary-relative (r,v) AT THE
// EPOCH ANOMALY (elements.trueAnomaly). Handles elliptic and hyperbolic conics.
StateVector ElementsToState(const ecs::OrbitalElements& elements, double mu);

// Primary-relative (r,v) + primary mu -> osculating elements (true anomaly = the
// anomaly at THIS state). Inverse of ElementsToState to tolerance. Handles
// elliptic and hyperbolic conics. Well-conditioned for inclined, eccentric orbits;
// near-circular/near-equatorial degeneracies fall back to defined reference angles.
ecs::OrbitalElements StateToElements(const StateVector& state, double mu);

// Propagate elliptic elements forward by dt seconds: advance the mean anomaly by
// n*dt (n = sqrt(mu/a^3)), solve with Markley, return elements with trueAnomaly
// updated (a, e, i, node, argp unchanged — a Kepler orbit is closed). For e>=1 use
// PropagateUniversal instead; this asserts elliptic in debug.
ecs::OrbitalElements PropagateElements(const ecs::OrbitalElements& elements,
                                       double mu, double dt);

// -----------------------------------------------------------------------------
// Anomaly conversions (exposed for tests and for PropagateElements)
// -----------------------------------------------------------------------------
double TrueToEccentricAnomaly(double trueAnomaly, double eccentricity);   // elliptic
double EccentricToMeanAnomaly(double eccentricAnomaly, double eccentricity); // M=E-e sinE
double EccentricToTrueAnomaly(double eccentricAnomaly, double eccentricity);
double TrueToHyperbolicAnomaly(double trueAnomaly, double eccentricity);  // hyperbolic
double HyperbolicToMeanAnomaly(double hyperbolicAnomaly, double eccentricity);
double HyperbolicToTrueAnomaly(double hyperbolicAnomaly, double eccentricity);

} // namespace sim
