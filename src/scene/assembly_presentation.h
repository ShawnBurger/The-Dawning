#pragma once

#include "assembly_instantiator.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace scene
{

enum class AssemblyPresentationStatus : uint8_t
{
    Success,
    InvalidArgument,
    ResourceLimitExceeded,
    InvalidRoot,
    InvalidTopology,
    InvalidLocalTransform,
    ArithmeticOverflow
};

const char* AssemblyPresentationStatusName(AssemblyPresentationStatus status);

struct AssemblyPresentationConfig
{
    uint64_t maxModules = 250'000ull;
    uint64_t maxMovingParts = 250'000ull;
    double uniformRootScaleTolerance = 1.0e-5;
    double minimumRootScale = 1.0e-6;
    double maximumRootScale = 1.0e6;
    double maximumLocalMagnitude = 1.0e7;
    bool requireUniformRootScale = true;
};

struct AssemblyPresentationResult
{
    AssemblyPresentationStatus status =
        AssemblyPresentationStatus::InvalidArgument;
    uint32_t failedStableIndex = asset::kAssemblyNoIndex;
    bool failedMovingPart = false;
    std::string_view error;

    bool Succeeded() const
    {
        return status == AssemblyPresentationStatus::Success;
    }
};

// Builds a complete world-pose batch without touching ECS state. Callers own
// the scratch spans and publish them only after this function succeeds, which
// makes malformed roots and stale topology atomic no-ops for the live scene.
AssemblyPresentationResult StageAssemblyPresentation(
    const ecs::Transform& root,
    std::span<const PreparedAssemblyModule> modules,
    std::span<const PreparedAssemblyMovingPart> movingParts,
    std::span<const ecs::Transform> movingPartLocalTransforms,
    std::span<ecs::Transform> moduleWorldTransforms,
    std::span<ecs::Transform> movingPartWorldTransforms,
    const AssemblyPresentationConfig& config = {});

} // namespace scene
