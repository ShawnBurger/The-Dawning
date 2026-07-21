#include "assembly_collision_runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace scene
{
namespace
{

constexpr double kEpsilon = 1.0e-10;
constexpr double kMaximumQueryMagnitude = 1.0e12;
constexpr double kDegreesToRadians =
    3.141592653589793238462643383279502884 / 180.0;

bool Finite(double value) { return std::isfinite(value); }

bool Finite(const core::Vec3d& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Bounded(double value)
{
    return Finite(value) && std::abs(value) <= kMaximumQueryMagnitude;
}

bool Bounded(const core::Vec3d& value)
{
    return Bounded(value.x) && Bounded(value.y) && Bounded(value.z);
}

double Component(const core::Vec3d& value, uint32_t axis)
{
    return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

void SetComponent(core::Vec3d& value, uint32_t axis, double component)
{
    if (axis == 0) value.x = component;
    else if (axis == 1) value.y = component;
    else value.z = component;
}

bool ValidCapsule(const InteriorCapsule& capsule)
{
    return Bounded(capsule.center) && Bounded(capsule.radius) &&
           Bounded(capsule.halfSegment) && capsule.radius > 0.0 &&
           capsule.halfSegment >= 0.0;
}

bool ValidBox(const InteriorCollisionBox& box)
{
    return Bounded(box.minimum) && Bounded(box.maximum) &&
           box.minimum.x < box.maximum.x &&
           box.minimum.y < box.maximum.y &&
           box.minimum.z < box.maximum.z;
}

bool ValidAssemblyTransform(const asset::AssemblyTransform& transform)
{
    for (double value : transform.positionMeters)
        if (!Bounded(value))
            return false;
    for (double value : transform.rotationEulerDegrees)
        if (!Bounded(value))
            return false;
    for (double value : transform.scale)
        if (!Bounded(value) || value <= 0.0)
            return false;
    return true;
}

core::Vec3d CapsuleBoxSeparation(
    const InteriorCapsule& capsule,
    const InteriorCollisionBox& box)
{
    core::Vec3d separation;
    if (capsule.center.x < box.minimum.x)
        separation.x = capsule.center.x - box.minimum.x;
    else if (capsule.center.x > box.maximum.x)
        separation.x = capsule.center.x - box.maximum.x;

    const double segmentMinimum = capsule.center.y - capsule.halfSegment;
    const double segmentMaximum = capsule.center.y + capsule.halfSegment;
    if (segmentMaximum < box.minimum.y)
        separation.y = segmentMaximum - box.minimum.y;
    else if (segmentMinimum > box.maximum.y)
        separation.y = segmentMinimum - box.maximum.y;

    if (capsule.center.z < box.minimum.z)
        separation.z = capsule.center.z - box.minimum.z;
    else if (capsule.center.z > box.maximum.z)
        separation.z = capsule.center.z - box.maximum.z;
    return separation;
}

InteriorCapsuleOverlap CapsuleBoxOverlap(
    const InteriorCapsule& capsule,
    const InteriorCollisionBox& box,
    double radius)
{
    InteriorCapsuleOverlap result;
    result.stableId = box.stableId;
    result.surfaceFlags = box.surfaceFlags;
    const core::Vec3d separation = CapsuleBoxSeparation(capsule, box);
    const double distanceSquared = separation.LengthSq();
    if (distanceSquared > kEpsilon)
    {
        const double distance = std::sqrt(distanceSquared);
        result.normal = separation / distance;
        result.depth = radius - distance;
        return result;
    }

    const double segmentMinimum = capsule.center.y - capsule.halfSegment;
    const double segmentMaximum = capsule.center.y + capsule.halfSegment;
    const std::array<double, 6> translations = {
        (box.minimum.x - radius) - capsule.center.x,
        (box.maximum.x + radius) - capsule.center.x,
        (box.minimum.y - radius) - segmentMaximum,
        (box.maximum.y + radius) - segmentMinimum,
        (box.minimum.z - radius) - capsule.center.z,
        (box.maximum.z + radius) - capsule.center.z
    };
    size_t best = 0;
    for (size_t i = 1; i < translations.size(); ++i)
    {
        if (std::abs(translations[i]) < std::abs(translations[best]))
            best = i;
    }
    const uint32_t axis = static_cast<uint32_t>(best / 2);
    SetComponent(result.normal, axis, translations[best] < 0.0 ? -1.0 : 1.0);
    result.depth = std::abs(translations[best]);
    return result;
}

void AddBreakpoint(
    std::vector<double>& points,
    double start,
    double displacement,
    double boundary)
{
    if (std::abs(displacement) <= kEpsilon)
        return;
    const double value = (boundary - start) / displacement;
    if (value > 0.0 && value < 1.0 && Finite(value))
        points.push_back(value);
}

struct LinearComponent
{
    double constant = 0.0;
    double slope = 0.0;
};

LinearComponent AxisDistance(
    double start,
    double displacement,
    double minimum,
    double maximum,
    double midpoint)
{
    const double value = start + displacement * midpoint;
    if (value < minimum)
        return { start - minimum, displacement };
    if (value > maximum)
        return { start - maximum, displacement };
    return {};
}

LinearComponent VerticalDistance(
    const InteriorCapsule& capsule,
    double displacement,
    const InteriorCollisionBox& box,
    double midpoint)
{
    const double center = capsule.center.y + displacement * midpoint;
    if (center + capsule.halfSegment < box.minimum.y)
    {
        return {
            capsule.center.y + capsule.halfSegment - box.minimum.y,
            displacement
        };
    }
    if (center - capsule.halfSegment > box.maximum.y)
    {
        return {
            capsule.center.y - capsule.halfSegment - box.maximum.y,
            displacement
        };
    }
    return {};
}

bool EarliestQuadraticRoot(
    double a,
    double b,
    double c,
    double intervalMinimum,
    double intervalMaximum,
    double& root)
{
    constexpr double tolerance = 1.0e-12;
    if (std::abs(a) <= tolerance)
    {
        if (std::abs(b) <= tolerance)
            return false;
        const double candidate = -c / b;
        if (candidate >= intervalMinimum - tolerance &&
            candidate <= intervalMaximum + tolerance)
        {
            root = std::clamp(candidate, intervalMinimum, intervalMaximum);
            return true;
        }
        return false;
    }
    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant < -tolerance)
        return false;
    const double squareRoot = std::sqrt(std::max(0.0, discriminant));
    const double first = (-b - squareRoot) / (2.0 * a);
    const double second = (-b + squareRoot) / (2.0 * a);
    for (double candidate : { first, second })
    {
        if (candidate >= intervalMinimum - tolerance &&
            candidate <= intervalMaximum + tolerance)
        {
            root = std::clamp(candidate, intervalMinimum, intervalMaximum);
            return true;
        }
    }
    return false;
}

bool SweepCapsuleBox(
    const InteriorCapsule& capsule,
    const core::Vec3d& displacement,
    double radius,
    const InteriorCollisionBox& box,
    double& fraction,
    core::Vec3d& normal,
    bool& startedOverlapping)
{
    const InteriorCapsuleOverlap initial =
        CapsuleBoxOverlap(capsule, box, radius);
    if (initial.depth > kEpsilon)
    {
        fraction = 0.0;
        normal = initial.normal;
        startedOverlapping = true;
        return true;
    }

    std::vector<double> points = { 0.0, 1.0 };
    points.reserve(8);
    AddBreakpoint(points, capsule.center.x, displacement.x, box.minimum.x);
    AddBreakpoint(points, capsule.center.x, displacement.x, box.maximum.x);
    AddBreakpoint(points, capsule.center.z, displacement.z, box.minimum.z);
    AddBreakpoint(points, capsule.center.z, displacement.z, box.maximum.z);
    AddBreakpoint(
        points,
        capsule.center.y + capsule.halfSegment,
        displacement.y,
        box.minimum.y);
    AddBreakpoint(
        points,
        capsule.center.y - capsule.halfSegment,
        displacement.y,
        box.maximum.y);
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end(), [](double lhs, double rhs) {
        return std::abs(lhs - rhs) <= 1.0e-12;
    }), points.end());

    for (size_t i = 0; i + 1 < points.size(); ++i)
    {
        const double intervalMinimum = points[i];
        const double intervalMaximum = points[i + 1];
        const double midpoint = 0.5 * (intervalMinimum + intervalMaximum);
        const LinearComponent x = AxisDistance(
            capsule.center.x, displacement.x,
            box.minimum.x, box.maximum.x, midpoint);
        const LinearComponent y = VerticalDistance(
            capsule, displacement.y, box, midpoint);
        const LinearComponent z = AxisDistance(
            capsule.center.z, displacement.z,
            box.minimum.z, box.maximum.z, midpoint);
        const double a = x.slope * x.slope + y.slope * y.slope +
                         z.slope * z.slope;
        const double b = 2.0 * (x.constant * x.slope +
                                y.constant * y.slope +
                                z.constant * z.slope);
        const double c = x.constant * x.constant +
                         y.constant * y.constant +
                         z.constant * z.constant - radius * radius;
        double candidate = 0.0;
        if (!EarliestQuadraticRoot(
                a, b, c, intervalMinimum, intervalMaximum, candidate))
            continue;

        InteriorCapsule atHit = capsule;
        atHit.center += displacement * candidate;
        const core::Vec3d separation = CapsuleBoxSeparation(atHit, box);
        const double distance = separation.Length();
        if (distance <= kEpsilon)
            continue;
        const core::Vec3d candidateNormal = separation / distance;
        if (candidate <= kEpsilon && displacement.Dot(candidateNormal) >= -kEpsilon)
            continue;
        fraction = candidate;
        normal = candidateNormal;
        return true;
    }
    return false;
}

bool ValidConfig(const InteriorLocomotionConfig& config)
{
    return Bounded(config.skinWidth) && config.skinWidth >= 0.0 &&
           Bounded(config.minimumMoveDistance) && config.minimumMoveDistance > 0.0 &&
           Bounded(config.maximumStepHeight) && config.maximumStepHeight >= 0.0 &&
           Bounded(config.groundProbeDistance) && config.groundProbeDistance >= 0.0 &&
           Finite(config.maximumSlopeRadians) && config.maximumSlopeRadians >= 0.0 &&
           config.maximumSlopeRadians < 0.5 * 3.14159265358979323846 &&
           config.maximumSlideIterations > 0 &&
           config.maximumDepenetrationIterations > 0;
}

struct StepResult
{
    InteriorCollisionStatus status = InteriorCollisionStatus::Success;
    bool succeeded = false;
    core::Vec3d center;
    core::Vec3d groundNormal;
    uint64_t groundStableId = 0;
};

StepResult TryStep(
    const InteriorCollisionWorld& world,
    const InteriorCapsule& capsule,
    const core::Vec3d& desired,
    const InteriorLocomotionConfig& config)
{
    StepResult result;
    if (config.maximumStepHeight <= 0.0)
        return result;

    InteriorCapsule raised = capsule;
    const core::Vec3d up{ 0.0, config.maximumStepHeight, 0.0 };
    if (!Bounded(raised.center + up))
        return result;
    const InteriorCapsuleSweep upHit =
        world.SweepCapsule(raised, up, config.skinWidth);
    if (!upHit.Succeeded())
    {
        result.status = upHit.status;
        return result;
    }
    if (upHit.startedOverlapping ||
        (upHit.hit && upHit.fraction < 1.0 - 1.0e-8))
        return result;
    raised.center += up;

    core::Vec3d forward{ desired.x, 0.0, desired.z };
    const double forwardLength = forward.Length();
    if (forwardLength <= config.minimumMoveDistance)
        return result;
    if (!Bounded(raised.center + forward))
        return result;
    const InteriorCapsuleSweep forwardHit =
        world.SweepCapsule(raised, forward, config.skinWidth);
    if (!forwardHit.Succeeded())
    {
        result.status = forwardHit.status;
        return result;
    }
    if (forwardHit.startedOverlapping)
        return result;
    const double forwardFraction = forwardHit.hit ? forwardHit.fraction : 1.0;
    if (forwardFraction * forwardLength <= config.minimumMoveDistance)
        return result;
    raised.center += forward * forwardFraction;

    const core::Vec3d down{
        0.0,
        -(config.maximumStepHeight + config.groundProbeDistance),
        0.0
    };
    const InteriorCapsuleSweep downHit =
        world.SweepCapsule(raised, down, config.skinWidth);
    if (!downHit.Succeeded())
    {
        result.status = downHit.status;
        return result;
    }
    if (!downHit.hit || downHit.startedOverlapping ||
        !IsWalkableInteriorSurface(
            downHit.normal, downHit.surfaceFlags, config.maximumSlopeRadians))
        return result;

    raised.center += down * downHit.fraction;
    result.succeeded = true;
    result.center = raised.center;
    result.groundNormal = downHit.normal;
    result.groundStableId = downHit.stableId;
    return result;
}

InteriorCollisionWorldBuildResult BuildFailure(
    InteriorCollisionStatus status,
    std::string error)
{
    InteriorCollisionWorldBuildResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

} // namespace

const char* InteriorCollisionStatusName(InteriorCollisionStatus status)
{
    switch (status)
    {
    case InteriorCollisionStatus::Success: return "success";
    case InteriorCollisionStatus::InvalidArgument: return "invalid_argument";
    case InteriorCollisionStatus::MissingResource: return "missing_resource";
    case InteriorCollisionStatus::DuplicateShape: return "duplicate_shape";
    case InteriorCollisionStatus::ResourceLimitExceeded: return "resource_limit_exceeded";
    case InteriorCollisionStatus::PenetrationUnresolved: return "penetration_unresolved";
    case InteriorCollisionStatus::AllocationFailure: return "allocation_failure";
    case InteriorCollisionStatus::InternalError: return "internal_error";
    }
    return "unknown";
}

bool IsWalkableInteriorSurface(
    const core::Vec3d& normal,
    asset::CollisionSurfaceFlags surfaceFlags,
    double maximumSlopeRadians)
{
    if (!Finite(normal) || !Finite(maximumSlopeRadians) ||
        maximumSlopeRadians < 0.0 ||
        maximumSlopeRadians >= 0.5 * 3.14159265358979323846 ||
        !asset::HasCollisionSurfaceFlag(
            surfaceFlags, asset::CollisionSurfaceFlags::Walkable))
        return false;
    const double length = normal.Length();
    if (length <= kEpsilon)
        return false;
    return normal.y / length >= std::cos(maximumSlopeRadians);
}

InteriorCollisionStatus InteriorCollisionWorld::OverlapCapsule(
    const InteriorCapsule& capsule,
    double inflation,
    std::vector<InteriorCapsuleOverlap>& overlaps) const
{
    overlaps.clear();
    if (!ValidCapsule(capsule) || !Bounded(inflation) || inflation < 0.0)
        return InteriorCollisionStatus::InvalidArgument;
    const double radius = capsule.radius + inflation;
    if (!Bounded(radius))
        return InteriorCollisionStatus::InvalidArgument;
    try
    {
        for (const InteriorCollisionBox& box : m_boxes)
        {
            InteriorCapsuleOverlap overlap = CapsuleBoxOverlap(capsule, box, radius);
            if (overlap.depth > kEpsilon)
                overlaps.push_back(overlap);
        }
        return InteriorCollisionStatus::Success;
    }
    catch (const std::bad_alloc&)
    {
        overlaps.clear();
        return InteriorCollisionStatus::AllocationFailure;
    }
}

InteriorCapsuleSweep InteriorCollisionWorld::SweepCapsule(
    const InteriorCapsule& capsule,
    const core::Vec3d& displacement,
    double inflation) const
{
    InteriorCapsuleSweep result;
    if (!ValidCapsule(capsule) || !Bounded(displacement) ||
        !Bounded(capsule.center + displacement) ||
        !Bounded(inflation) || inflation < 0.0)
        return result;
    const double radius = capsule.radius + inflation;
    if (!Bounded(radius))
        return result;
    result.status = InteriorCollisionStatus::Success;
    try
    {
        for (const InteriorCollisionBox& box : m_boxes)
        {
            double fraction = 1.0;
            core::Vec3d normal;
            bool startedOverlapping = false;
            if (!SweepCapsuleBox(
                    capsule, displacement, radius, box,
                    fraction, normal, startedOverlapping))
                continue;
            const bool earlier = !result.hit || fraction < result.fraction - kEpsilon;
            const bool tie = result.hit &&
                             std::abs(fraction - result.fraction) <= kEpsilon &&
                             box.stableId < result.stableId;
            if (earlier || tie)
            {
                result.hit = true;
                result.fraction = fraction;
                result.normal = normal;
                result.stableId = box.stableId;
                result.surfaceFlags = box.surfaceFlags;
                result.startedOverlapping = startedOverlapping;
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        result = {};
        result.status = InteriorCollisionStatus::AllocationFailure;
    }
    return result;
}

InteriorCapsuleMotion InteriorCollisionWorld::MoveCapsule(
    const InteriorCapsule& source,
    const core::Vec3d& displacement,
    const InteriorLocomotionConfig& config) const
{
    InteriorCapsuleMotion result;
    result.center = source.center;
    if (!ValidCapsule(source) || !Bounded(displacement) ||
        !Bounded(source.center + displacement) || !ValidConfig(config))
        return result;

    InteriorCapsule capsule = source;
    std::vector<InteriorCapsuleOverlap> overlaps;
    for (uint32_t iteration = 0;
         iteration < config.maximumDepenetrationIterations;
         ++iteration)
    {
        const InteriorCollisionStatus overlapStatus =
            OverlapCapsule(capsule, config.skinWidth, overlaps);
        if (overlapStatus != InteriorCollisionStatus::Success)
        {
            result.status = overlapStatus;
            return result;
        }
        if (overlaps.empty())
            break;
        const auto deepest = std::max_element(
            overlaps.begin(), overlaps.end(),
            [](const InteriorCapsuleOverlap& lhs, const InteriorCapsuleOverlap& rhs) {
                if (std::abs(lhs.depth - rhs.depth) > kEpsilon)
                    return lhs.depth < rhs.depth;
                return lhs.stableId > rhs.stableId;
            });
        capsule.center += deepest->normal *
            (deepest->depth + config.minimumMoveDistance);
        if (!Bounded(capsule.center))
            return result;
        result.depenetrated = true;
        result.depenetrationIterations = iteration + 1;
    }
    if (OverlapCapsule(capsule, config.skinWidth, overlaps) !=
            InteriorCollisionStatus::Success || !overlaps.empty())
    {
        result.status = InteriorCollisionStatus::PenetrationUnresolved;
        return result;
    }

    core::Vec3d remaining = displacement;
    for (uint32_t iteration = 0;
         iteration < config.maximumSlideIterations &&
         remaining.Length() > config.minimumMoveDistance;
         ++iteration)
    {
        result.slideIterations = iteration + 1;
        const InteriorCapsule before = capsule;
        const InteriorCapsuleSweep hit =
            SweepCapsule(capsule, remaining, config.skinWidth);
        if (!hit.Succeeded())
        {
            result.status = hit.status;
            return result;
        }
        if (hit.startedOverlapping)
        {
            result.status = InteriorCollisionStatus::PenetrationUnresolved;
            return result;
        }
        if (!hit.hit)
        {
            capsule.center += remaining;
            remaining = {};
            break;
        }

        capsule.center += remaining * hit.fraction;
        result.blocked = true;
        const StepResult step = TryStep(*this, before, remaining, config);
        if (step.status != InteriorCollisionStatus::Success)
        {
            result.status = step.status;
            return result;
        }
        if (step.succeeded)
        {
            const double regularProgress =
                std::hypot(capsule.center.x - before.center.x,
                           capsule.center.z - before.center.z);
            const double stepProgress =
                std::hypot(step.center.x - before.center.x,
                           step.center.z - before.center.z);
            if (stepProgress > regularProgress + config.minimumMoveDistance)
            {
                capsule.center = step.center;
                result.stepped = true;
                result.grounded = true;
                result.groundNormal = step.groundNormal;
                result.groundStableId = step.groundStableId;
                remaining = {};
                break;
            }
        }

        remaining *= 1.0 - hit.fraction;
        const double intoSurface = remaining.Dot(hit.normal);
        if (intoSurface < 0.0)
            remaining -= hit.normal * intoSurface;
    }

    if (remaining.Length() > config.minimumMoveDistance)
        result.blocked = true;

    if (!result.grounded && displacement.y <= 0.0 &&
        config.groundProbeDistance > 0.0)
    {
        const core::Vec3d down{ 0.0, -config.groundProbeDistance, 0.0 };
        const InteriorCapsuleSweep ground =
            SweepCapsule(capsule, down, config.skinWidth);
        if (!ground.Succeeded())
        {
            result.status = ground.status;
            return result;
        }
        if (ground.hit && !ground.startedOverlapping &&
            IsWalkableInteriorSurface(
                ground.normal, ground.surfaceFlags, config.maximumSlopeRadians))
        {
            capsule.center += down * ground.fraction;
            result.grounded = true;
            result.groundNormal = ground.normal;
            result.groundStableId = ground.stableId;
        }
    }

    result.status = InteriorCollisionStatus::Success;
    result.center = capsule.center;
    result.achievedDisplacement = capsule.center - source.center;
    return result;
}

InteriorCollisionWorldBuildResult BuildInteriorCollisionWorld(
    std::span<const InteriorCollisionBox> boxes,
    const InteriorCollisionWorldLimits& limits)
{
    if (boxes.empty())
        return BuildFailure(
            InteriorCollisionStatus::InvalidArgument,
            "collision world requires at least one box");
    if (boxes.size() > limits.maxBoxes)
        return BuildFailure(
            InteriorCollisionStatus::ResourceLimitExceeded,
            "collision world exceeds the box limit");
    try
    {
        std::vector<InteriorCollisionBox> stable(boxes.begin(), boxes.end());
        for (const InteriorCollisionBox& box : stable)
        {
            if (!ValidBox(box))
                return BuildFailure(
                    InteriorCollisionStatus::InvalidArgument,
                    "collision world box is invalid");
        }
        std::sort(stable.begin(), stable.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.stableId < rhs.stableId;
        });
        for (size_t i = 1; i < stable.size(); ++i)
        {
            if (stable[i - 1].stableId == stable[i].stableId)
                return BuildFailure(
                    InteriorCollisionStatus::DuplicateShape,
                    "collision world stable shape ID is duplicated");
        }
        InteriorCollisionWorldBuildResult result;
        result.status = InteriorCollisionStatus::Success;
        result.world = std::shared_ptr<const InteriorCollisionWorld>(
            new InteriorCollisionWorld(std::move(stable)));
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return BuildFailure(
            InteriorCollisionStatus::AllocationFailure,
            "out of memory while building collision world");
    }
}

InteriorCollisionWorldBuildResult BuildAssemblyCollisionWorld(
    const asset::CookedAssembly& assembly,
    const std::map<std::string, std::shared_ptr<const asset::CookedCollision>>&
        collisionByLocator,
    const InteriorCollisionWorldLimits& limits)
{
    try
    {
        if (limits.maxBoxes == 0)
        {
            return BuildFailure(
                InteriorCollisionStatus::ResourceLimitExceeded,
                "assembly collision world box limit is zero");
        }
        uint64_t count = 0;
        for (const asset::AssemblyModule& module : assembly.modules)
        {
            if (!ValidAssemblyTransform(module.transform))
            {
                return BuildFailure(
                    InteriorCollisionStatus::InvalidArgument,
                    "assembly module collision transform is invalid");
            }
            const auto found = collisionByLocator.find(module.collisionSource);
            if (found == collisionByLocator.end() || !found->second)
            {
                return BuildFailure(
                    InteriorCollisionStatus::MissingResource,
                    "assembly collision locator has no concrete package: " +
                        module.collisionSource);
            }
            if (found->second->boxes.size() > limits.maxBoxes - count)
            {
                return BuildFailure(
                    InteriorCollisionStatus::ResourceLimitExceeded,
                    "assembly collision world exceeds the box limit");
            }
            count += found->second->boxes.size();
        }

        std::vector<InteriorCollisionBox> boxes;
        boxes.reserve(static_cast<size_t>(count));
        for (size_t moduleIndex = 0; moduleIndex < assembly.modules.size(); ++moduleIndex)
        {
            const asset::AssemblyModule& module = assembly.modules[moduleIndex];
            const asset::CookedCollision& collision =
                *collisionByLocator.at(module.collisionSource);
            const float pitch = static_cast<float>(
                std::remainder(module.transform.rotationEulerDegrees[0], 360.0) *
                kDegreesToRadians);
            const float yaw = static_cast<float>(
                std::remainder(module.transform.rotationEulerDegrees[1], 360.0) *
                kDegreesToRadians);
            const float roll = static_cast<float>(
                std::remainder(module.transform.rotationEulerDegrees[2], 360.0) *
                kDegreesToRadians);
            const core::Quatf rotation =
                core::Quatf::FromEuler(pitch, yaw, roll).Normalized();

            for (size_t shapeIndex = 0; shapeIndex < collision.boxes.size(); ++shapeIndex)
            {
                const asset::CookedCollisionBox& source = collision.boxes[shapeIndex];
                const core::Vec3f scaledCenter{
                    static_cast<float>(source.centerMeters[0] * module.transform.scale[0]),
                    static_cast<float>(source.centerMeters[1] * module.transform.scale[1]),
                    static_cast<float>(source.centerMeters[2] * module.transform.scale[2])
                };
                const core::Vec3f center = rotation.Rotate(scaledCenter);
                const core::Vec3f scaledExtent{
                    static_cast<float>(source.halfExtentsMeters[0] * module.transform.scale[0]),
                    static_cast<float>(source.halfExtentsMeters[1] * module.transform.scale[1]),
                    static_cast<float>(source.halfExtentsMeters[2] * module.transform.scale[2])
                };
                const core::Vec3f axisX = rotation.Rotate({ scaledExtent.x, 0.0f, 0.0f });
                const core::Vec3f axisY = rotation.Rotate({ 0.0f, scaledExtent.y, 0.0f });
                const core::Vec3f axisZ = rotation.Rotate({ 0.0f, 0.0f, scaledExtent.z });
                const core::Vec3d extent{
                    std::abs(axisX.x) + std::abs(axisY.x) + std::abs(axisZ.x),
                    std::abs(axisX.y) + std::abs(axisY.y) + std::abs(axisZ.y),
                    std::abs(axisX.z) + std::abs(axisY.z) + std::abs(axisZ.z)
                };
                const core::Vec3d position{
                    module.transform.positionMeters[0] + center.x,
                    module.transform.positionMeters[1] + center.y,
                    module.transform.positionMeters[2] + center.z
                };
                InteriorCollisionBox box;
                box.minimum = position - extent;
                box.maximum = position + extent;
                box.stableId =
                    (static_cast<uint64_t>(moduleIndex) << 32) |
                    static_cast<uint64_t>(shapeIndex);
                box.surfaceFlags = source.surfaceFlags;
                boxes.push_back(box);
            }
        }
        return BuildInteriorCollisionWorld(boxes, limits);
    }
    catch (const std::out_of_range&)
    {
        return BuildFailure(
            InteriorCollisionStatus::MissingResource,
            "assembly collision locator disappeared during publication");
    }
    catch (const std::bad_alloc&)
    {
        return BuildFailure(
            InteriorCollisionStatus::AllocationFailure,
            "out of memory while publishing assembly collision");
    }
}

} // namespace scene
