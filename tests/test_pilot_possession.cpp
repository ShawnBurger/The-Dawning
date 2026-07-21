#include "test_framework.h"
#include "gameplay/pilot_possession.h"
#include "render/camera.h"

#include <cmath>
#include <limits>
#include <memory>

namespace
{

struct Fixture
{
    Fixture()
    {
        assembly.sourceManifestSha256.bytes[0] = 0x5a;

        asset::AssemblyModule cockpit;
        cockpit.id = "cockpit";
        cockpit.role = asset::AssemblyModuleRole::Interior;
        cockpit.transform.positionMeters = { 0.0, 0.0, 1.0 };
        assembly.modules.push_back(cockpit);

        asset::AssemblySocket spawn;
        spawn.id = "pilot_exit_spawn";
        spawn.moduleIndex = 0;
        spawn.type = asset::AssemblySocketType::Spawn;
        spawn.positionMeters = { 0.0, -0.5, 0.0 };
        spawn.forward = { 0.0, 0.0, 1.0 };
        spawn.up = { 0.0, 1.0, 0.0 };
        assembly.sockets.push_back(spawn);

        asset::AssemblySocket seat;
        seat.id = "pilot_seat_anchor";
        seat.moduleIndex = 0;
        seat.type = asset::AssemblySocketType::Interaction;
        seat.positionMeters = { 0.0, 0.6, 1.0 };
        seat.forward = { 0.0, 0.0, 1.0 };
        seat.up = { 0.0, 1.0, 0.0 };
        assembly.sockets.push_back(seat);

        asset::AssemblyInteraction interaction;
        interaction.id = "pilot_seat";
        interaction.type = asset::AssemblyInteractionType::Seat;
        interaction.moduleIndex = 0;
        interaction.socketIndex = 1;
        interaction.states = { "available", "occupied" };
        interaction.initialStateIndex = 0;
        assembly.interactions.push_back(interaction);

        scene::InteriorCollisionBox floor;
        floor.stableId = 1;
        floor.minimum = { -10.0, -0.6, -10.0 };
        floor.maximum = { 10.0, -0.5, 10.0 };
        floor.surfaceFlags = asset::CollisionSurfaceFlags::Walkable;
        snapshot.topologySha256 = assembly.sourceManifestSha256;
        snapshot.revision = 3;
        snapshot.collisionWorld =
            scene::BuildInteriorCollisionWorld({ &floor, 1 }).world;
    }

    gameplay::PilotSeatBinding Binding() const
    {
        const auto result = gameplay::ResolvePilotSeatBinding(assembly);
        CHECK(result.Succeeded());
        return result.binding;
    }

    asset::CookedAssembly assembly;
    scene::AssemblyInteriorCollisionSnapshot snapshot;
};

bool SameState(
    const gameplay::PlayerPossessionState& a,
    const gameplay::PlayerPossessionState& b)
{
    return a.context == b.context &&
           a.localViewYawDegrees == b.localViewYawDegrees &&
           a.localViewPitchDegrees == b.localViewPitchDegrees &&
           a.onFoot.capsule.center == b.onFoot.capsule.center &&
           a.onFoot.capsule.radius == b.onFoot.capsule.radius &&
           a.onFoot.capsule.halfSegment == b.onFoot.capsule.halfSegment &&
           a.onFoot.velocity == b.onFoot.velocity &&
           a.onFoot.collisionTopologySha256 ==
               b.onFoot.collisionTopologySha256 &&
           a.onFoot.collisionRevision == b.onFoot.collisionRevision &&
           a.onFoot.groundStableId == b.onFoot.groundStableId &&
           a.onFoot.grounded == b.onFoot.grounded &&
           a.onFoot.jumpHeld == b.onFoot.jumpHeld;
}

void CheckVec3d(
    const core::Vec3d& actual,
    const core::Vec3d& expected,
    double epsilon = 1.0e-5)
{
    CHECK_APPROX_EPS(actual.x, expected.x, epsilon);
    CHECK_APPROX_EPS(actual.y, expected.y, epsilon);
    CHECK_APPROX_EPS(actual.z, expected.z, epsilon);
}

void CheckVec3f(
    const core::Vec3f& actual,
    const core::Vec3f& expected,
    float epsilon = 1.0e-5f)
{
    CHECK_APPROX_EPS(actual.x, expected.x, epsilon);
    CHECK_APPROX_EPS(actual.y, expected.y, epsilon);
    CHECK_APPROX_EPS(actual.z, expected.z, epsilon);
}

} // namespace

