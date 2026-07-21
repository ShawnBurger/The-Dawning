#include "test_framework.h"
#include "scene/assembly_interior_runtime.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

asset::Sha256Digest HashText(std::string_view text)
{
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    return asset::ComputeSha256(std::span<const std::byte>(bytes, text.size()));
}

asset::AssemblyInteraction MovingInteraction(
    std::string id,
    asset::AssemblyInteractionType type,
    uint32_t socket,
    uint32_t movingPart,
    uint32_t portal)
{
    asset::AssemblyInteraction interaction;
    interaction.id = std::move(id);
    interaction.type = type;
    interaction.moduleIndex = 0;
    interaction.socketIndex = socket;
    interaction.states = { "closed", "opening", "open", "closing", "locked" };
    interaction.initialStateIndex = 0;
    interaction.movingPartIndex = movingPart;
    interaction.portalIndex = portal;
    return interaction;
}

struct Fixture
{
    Fixture()
    {
        assembly = std::make_shared<asset::CookedAssembly>();
        assembly->schemaVersion = 1;
        assembly->assetId = "ship.test.interior";
        assembly->sourceManifestSha256 = HashText("interior topology");

        asset::AssemblyModule module;
        module.id = "interior";
        assembly->modules.push_back(module);

        for (uint32_t i = 0; i < 3; ++i)
        {
            asset::AssemblySocket socket;
            socket.id = "socket-" + std::to_string(i);
            socket.moduleIndex = 0;
            socket.positionMeters = { 0.0, 0.0, 1.0 + i };
            socket.forward = { 0.0, 0.0, 1.0 };
            socket.up = { 0.0, 1.0, 0.0 };
            assembly->sockets.push_back(socket);
        }

        assembly->interactions.push_back(MovingInteraction(
            "outer_hatch", asset::AssemblyInteractionType::Hatch, 0, 0, 0));
        assembly->interactions.push_back(MovingInteraction(
            "inner_door", asset::AssemblyInteractionType::Door, 2, 1, 1));

        asset::AssemblyInteraction seat;
        seat.id = "pilot_seat";
        seat.type = asset::AssemblyInteractionType::Seat;
        seat.moduleIndex = 0;
        seat.socketIndex = 1;
        seat.states = { "available", "occupied" };
        seat.initialStateIndex = 0;
        assembly->interactions.push_back(seat);

        for (uint32_t i = 0; i < 2; ++i)
        {
            asset::AssemblyPortal portal;
            portal.id = i == 0 ? "outer_entry" : "cabin_entry";
            portal.socketA = i == 0 ? 0u : 2u;
            portal.socketB = i == 0 ? 0u : 2u;
            portal.closureInteraction = i;
            portal.sealable = true;
            portal.navLink = true;
            assembly->portals.push_back(portal);
        }

        asset::AssemblyMovingPart hatch;
        hatch.id = "hatch_leaf";
        hatch.moduleIndex = 0;
        hatch.interactionIndex = 0;
        hatch.motionType = asset::AssemblyMotionType::Rotational;
        hatch.axis = { 0.0, 1.0, 0.0 };
        hatch.travel = 90.0;
        assembly->movingParts.push_back(hatch);

        asset::AssemblyMovingPart door;
        door.id = "door_leaf";
        door.moduleIndex = 0;
        door.interactionIndex = 1;
        door.motionType = asset::AssemblyMotionType::Linear;
        door.axis = { 1.0, 0.0, 0.0 };
        door.travel = 1.0;
        assembly->movingParts.push_back(door);

        scene::PreparedAssemblyModule preparedModule;
        preparedModule.stableIndex = 0;
        preparedModule.worldTransform.position = { 10.0, 0.0, 0.0 };
        preparedModule.worldTransform.scale = { 2.0f, 1.0f, 1.0f };
        modules.push_back(preparedModule);

        scene::PreparedAssemblyMovingPart preparedHatch;
        preparedHatch.stableIndex = 0;
        preparedHatch.moduleIndex = 0;
        preparedHatch.interactionIndex = 0;
        preparedHatch.worldTransform.position = { 11.0, 0.0, 0.0 };
        movingParts.push_back(preparedHatch);

        scene::PreparedAssemblyMovingPart preparedDoor;
        preparedDoor.stableIndex = 1;
        preparedDoor.moduleIndex = 0;
        preparedDoor.interactionIndex = 1;
        preparedDoor.worldTransform.position = { 10.0, 0.0, 3.0 };
        movingParts.push_back(preparedDoor);
    }

