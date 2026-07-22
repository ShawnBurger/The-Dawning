// =============================================================================
// sim/transfer_planner.cpp — launch-window search. See transfer_planner.h.
// =============================================================================

#include "transfer_planner.h"

#include "lambert.h"   // SolveLambert, LambertSolution
// kepler.h (StateVector, PropagateUniversal) comes in through transfer_planner.h.

#include <cmath>

namespace sim {

using core::Vec3d;

namespace {

bool Finite(const Vec3d& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Inclusive linear grid: sample i of n is min + (max-min)*i/(n-1); a 1-sample
// grid is exactly `min`. Callers guarantee n>=1 and i<n.
double GridSample(double lo, double hi, uint32_t n, uint32_t i)
{
    if (n <= 1u)
        return lo;
    return lo + (hi - lo) * (static_cast<double>(i) / static_cast<double>(n - 1u));
}

} // namespace

TransferWindow PlanTransferWindow(const StateVector& chaser,
                                  const StateVector& target,
                                  double mu,
                                  const TransferSearchParams& params)
{
    TransferWindow best; // feasible == false until a valid cell is found

    // Reject an ill-formed box up front. tofMin must be strictly positive because
    // a zero/negative transfer time has no conic arc; departureMin may be zero
    // (leave now) but the ranges must be ordered and the grids non-empty.
    if (!Finite(chaser.position) || !Finite(chaser.velocity) ||
        !Finite(target.position) || !Finite(target.velocity) ||
        !std::isfinite(mu) || mu <= 0.0 ||
        params.departureSamples == 0u || params.tofSamples == 0u ||
        !std::isfinite(params.departureMin) || !std::isfinite(params.departureMax) ||
        !std::isfinite(params.tofMin) || !std::isfinite(params.tofMax) ||
        params.departureMax < params.departureMin ||
        params.tofMax < params.tofMin ||
        params.tofMin <= 0.0)
        return best;

    double bestKey = 0.0; // meaningful only once best.feasible is true

    for (uint32_t di = 0; di < params.departureSamples; ++di)
    {
        const double td = GridSample(params.departureMin, params.departureMax,
                                     params.departureSamples, di);

        // The chaser coasts to the departure epoch once per departure sample; its
        // velocity THERE is what the departure burn is measured against.
        bool chaserOk = false;
        const StateVector depart = PropagateUniversal(chaser, mu, td, &chaserOk);
        if (!chaserOk || !Finite(depart.position) || !Finite(depart.velocity))
            continue;

        for (uint32_t ti = 0; ti < params.tofSamples; ++ti)
        {
            const double tof = GridSample(params.tofMin, params.tofMax,
                                          params.tofSamples, ti);

            // Where the target is when the transfer arrives (t = td + tof).
            // PropagateUniversal's `ok` reflects only Newton convergence, not
            // output finiteness (kepler.cpp) — a converged-but-singular
            // propagation (a state reaching r~0) can return ok=true with a
            // non-finite velocity. Guard it, symmetric with the chaser above.
            // In intercept mode (rendezvous=false) the rank key excludes the
            // arrival burn, so the finite-key test below would NOT catch a
            // non-finite arrival velocity; this is where that is caught. (Such an
            // arrival's position is also non-finite/degenerate, so SolveLambert
            // would reject it too — this is defense-in-depth on that coupling,
            // not an independently reachable path.)
            bool targetOk = false;
            const StateVector arrival =
                PropagateUniversal(target, mu, td + tof, &targetOk);
            if (!targetOk || !Finite(arrival.position) || !Finite(arrival.velocity))
                continue;

            // The connecting arc — identical per-cell math to PlanIntercept.
            const LambertSolution transfer =
                SolveLambert(depart.position, arrival.position, tof, mu,
                             params.prograde);
            if (!transfer.converged)
                continue;

            const Vec3d departureBurn = transfer.v1 - depart.velocity;
            const Vec3d arrivalBurn   = arrival.velocity - transfer.v2;
            const double departureCost = departureBurn.Length();
            const double arrivalCost   = arrivalBurn.Length();
            const double totalCost     = departureCost + arrivalCost;

            // Rank by the rendezvous total or the intercept-only departure cost.
            const double key = params.rendezvous ? totalCost : departureCost;
            if (!std::isfinite(key))
                continue;

            if (!best.feasible || key < bestKey)
            {
                bestKey            = key;
                best.departureTime = td;
                best.timeOfFlight  = tof;
                best.departureBurn = departureBurn;
                best.arrivalBurn   = arrivalBurn;
                best.departureCost = departureCost;
                best.arrivalCost   = arrivalCost;
                best.totalCost     = totalCost;
                best.feasible      = true;
            }
        }
    }

    return best;
}

} // namespace sim