TEST_CASE(PilotPossession_ResolvesAuthoredSeatAndFloorSpawnExactly)
{
    Fixture fixture;
    const auto result = gameplay::ResolvePilotSeatBinding(fixture.assembly);
    CHECK(result.Succeeded());
    CHECK_EQ(result.binding.interactionIndex, 0u);
    CHECK_EQ(result.binding.availableStateIndex, 0u);
    CHECK_EQ(result.binding.occupiedStateIndex, 1u);
    CHECK_EQ(result.binding.spawn.stableIndex, 0u);
    CHECK_EQ(result.binding.seat.stableIndex, 1u);
    CheckVec3d(result.binding.spawn.position, { 0.0, -0.5, 1.0 });
    CheckVec3d(result.binding.seat.position, { 0.0, 0.6, 2.0 });
    CheckVec3f(result.binding.spawn.forward, { 0.0f, 0.0f, 1.0f });
    CheckVec3f(result.binding.spawn.up, { 0.0f, 1.0f, 0.0f });
}

TEST_CASE(PilotPossession_ComposesSocketFramesThroughModuleTransform)
{
    Fixture fixture;
    fixture.assembly.modules[0].transform.positionMeters = {
        10.0, 20.0, 30.0
    };
    fixture.assembly.modules[0].transform.rotationEulerDegrees = {
        0.0, 90.0, 0.0
    };
    fixture.assembly.modules[0].transform.scale = { 2.0, 2.0, 2.0 };

    const auto result = gameplay::ResolvePilotSeatBinding(fixture.assembly);
    CHECK(result.Succeeded());
    CheckVec3d(result.binding.spawn.position, { 10.0, 19.0, 30.0 });
    CheckVec3d(result.binding.seat.position, { 12.0, 21.2, 30.0 });
    CheckVec3f(result.binding.spawn.forward, { 1.0f, 0.0f, 0.0f });
    CheckVec3f(result.binding.spawn.up, { 0.0f, 1.0f, 0.0f });

    fixture.assembly.modules[0].transform.rotationEulerDegrees[1] =
        360000000090.0;
    const auto wrapped = gameplay::ResolvePilotSeatBinding(fixture.assembly);
    CHECK(wrapped.Succeeded());
    CheckVec3d(wrapped.binding.seat.position, { 12.0, 21.2, 30.0 });
    CheckVec3f(wrapped.binding.spawn.forward, { 1.0f, 0.0f, 0.0f });
}

TEST_CASE(PilotPossession_ExitAndEntryStageCompleteAtomicCandidates)
{
    Fixture fixture;
    const auto binding = fixture.Binding();
    const auto initialized = gameplay::InitializeShipPossession(
        binding, binding.occupiedStateIndex);
    CHECK(initialized.Succeeded());
    CHECK(gameplay::OwnsShipInput(initialized.state));
    CHECK(!gameplay::OwnsOnFootInput(initialized.state));
    CHECK(!gameplay::OwnsShipInput({}));
    CHECK(!gameplay::OwnsOnFootInput({}));

    const auto exited = gameplay::StagePilotExit(
        binding,
        initialized.state,
        binding.occupiedStateIndex,
        fixture.snapshot);
    CHECK(exited.Succeeded());
    CHECK(gameplay::OwnsOnFootInput(exited.state));
    CHECK(!gameplay::OwnsShipInput(exited.state));
    CheckVec3d(exited.state.onFoot.capsule.center, { 0.0, 0.32, 1.0 });
    CHECK_EQ(exited.state.onFoot.collisionRevision, fixture.snapshot.revision);
    CHECK(exited.state.onFoot.collisionTopologySha256 ==
          fixture.snapshot.topologySha256);

    const auto entered = gameplay::StagePilotEntry(
        binding,
        exited.state,
        binding.availableStateIndex,
        fixture.snapshot);
    CHECK(entered.Succeeded());
    CHECK(gameplay::OwnsShipInput(entered.state));
    CHECK(!gameplay::OwnsOnFootInput(entered.state));
    CHECK(entered.state.onFoot.capsule.center ==
          exited.state.onFoot.capsule.center);

    const auto duplicateEntry = gameplay::StagePilotEntry(
        binding,
        entered.state,
        binding.availableStateIndex,
        fixture.snapshot);
    CHECK_EQ(
        duplicateEntry.status,
        gameplay::PilotPossessionStatus::WrongContext);
    CHECK(SameState(duplicateEntry.state, entered.state));
}

