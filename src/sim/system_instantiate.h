#pragma once
// =============================================================================
// sim/system_instantiate.h — bring a StarSystem to life (Stage 12). Instantiates
// a Stage-11 StarSystem into live ECS entities so the shipped StepSimulation
// actually drives it: each body becomes an entity with Transform / RigidBody /
// SpatialFrame / GravitationalBody (+ OrbitState for rails bodies), with its
// initial in-frame position/velocity resolved top-down from its circular orbit.
//
// This is the bridge between the pure-data builder and the running simulation —
// the piece that finally gives the whole astrodynamics stack a world to move.
// The central star is placed AT the frame origin, so a body's in-frame position
// is its position relative to the star.
// =============================================================================

#include "star_system.h"
#include "reference_frame.h"   // sim::FrameId
#include "../ecs/registry.h"   // ecs::Registry

#include <cstdint>

namespace sim {

// Instantiate every body of `system` as an entity in `frame` (the star at the
// frame origin). Bodies must be parent-before-child (BuildReferenceSystem is).
// Returns the number of entities created. A body whose primary has not been
// placed earlier (malformed input) is SKIPPED, not defaulted onto the star.
uint32_t InstantiateStarSystem(ecs::Registry& registry, FrameId frame,
                               const StarSystem& system);

} // namespace sim
