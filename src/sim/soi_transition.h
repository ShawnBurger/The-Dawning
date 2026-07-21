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

#include <cstdint>

namespace sim {

struct SoiTransitionResult
{
    uint32_t evaluated   = 0; // on-rails bodies whose SOI was resolved this step
    uint32_t transitions = 0; // bodies whose primary changed (Repatch applied)
};

// Resolve each on-rails body's dominant SOI from the current source positions and
// re-fit its OrbitState about the new primary if it crossed a boundary. `now` is
// the coordinate time (the new epoch of any re-fit orbit). `hysteresis` in [0,1)
// opens a deadband around each SOI boundary to suppress per-step primary thrash
// (0.01 is a sane default). Deterministic: sources and candidates are processed
// in ascending bodyId order. A body is never made its own primary.
SoiTransitionResult StepSoiTransitions(ecs::Registry& registry, double now,
                                       double hysteresis);

} // namespace sim