TEST_CASE(PilotPossession_RejectedTransitionsPreserveEverySourceField)
{
    Fixture fixture;
    const auto binding = fixture.Binding();
    const auto ship = gameplay::InitializeShipPossession(
        binding, binding.occupiedStateIndex).state;

    auto rejected = gameplay::StagePilotExit(
        binding, ship, binding.availableStateIndex, fixture.snapshot);
    CHECK_EQ(rejected.status, gameplay::PilotPossessionStatus::SeatUnavailable);
    CHECK(SameState(rejected.state, ship));

    rejected = gameplay::StagePilotExit(
        binding,
        gameplay::StagePilotExit(
            binding, ship, binding.occupiedStateIndex, fixture.snapshot).state,
        binding.occupiedStateIndex,
        fixture.snapshot);
    CHECK_EQ(rejected.status, gameplay::PilotPossessionStatus::WrongContext);

    auto differentTopology = fixture.snapshot;
    differentTopology.topologySha256.bytes[0] ^= 0xff;
    rejected = gameplay::StagePilotExit(
        binding, ship, binding.occupiedStateIndex, differentTopology);
    CHECK_EQ(rejected.status, gameplay::PilotPossessionStatus::TopologyMismatch);
    CHECK(SameState(rejected.state, ship));

    auto missingWorld = fixture.snapshot;
    missingWorld.collisionWorld.reset();
    rejected = gameplay::StagePilotExit(
        binding, ship, binding.occupiedStateIndex, missingWorld);
    CHECK_EQ(rejected.status, gameplay::PilotPossessionStatus::InvalidArgument);
    CHECK(SameState(rejected.state, ship));
}

TEST_CASE(PilotPossession_BlockedSpawnCannotReleaseShipControl)
{
    Fixture fixture;
    const auto binding = fixture.Binding();
    const auto ship = gameplay::InitializeShipPossession(
        binding, binding.occupiedStateIndex).state;

    scene::InteriorCollisionBox obstruction;
    obstruction.stableId = 2;
    obstruction.minimum = { -0.5, -0.5, 0.5 };
    obstruction.maximum = { 0.5, 1.2, 1.5 };
    auto blocked = fixture.snapshot;
    blocked.collisionWorld =
        scene::BuildInteriorCollisionWorld({ &obstruction, 1 }).world;
    const auto result = gameplay::StagePilotExit(
        binding, ship, binding.occupiedStateIndex, blocked);
    CHECK_EQ(result.status, gameplay::PilotPossessionStatus::SpawnBlocked);
    CHECK(SameState(result.state, ship));
}

