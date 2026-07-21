#pragma once

#include "../asset/cooked_assembly.h"
#include "../asset/cooked_collision.h"
#include "../core/types.h"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace scene
{

enum class InteriorCollisionStatus : uint8_t
{
    Success,
    InvalidArgument,
    MissingResource,
    DuplicateShape,
    ResourceLimitExceeded,
    PenetrationUnresolved,
    AllocationFailure,
    InternalError
};

const char* InteriorCollisionStatusName(InteriorCollisionStatus status);

struct InteriorCollisionBox
{
    core::Vec3d minimum;
    core::Vec3d maximum;
    uint64_t stableId = 0;
    asset::CollisionSurfaceFlags surfaceFlags =
        asset::CollisionSurfaceFlags::None;
};

struct InteriorCapsule
{
    core::Vec3d center;
    double radius = 0.35;
    double halfSegment = 0.45;
};

struct InteriorCapsuleOverlap
{
    uint64_t stableId = 0;
    core::Vec3d normal;
    double depth = 0.0;
    asset::CollisionSurfaceFlags surfaceFlags =
        asset::CollisionSurfaceFlags::None;
};

struct InteriorCapsuleSweep
{
    InteriorCollisionStatus status = InteriorCollisionStatus::InvalidArgument;
    bool hit = false;
    bool startedOverlapping = false;
    double fraction = 1.0;
    core::Vec3d normal;
    uint64_t stableId = 0;
    asset::CollisionSurfaceFlags surfaceFlags =
        asset::CollisionSurfaceFlags::None;

    bool Succeeded() const { return status == InteriorCollisionStatus::Success; }
};

struct InteriorLocomotionConfig
{
    double skinWidth = 0.02;
    double minimumMoveDistance = 1.0e-5;
    double maximumStepHeight = 0.35;
    double groundProbeDistance = 0.08;
    double maximumSlopeRadians = 0.8726646259971648; // 50 degrees
    uint32_t maximumSlideIterations = 6;
    uint32_t maximumDepenetrationIterations = 8;
};

struct InteriorCapsuleMotion
{
    InteriorCollisionStatus status = InteriorCollisionStatus::InvalidArgument;
    core::Vec3d center;
    core::Vec3d achievedDisplacement;
    core::Vec3d groundNormal;
    uint64_t groundStableId = 0;
    uint32_t slideIterations = 0;
    uint32_t depenetrationIterations = 0;
    bool grounded = false;
    bool blocked = false;
    bool stepped = false;
    bool depenetrated = false;

    bool Succeeded() const { return status == InteriorCollisionStatus::Success; }
};

struct InteriorCollisionWorldLimits
{
    uint64_t maxBoxes = 1'000'000ull;
};

class InteriorCollisionWorld;

struct InteriorCollisionWorldBuildResult
{
    InteriorCollisionStatus status = InteriorCollisionStatus::InvalidArgument;
    std::shared_ptr<const InteriorCollisionWorld> world;
    std::string error;

    bool Succeeded() const
    {
        return status == InteriorCollisionStatus::Success && world != nullptr;
    }
};

class InteriorCollisionWorld final
{
public:
    size_t BoxCount() const { return m_boxes.size(); }
    std::span<const InteriorCollisionBox> Boxes() const { return m_boxes; }

    InteriorCollisionStatus OverlapCapsule(
        const InteriorCapsule& capsule,
        double inflation,
        std::vector<InteriorCapsuleOverlap>& overlaps) const;

    InteriorCapsuleSweep SweepCapsule(
        const InteriorCapsule& capsule,
        const core::Vec3d& displacement,
        double inflation = 0.0) const;

    InteriorCapsuleMotion MoveCapsule(
        const InteriorCapsule& capsule,
        const core::Vec3d& displacement,
        const InteriorLocomotionConfig& config = {}) const;

private:
    explicit InteriorCollisionWorld(std::vector<InteriorCollisionBox> boxes)
        : m_boxes(std::move(boxes)) {}

    std::vector<InteriorCollisionBox> m_boxes;

    friend InteriorCollisionWorldBuildResult BuildInteriorCollisionWorld(
        std::span<const InteriorCollisionBox>,
        const InteriorCollisionWorldLimits&);
};

InteriorCollisionWorldBuildResult BuildInteriorCollisionWorld(
    std::span<const InteriorCollisionBox> boxes,
    const InteriorCollisionWorldLimits& limits = {});

InteriorCollisionWorldBuildResult BuildAssemblyCollisionWorld(
    const asset::CookedAssembly& assembly,
    const std::map<std::string, std::shared_ptr<const asset::CookedCollision>>&
        collisionByLocator,
    const InteriorCollisionWorldLimits& limits = {});

bool IsWalkableInteriorSurface(
    const core::Vec3d& normal,
    asset::CollisionSurfaceFlags surfaceFlags,
    double maximumSlopeRadians);

} // namespace scene
