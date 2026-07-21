#pragma once
// =============================================================================
// sim/collision.h - N-body close-encounter / collision policy
// =============================================================================
// SIM STAGE 5, per docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md section 6 and
// the AGENT_COORDINATION priority "collision/close-encounter policy before
// production N-body activation". CPU-only, GPU-free. Includes ONLY nbody.h (and
// through it core/types.h), so it links into BOTH TheDawningV3 and
// TheDawningTests and the unit tests drive the SHIPPED arithmetic.
//
// nbody.h's DetectCloseEncounter returns a system-wide BOOL: softening keeps the
// 1/r^2 force finite, but that is NOT permission to integrate a collision through
// the softened core. This module is the POLICY that boolean routes to. One fixed
// step advances the whole active set by dt through GLOBAL power-of-two subdivision
// of the transport - every micro-step leaf is the UNMODIFIED StepNBody, so between
// real contacts the operator is bit-identical to StepNBody - with contact
// detection + resolution interleaved at every micro-step boundary via a swept
// closest-approach test. Collisions are discrete, momentum-conserving events.
//
// WHAT IS ASSERTED AND WHAT IS NOT (honest accounting):
//   - Linear momentum sum(mu*v) is the leak witness: conserved bit-exact across a
//     merge's mass add and to a few ULP across the divide-through velocity update.
//   - A central-normal bounce conserves sum(mu*v) AND sum(mu*(r x v)) exactly.
//   - Energy and internal angular momentum CHANGE across a merge (physical) and are
//     NEVER asserted. A bounce can only remove or preserve kinetic energy, never
//     inject it - there is deliberately NO de-penetration operator (deep overlap
//     routes to merge), which removes the whole energy-injection failure class.
//
// SCOPE: point-mass close-encounter free functions + tests. ECS reconciliation
// (destroying absorbed entities, handing the lost internal spin to the survivor's
// RigidBody, relativistic-momentum merges) is the ship/integration lane's, exactly
// as every prior sim stage deferred its wiring.

#include "nbody.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace sim
{

// -----------------------------------------------------------------------------
// Tunables. Defaults are chosen so the shipped path is correctness-first: e=0
// (perfectly inelastic => accretion, the standard outcome for gravitating bodies).
// -----------------------------------------------------------------------------
struct CloseEncounterConfig
{
    double eta             = 0.15; // accuracy: max fraction of the resolution scale a pair
                                   //           may close per micro-step (curvature control)
    double etaContact      = 0.5;  // anti-tunnel: max fraction of the CONTACT shell a pair
                                   //              may close per micro-step (absolute bound)
    double contactScale    = 1.0;  // surfaces touch when sep < contactScale*(radius_i+radius_j)
    double restitution     = 0.0;  // global e in [0,1]; 0 => perfectly inelastic (merge)
    double stickRestitution = 1e-3;// e <= this => MERGE instead of BOUNCE (no co-moving restick)
    double deepMergeFrac   = 0.5;  // overlap > frac*min(radius) => MERGE regardless of e
    int    solverIterations = 1;   // FIXED bounce solver passes (determinism + fixed cost)
    int    maxLevel        = 16;   // subdivision cap (2^L leaves); exceeding it sets hitDepthCap
};

// One discrete collision outcome at a micro-step boundary.
struct CollisionEvent
{
    uint64_t              survivorId  = 0;   // MERGE: min(bodyId) of the cluster.
                                             // BOUNCE: kBounceSurvivor (no topology change).
    std::vector<uint64_t> absorbedIds;       // MERGE: absorbed ids, ASCENDING.
                                             // BOUNCE: the two participant ids, ASCENDING.
    bool                  merged      = false;
    double                penetration = 0.0; // contact - sep at resolution (>= 0)
};

// Sentinel survivorId marking a BOUNCE event (velocity-only, no body removed). Sorts
// last so merges (real ids) lead the report.
inline constexpr uint64_t kBounceSurvivor = 0xFFFFFFFFFFFFFFFFull;

struct CloseEncounterReport
{
    int      subdivisionLevel = 0;   // L used this step (diagnostic)
    uint32_t microSteps       = 0;   // == 2^L
    bool     hitDepthCap      = false; // desired L exceeded cfg.maxLevel (WATCHED, T7)
    std::vector<CollisionEvent> events; // sorted ASCENDING by survivorId
};

// -----------------------------------------------------------------------------
// Public API - free functions, pure, dt-as-parameter, deterministic bodyId order,
// house guard (dt<=0 / non-finite / empty => no-op).
// -----------------------------------------------------------------------------

// Desired (UNCLAMPED) power-of-two subdivision level for this block. 0 => no
// eligible pair needs refinement, so StepNBodyCollisional runs one bare
// StepNBody(dt), bit-identical to the unmodified integrator. Pure: max/ceil/log2
// over pairs, no FP summation, so the level is order-independent.
int RequiredSubdivisionLevel(const std::vector<NBodyParticle>& bodies,
                             double dt, const CloseEncounterConfig& cfg);

// The "hot" pairs (indices, lower-bodyId body first), within DetectCloseEncounter's
// reach shell, emitted in deterministic (bodyId_i < bodyId_j) order. Fills the gap
// that DetectCloseEncounter reports only a system-wide bool with no pair identity.
std::vector<std::pair<uint32_t, uint32_t>>
FindCloseEncounterPairs(const std::vector<NBodyParticle>& bodies,
                        double dt, const CloseEncounterConfig& cfg);

// Detect + resolve all contacts at ONE micro-step boundary, using the swept segment
// prevPos[k] -> bodies[k].position for detection. May SHRINK `bodies` (merges) and
// mutate velocities (bounces). Appends events to `report`. Exposed (not file-static)
// so conservation/classifier tests need no gravitationally-bound fixture. prevPos
// must be the same size as bodies at entry.
void ResolveContactsAtBoundary(std::vector<NBodyParticle>& bodies,
                               const std::vector<Vec3d>& prevPos,
                               const CloseEncounterConfig& cfg,
                               CloseEncounterReport& report);

// THE composed one-fixed-step advance - replaces the bare StepNBody at the
// active-set call site. Substepped symplectic transport (every leaf is StepNBody)
// with per-micro-step contact resolution interleaved. RULE 6: dt is the fixed
// parameter; h = dt/2^L is internal and never re-enters the accumulator.
void StepNBodyCollisional(std::vector<NBodyParticle>& bodies, double dt,
                          const CloseEncounterConfig& cfg,
                          CloseEncounterReport& report);

} // namespace sim