TEST_CASE(PilotPossession_EntryRequiresCurrentTopologyRangeAndFacing)
{
    Fixture fixture;
    const auto binding = fixture.Binding();
    const auto ship = gameplay::InitializeShipPossession(
        binding, binding.occupiedStateIndex).state;
    const auto onFoot = gameplay::StagePilotExit(
        binding, ship, binding.occupiedStateIndex, fixture.snapshot).state;

    auto unavailable = gameplay::StagePilotEntry(
        binding, onFoot, binding.occupiedStateIndex, fixture.snapshot);
    CHECK_EQ(
        unavailable.status,
        gameplay::PilotPossessionStatus::SeatUnavailable);
    CHECK(SameState(unavailable.state, onFoot));

    auto far = onFoot;
    far.onFoot.capsule.center.z = -20.0;
    const auto outOfRange = gameplay::StagePilotEntry(
        binding, far, binding.availableStateIndex, fixture.snapshot);
    CHECK_EQ(outOfRange.status, gameplay::PilotPossessionStatus::OutOfRange);
    CHECK(SameState(outOfRange.state, far));

    auto backward = onFoot;
    backward.localViewYawDegrees = 180.0f;
    const auto notFacing = gameplay::StagePilotEntry(
        binding, backward, binding.availableStateIndex, fixture.snapshot);
    CHECK_EQ(notFacing.status, gameplay::PilotPossessionStatus::NotFacing);
    CHECK(SameState(notFacing.state, backward));

    auto future = onFoot;
    future.onFoot.collisionRevision = fixture.snapshot.revision + 1;
    const auto stale = gameplay::StagePilotEntry(
        binding, future, binding.availableStateIndex, fixture.snapshot);
    CHECK_EQ(
        stale.status,
        gameplay::PilotPossessionStatus::StaleCollisionSnapshot);
    CHECK(SameState(stale.state, future));

    auto wrongPlayerTopology = onFoot;
    wrongPlayerTopology.onFoot.collisionTopologySha256.bytes[0] ^= 0xff;
    const auto mismatchedPlayer = gameplay::StagePilotEntry(
        binding,
        wrongPlayerTopology,
        binding.availableStateIndex,
        fixture.snapshot);
    CHECK_EQ(
        mismatchedPlayer.status,
        gameplay::PilotPossessionStatus::TopologyMismatch);
    CHECK(SameState(mismatchedPlayer.state, wrongPlayerTopology));
}

TEST_CASE(PilotPossession_LookIsBoundedAndFailureIsAtomic)
{
    Fixture fixture;
    const auto binding = fixture.Binding();
    const auto ship = gameplay::InitializeShipPossession(
        binding, binding.occupiedStateIndex).state;
    const auto onFoot = gameplay::StagePilotExit(
        binding, ship, binding.occupiedStateIndex, fixture.snapshot).state;

    const auto looked = gameplay::ApplyOnFootLook(onFoot, 100.0f, -1000.0f);
    CHECK(looked.Succeeded());
    CHECK_APPROX(looked.state.localViewYawDegrees, 15.0f);
    CHECK_APPROX(looked.state.localViewPitchDegrees, 89.0f);
    CHECK_APPROX_EPS(
        gameplay::OnFootViewForward(looked.state).Length(), 1.0, 1.0e-9);

    const auto invalid = gameplay::ApplyOnFootLook(
        onFoot, (std::numeric_limits<float>::quiet_NaN)(), 0.0f);
    CHECK_EQ(invalid.status, gameplay::PilotPossessionStatus::InvalidArgument);
    CHECK(SameState(invalid.state, onFoot));

    const auto wrongContext = gameplay::ApplyOnFootLook(ship, 1.0f, 1.0f);
    CHECK_EQ(
        wrongContext.status,
        gameplay::PilotPossessionStatus::WrongContext);
    CHECK(SameState(wrongContext.state, ship));

    auto invalidPitch = onFoot;
    invalidPitch.localViewPitchDegrees = 90.0f;
    const auto rejectedPitch = gameplay::ApplyOnFootLook(
        invalidPitch, 0.0f, 0.0f);
    CHECK_EQ(
        rejectedPitch.status,
        gameplay::PilotPossessionStatus::InvalidArgument);
    CHECK(SameState(rejectedPitch.state, invalidPitch));
}

