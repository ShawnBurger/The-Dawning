#pragma once

#include "interior_state.h"
#include "../ecs/components.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ship
{

enum class InteriorLightingStatus : uint8_t
{
    Success,
    InvalidArgument,
    InvalidSnapshot,
    InvalidTransform,
    ResourceLimitExceeded,
    ArithmeticOverflow,
    AllocationFailure,
    InternalError
};

const char* InteriorLightingStatusName(InteriorLightingStatus status);

struct InteriorLightFrameConfig
{
    uint32_t maxLights = 4096;
    double maximumCameraRelativeMagnitude = 1.0e8;
    double uniformScaleTolerance = 1.0e-5;
    double minimumScale = 1.0e-6;
    double maximumScale = 1.0e6;
    // Covers the assembly intensity ceiling multiplied by all default runtime
    // controls, including the emergency-only multiplier.
    double maximumResolvedIntensity = 1.0e12;
};

struct InteriorGpuLight
{
    core::Vec3f positionCameraRelative;
    float rangeMeters = 0.0f;
    core::Vec3f direction{ 0.0f, 0.0f, 1.0f };
    float intensityLumensOrCandela = 0.0f;
    core::Vec3f colorLinear{ 1.0f, 1.0f, 1.0f };
    float innerConeCosine = -1.0f;
    float outerConeCosine = -1.0f;
    float importance = 0.0f;
    uint32_t stableIndex = asset::kAssemblyNoIndex;
    uint32_t moduleIndex = asset::kAssemblyNoIndex;
    asset::AssemblyLightType type = asset::AssemblyLightType::Point;
    asset::AssemblyLightShadowPolicy shadowPolicy =
        asset::AssemblyLightShadowPolicy::None;
};

struct InteriorModulePoseView
{
    asset::Sha256Digest topologySha256;
    uint64_t topologyRevision = 0;
    std::span<const ecs::Transform> worldTransforms;
};

struct InteriorLightFrame
{
    asset::Sha256Digest topologySha256;
    uint64_t topologyRevision = 0;
    uint64_t stateRevision = 0;
    uint64_t simulationTick = 0;
    std::vector<InteriorGpuLight> lights;
};

struct InteriorLightingResult
{
    InteriorLightingStatus status = InteriorLightingStatus::InvalidArgument;
    uint32_t failedStableIndex = asset::kAssemblyNoIndex;
    std::string error;

    bool Succeeded() const
    {
        return status == InteriorLightingStatus::Success;
    }
};

// Converts immutable resolved fixture state into a compact renderer-facing
// frame. Module poses remain double precision until the camera has been
// subtracted; the output is the sole float narrowing boundary.
InteriorLightingResult BuildInteriorLightFrame(
    const ShipInteriorSnapshot& snapshot,
    const InteriorModulePoseView& modulePoses,
    const core::Vec3d& cameraWorldPosition,
    InteriorLightFrame& output,
    const InteriorLightFrameConfig& config = {});

} // namespace ship
