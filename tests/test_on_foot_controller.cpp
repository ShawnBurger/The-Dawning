#include "test_framework.h"
#include "gameplay/on_foot_controller.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{

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

scene::AssemblyInteriorCollisionSnapshot Snapshot(
    std::initializer_list<scene::InteriorCollisionBox> boxes,
    uint8_t topologyByte = 1,
    uint64_t revision = 1)
{
    const std::vector<scene::InteriorCollisionBox> values(boxes);
    scene::AssemblyInteriorCollisionSnapshot snapshot;
    snapshot.topologySha256.bytes[0] = topologyByte;
    snapshot.revision = revision;
    snapshot.collisionWorld = scene::BuildInteriorCollisionWorld(values).world;
    return snapshot;
}

scene::AssemblyInteriorCollisionSnapshot FloorSnapshot(
    uint8_t topologyByte = 1,
    uint64_t revision = 1)
{
    return Snapshot({
        Box(1, { -100.0, -0.2, -100.0 }, { 100.0, 0.0, 100.0 },
            asset::CollisionSurfaceFlags::Walkable)
    }, topologyByte, revision);
}

gameplay::OnFootState StandingState(core::Vec3d center = { 0.0, 0.82, 0.0 })
{
    gameplay::OnFootState state;
    state.capsule.center = center;
    state.capsule.radius = 0.35;
    state.capsule.halfSegment = 0.45;
    return state;
}

bool SameState(
    const gameplay::OnFootState& lhs,
    const gameplay::OnFootState& rhs)
{
    return lhs.capsule.center == rhs.capsule.center &&
           lhs.capsule.radius == rhs.capsule.radius &&
           lhs.capsule.halfSegment == rhs.capsule.halfSegment &&
           lhs.velocity == rhs.velocity &&
           lhs.collisionTopologySha256 == rhs.collisionTopologySha256 &&
           lhs.collisionRevision == rhs.collisionRevision &&
           lhs.groundStableId == rhs.groundStableId &&
           lhs.grounded == rhs.grounded && lhs.jumpHeld == rhs.jumpHeld;
}

gameplay::OnFootStepResult Step(
    const scene::AssemblyInteriorCollisionSnapshot& snapshot,
    const gameplay::OnFootState& state,
    double forward,
    double dt = 1.0 / 60.0)
{
    gameplay::OnFootCommand command;
    command.moveForward = forward;
    return gameplay::StepOnFootController(snapshot, state, command, dt);
}

} // namespace

TEST_CASE(OnFootController_ViewRelativeInputAcceleratesBrakesAndClampsDiagonal)
{
    const auto floor = FloorSnapshot();
    gameplay::OnFootState state = StandingState();
    gameplay::OnFootCommand command;
    command.moveForward = 1.0;
    command.viewForward = { 1.0, 0.0, 0.0 };
    auto result = gameplay::StepOnFootController(floor, state, command, 0.05);
    CHECK(result.Succeeded());
    CHECK(result.state.velocity.x > 1.1);
    CHECK_APPROX_EPS(result.state.velocity.z, 0.0, 1.0e-12);

    state = result.state;
    command.viewForward = { 0.0, 0.0, 1.0 };
    command.moveRight = 1.0;
    for (int i = 0; i < 120; ++i)
    {
        result = gameplay::StepOnFootController(
            floor, state, command, 1.0 / 60.0);
        CHECK(result.Succeeded());
        state = result.state;
    }
    CHECK(state.velocity.Length() <= 4.5 + 1.0e-9);

    command = {};
    const double beforeBrake = std::hypot(state.velocity.x, state.velocity.z);
    result = gameplay::StepOnFootController(floor, state, command, 0.05);
    CHECK(result.Succeeded());
    CHECK(std::hypot(result.state.velocity.x, result.state.velocity.z) <
          beforeBrake);
}

