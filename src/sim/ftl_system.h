#pragma once
// =============================================================================
// sim/ftl_system.h - atomic ECS adapter for FTL frame transitions
// =============================================================================
// GPU-free glue between ECS-owned ship state, FrameGraph coordinates and the
// pure TryApplyTeleport kernel. This module owns no input, rendering or timer
// state. A successful result tells the host which discontinuity cleanup actions
// remain mandatory at the application boundary.

#include "ftl.h"
#include "../ecs/registry.h"
#include "../ecs/components.h"

namespace sim
{

struct FtlTransitionResult
{
    bool accepted = false;
    bool resetRenderHistory = false;
    bool drainFixedAccumulator = false;
};

// Atomically teleport one frame-aware rigid body. Required components are
// Transform + SpatialFrame + RigidBody. RelativisticBody and GravitationalBody
// participate when present; an accepted transition rotates momentum and promotes
// an on-rails gravitational body to NBodyActive. Rejection is a registry no-op.
FtlTransitionResult TryTeleportEntity(ecs::Registry& registry,
                                      ecs::Entity entity,
                                      const FrameGraph& frames,
                                      FrameId destinationFrame,
                                      const MouthTransform& mouth);

} // namespace sim
