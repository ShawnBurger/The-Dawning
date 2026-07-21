#include "test_framework.h"
#include "scene/assembly_collision_runtime.h"

#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

constexpr asset::CollisionSurfaceFlags kWalkable =
    asset::CollisionSurfaceFlags::Walkable;

scene::InteriorCollisionBox Box(
    uint64_t id,
    core::Vec3d minimum,
    core::Vec3d maximum,
    asset::CollisionSurfaceFlags flags = asset::CollisionSurfaceFlags::None)
{
    scene::InteriorCollisionBox box;
    box.stableId = id;
    box.minimum = minimum;
    box.maximum = maximum;
    box.surfaceFlags = flags;
    return box;
}

std::shared_ptr<const scene::InteriorCollisionWorld> World(
    std::initializer_list<scene::InteriorCollisionBox> boxes)
{
    const std::vector<scene::InteriorCollisionBox> values(boxes);
    const scene::InteriorCollisionWorldBuildResult built =
        scene::BuildInteriorCollisionWorld(values);
    CHECK(built.Succeeded());
    if (!built.Succeeded())
        throw std::runtime_error("test collision world failed to build");
    return built.world;
}

scene::InteriorCapsule StandingCapsule(core::Vec3d center = { 0.0, 0.82, 0.0 })
{
    scene::InteriorCapsule capsule;
    capsule.center = center;
    capsule.radius = 0.35;
    capsule.halfSegment = 0.45;
    return capsule;
}

} // namespace

TEST_CASE(InteriorCollisionWorld_BuildIsStableAndRejectsInvalidIdentity)
{
    const std::vector<scene::InteriorCollisionBox> boxes = {
        Box(9, { -1, -1, -1 }, { 1, 1, 1 }),
        Box(2, { 2, -1, -1 }, { 3, 1, 1 })
    };
    const auto built = scene::BuildInteriorCollisionWorld(boxes);
    CHECK(built.Succeeded());
    CHECK_EQ(built.world->BoxCount(), 2u);
    CHECK_EQ(built.world->Boxes()[0].stableId, 2u);
    CHECK_EQ(built.world->Boxes()[1].stableId, 9u);

    const std::vector<scene::InteriorCollisionBox> duplicate = {
        Box(1, { -1, -1, -1 }, { 1, 1, 1 }),
        Box(1, { 2, -1, -1 }, { 3, 1, 1 })
    };
    CHECK_EQ(scene::BuildInteriorCollisionWorld(duplicate).status,
             scene::InteriorCollisionStatus::DuplicateShape);
    const std::vector<scene::InteriorCollisionBox> invalid = {
        Box(1, { 1, -1, -1 }, { 1, 1, 1 })
    };
    CHECK_EQ(scene::BuildInteriorCollisionWorld(invalid).status,
             scene::InteriorCollisionStatus::InvalidArgument);
    const std::vector<scene::InteriorCollisionBox> excessive = {
        Box(1, { -1, -1, -1 }, { 1.0e13, 1, 1 })
    };
    CHECK_EQ(scene::BuildInteriorCollisionWorld(excessive).status,
             scene::InteriorCollisionStatus::InvalidArgument);
}

TEST_CASE(InteriorCollisionQueries_RejectUnsafeArithmeticRanges)
{
    const auto world = World({
        Box(1, { -10.0, -0.2, -10.0 }, { 10.0, 0.0, 10.0 }, kWalkable)
    });
    scene::InteriorCapsule capsule = StandingCapsule();
    CHECK_EQ(world->SweepCapsule(capsule, { 1.0e13, 0.0, 0.0 }).status,
             scene::InteriorCollisionStatus::InvalidArgument);
    CHECK_EQ(world->MoveCapsule(capsule, { 1.0e13, 0.0, 0.0 }).status,
             scene::InteriorCollisionStatus::InvalidArgument);
    std::vector<scene::InteriorCapsuleOverlap> overlaps;
    CHECK_EQ(world->OverlapCapsule(capsule, 1.0e13, overlaps),
             scene::InteriorCollisionStatus::InvalidArgument);
}

TEST_CASE(InteriorCapsuleSweep_IsContinuousAndUsesRoundedCornerDistance)
{
    const auto wall = World({ Box(7, { 1.0, -2.0, -2.0 }, { 1.2, 2.0, 2.0 }) });
    scene::InteriorCapsule capsule = StandingCapsule({ 0.0, 0.0, 0.0 });
    capsule.radius = 0.25;
    const auto fast = wall->SweepCapsule(capsule, { 5.0, 0.0, 0.0 });
    CHECK(fast.Succeeded());
    CHECK(fast.hit);
    CHECK_APPROX_EPS(fast.fraction, 0.15, 1.0e-10);
    CHECK_APPROX_EPS(fast.normal.x, -1.0, 1.0e-10);
    CHECK_EQ(fast.stableId, 7u);

    const auto corner = World({ Box(3, { 1.0, -2.0, 1.0 }, { 2.0, 2.0, 2.0 }) });
    capsule.radius = 0.5;
    const auto rounded = corner->SweepCapsule(capsule, { 2.0, 0.0, 2.0 });
    CHECK(rounded.hit);
    CHECK_APPROX_EPS(rounded.fraction, 0.3232233047033631, 1.0e-9);
    CHECK(rounded.fraction > 0.30);
}

