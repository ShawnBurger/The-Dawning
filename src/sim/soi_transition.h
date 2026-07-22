#pragma once
// =============================================================================
// sim/soi_transition.h — patched-conics orchestration (Stage 13). Wires the
// Stage-7 sphere-of-influence primitive into the running simulation: each step,
// every on-rails body's dominant SOI is resolved from the live source positions,
// and a body that has crossed into a DIFFERENT primary's sphere has its orbit
// re-fit about that new primary (Repatch), continuous in global position AND
// velocity. This is what makes interplanetary travel real — a ship coasting from
// one planet's neighbourhood into another's switches primaries at the boundary.
//
// Operates on ECS components in a single flat frame: a body's Transform.position
// is its world offset (the central star at the origin), so WorldPos::FromOffset
// resolves it globally. A gravity source's SOI radius comes from its own orbit
// (Stage-7 SphereOfInfluenceRadius); a source with no orbit (the central star) is
// the unbounded root. Pure CPU; no rendering.
// =============================================================================

#include "../ecs/registry.h"
#include "../core/types.h"

#include <cstdint>

namespace sim {

struct SoiTransitionResult
{
    uint32_t evaluated   = 0; // on-rails bodies whose SOI was resolved this step
    uint32_t transitions = 0; // bodies whose primary changed (Repatch applied)
};

// The gravitational primary a body currently falls under, plus that primary's live
// world state — everything needed to express the body's motion as a two-body orbit.
struct ResolvedPrimary
{
    bool        found    = false;
    uint64_t    bodyId   = 0;
    core::Vec3d position;         // primary world position (frame offset, m)
    core::Vec3d velocity;         // primary world velocity (m/s)
    double      mu       = 0.0;   // primary GM (m^3 s^-2)
};

// Resolve the patched-conics primary for a body at world position `where` whose own
// id is `selfBodyId` (excluded from the wells so a source can never capture itself).
// Unlike StepSoiTransitions this is a const, read-only query that works for ANY body,
// not just on-rails ones: it is how the player ship — which is ForceIntegrated and so
// untouched by the transition pass — learns which planet's gravity dominates it. It
// builds the SAME source-well set StepSoiTransitions builds, so ship and rails agree
// on where every boundary is. `found` is false only when the point lies in no well
// (interstellar space with no unbounded root — never inside the seeded system, whose
// central star has an infinite sphere).
ResolvedPrimary ResolvePrimaryFor(const ecs::Registry& registry,
                                  const core::Vec3d& where, uint64_t selfBodyId);

// Resolve each on-rails body's dominant SOI from the current source positions and
// re-fit its OrbitState about the new primary if it crossed a boundary. `now` is
// the coordinate time (the new epoch of any re-fit orbit). `hysteresis` in [0,1)
// opens a deadband around each SOI boundary to suppress per-step primary thrash
// (0.01 is a sane default). Deterministic: sources and candidates are processed
// in ascending bodyId order. A body is never made its own primary.
SoiTransitionResult StepSoiTransitions(ecs::Registry& registry, double now,
                                       double hysteresis);

} // namespace sim
