#pragma once
// =============================================================================
// sim/orbit_system.h - passive orbital ECS adapter
// =============================================================================
// Gathers the exclusive NBodyActive and OnRails motion owners from the live ECS,
// advances them through the shipped GPU-free kernels, and reconciles the result
// atomically. ForceIntegrated bodies may be gravity sources or rail primaries,
// but this adapter never advances them.

#include "collision.h"
#include "reference_frame.h"
#include "../ecs/registry.h"

namespace sim
{

struct PassiveOrbitStepResult
{
    bool accepted = false;
    uint32_t nbodyBodyCount = 0;
    uint32_t railBodyCount = 0;
    uint32_t destroyedEntityCount = 0;
    CloseEncounterReport collisions;
};

// Advance one fixed step of passive orbital motion. N-body particles are
// expressed in activeFrame before collision-aware Forest-Ruth integration.
// Rails are evaluated at coordinateTime + dt and may depend on another rail
// recursively; missing primaries and cycles reject before any registry write.
// A collision merge updates the survivor's gravity/inertial mass and destroys
// every absorbed ECS entity using its current generational handle.
PassiveOrbitStepResult StepPassiveOrbits(
    ecs::Registry& registry,
    const FrameGraph& frames,
    FrameId activeFrame,
    double coordinateTime,
    double dt,
    const CloseEncounterConfig& collisionConfig = CloseEncounterConfig{});

} // namespace sim