TEST_CASE(InteriorCapsuleSweep_TangentAwayAndStableTiesAreDeterministic)
{
    const auto world = World({
        Box(9, { 1.0, -2.0, -1.0 }, { 1.2, 2.0, 1.0 }),
        Box(2, { 1.0, -2.0, -1.0 }, { 1.2, 2.0, 1.0 })
    });
    scene::InteriorCapsule capsule = StandingCapsule({ 0.65, 0.0, 0.0 });
    capsule.radius = 0.35;
    const auto away = world->SweepCapsule(capsule, { -1.0, 0.0, 0.0 });
    CHECK(away.Succeeded());
    CHECK(!away.hit);

    capsule.center.x = 0.0;
    const auto toward = world->SweepCapsule(capsule, { 2.0, 0.0, 0.0 });
    CHECK(toward.Succeeded());
    CHECK(toward.hit);
    CHECK_EQ(toward.stableId, 2u);
}

TEST_CASE(InteriorCapsuleOverlap_ReportsStableDepenetrationNormalAndDepth)
{
    const auto world = World({ Box(4, { -1.0, -1.0, -1.0 }, { 1.0, 1.0, 1.0 }) });
    scene::InteriorCapsule capsule = StandingCapsule({ 1.2, 0.0, 0.0 });
    capsule.radius = 0.35;
    capsule.halfSegment = 0.2;
    std::vector<scene::InteriorCapsuleOverlap> overlaps;
    CHECK_EQ(world->OverlapCapsule(capsule, 0.0, overlaps),
               scene::InteriorCollisionStatus::Success);
    CHECK_EQ(overlaps.size(), 1u);
    CHECK_APPROX_EPS(overlaps[0].normal.x, 1.0, 1.0e-12);
    CHECK_APPROX_EPS(overlaps[0].depth, 0.15, 1.0e-12);
    CHECK_EQ(overlaps[0].stableId, 4u);

    const auto motion = world->MoveCapsule(capsule, { 0.0, 0.0, 0.0 });
    CHECK(motion.Succeeded());
    CHECK(motion.depenetrated);
    CHECK(motion.center.x > 1.35);
}

TEST_CASE(InteriorLocomotion_BoundedRecoveryReportsUnresolvedPenetration)
{
    const auto world = World({
        Box(1, { -1.0, -2.0, -1.0 }, { 0.2, 2.0, 1.0 }),
        Box(2, { -0.2, -2.0, -1.0 }, { 1.0, 2.0, 1.0 })
    });
    scene::InteriorLocomotionConfig config;
    config.maximumDepenetrationIterations = 1;
    const auto motion = world->MoveCapsule(
        StandingCapsule({ 0.0, 0.0, 0.0 }), { 0.0, 0.0, 0.0 }, config);
    CHECK_EQ(motion.status,
             scene::InteriorCollisionStatus::PenetrationUnresolved);
    CHECK(!motion.Succeeded());
}

TEST_CASE(InteriorLocomotion_SlidesAlongWallsWithoutTunneling)
{
    const auto world = World({
        Box(1, { -10.0, -0.2, -10.0 }, { 10.0, 0.0, 10.0 }, kWalkable),
        Box(2, { 1.0, 0.0, -10.0 }, { 1.2, 3.0, 10.0 })
    });
    const auto motion = world->MoveCapsule(
        StandingCapsule(), { 2.0, 0.0, 1.0 });
    CHECK(motion.Succeeded());
    CHECK(motion.blocked);
    CHECK(motion.grounded);
    CHECK_APPROX_EPS(motion.center.x, 0.63, 1.0e-8);
    CHECK_APPROX_EPS(motion.center.z, 1.0, 1.0e-8);
    CHECK_EQ(motion.groundStableId, 1u);
}

TEST_CASE(InteriorLocomotion_StepsSmallObstaclesAndRejectsTallOnes)
{
    const auto low = World({
        Box(1, { -10.0, -0.2, -2.0 }, { 10.0, 0.0, 2.0 }, kWalkable),
        Box(2, { 1.0, 0.0, -1.0 }, { 1.5, 0.25, 1.0 }, kWalkable)
    });
    const auto stepped = low->MoveCapsule(
        StandingCapsule(), { 2.0, 0.0, 0.0 });
    CHECK(stepped.Succeeded());
    CHECK(stepped.stepped);
    CHECK(stepped.grounded);
    CHECK(stepped.center.x > 1.5);
    CHECK_APPROX_EPS(stepped.center.y, 0.82, 1.0e-8);

    const auto tall = World({
        Box(1, { -10.0, -0.2, -2.0 }, { 10.0, 0.0, 2.0 }, kWalkable),
        Box(2, { 1.0, 0.0, -1.0 }, { 1.5, 0.65, 1.0 }, kWalkable)
    });
    const auto blocked = tall->MoveCapsule(
        StandingCapsule(), { 2.0, 0.0, 0.0 });
    CHECK(blocked.Succeeded());
    CHECK(!blocked.stepped);
    CHECK(blocked.blocked);
    CHECK_APPROX_EPS(blocked.center.x, 0.63, 1.0e-8);
}

