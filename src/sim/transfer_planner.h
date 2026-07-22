#pragma once
// =============================================================================
// sim/transfer_planner.h — launch-window search over impulsive transfers
// (Stage 14). PlanIntercept (Stage 9) plans ONE transfer for a GIVEN time of
// flight; this layer answers the mission question above it — "when do I leave
// and how long is the trip?" — by sweeping a grid of (departure time, time of
// flight) and returning the cell of least delta-v. This is the porkchop-plot
// search: propagate both bodies to each candidate departure/arrival epoch,
// Lambert-solve the connecting arc, and rank the resulting burns.
//
// Pure CPU math on top of PropagateUniversal + SolveLambert + PlanIntercept. The
// autopilot / navigation-UI layer is the consumer: give it two bodies on rails
// about a shared primary and a search box, get back the best departure epoch,
// trip time, and the two burns to fly it.
// =============================================================================

#include "kepler.h"          // StateVector
#include "../core/types.h"   // core::Vec3d

#include <cstdint>

namespace sim {

// The best transfer found in the search box. `feasible` is false (all other
// fields zero) if the box was ill-formed or no grid cell produced a valid
// transfer. When feasible, (departureTime, timeOfFlight) name the winning cell
// and the burns are primary-relative inertial (m/s):
//   * departureBurn — applied at t = departureTime, turning the chaser's coasting
//     velocity there onto the transfer arc.
//   * arrivalBurn   — applied at t = departureTime + timeOfFlight, matching the
//     target's velocity (the rendezvous cost). Ignore it for a pure intercept.
struct TransferWindow
{
    double      departureTime = 0.0;  // s from epoch — when to burn
    double      timeOfFlight  = 0.0;  // s — transfer duration
    core::Vec3d departureBurn;        // transferV1 − chaserVel(departureTime)
    core::Vec3d arrivalBurn;          // targetVel(arrival) − transferV2
    double      departureCost = 0.0;  // |departureBurn|
    double      arrivalCost   = 0.0;  // |arrivalBurn|
    double      totalCost     = 0.0;  // the ranking key (see `rendezvous`)
    bool        feasible      = false;
};

// The search box. Departure time and time of flight are each swept over an
// INCLUSIVE linear grid: sample i of N is min + (max-min)*i/(N-1) for N>1, or
// exactly `min` for N==1. Costs are ranked by `rendezvous`:
//   * rendezvous=true  — rank by departureCost + arrivalCost (arrive AND match).
//   * rendezvous=false — rank by departureCost only (intercept; arrivalBurn is
//     still reported so the caller sees the rendezvous cost it is declining).
struct TransferSearchParams
{
    double   departureMin  = 0.0;   // earliest departure [s from epoch]
    double   departureMax  = 0.0;   // latest departure [s from epoch]
    uint32_t departureSamples = 1;  // grid resolution in departure time (>=1)

    double   tofMin        = 0.0;   // shortest trip [s] (must be > 0)
    double   tofMax        = 0.0;   // longest trip [s]
    uint32_t tofSamples    = 1;     // grid resolution in time of flight (>=1)

    bool     prograde      = true;  // transfer sense handed to SolveLambert
    bool     rendezvous    = true;  // rank by total (true) or departure-only (false)
};

// Sweep the box and return the least-cost transfer between two primary-relative
// states about a primary of parameter `mu` (SI). Both states are coasting conics
// propagated to each candidate epoch with PropagateUniversal; each cell's arc is
// Lambert-solved exactly as PlanIntercept does, so a 1x1 box reproduces
// PlanIntercept at that (departureTime, timeOfFlight). Returns feasible=false on
// a non-finite/non-positive mu, an empty grid (any samples==0), a non-positive
// tofMin, an inverted range (max<min), or a box in which every cell's transfer
// was degenerate/non-convergent.
TransferWindow PlanTransferWindow(const StateVector& chaser,
                                  const StateVector& target,
                                  double mu,
                                  const TransferSearchParams& params);

} // namespace sim