    scene::AssemblyInteriorResult Initialize(scene::AssemblyInteriorRuntime& runtime)
    {
        return runtime.Initialize(assembly, modules, movingParts);
    }

    std::shared_ptr<asset::CookedAssembly> assembly;
    std::vector<scene::PreparedAssemblyModule> modules;
    std::vector<scene::PreparedAssemblyMovingPart> movingParts;
};

void CheckSameSnapshot(
    const scene::AssemblyInteriorSnapshot& a,
    const scene::AssemblyInteriorSnapshot& b)
{
    CHECK(a.topologySha256 == b.topologySha256);
    CHECK_EQ(a.interactions.size(), b.interactions.size());
    for (size_t i = 0; i < a.interactions.size(); ++i)
    {
        CHECK_EQ(a.interactions[i].stateIndex, b.interactions[i].stateIndex);
        CHECK_APPROX(
            a.interactions[i].motionProgress,
            b.interactions[i].motionProgress);
    }
}

} // namespace

TEST_CASE(AssemblyInterior_InitializesClosedTopologyAndShutsDownIdempotently)
{
    Fixture fixture;
    scene::AssemblyInteriorRuntime runtime;
    const auto initialized = fixture.Initialize(runtime);
    CHECK(initialized.Succeeded());
    CHECK(runtime.IsInitialized());
    CHECK_EQ(runtime.InteractionCount(), 3u);
    CHECK_EQ(runtime.PortalCount(), 2u);
    CHECK_EQ(runtime.MovingPartCount(), 2u);
    CHECK_EQ(runtime.InteractionIndex("outer_hatch"), 0u);
    CHECK_EQ(runtime.InteractionPortalIndex(0), 0u);
    CHECK_EQ(
        runtime.InteractionIndex("missing"),
        asset::kAssemblyNoIndex);
    CHECK_EQ(
        runtime.InteractionPortalIndex(2),
        asset::kAssemblyNoIndex);
    CHECK_EQ(runtime.InteractionStateName(0), std::string_view("closed"));
    CHECK(!runtime.IsPortalTraversable(0));

    runtime.Shutdown();
    runtime.Shutdown();
    CHECK(!runtime.IsInitialized());
    CHECK_EQ(runtime.InteractionCount(), 0u);
    CHECK_EQ(runtime.PortalCount(), 0u);
    CHECK_EQ(
        runtime.ActivateInteraction(0).status,
        scene::AssemblyInteriorStatus::NotInitialized);
}

TEST_CASE(AssemblyInterior_RotationalMotionReversesWithoutDriftAndGatesPortal)
{
    Fixture fixture;
    scene::AssemblyInteriorRuntime runtime;
    CHECK(fixture.Initialize(runtime).Succeeded());

    CHECK(runtime.ActivateInteraction("outer_hatch").Succeeded());
    CHECK_EQ(runtime.InteractionStateName(0), std::string_view("opening"));
    CHECK(runtime.Advance(0.5).Succeeded());
    CHECK_APPROX(runtime.InteractionMotionProgress(0), 0.5);
    CHECK(!runtime.IsPortalTraversable(0));

    CHECK(runtime.ActivateInteraction(0).Succeeded());
    CHECK_EQ(runtime.InteractionStateName(0), std::string_view("closing"));
    CHECK(runtime.Advance(0.5).Succeeded());
    CHECK_EQ(runtime.InteractionStateName(0), std::string_view("closed"));
    const ecs::Transform* closed = runtime.MovingPartTransform(0);
    CHECK(closed != nullptr);
    CHECK_APPROX(closed->position.x, 11.0);
    CHECK_APPROX(closed->position.z, 0.0);

    CHECK(runtime.ActivateInteraction(0).Succeeded());
    CHECK(runtime.Advance(1.0).Succeeded());
    CHECK_EQ(runtime.InteractionStateName(0), std::string_view("open"));
    CHECK(runtime.IsPortalTraversable(0));
    const ecs::Transform* open = runtime.MovingPartTransform(0);
    CHECK(open != nullptr);
    CHECK_APPROX(open->position.x, 10.0);
    CHECK_APPROX(open->position.z, -1.0);
}

