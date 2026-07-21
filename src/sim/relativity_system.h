#pragma once
// =============================================================================
// sim/relativity_system.h - frame-aware ECS proper-time adapter
// =============================================================================
// Advances RelativisticClock sidecars after fixed-step motion. SR velocity is
// measured in one designated master frame. GR uses an explicit dominant-primary
// bodyId binding, preserving the architecture's named single-primary
// approximation without guessing from iteration order or nearest distance.

#include "relativity.h"
#include "../ecs/registry.h"

#include <cstdint>
#include <span>

namespace sim
{

inline constexpr uint64_t kNoClockGravitySource = UINT64_MAX;

struct ClockGravityBinding
{
    ecs::Entity entity = ecs::NullEntity;
    uint64_t primaryBodyId = kNoClockGravitySource;
};

struct RelativisticClockStepResult
{
    bool accepted = false;
    uint32_t clockCount = 0;
    uint32_t gravitationalClockCount = 0;
};

// Advance every RelativisticClock atomically. A clock requires Transform,
// SpatialFrame and RigidBody. Bindings are optional; an unbound clock receives
// SR dilation only, while a bound clock composes SR with the explicitly named
// source's softened Schwarzschild factor. Any malformed clock, frame, source,
// duplicate binding or duplicate gravitational bodyId rejects before a write.
RelativisticClockStepResult StepRelativisticClocks(
    ecs::Registry& registry,
    const FrameGraph& frames,
    FrameId masterFrame,
    double dt,
    std::span<const ClockGravityBinding> gravityBindings = {});

} // namespace sim
