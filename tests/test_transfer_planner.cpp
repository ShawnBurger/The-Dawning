// =============================================================================
// tests/test_transfer_planner.cpp — launch-window search (Sim Stage 14).
//
// Invariants:
//   * A 1x1 box REPRODUCES PlanIntercept — at departureTime 0 against the raw
//     states, and at departureTime D>0 against BOTH states propagated to D. The
//     D>0 case is what pins that the chaser (not only the target) is coasted to
//     the departure epoch: a planner that forgot to propagate the chaser would
//     still pass the td=0 case and fail this one.
//   * The SEARCH returns the grid MINIMUM — checked against a brute-force ORACLE
//     that ranks every cell with the already-tested PlanIntercept in the same
//     iteration order. The planner's whole contract is "argmin of that per-cell
//     transfer over the box", so it must return the oracle's least cost (the box
//     is chosen to have a real spread, worst > 1.5x best, so this is not vacuous),
//     and the winning cell it names must re-derive to that cost. This sidesteps
//     any co-orbital degeneracy where every flight time ties. Watched failing:
//     inverting the internal rank (min->max) returns the worst cell instead.
//   * Ill-formed boxes (bad mu, empty grid, non-positive tof, inverted ranges)
//     return feasible=false.
// =============================================================================

#include "test_framework.h"
#include "sim/transfer_planner.h"
#include "sim/maneuver.h"     // PlanIntercept (the per-cell reference)
#include "sim/kepler.h"       // StateVector, PropagateUniversal

#include <cmath>
#include <limits>

namespace
{
using core::Vec3d;
using namespace sim;

constexpr double kMu = 3.986004418e14; // Earth, m^3/s^2

double Rel(const Vec3d& a, const Vec3d& b)
{
    const double d = (a - b).Length();
    const double s = (std::max)(a.Length(), b.Length());
    return s > 0.0 ? d / s : d;
}

// A circular-orbit state in the xy-plane at radius r (prograde, +z angular
// momentum), rotated to start at angle `phase` about +z.
StateVector Circular(double r, double phase)
{
    const double v = std::sqrt(kMu / r);
    const double c = std::cos(phase), s = std::sin(phase);
    return StateVector{ Vec3d{ r * c, r * s, 0.0 },
                        Vec3d{ -v * s, v * c, 0.0 } };
}

} // namespace

TEST_CASE(TransferPlanner_UnitBoxReproducesPlanIntercept)
{
    const StateVector chaser = Circular(7.0e6, 0.0);
    const StateVector target = Circular(9.0e6, 1.2); // different orbit and phase
    const double T = 1800.0;                          // fixed time of flight

    // --- td == 0: the box collapses onto PlanIntercept of the RAW states. ---
    TransferSearchParams p0;
    p0.departureMin = p0.departureMax = 0.0; p0.departureSamples = 1;
    p0.tofMin = p0.tofMax = T;               p0.tofSamples = 1;
    p0.prograde = true; p0.rendezvous = true;

    const TransferWindow w0 = PlanTransferWindow(chaser, target, kMu, p0);
    const InterceptPlan  r0 = PlanIntercept(chaser.position, chaser.velocity,
                                            target.position, target.velocity,
                                            T, kMu, true);
    CHECK(w0.feasible);
    CHECK(r0.feasible);
    CHECK_EQ(w0.departureTime, 0.0);
    CHECK_EQ(w0.timeOfFlight, T);
    CHECK(Rel(w0.departureBurn, r0.departureBurn) < 1.0e-6);
    CHECK(Rel(w0.arrivalBurn,   r0.arrivalBurn)   < 1.0e-6);
    CHECK(std::fabs(w0.totalCost - r0.totalCost) < 1.0e-3);

    // --- td == D > 0: the box must match PlanIntercept of BOTH states coasted
    //     to D. This is the assertion that pins chaser propagation. ---
    const double D = 1200.0;
    bool okc = false, okt = false;
    const StateVector chaserAtD = PropagateUniversal(chaser, kMu, D, &okc);
    const StateVector targetAtD = PropagateUniversal(target, kMu, D, &okt);
    CHECK(okc);
    CHECK(okt);

    TransferSearchParams pD = p0;
    pD.departureMin = pD.departureMax = D;

    const TransferWindow wD = PlanTransferWindow(chaser, target, kMu, pD);
    const InterceptPlan  rD = PlanIntercept(chaserAtD.position, chaserAtD.velocity,
                                            targetAtD.position, targetAtD.velocity,
                                            T, kMu, true);
    CHECK(wD.feasible);
    CHECK(rD.feasible);
    CHECK_EQ(wD.departureTime, D);
    CHECK(Rel(wD.departureBurn, rD.departureBurn) < 1.0e-6);
    CHECK(Rel(wD.arrivalBurn,   rD.arrivalBurn)   < 1.0e-6);
}