TEST_CASE(OnFootController_UnobstructedAccelerationIsPartitionInvariant)
{
    const auto floor = FloorSnapshot();
    const gameplay::OnFootState start = StandingState();
    gameplay::OnFootCommand command;
    command.moveForward = 1.0;

    const auto whole = gameplay::StepOnFootController(
        floor, start, command, 0.04);
    CHECK(whole.Succeeded());

    gameplay::OnFootState split = start;
    for (int i = 0; i < 4; ++i)
    {
        const auto part = gameplay::StepOnFootController(
            floor, split, command, 0.01);
        CHECK(part.Succeeded());
        split = part.state;
    }
    CHECK_APPROX_EPS(whole.state.capsule.center.x,
                     split.capsule.center.x, 1.0e-12);
    CHECK_APPROX_EPS(whole.state.capsule.center.z,
                     split.capsule.center.z, 1.0e-12);
    CHECK_APPROX_EPS(whole.state.velocity.z,
                     split.velocity.z, 1.0e-12);
}

TEST_CASE(OnFootController_SpeedCapsBrakingGravityAndJumpArePartitionInvariant)
{
    const auto floor = FloorSnapshot();
    gameplay::OnFootControllerConfig config;
    config.maximumTimeStepSeconds = 0.5;
    gameplay::OnFootCommand forward;
    forward.moveForward = 1.0;

    gameplay::OnFootState accelerated = StandingState();
    accelerated.velocity.z = 3.0;
    const auto capWhole = gameplay::StepOnFootController(
        floor, accelerated, forward, 0.2, config);
    CHECK(capWhole.Succeeded());
    gameplay::OnFootState capSplit = accelerated;
    for (int i = 0; i < 4; ++i)
    {
        const auto part = gameplay::StepOnFootController(
            floor, capSplit, forward, 0.05, config);
        CHECK(part.Succeeded());
        capSplit = part.state;
    }
    CHECK_APPROX_EPS(capWhole.state.capsule.center.z,
                     capSplit.capsule.center.z, 1.0e-12);
    CHECK_APPROX_EPS(capWhole.state.velocity.z,
                     capSplit.velocity.z, 1.0e-12);

    gameplay::OnFootState braking = StandingState();
    braking.velocity.z = 4.5;
    const auto brakeWhole = gameplay::StepOnFootController(
        floor, braking, {}, 0.2, config);
    CHECK(brakeWhole.Succeeded());
    gameplay::OnFootState brakeSplit = braking;
    for (int i = 0; i < 4; ++i)
    {
        const auto part = gameplay::StepOnFootController(
            floor, brakeSplit, {}, 0.05, config);
        CHECK(part.Succeeded());
        brakeSplit = part.state;
    }
    CHECK_APPROX_EPS(brakeWhole.state.capsule.center.z,
                     brakeSplit.capsule.center.z, 1.0e-12);
    CHECK_APPROX_EPS(brakeWhole.state.velocity.z,
                     brakeSplit.velocity.z, 1.0e-12);

    const auto distantFloor = Snapshot({
        Box(1, { -10.0, -1001.0, -10.0 }, { 10.0, -1000.0, 10.0 },
            asset::CollisionSurfaceFlags::Walkable)
    });
    const gameplay::OnFootState airborne = StandingState({ 0.0, 20.0, 0.0 });
    const auto gravityWhole = gameplay::StepOnFootController(
        distantFloor, airborne, {}, 0.05, config);
    CHECK(gravityWhole.Succeeded());
    gameplay::OnFootState gravitySplit = airborne;
    for (int i = 0; i < 5; ++i)
    {
        const auto part = gameplay::StepOnFootController(
            distantFloor, gravitySplit, {}, 0.01, config);
        CHECK(part.Succeeded());
        gravitySplit = part.state;
    }
    CHECK_APPROX_EPS(gravityWhole.state.capsule.center.y,
                     gravitySplit.capsule.center.y, 1.0e-12);
    CHECK_APPROX_EPS(gravityWhole.state.velocity.y,
                     gravitySplit.velocity.y, 1.0e-12);

    gameplay::OnFootCommand jump;
    jump.jumpDown = true;
    const gameplay::OnFootState grounded = StandingState();
    const auto jumpWhole = gameplay::StepOnFootController(
        floor, grounded, jump, 0.05, config);
    CHECK(jumpWhole.Succeeded());
    gameplay::OnFootState jumpSplit = grounded;
    for (int i = 0; i < 5; ++i)
    {
        const auto part = gameplay::StepOnFootController(
            floor, jumpSplit, jump, 0.01, config);
        CHECK(part.Succeeded());
        jumpSplit = part.state;
    }
    CHECK(jumpWhole.jumped);
    CHECK(!jumpSplit.grounded);
    CHECK_APPROX_EPS(jumpWhole.state.capsule.center.y,
                     jumpSplit.capsule.center.y, 1.0e-12);
    CHECK_APPROX_EPS(jumpWhole.state.velocity.y,
                     jumpSplit.velocity.y, 1.0e-12);
}

