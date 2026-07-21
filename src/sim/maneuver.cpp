// =============================================================================
// sim/maneuver.cpp — impulsive maneuver planning. See maneuver.h.
// =============================================================================

#include "maneuver.h"

#include "kepler.h"   // StateVector, PropagateUniversal
#include "lambert.h"  // SolveLambert

#include <cmath>

namespace sim {

using core::Vec3d;

HohmannTransfer HohmannDeltaV(double r1, double r2, double mu)
{
    HohmannTransfer h;
    if (!std::isfinite(r1) || r1 <= 0.0 || !std::isfinite(r2) || r2 <= 0.0 ||
        !std::isfinite(mu) || mu <= 0.0)
        return h;

    const double at  = 0.5 * (r1 + r2);                 // transfer semi-major axis
    const double v1c = std::sqrt(mu / r1);              // circular speed at r1
    const double v2c = std::sqrt(mu / r2);              // circular speed at r2
    // vis-viva on the transfer ellipse at each apse: v = sqrt(mu(2/r − 1/a)).
    const double vDepart = std::sqrt(mu * (2.0 / r1 - 1.0 / at));
    const double vArrive = std::sqrt(mu * (2.0 / r2 - 1.0 / at));

    h.departV = std::fabs(vDepart - v1c);
    h.arriveV = std::fabs(v2c - vArrive);
    h.totalV  = h.departV + h.arriveV;
    return h;
}

InterceptPlan PlanIntercept(const Vec3d& chaserPos, const Vec3d& chaserVel,
                            const Vec3d& targetPos, const Vec3d& targetVel,
                            double tof, double mu, bool prograde)
{
    InterceptPlan plan;

    // Where the target will be after tof.
    bool ok = false;
    const StateVector arrival =
        PropagateUniversal(StateVector{ targetPos, targetVel }, mu, tof, &ok);
    if (!ok)
        return plan;

    // The chaser's transfer to that point.
    const LambertSolution transfer =
        SolveLambert(chaserPos, arrival.position, tof, mu, prograde);
    if (!transfer.converged)
        return plan;

    // Burns are the velocity discontinuities the impulsive maneuver supplies.
    plan.departureBurn  = transfer.v1 - chaserVel;         // enter the transfer
    plan.arrivalBurn    = arrival.velocity - transfer.v2;  // match the target (rendezvous)
    plan.interceptPoint = arrival.position;
    plan.departureCost  = plan.departureBurn.Length();
    plan.arrivalCost    = plan.arrivalBurn.Length();
    plan.totalCost      = plan.departureCost + plan.arrivalCost;
    plan.feasible       = true;
    return plan;
}

} // namespace sim