TEST_CASE(InteriorLocomotion_WalkableSlopeClassificationIsExplicit)
{
    const double angle45 = 0.7853981633974483;
    const core::Vec3d normal45{ std::sin(angle45), std::cos(angle45), 0.0 };
    CHECK(scene::IsWalkableInteriorSurface(normal45, kWalkable, 0.8726646259971648));
    CHECK(!scene::IsWalkableInteriorSurface(normal45, kWalkable, 0.6981317007977318));
    CHECK(!scene::IsWalkableInteriorSurface(
        { 0.0, 1.0, 0.0 }, asset::CollisionSurfaceFlags::None,
        0.8726646259971648));
    CHECK(!scene::IsWalkableInteriorSurface(
        { 1.0, 0.0, 0.0 }, kWalkable, 0.8726646259971648));
}

TEST_CASE(InteriorLocomotion_UnobstructedPartitioningIsDeterministic)
{
    const auto world = World({
        Box(1, { -100.0, -0.2, -100.0 }, { 100.0, 0.0, 100.0 }, kWalkable)
    });
    const scene::InteriorCapsule start = StandingCapsule();
    const auto whole = world->MoveCapsule(start, { 1.0, 0.0, 2.0 });
    CHECK(whole.Succeeded());
    const auto first = world->MoveCapsule(start, { 0.5, 0.0, 1.0 });
    CHECK(first.Succeeded());
    scene::InteriorCapsule secondStart = start;
    secondStart.center = first.center;
    const auto second = world->MoveCapsule(secondStart, { 0.5, 0.0, 1.0 });
    CHECK(second.Succeeded());
    CHECK_APPROX_EPS(whole.center.x, second.center.x, 1.0e-12);
    CHECK_APPROX_EPS(whole.center.y, second.center.y, 1.0e-12);
    CHECK_APPROX_EPS(whole.center.z, second.center.z, 1.0e-12);
}

TEST_CASE(AssemblyCollisionWorld_RequiresConcretePackagesAndPublishesLocalAabbs)
{
    asset::CookedAssembly assembly;
    assembly.modules.resize(1);
    assembly.modules[0].collisionSource = "collision://room";
    assembly.modules[0].transform.positionMeters = { 3.0, 0.0, 4.0 };
    assembly.modules[0].transform.rotationEulerDegrees = { 0.0, 90.0, 0.0 };

    CHECK_EQ(scene::BuildAssemblyCollisionWorld(assembly, {}).status,
             scene::InteriorCollisionStatus::MissingResource);

    auto package = std::make_shared<asset::CookedCollision>();
    package->collisionId = "room";
    asset::CookedCollisionBox source;
    source.id = "wall";
    source.centerMeters = { 1.0, 0.0, 0.0 };
    source.halfExtentsMeters = { 2.0, 1.0, 0.5 };
    package->boxes.push_back(source);
    const std::map<std::string, std::shared_ptr<const asset::CookedCollision>> packages = {
        { "collision://room", package }
    };
    const auto built = scene::BuildAssemblyCollisionWorld(assembly, packages);
    CHECK(built.Succeeded());
    CHECK_EQ(built.world->BoxCount(), 1u);
    const auto& box = built.world->Boxes()[0];
    CHECK_APPROX_EPS(box.minimum.x, 2.5, 1.0e-5);
    CHECK_APPROX_EPS(box.maximum.x, 3.5, 1.0e-5);
    CHECK_APPROX_EPS(box.minimum.z, 1.0, 1.0e-5);
    CHECK_APPROX_EPS(box.maximum.z, 5.0, 1.0e-5);

    scene::InteriorCollisionWorldLimits zeroLimit;
    zeroLimit.maxBoxes = 0;
    CHECK_EQ(scene::BuildAssemblyCollisionWorld(
                 assembly, packages, zeroLimit).status,
             scene::InteriorCollisionStatus::ResourceLimitExceeded);

    asset::CookedAssembly invalidAssembly = assembly;
    invalidAssembly.modules[0].transform.scale[0] = 0.0;
    CHECK_EQ(scene::BuildAssemblyCollisionWorld(
                 invalidAssembly, packages).status,
             scene::InteriorCollisionStatus::InvalidArgument);
}

TEST_CASE(InteriorCollisionStatusNamesRemainStable)
{
    CHECK_EQ(std::string(scene::InteriorCollisionStatusName(
                 scene::InteriorCollisionStatus::PenetrationUnresolved)),
             std::string("penetration_unresolved"));
    CHECK_EQ(std::string(scene::InteriorCollisionStatusName(
                 static_cast<scene::InteriorCollisionStatus>(255))),
             std::string("unknown"));
}