TEST_CASE(OnFootController_JumpIsGroundVerifiedAndRisingEdgeTriggered)
{
    const auto floor = FloorSnapshot();
    gameplay::OnFootState grounded = StandingState();
    gameplay::OnFootCommand command;
    command.jumpDown = true;
    auto jumped = gameplay::StepOnFootController(
        floor, grounded, command, 1.0 / 60.0);
    CHECK(jumped.Succeeded());
    CHECK(jumped.jumped);
    CHECK(jumped.state.velocity.y > 0.0);
    CHECK(!jumped.state.grounded);

    const auto held = gameplay::StepOnFootController(
        floor, jumped.state, command, 1.0 / 60.0);
    CHECK(held.Succeeded());
    CHECK(!held.jumped);
    CHECK(held.state.velocity.y < jumped.state.velocity.y);

    gameplay::OnFootState staleGround = StandingState({ 0.0, 5.0, 0.0 });
    staleGround.grounded = true;
    const auto rejectedJump = gameplay::StepOnFootController(
        floor, staleGround, command, 1.0 / 60.0);
    CHECK(rejectedJump.Succeeded());
    CHECK(!rejectedJump.jumped);
    CHECK(rejectedJump.state.velocity.y < 0.0);
}

TEST_CASE(OnFootController_LowCeilingCancelsUpwardVelocity)
{
    const auto room = Snapshot({
        Box(1, { -10.0, -0.2, -10.0 }, { 10.0, 0.0, 10.0 },
            asset::CollisionSurfaceFlags::Walkable),
        Box(2, { -10.0, 1.72, -10.0 }, { 10.0, 2.0, 10.0 })
    });
    gameplay::OnFootCommand jump;
    jump.jumpDown = true;
    const auto result = gameplay::StepOnFootController(
        room, StandingState(), jump, 0.05);
    CHECK(result.Succeeded());
    CHECK(result.jumped);
    CHECK(result.blocked);
    CHECK_APPROX_EPS(result.state.velocity.y, 0.0, 1.0e-12);
    CHECK(result.state.capsule.center.y < 0.91);
}

TEST_CASE(OnFootController_GravityClampsAtTerminalSpeedAndLands)
{
    const auto distantFloor = Snapshot({
        Box(1, { -10.0, -1001.0, -10.0 }, { 10.0, -1000.0, 10.0 },
            asset::CollisionSurfaceFlags::Walkable)
    });
    gameplay::OnFootState falling = StandingState({ 0.0, 20.0, 0.0 });
    for (int i = 0; i < 100; ++i)
    {
        const auto result = gameplay::StepOnFootController(
            distantFloor, falling, {}, 0.05);
        CHECK(result.Succeeded());
        falling = result.state;
    }
    CHECK_APPROX_EPS(falling.velocity.y, -55.0, 1.0e-9);

    const auto floor = FloorSnapshot();
    falling = StandingState({ 0.0, 3.0, 0.0 });
    for (int i = 0; i < 240 && !falling.grounded; ++i)
    {
        const auto result = gameplay::StepOnFootController(
            floor, falling, {}, 1.0 / 60.0);
        CHECK(result.Succeeded());
        falling = result.state;
    }
    CHECK(falling.grounded);
    CHECK_APPROX_EPS(falling.velocity.y, 0.0, 1.0e-12);
    CHECK_EQ(falling.groundStableId, 1u);
}

