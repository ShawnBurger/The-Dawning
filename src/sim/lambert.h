#pragma once
// =============================================================================
// sim/lambert.h — Lambert's problem: the two-point boundary-value solver of
// astrodynamics. Given two position vectors about a primary and a time of
// flight, find the transfer orbit connecting them (the velocity at each end).
// This is the PLANNING primitive that turns patched-conics rails (Stage 7) into
// navigable space: "what orbit gets me from A to B in time T?" — the core of
// course plotting, intercepts, and rendezvous burns.
//
// Universal-variable (Stumpff) formulation — Bate-Mueller-White / Vallado /
// Curtis Algorithm 5.2 — reusing the shipped StumpffC2/StumpffC3 so ONE conic
// machinery spans elliptic, parabolic, and hyperbolic transfers with no e→1
// singularity. Pure CPU math; the navigation / gameplay layer is the consumer.
//
// VERIFICATION: the load-bearing invariant is a round-trip against the shipped
// PropagateUniversal — take a known orbit, sample (r1,v1) and (r2,v2)=propagate,
// feed (r1,r2,tof) to SolveLambert, and recover v1,v2. That closes Lambert
// against the already-tested propagator without any external reference numbers.
// =============================================================================

#include "../core/types.h"  // core::Vec3d

#include <cstdint>

namespace sim {

struct LambertSolution
{
    core::Vec3d v1;                 // transfer velocity at r1 (m/s)
    core::Vec3d v2;                 // transfer velocity at r2 (m/s)
    bool        converged = false;  // false => degenerate geometry / no convergence
    uint32_t    iterations = 0;     // Newton iterations used (diagnostic)
};

// Solve the zero-revolution Lambert problem about a primary with gravitational
// parameter `mu` (m^3/s^2, > 0). r1, r2 are primary-relative positions (m); `tof`
// is the time of flight (s, > 0). `prograde` selects the transfer sense: the
// short vs long way is fixed by the sign of (r1 × r2).z against the chosen sense
// (prograde => the transfer angle is < 180° when (r1×r2).z >= 0). Returns
// converged=false (with zero velocities) for a degenerate transfer geometry
// (r1, r2 colinear — Δν = 0 or π, the plane is undefined), non-finite or
// non-positive inputs, or if the Newton iteration fails to converge.
LambertSolution SolveLambert(const core::Vec3d& r1, const core::Vec3d& r2,
                             double tof, double mu, bool prograde);

} // namespace sim