TEST_CASE(AssemblyInterior_LinearMotionUsesAuthoredModuleScale)
{
    Fixture fixture;
    scene::AssemblyInteriorRuntime runtime;
    CHECK(fixture.Initialize(runtime).Succeeded());
    CHECK(runtime.ActivateInteraction("inner_door").Succeeded());
    CHECK(runtime.Advance(1.0).Succeeded());

    const ecs::Transform* open = runtime.MovingPartTransform(1);
    CHECK(open != nullptr);
    CHECK_APPROX(open->position.x, 12.0);
    CHECK_APPROX(open->position.y, 0.0);
    CHECK_APPROX(open->position.z, 3.0);
    CHECK(runtime.IsPortalTraversable(1));
}

TEST_CASE(AssemblyInterior_TimePartitioningIsDeterministic)
{
    Fixture fixture;
    scene::AssemblyInteriorRuntime once;
    scene::AssemblyInteriorRuntime partitioned;
    CHECK(fixture.Initialize(once).Succeeded());
    CHECK(fixture.Initialize(partitioned).Succeeded());
    CHECK(once.ActivateInteraction(1).Succeeded());
    CHECK(partitioned.ActivateInteraction(1).Succeeded());

    CHECK(once.Advance(1.0).Succeeded());
    for (int i = 0; i < 10; ++i)
        CHECK(partitioned.Advance(0.1).Succeeded());

    CHECK_EQ(once.InteractionStateName(1), partitioned.InteractionStateName(1));
    CHECK_APPROX(
        once.InteractionMotionProgress(1),
        partitioned.InteractionMotionProgress(1));
    const ecs::Transform* a = once.MovingPartTransform(1);
    const ecs::Transform* b = partitioned.MovingPartTransform(1);
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK_APPROX(a->position.x, b->position.x);
    CHECK_APPROX(a->position.z, b->position.z);
}

TEST_CASE(AssemblyInterior_AdvanceRejectsInvalidRatesAtomically)
{
    Fixture fixture;
    scene::AssemblyInteriorRuntime runtime;
    CHECK(fixture.Initialize(runtime).Succeeded());
    CHECK(runtime.ActivateInteraction(0).Succeeded());
    CHECK(runtime.ActivateInteraction(1).Succeeded());
    const auto before = runtime.CaptureSnapshot();

    scene::AssemblyInteriorConfig config;
    config.linearSpeedMetersPerSecond =
        (std::numeric_limits<double>::denorm_min)();
    const auto rejected = runtime.Advance(0.5, config);
    CHECK_EQ(rejected.status, scene::AssemblyInteriorStatus::InvalidArgument);
    CheckSameSnapshot(before, runtime.CaptureSnapshot());
}

TEST_CASE(AssemblyInterior_NearestQueryUsesRangeViewAndStableOrder)
{
    Fixture fixture;
    scene::AssemblyInteriorRuntime runtime;
    CHECK(fixture.Initialize(runtime).Succeeded());

    scene::AssemblyInteractionQuery query;
    query.worldPosition = { 10.0, 0.0, 0.0 };
    query.worldForward = { 0.0f, 0.0f, 1.0f };
    query.maxDistanceMeters = 1.5;
    const auto activated = runtime.ActivateNearest(query);
    CHECK(activated.Succeeded());
    CHECK_EQ(activated.stableIndex, 0u);
    CHECK_EQ(runtime.InteractionStateName(0), std::string_view("opening"));

    query.worldForward = { 0.0f, 0.0f, -1.0f };
    CHECK_EQ(
        runtime.ActivateNearest(query).status,
        scene::AssemblyInteriorStatus::NotFound);
    query.worldForward = {};
    CHECK_EQ(
        runtime.ActivateNearest(query).status,
        scene::AssemblyInteriorStatus::InvalidArgument);
}

