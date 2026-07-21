#pragma once
// =============================================================================
// sim/maneuver.h — impulsive maneuver planning on top of the Lambert solver
// (Stage 9). This is the layer that turns a transfer ORBIT into the BURNS a ship
// executes: the delta-v budget of "go there", an intercept, or a rendezvous.
//   * HohmannDeltaV — the closed-form minimum-Δv coplanar circular transfer, a Δv
//     anchor independent of the Lambert iteration.
//   * PlanIntercept — propagate the target, Lambert-solve the chaser's transfer to
//     where it will be, and difference velocities into the departure/arrival burns.
// Pure CPU math; the autopilot / navigation-UI layer is the consumer.
// =============================================================================

#include "../core/types.h"  // core::Vec3d

#include <cstdint>

namespace sim {

// Closed-form two-impulse Hohmann transfer between COPLANAR CIRCULAR orbits of
// radii r1 (start) and r2 (target) about a primary of parameter mu (all SI). The
// departure burn raises/lowers the orbit onto the transfer ellipse; the arrival
// burn circularises at r2. Magnitudes only (m/s). All-zero for non-finite or
// non-positive inputs.
struct HohmannTransfer
{
    double departV = 0.0;  // |Δv| of the departure (r1) burn
    double arriveV = 0.0;  // |Δv| of the arrival (r2) burn
    double totalV  = 0.0;  // departV + arriveV
};
HohmannTransfer HohmannDeltaV(double r1, double r2, double mu);

// The plan for a chaser to INTERCEPT (arrive at the target's future position) and
// RENDEZVOUS (also match its velocity there). The chaser burns `departureBurn` now
// to fly a `tof`-second transfer, then burns `arrivalBurn` to match the target's
// arrival velocity (arrivalBurn is the rendezvous cost; ignore it for a pure
// intercept). All vectors/states are primary-relative inertial (m, m/s).
struct InterceptPlan
{
    core::Vec3d departureBurn;   // transferV1 − chaserVel
    core::Vec3d arrivalBurn;     // targetArrivalVel − transferV2
    core::Vec3d interceptPoint;  // where they meet: target's position at t+tof
    double departureCost = 0.0;  // |departureBurn|
    double arrivalCost   = 0.0;  // |arrivalBurn|
    double totalCost     = 0.0;  // departureCost + arrivalCost
    bool   feasible      = false;// false if the target propagation or Lambert failed
};

// Plan the intercept/rendezvous: propagate (targetPos, targetVel) forward by `tof`
// (PropagateUniversal), Lambert-solve the chaser's transfer from chaserPos to that
// point, and difference the velocities into the two burns. `prograde` selects the
// transfer sense. Returns feasible=false with zero burns on a degenerate/failed
// transfer or non-finite/non-positive tof/mu.
InterceptPlan PlanIntercept(const core::Vec3d& chaserPos, const core::Vec3d& chaserVel,
                            const core::Vec3d& targetPos, const core::Vec3d& targetVel,
                            double tof, double mu, bool prograde);

} // namespace sim
