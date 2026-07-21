#pragma once
// =============================================================================
// sim/relative_motion.h — Clohessy-Wiltshire / Hill relative motion (Stage 10).
// The close-proximity regime that the two-body Lambert transfers (Stage 8/9) do
// NOT cover: a chaser's motion RELATIVE to a target on a near-circular reference
// orbit, linearised in the target's rotating LVLH frame. This is the math of
// docking, formation flying, and station-keeping.
//
// Frame (LVLH / Hill): x = radial (outward from the primary), y = along-track
// (direction of motion), z = cross-track (orbit normal). Mean motion n =
// sqrt(mu/a^3). The linearised equations of motion are
//     ẍ − 2n·ẏ − 3n²·x = 0   (radial, Coriolis-coupled)
//     ÿ + 2n·ẋ         = 0   (along-track)
//     z̈ + n²·z         = 0   (cross-track, simple harmonic)
// whose closed-form state-transition matrix is exact (no integrator, cannot blow
// up), in the same spirit as the on-rails Kepler propagator. Pure CPU math; the
// docking/autopilot layer is the consumer.
// =============================================================================

#include "../core/types.h"  // core::Vec3d

#include <cstdint>

namespace sim {

// Mean motion of a circular reference orbit: n = sqrt(mu / a^3) (rad/s). Returns
// 0 for non-finite / non-positive inputs. This is the CW frame's rotation rate.
double MeanMotion(double mu, double a);

// A relative state in the target's LVLH frame (metres, m/s in the ROTATING frame).
struct CWState
{
    core::Vec3d position;
    core::Vec3d velocity;
};

// Propagate a relative state by t seconds under the Clohessy-Wiltshire state-
// transition matrix about a reference of mean motion n (> 0). Exact closed form;
// Φ(0) = I. Returns the input unchanged for n <= 0 / non-finite inputs.
CWState PropagateCW(const CWState& rel, double n, double t);

// Two-impulse CW targeting: the initial relative velocity v0 that carries the
// relative position r0 to rf in time t under CW dynamics (inverting the position
// STM's velocity block), and the relative velocity at arrival. A rendezvous burn
// plan is then departureΔv = v0 − currentVel and arrivalΔv = 0 − arrivalVel.
struct CWTargeting
{
    core::Vec3d v0;          // required initial relative velocity
    core::Vec3d arrivalVel;  // relative velocity when it reaches rf
    bool        feasible = false;  // false if the STM velocity block is singular
};
// t must avoid the transfer times where the position→velocity map is singular:
// the in-plane 2×2 determinant vanishes — det·n² = 8(1−cos nt) − 3·nt·sin nt = 0
// — which happens at EVERY integer period AND at interior times (the first near
// t ≈ 1.4·period), NOT only at k·period; or the cross-track sin(n·t) → 0 (near
// t ≈ k·period/2). Both are rejected on a scale-free conditioning threshold, so
// near-singular geometry returns feasible=false too, not just the exact roots.
CWTargeting SolveCWTargeting(const core::Vec3d& r0, const core::Vec3d& rf,
                             double n, double t);

} // namespace sim
