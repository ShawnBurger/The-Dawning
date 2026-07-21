#include "assembly_presentation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace scene
{
namespace
{

constexpr float kQuaternionUnitTolerance = 1.0e-3f;

bool Finite(double value)
{
    return std::isfinite(value);
}

bool Finite(float value)
{
    return std::isfinite(value);
}

bool Finite(const core::Vec3d& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Finite(const core::Vec3f& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Finite(const core::Quatf& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z) &&
           Finite(value.w);
}

bool ValidConfig(const AssemblyPresentationConfig& config)
{
    return config.maxModules > 0 && config.maxMovingParts > 0 &&
           Finite(config.uniformRootScaleTolerance) &&
           config.uniformRootScaleTolerance > 0.0 &&
           config.uniformRootScaleTolerance <= 0.01 &&
           Finite(config.minimumRootScale) &&
           Finite(config.maximumRootScale) &&
           config.minimumRootScale > 0.0 &&
           config.maximumRootScale >= config.minimumRootScale &&
           Finite(config.maximumLocalMagnitude) &&
           config.maximumLocalMagnitude > 0.0;
}

bool UnitRotation(const core::Quatf& rotation)
{
    if (!Finite(rotation))
        return false;
    const float lengthSquared = rotation.LengthSq();
    return Finite(lengthSquared) &&
           std::abs(lengthSquared - 1.0f) <= kQuaternionUnitTolerance;
}

bool ValidRoot(
    const ecs::Transform& root,
    const AssemblyPresentationConfig& config)
{
    if (!Finite(root.position) || !UnitRotation(root.rotation) ||
        !Finite(root.scale))
    {
        return false;
    }

    const double x = root.scale.x;
    const double y = root.scale.y;
    const double z = root.scale.z;
    if (x < config.minimumRootScale || y < config.minimumRootScale ||
        z < config.minimumRootScale || x > config.maximumRootScale ||
        y > config.maximumRootScale || z > config.maximumRootScale)
    {
        return false;
    }
    if (!config.requireUniformRootScale)
        return true;

    const double scale = (std::max)({ x, y, z });
    return std::abs(x - y) <= config.uniformRootScaleTolerance * scale &&
           std::abs(x - z) <= config.uniformRootScaleTolerance * scale;
}

bool ValidLocal(
    const ecs::Transform& local,
    const AssemblyPresentationConfig& config)
{
    return Finite(local.position) && UnitRotation(local.rotation) &&
           Finite(local.scale) && local.scale.x > 0.0f &&
           local.scale.y > 0.0f && local.scale.z > 0.0f &&
           std::abs(local.position.x) <= config.maximumLocalMagnitude &&
           std::abs(local.position.y) <= config.maximumLocalMagnitude &&
           std::abs(local.position.z) <= config.maximumLocalMagnitude;
}

bool Compose(
    const ecs::Transform& root,
    const ecs::Transform& local,
    ecs::Transform& world)
{
    const double scaledX = local.position.x * root.scale.x;
    const double scaledY = local.position.y * root.scale.y;
    const double scaledZ = local.position.z * root.scale.z;
    const double maximumFloat = (std::numeric_limits<float>::max)();
    if (!Finite(scaledX) || !Finite(scaledY) || !Finite(scaledZ) ||
        std::abs(scaledX) > maximumFloat ||
        std::abs(scaledY) > maximumFloat ||
        std::abs(scaledZ) > maximumFloat)
    {
        return false;
    }

    const core::Vec3f rotated = root.rotation.Rotate({
        static_cast<float>(scaledX),
        static_cast<float>(scaledY),
        static_cast<float>(scaledZ)
    });
    world.position = root.position + core::Vec3d::FromFloat(rotated);
    if (!Finite(world.position))
        return false;

    world.rotation = (root.rotation * local.rotation).Normalized();
    if (!UnitRotation(world.rotation))
        return false;

    const double scaleX = static_cast<double>(root.scale.x) * local.scale.x;
    const double scaleY = static_cast<double>(root.scale.y) * local.scale.y;
    const double scaleZ = static_cast<double>(root.scale.z) * local.scale.z;
    if (!Finite(scaleX) || !Finite(scaleY) || !Finite(scaleZ) ||
        scaleX <= 0.0 || scaleY <= 0.0 || scaleZ <= 0.0 ||
        scaleX > maximumFloat || scaleY > maximumFloat ||
        scaleZ > maximumFloat)
    {
        return false;
    }
    world.scale = {
        static_cast<float>(scaleX),
        static_cast<float>(scaleY),
        static_cast<float>(scaleZ)
    };
    return Finite(world.scale);
}

AssemblyPresentationResult Failure(
    AssemblyPresentationStatus status,
    std::string_view error,
    uint32_t stableIndex = asset::kAssemblyNoIndex,
    bool movingPart = false)
{
    AssemblyPresentationResult result;
    result.status = status;
    result.failedStableIndex = stableIndex;
    result.failedMovingPart = movingPart;
    result.error = error;
    return result;
}

} // namespace

const char* AssemblyPresentationStatusName(AssemblyPresentationStatus status)
{
    switch (status)
    {
    case AssemblyPresentationStatus::Success: return "success";
    case AssemblyPresentationStatus::InvalidArgument: return "invalid argument";
    case AssemblyPresentationStatus::ResourceLimitExceeded:
        return "resource limit exceeded";
    case AssemblyPresentationStatus::InvalidRoot: return "invalid root";
    case AssemblyPresentationStatus::InvalidTopology: return "invalid topology";
    case AssemblyPresentationStatus::InvalidLocalTransform:
        return "invalid local transform";
    case AssemblyPresentationStatus::ArithmeticOverflow:
        return "arithmetic overflow";
    default: return "unknown";
    }
}

AssemblyPresentationResult StageAssemblyPresentation(
    const ecs::Transform& root,
    std::span<const PreparedAssemblyModule> modules,
    std::span<const PreparedAssemblyMovingPart> movingParts,
    std::span<const ecs::Transform> movingPartLocalTransforms,
    std::span<ecs::Transform> moduleWorldTransforms,
    std::span<ecs::Transform> movingPartWorldTransforms,
    const AssemblyPresentationConfig& config)
{
    if (!ValidConfig(config) || modules.size() != moduleWorldTransforms.size() ||
        movingParts.size() != movingPartLocalTransforms.size() ||
        movingParts.size() != movingPartWorldTransforms.size())
    {
        return Failure(
            AssemblyPresentationStatus::InvalidArgument,
            "presentation spans or configuration are invalid");
    }
    if (modules.size() > config.maxModules ||
        movingParts.size() > config.maxMovingParts)
    {
        return Failure(
            AssemblyPresentationStatus::ResourceLimitExceeded,
            "presentation topology exceeds configured limits");
    }
    if (!ValidRoot(root, config))
    {
        return Failure(
            AssemblyPresentationStatus::InvalidRoot,
            "assembly root is not a finite representable transform");
    }

    for (size_t i = 0; i < modules.size(); ++i)
    {
        if (modules[i].stableIndex != i)
        {
            return Failure(
                AssemblyPresentationStatus::InvalidTopology,
                "module stable index does not match presentation order",
                static_cast<uint32_t>(i));
        }
        if (!ValidLocal(modules[i].localTransform, config))
        {
            return Failure(
                AssemblyPresentationStatus::InvalidLocalTransform,
                "module local transform is invalid",
                static_cast<uint32_t>(i));
        }
        if (!Compose(
                root,
                modules[i].localTransform,
                moduleWorldTransforms[i]))
        {
            return Failure(
                AssemblyPresentationStatus::ArithmeticOverflow,
                "module world transform cannot be represented",
                static_cast<uint32_t>(i));
        }
    }

    for (size_t i = 0; i < movingParts.size(); ++i)
    {
        if (movingParts[i].stableIndex != i ||
            movingParts[i].moduleIndex >= modules.size())
        {
            return Failure(
                AssemblyPresentationStatus::InvalidTopology,
                "moving-part ownership does not match presentation topology",
                static_cast<uint32_t>(i),
                true);
        }
        if (!ValidLocal(movingPartLocalTransforms[i], config))
        {
            return Failure(
                AssemblyPresentationStatus::InvalidLocalTransform,
                "moving-part local transform is invalid",
                static_cast<uint32_t>(i),
                true);
        }
        if (!Compose(
                root,
                movingPartLocalTransforms[i],
                movingPartWorldTransforms[i]))
        {
            return Failure(
                AssemblyPresentationStatus::ArithmeticOverflow,
                "moving-part world transform cannot be represented",
                static_cast<uint32_t>(i),
                true);
        }
    }

    AssemblyPresentationResult result;
    result.status = AssemblyPresentationStatus::Success;
    return result;
}

} // namespace scene