TEST_CASE(PilotPossession_RootRoundTripPreservesLargeWorldPrecisionAndBasis)
{
    ecs::Transform root;
    root.position = { 1.0e12 + 0.25, -2.0e12 + 0.5, 3.0e12 - 0.75 };
    root.rotation = core::Quatf::FromEuler(0.37f, -0.82f, 0.41f);
    root.scale = { 2.0f, 2.0f, 2.0f };
    CHECK(gameplay::IsValidPossessionRoot(root));

    const core::Vec3d local{ 4.25, -1.5, 8.75 };
    core::Vec3d world{ 9.0, 9.0, 9.0 };
    core::Vec3d roundTrip;
    CHECK(gameplay::AssemblyLocalPointToWorld(root, local, world));
    CHECK(gameplay::WorldPointToAssemblyLocal(root, world, roundTrip));
    CheckVec3d(roundTrip, local, 5.0e-4);

    const core::Vec3d localDirection =
        core::Vec3d{ 0.2, -0.3, 0.9 }.Normalized();
    core::Vec3f worldDirection;
    core::Vec3d directionRoundTrip;
    CHECK(gameplay::AssemblyLocalDirectionToWorld(
        root, localDirection, worldDirection));
    CHECK(gameplay::WorldDirectionToAssemblyLocal(
        root, worldDirection, directionRoundTrip));
    CheckVec3d(directionRoundTrip, localDirection, 1.0e-5);

    ecs::Transform nonuniform = root;
    nonuniform.scale.z = 2.1f;
    core::Vec3d unchanged{ 7.0, 8.0, 9.0 };
    CHECK(!gameplay::AssemblyLocalPointToWorld(
        nonuniform, local, unchanged));
    CheckVec3d(unchanged, { 7.0, 8.0, 9.0 });

    ecs::Transform nonunit = root;
    nonunit.rotation.w *= 2.0f;
    CHECK(!gameplay::IsValidPossessionRoot(nonunit));

    ecs::Transform tinyNonuniform = root;
    tinyNonuniform.scale = { 1.0e-5f, 2.0e-5f, 3.0e-5f };
    CHECK(!gameplay::IsValidPossessionRoot(tinyNonuniform));

    ecs::Transform effectivelySingular = root;
    effectivelySingular.scale = { 1.0e-8f, 1.0e-8f, 1.0e-8f };
    CHECK(!gameplay::IsValidPossessionRoot(effectivelySingular));

    ecs::Transform nonfinite = root;
    nonfinite.position.x =
        (std::numeric_limits<double>::quiet_NaN)();
    CHECK(!gameplay::IsValidPossessionRoot(nonfinite));

    core::Vec3d hugeLocal{ 1.0e8, 0.0, 0.0 };
    core::Vec3d preserved{ 2.0, 3.0, 4.0 };
    CHECK(!gameplay::AssemblyLocalPointToWorld(root, hugeLocal, preserved));
    CheckVec3d(preserved, { 2.0, 3.0, 4.0 });
}

TEST_CASE(PilotPossession_CameraCarriesLocalUpThroughRolledMovingRoot)
{
    Fixture fixture;
    const auto binding = fixture.Binding();
    const auto ship = gameplay::InitializeShipPossession(
        binding, binding.occupiedStateIndex).state;
    const auto onFoot = gameplay::StagePilotExit(
        binding, ship, binding.occupiedStateIndex, fixture.snapshot).state;

    ecs::Transform root;
    root.position = { 50000000.0, -40000000.0, 30000000.0 };
    root.rotation = core::Quatf::FromEuler(0.2f, 0.7f, 1.1f);
    const auto first = gameplay::BuildOnFootCameraPose(onFoot, root);
    CHECK(first.Succeeded());
    CheckVec3f(
        first.pose.forward,
        root.rotation.Rotate({ 0.0f, 0.0f, 1.0f }).Normalized());
    CHECK(std::abs(first.pose.forward.Dot(first.pose.up)) < 1.0e-5f);

    render::Camera camera;
    camera.Init({ 1.0, 2.0, 3.0 }, 10.0f, -5.0f);
    const core::Vec3d oldPosition = camera.Position();
    const core::Vec3f oldForward = camera.Forward();
    const core::Vec3f oldUp = camera.Up();
    CHECK(!camera.InitBasis({}, {}, { 0.0f, 1.0f, 0.0f }));
    CHECK(camera.Position() == oldPosition);
    CheckVec3f(camera.Forward(), oldForward);
    CheckVec3f(camera.Up(), oldUp);
    CHECK(!camera.InitBasis(
        {}, { 0.0f, 1.0f, 0.0f }, { 0.0f, 2.0f, 0.0f }));
    CHECK(camera.Position() == oldPosition);
    CheckVec3f(camera.Forward(), oldForward);
    CheckVec3f(camera.Up(), oldUp);
    CHECK(camera.InitBasis(
        first.pose.position, first.pose.forward, first.pose.up));
    CheckVec3f(camera.Forward(), first.pose.forward);
    CheckVec3f(camera.Up(), first.pose.up);
    CheckVec3f(
        camera.Up(),
        root.rotation.Rotate({ 0.0f, 1.0f, 0.0f }).Normalized());
    CHECK(std::abs(camera.Right().Dot(camera.Forward())) < 1.0e-5f);
    CHECK(std::abs(camera.Right().Dot(camera.Up())) < 1.0e-5f);
    const core::Mat4x4 expectedView = core::Mat4x4::LookAt(
        {}, first.pose.forward, first.pose.up);
    const core::Mat4x4 actualView = camera.ViewMatrix();
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            CHECK_APPROX_EPS(
                actualView(row, column), expectedView(row, column), 1.0e-6f);

    const auto localBefore = onFoot.onFoot.capsule.center;
    root.position += core::Vec3d{ 1000.0, 2000.0, -3000.0 };
    root.rotation = core::Quatf::FromEuler(-0.4f, 1.2f, -0.7f);
    const auto moved = gameplay::BuildOnFootCameraPose(onFoot, root);
    CHECK(moved.Succeeded());
    CHECK(moved.pose.position != first.pose.position);
    CHECK(onFoot.onFoot.capsule.center == localBefore);
}