TEST_CASE(AssemblyInterior_NonmovingStatesCycleAndLockedMovingStateRejectsUse)
{
    Fixture fixture;
    scene::AssemblyInteriorRuntime runtime;
    CHECK(fixture.Initialize(runtime).Succeeded());
    CHECK(runtime.ActivateInteraction("pilot_seat").Succeeded());
    CHECK_EQ(runtime.InteractionStateName(2), std::string_view("occupied"));
    CHECK(runtime.ActivateInteraction(2).Succeeded());
    CHECK_EQ(runtime.InteractionStateName(2), std::string_view("available"));

    auto snapshot = runtime.CaptureSnapshot();
    snapshot.interactions[0].stateIndex = 4;
    snapshot.interactions[0].motionProgress = 0.0;
    CHECK(runtime.ApplySnapshot(snapshot).Succeeded());
    CHECK_EQ(
        runtime.ActivateInteraction(0).status,
        scene::AssemblyInteriorStatus::Locked);
}

TEST_CASE(AssemblyInterior_SnapshotRestoreIsAtomicAndTopologyBound)
{
    Fixture fixture;
    scene::AssemblyInteriorRuntime runtime;
    CHECK(fixture.Initialize(runtime).Succeeded());
    const auto closed = runtime.CaptureSnapshot();

    CHECK(runtime.ActivateInteraction(0).Succeeded());
    CHECK(runtime.Advance(0.25).Succeeded());
    const auto partial = runtime.CaptureSnapshot();
    CHECK(runtime.ApplySnapshot(closed).Succeeded());
    CHECK_EQ(runtime.InteractionStateName(0), std::string_view("closed"));
    CHECK(runtime.ApplySnapshot(partial).Succeeded());
    CheckSameSnapshot(partial, runtime.CaptureSnapshot());

    auto invalid = partial;
    invalid.interactions[0].stateIndex = 0;
    invalid.interactions[0].motionProgress = 0.5;
    CHECK_EQ(
        runtime.ApplySnapshot(invalid).status,
        scene::AssemblyInteriorStatus::InvalidSnapshot);
    CheckSameSnapshot(partial, runtime.CaptureSnapshot());

    invalid = partial;
    invalid.topologySha256 = HashText("different topology");
    CHECK_EQ(
        runtime.ApplySnapshot(invalid).status,
        scene::AssemblyInteriorStatus::TopologyMismatch);
    CheckSameSnapshot(partial, runtime.CaptureSnapshot());
}

TEST_CASE(AssemblyInterior_RejectsMalformedTopologyWithoutPublishingState)
{
    Fixture fixture;
    fixture.assembly->movingParts[0].axis = {};
    scene::AssemblyInteriorRuntime runtime;
    const auto invalid = fixture.Initialize(runtime);
    CHECK_EQ(invalid.status, scene::AssemblyInteriorStatus::InvalidTopology);
    CHECK(!runtime.IsInitialized());

    Fixture orphaned;
    orphaned.assembly->interactions[0].movingPartIndex = 1;
    const auto orphanResult = orphaned.Initialize(runtime);
    CHECK_EQ(
        orphanResult.status,
        scene::AssemblyInteriorStatus::InvalidTopology);
    CHECK(!runtime.IsInitialized());
}

TEST_CASE(AssemblyInterior_StatusNamesRemainStable)
{
    CHECK_EQ(
        std::string_view(scene::AssemblyInteriorStatusName(
            scene::AssemblyInteriorStatus::Success)),
        std::string_view("success"));
    CHECK_EQ(
        std::string_view(scene::AssemblyInteriorStatusName(
            static_cast<scene::AssemblyInteriorStatus>(255))),
        std::string_view("unknown"));
}
