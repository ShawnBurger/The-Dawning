#pragma once
// =============================================================================
// sim/snapshot_system.h - topology-preserving ECS <-> SimSnapshot bridge
// =============================================================================
// The binary codec stores simulation state only. This bridge maps that state to
// live entities by GravitationalBody::bodyId while preserving render/gameplay
// components. Apply requires the same simulation topology so it can validate and
// stage every write before committing without creating or destroying entities.

#include "sim_serialize.h"
#include "../ecs/components.h"
#include "../ecs/registry.h"

#include <cstdint>
#include <string>

namespace sim
{

struct SnapshotBuildResult
{
    bool accepted = false;
    SimSnapshot snapshot;
    std::string error;
};

struct SnapshotApplyResult
{
    bool accepted = false;
    std::string error;
};

// Build a canonical, codec-validated snapshot. Every persisted body must carry
// Transform + SpatialFrame + RigidBody + GravitationalBody. RelativisticBody and
// RelativisticClock are an inseparable pair. A pending wrench is rejected because
// the fixed-step save format intentionally stores only boundary state.
SnapshotBuildResult BuildSnapshot(const ecs::Registry& registry,
                                  const FrameGraph& frames,
                                  double coordinateTime,
                                  double fixedDt,
                                  uint64_t simTick,
                                  FrameId masterFrame);

// Apply a validated snapshot onto the same stable body-ID topology. Rendering,
// input, thrusters, names and other non-simulation components are untouched.
// On any mismatch or malformed value, registry and frame graph remain unchanged.
SnapshotApplyResult ApplySnapshot(ecs::Registry& registry,
                                  FrameGraph& frames,
                                  const SimSnapshot& snapshot);

} // namespace sim