TEST_CASE(PilotPossession_RejectsMalformedSeatSpawnAndStateTopology)
{
    Fixture fixture;

    auto malformed = fixture.assembly;
    malformed.sockets[0].type = asset::AssemblySocketType::Interaction;
    CHECK_EQ(
        gameplay::ResolvePilotSeatBinding(malformed).status,
        gameplay::PilotPossessionStatus::InvalidTopology);

    malformed = fixture.assembly;
    malformed.interactions[0].type = asset::AssemblyInteractionType::Console;
    CHECK_EQ(
        gameplay::ResolvePilotSeatBinding(malformed).status,
        gameplay::PilotPossessionStatus::InvalidTopology);

    malformed = fixture.assembly;
    malformed.interactions[0].states = { "available", "reserved" };
    CHECK_EQ(
        gameplay::ResolvePilotSeatBinding(malformed).status,
        gameplay::PilotPossessionStatus::InvalidTopology);

    malformed = fixture.assembly;
    malformed.sockets[0].up = { 1.0, 0.0, 0.0 };
    CHECK_EQ(
        gameplay::ResolvePilotSeatBinding(malformed).status,
        gameplay::PilotPossessionStatus::InvalidTopology);

    malformed = fixture.assembly;
    malformed.sockets.push_back(malformed.sockets[0]);
    CHECK_EQ(
        gameplay::ResolvePilotSeatBinding(malformed).status,
        gameplay::PilotPossessionStatus::InvalidTopology);

    malformed = fixture.assembly;
    malformed.modules[0].transform.positionMeters[0] = 1.0e8;
    CHECK_EQ(
        gameplay::ResolvePilotSeatBinding(malformed).status,
        gameplay::PilotPossessionStatus::InvalidTopology);

    auto forgedBinding = fixture.Binding();
    forgedBinding.spawn.stableIndex = forgedBinding.seat.stableIndex;
    CHECK_EQ(
        gameplay::InitializeShipPossession(
            forgedBinding, forgedBinding.occupiedStateIndex).status,
        gameplay::PilotPossessionStatus::InvalidArgument);
}

TEST_CASE(PilotPossession_StatusNamesRemainStable)
{
    CHECK_EQ(std::string(gameplay::PilotPossessionStatusName(
                 gameplay::PilotPossessionStatus::StaleCollisionSnapshot)),
             std::string("stale_collision_snapshot"));
    CHECK_EQ(std::string(gameplay::PilotPossessionStatusName(
                 gameplay::PilotPossessionStatus::InternalError)),
             std::string("internal_error"));
    CHECK_EQ(std::string(gameplay::PilotPossessionStatusName(
                 static_cast<gameplay::PilotPossessionStatus>(255))),
             std::string("unknown"));
}