TEST_CASE(OnFootController_ContinuousSweepCannotTunnelThroughClosedDoor)
{
    const auto closed = Snapshot({
        Box(1, { -10.0, -0.2, -10.0 }, { 10.0, 0.0, 10.0 },
            asset::CollisionSurfaceFlags::Walkable),
        Box((uint64_t{ 1 } << 63) | 1,
            { -0.5, 0.0, 1.0 }, { 0.5, 2.1, 1.12 })
    });
    gameplay::OnFootState state = StandingState({ 0.0, 0.82, 0.55 });
    state.velocity.z = 100.0;
    gameplay::OnFootCommand command;
    command.moveForward = 1.0;
    command.sprint = true;
    const auto result = gameplay::StepOnFootController(
        closed, state, command, 0.05);
    CHECK(result.Succeeded());
    CHECK(result.blocked);
    CHECK(result.state.capsule.center.z < 0.64);
    CHECK(result.state.velocity.z < 2.0);
}

TEST_CASE(OnFootController_NewerIntrudingClosureDepenetratesAndReportsBlocked)
{
    const auto intruding = Snapshot({
        Box(1, { -10.0, -0.2, -10.0 }, { 10.0, 0.0, 10.0 },
            asset::CollisionSurfaceFlags::Walkable),
        Box((uint64_t{ 1 } << 63),
            { 1.0, 0.0, -1.0 }, { 1.2, 2.1, 1.0 })
    }, 1, 2);
    gameplay::OnFootState state = StandingState({ 0.9, 0.82, 0.0 });
    state.collisionTopologySha256 = intruding.topologySha256;
    state.collisionRevision = 1;
    const auto result = gameplay::StepOnFootController(
        intruding, state, {}, 1.0 / 60.0);
    CHECK(result.Succeeded());
    CHECK(result.blocked);
    CHECK(result.state.capsule.center.x < 0.64);
    CHECK_EQ(result.state.collisionRevision, 2u);
}

TEST_CASE(OnFootController_SlidesAlongWallsAndKeepsAirControlBounded)
{
    const auto world = Snapshot({
        Box(1, { -10.0, -0.2, -10.0 }, { 10.0, 0.0, 10.0 },
            asset::CollisionSurfaceFlags::Walkable),
        Box(2, { 1.0, 0.0, -10.0 }, { 1.2, 3.0, 10.0 })
    });
    gameplay::OnFootState state = StandingState({ 0.62, 0.82, 0.0 });
    gameplay::OnFootCommand command;
    command.moveRight = 1.0;
    command.moveForward = 1.0;
    auto result = gameplay::StepOnFootController(world, state, command, 0.05);
    CHECK(result.Succeeded());
    CHECK(result.blocked);
    CHECK(result.state.capsule.center.x < 0.64);
    CHECK(result.state.capsule.center.z > 0.0);

    state = StandingState({ 0.0, 5.0, 0.0 });
    for (int i = 0; i < 20; ++i)
    {
        result = gameplay::StepOnFootController(
            world, state, command, 0.05);
        CHECK(result.Succeeded());
        state = result.state;
    }
    CHECK(std::hypot(state.velocity.x, state.velocity.z) <= 6.5 + 1.0e-9);
}