TEST_CASE(TransferPlanner_SearchReturnsTheGridMinimum)
{
    // Two generic, DISTINCT coasting orbits (different radius and phase) so the
    // grid's cost landscape has a real spread and a well-defined interior best —
    // no co-orbital degeneracy where every flight time ties.
    const StateVector chaser = Circular(7.0e6, 0.0);
    const StateVector target = Circular(1.1e7, 2.3);

    TransferSearchParams p;
    p.departureMin = 0.0;   p.departureMax = 4000.0; p.departureSamples = 5;
    p.tofMin       = 900.0; p.tofMax       = 6300.0; p.tofSamples       = 7;
    p.prograde = true; p.rendezvous = true;

    // Brute-force ORACLE over the same grid, in the SAME iteration order, ranking
    // each cell with the already-tested PlanIntercept. PlanTransferWindow's whole
    // contract is "argmin of this per-cell transfer over the box", so the oracle's
    // min (and max) is exactly what the planner must return — tie-or-not, because
    // both keep the first cell on a strict-`<` comparison.
    auto sample = [](double lo, double hi, uint32_t n, uint32_t i) {
        return n <= 1u ? lo
                       : lo + (hi - lo) * (static_cast<double>(i) /
                                           static_cast<double>(n - 1u));
    };
    bool   have = false;
    double bestKey = 0.0, worstKey = 0.0;
    for (uint32_t di = 0; di < p.departureSamples; ++di)
    {
        const double td = sample(p.departureMin, p.departureMax, p.departureSamples, di);
        bool okc = false;
        const StateVector cAt = PropagateUniversal(chaser, kMu, td, &okc);
        const StateVector tAt = PropagateUniversal(target, kMu, td, &okc); // reuse ok flag
        for (uint32_t ti = 0; ti < p.tofSamples; ++ti)
        {
            const double tof = sample(p.tofMin, p.tofMax, p.tofSamples, ti);
            const InterceptPlan plan = PlanIntercept(cAt.position, cAt.velocity,
                                                     tAt.position, tAt.velocity,
                                                     tof, kMu, p.prograde);
            if (!plan.feasible)
                continue;
            const double key = plan.totalCost; // rendezvous ranking
            if (!have || key < bestKey)  { bestKey  = key; have = true; }
            if (key > worstKey)          { worstKey = key; }
        }
    }
    CHECK(have); // the box has at least one feasible cell

    // The landscape genuinely varies — otherwise "returns the minimum" is vacuous.
    CHECK(worstKey > 1.5 * bestKey);

    const TransferWindow w = PlanTransferWindow(chaser, target, kMu, p);
    CHECK(w.feasible);
    // The planner returns the grid MINIMUM cost, not the first/last/any fixed cell.
    // (Watched failing: inverting the internal rank returns worstKey instead.)
    CHECK(std::fabs(w.totalCost - bestKey) < 1.0e-3 * bestKey + 1.0e-6);

    // The winning cell it names is real: re-deriving the transfer at (departureTime,
    // timeOfFlight) through PlanIntercept reproduces the reported cost.
    bool okw = false;
    const StateVector cW = PropagateUniversal(chaser, kMu, w.departureTime, &okw);
    const StateVector tW = PropagateUniversal(target, kMu, w.departureTime, &okw);
    const InterceptPlan chk = PlanIntercept(cW.position, cW.velocity,
                                            tW.position, tW.velocity,
                                            w.timeOfFlight, kMu, p.prograde);
    CHECK(chk.feasible);
    CHECK(std::fabs(chk.totalCost - w.totalCost) < 1.0e-3 * w.totalCost + 1.0e-6);
    CHECK(Rel(chk.departureBurn, w.departureBurn) < 1.0e-6);
    CHECK(Rel(chk.arrivalBurn,   w.arrivalBurn)   < 1.0e-6);
}

TEST_CASE(TransferPlanner_RejectsIllFormedBox)
{
    const StateVector chaser = Circular(7.0e6, 0.0);
    const StateVector target = Circular(9.0e6, 1.2);

    TransferSearchParams good;
    good.departureMin = 0.0; good.departureMax = 1000.0; good.departureSamples = 3;
    good.tofMin = 600.0;     good.tofMax = 3000.0;       good.tofSamples = 3;

    // Baseline is feasible so each rejection below is attributable to its edit.
    CHECK(PlanTransferWindow(chaser, target, kMu, good).feasible);

    auto bad = [&](auto edit) {
        TransferSearchParams p = good; edit(p);
        return PlanTransferWindow(chaser, target, kMu, p).feasible;
    };
    CHECK(!PlanTransferWindow(chaser, target, 0.0, good).feasible);   // mu <= 0
    CHECK(!PlanTransferWindow(chaser, target,
            std::numeric_limits<double>::quiet_NaN(), good).feasible); // mu NaN
    CHECK(!bad([](TransferSearchParams& p){ p.departureSamples = 0; }));
    CHECK(!bad([](TransferSearchParams& p){ p.tofSamples = 0; }));
    CHECK(!bad([](TransferSearchParams& p){ p.tofMin = 0.0; }));      // non-positive tof
    CHECK(!bad([](TransferSearchParams& p){ p.tofMin = -100.0; }));
    CHECK(!bad([](TransferSearchParams& p){ p.departureMax = -1.0; })); // inverted (max<min)
    CHECK(!bad([](TransferSearchParams& p){ p.tofMax = 100.0; }));      // tofMax < tofMin
}