TEST_CASE(OnFootController_InvalidAndStaleInputsAreExactlyAtomic)
{
    const auto snapshot = FloorSnapshot(1, 3);
    gameplay::OnFootState source = StandingState();
    gameplay::OnFootCommand invalid;
    invalid.moveForward = std::numeric_limits<double>::quiet_NaN();
    auto result = gameplay::StepOnFootController(
        snapshot, source, invalid, 1.0 / 60.0);
    CHECK_EQ(result.status, gameplay::OnFootControllerStatus::InvalidArgument);
    CHECK(SameState(result.state, source));

    result = gameplay::StepOnFootController(
        snapshot, source, {}, 0.051);
    CHECK_EQ(result.status, gameplay::OnFootControllerStatus::InvalidArgument);
    CHECK(SameState(result.state, source));

    invalid = {};
    invalid.moveForward = 1.0;
    invalid.viewForward = { 0.0, 1.0, 0.0 };
    result = gameplay::StepOnFootController(
        snapshot, source, invalid, 1.0 / 60.0);
    CHECK_EQ(result.status, gameplay::OnFootControllerStatus::InvalidArgument);
    CHECK(SameState(result.state, source));

    gameplay::OnFootControllerConfig invalidConfig;
    invalidConfig.locomotion.maximumSlideIterations = 0;
    result = gameplay::StepOnFootController(
        snapshot, source, {}, 1.0 / 60.0, invalidConfig);
    CHECK_EQ(result.status, gameplay::OnFootControllerStatus::InvalidArgument);
    CHECK(SameState(result.state, source));

    gameplay::OnFootState oversized = source;
    oversized.capsule.radius = 2.0;
    result = gameplay::StepOnFootController(
        snapshot, oversized, {}, 1.0 / 60.0);
    CHECK_EQ(result.status, gameplay::OnFootControllerStatus::InvalidArgument);
    CHECK(SameState(result.state, oversized));

    gameplay::OnFootState fastRise = source;
    fastRise.capsule.center.y = 5.0;
    fastRise.velocity.y = 1.0e9;
    result = gameplay::StepOnFootController(
        snapshot, fastRise, {}, 1.0 / 60.0);
    CHECK(result.Succeeded());
    CHECK(result.state.velocity.y <= 30.0);

    source.collisionTopologySha256 = snapshot.topologySha256;
    source.collisionRevision = 4;
    result = gameplay::StepOnFootController(
        snapshot, source, {}, 1.0 / 60.0);
    CHECK_EQ(result.status,
             gameplay::OnFootControllerStatus::StaleCollisionSnapshot);
    CHECK(SameState(result.state, source));

    const auto otherTopology = FloorSnapshot(2, 5);
    result = gameplay::StepOnFootController(
        otherTopology, source, {}, 1.0 / 60.0);
    CHECK_EQ(result.status,
             gameplay::OnFootControllerStatus::TopologyMismatch);
    CHECK(SameState(result.state, source));
}

TEST_CASE(OnFootController_UnresolvedPenetrationLeavesSourceUnchanged)
{
    const auto trapped = Snapshot({
        Box(1, { -1.0, -2.0, -1.0 }, { 0.2, 2.0, 1.0 }),
        Box(2, { -0.2, -2.0, -1.0 }, { 1.0, 2.0, 1.0 })
    });
    const gameplay::OnFootState source = StandingState({ 0.0, 0.0, 0.0 });
    gameplay::OnFootControllerConfig config;
    config.locomotion.maximumDepenetrationIterations = 1;
    const auto result = gameplay::StepOnFootController(
        trapped, source, {}, 1.0 / 60.0, config);
    CHECK_EQ(result.status, gameplay::OnFootControllerStatus::CollisionFailure);
    CHECK_EQ(result.collisionStatus,
             scene::InteriorCollisionStatus::PenetrationUnresolved);
    CHECK(SameState(result.state, source));
}

TEST_CASE(OnFootController_StatusNamesRemainStable)
{
    CHECK_EQ(std::string(gameplay::OnFootControllerStatusName(
                 gameplay::OnFootControllerStatus::StaleCollisionSnapshot)),
             std::string("stale_collision_snapshot"));
    CHECK_EQ(std::string(gameplay::OnFootControllerStatusName(
                 static_cast<gameplay::OnFootControllerStatus>(255))),
             std::string("unknown"));
}
