#include "test_framework.h"
#include "scene/assembly_dynamic_collision_runtime.h"

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

std::shared_ptr<const scene::InteriorCollisionWorld> StaticWorld(
    std::initializer_list<scene::InteriorCollisionBox> boxes = {
        Box(1, { -10.0, -1.0, -10.0 }, { 10.0, -0.5, 10.0 },
            asset::CollisionSurfaceFlags::Walkable) })
{
    const std::vector<scene::InteriorCollisionBox> values(boxes);
    return scene::BuildInteriorCollisionWorld(values).world;
}

struct DynamicFixture
{
    std::shared_ptr<asset::CookedAssembly> assembly =
        std::make_shared<asset::CookedAssembly>();
    std::vector<scene::PreparedAssemblyModule> modules;
    std::vector<scene::PreparedAssemblyMovingPart> movingParts;
    scene::AssemblyInteriorRuntime interior;
    scene::AssemblyDynamicCollisionRuntime dynamic;
    std::shared_ptr<const scene::InteriorCollisionWorld> staticWorld =
        StaticWorld();

    explicit DynamicFixture(uint8_t topologyByte = 1)
    {
        assembly->sourceManifestSha256.bytes[0] = topologyByte;
        assembly->minimumDoorWidthMeters = 1.0;
        assembly->minimumClearanceMeters = 2.0;
        assembly->modules.resize(1);

        asset::AssemblySocket socket;
        socket.id = "door_socket";
        socket.moduleIndex = 0;
        socket.type = asset::AssemblySocketType::Portal;
        socket.positionMeters = { 0.0, 0.5, 0.0 };
        socket.forward = { 0.0, 0.0, 1.0 };
        socket.up = { 0.0, 1.0, 0.0 };
        assembly->sockets.push_back(socket);

        asset::AssemblyInteraction interaction;
        interaction.id = "door";
        interaction.type = asset::AssemblyInteractionType::Door;
        interaction.moduleIndex = 0;
        interaction.socketIndex = 0;
        interaction.states = { "closed", "opening", "open", "closing", "locked" };
        interaction.initialStateIndex = 0;
        interaction.movingPartIndex = 0;
        interaction.portalIndex = 0;
        assembly->interactions.push_back(interaction);

        asset::AssemblyMovingPart part;
        part.id = "door_panel";
        part.moduleIndex = 0;
        part.interactionIndex = 0;
        part.motionType = asset::AssemblyMotionType::Linear;
        part.axis = { 1.0, 0.0, 0.0 };
        part.travel = 1.1;
        assembly->movingParts.push_back(part);

        asset::AssemblyPortal portal;
        portal.id = "door_portal";
        portal.socketA = 0;
        portal.socketB = 0;
        portal.closureInteraction = 0;
        portal.sealable = true;
        portal.navLink = true;
        assembly->portals.push_back(portal);

        scene::PreparedAssemblyModule module;
        module.stableIndex = 0;
        modules.push_back(module);
        scene::PreparedAssemblyMovingPart preparedPart;
        preparedPart.stableIndex = 0;
        preparedPart.moduleIndex = 0;
        preparedPart.interactionIndex = 0;
        movingParts.push_back(preparedPart);
    }

    bool InitializeInterior()
    {
        return interior.Initialize(assembly, modules, movingParts).Succeeded();
    }

    bool Initialize()
    {
        return InitializeInterior() &&
               dynamic.Initialize(assembly, staticWorld, interior).Succeeded();
    }
};

const scene::InteriorCollisionBox* DynamicBox(
    const scene::AssemblyInteriorCollisionSnapshot& snapshot,
    bool guard)
{
    for (const auto& box : snapshot.dynamicBoxes)
    {
        if ((box.stableId & 1u) == static_cast<uint64_t>(guard ? 1u : 0u))
            return &box;
    }
    return nullptr;
}

} // namespace

TEST_CASE(DynamicInteriorCollision_ClosedMovingOpenAndReclosingAreAtomic)
{
    DynamicFixture fixture;
    CHECK(fixture.Initialize());
    const auto closed = fixture.dynamic.Snapshot();
    CHECK(closed != nullptr);
    CHECK_EQ(closed->revision, 1u);
    CHECK_EQ(closed->dynamicBoxes.size(), 2u);
    CHECK(DynamicBox(*closed, false) != nullptr);
    CHECK(DynamicBox(*closed, true) != nullptr);

    CHECK(fixture.interior.ActivateInteraction("door").Succeeded());
    const auto openingAtZero = fixture.dynamic.Refresh(fixture.interior);
    CHECK(openingAtZero.Succeeded());
    CHECK(!openingAtZero.changed);
    CHECK(fixture.dynamic.Snapshot() == closed);

    CHECK(fixture.interior.Advance(0.55).Succeeded());
    const auto moving = fixture.dynamic.Refresh(fixture.interior);
    CHECK(moving.Succeeded());
    CHECK(moving.changed);
    const auto halfway = fixture.dynamic.Snapshot();
    CHECK_EQ(halfway->revision, 2u);
    CHECK_EQ(halfway->dynamicBoxes.size(), 2u);
    CHECK(DynamicBox(*halfway, false)->minimum.x >
          DynamicBox(*closed, false)->minimum.x + 0.5);
    CHECK_APPROX_EPS(DynamicBox(*halfway, true)->minimum.x,
                     DynamicBox(*closed, true)->minimum.x, 1.0e-12);

    CHECK(fixture.interior.Advance(0.55).Succeeded());
    CHECK(fixture.interior.IsPortalTraversable(0));
    CHECK(fixture.dynamic.Refresh(fixture.interior).Succeeded());
    const auto opened = fixture.dynamic.Snapshot();
    CHECK_EQ(opened->dynamicBoxes.size(), 1u);
    CHECK(DynamicBox(*opened, false) != nullptr);
    CHECK(DynamicBox(*opened, true) == nullptr);

    CHECK(fixture.interior.ActivateInteraction("door").Succeeded());
    const auto closing = fixture.dynamic.Refresh(fixture.interior);
    CHECK(closing.Succeeded());
    CHECK(closing.changed);
    CHECK_EQ(fixture.dynamic.Snapshot()->dynamicBoxes.size(), 2u);
    CHECK(!fixture.interior.IsPortalTraversable(0));
}

TEST_CASE(DynamicInteriorCollision_SnapshotRestoreRebuildsExactClosureGeometry)
{
    DynamicFixture fixture;
    CHECK(fixture.Initialize());
    const auto authoredClosed = fixture.interior.CaptureSnapshot();
    const auto collisionClosed = fixture.dynamic.Snapshot();

    CHECK(fixture.interior.ActivateInteraction(0).Succeeded());
    CHECK(fixture.interior.Advance(1.1).Succeeded());
    CHECK(fixture.dynamic.Refresh(fixture.interior).Succeeded());
    const auto collisionOpen = fixture.dynamic.Snapshot();
    CHECK_EQ(collisionOpen->dynamicBoxes.size(), 1u);
    CHECK(!fixture.dynamic.IsCurrent(collisionClosed));

    CHECK(fixture.interior.ApplySnapshot(authoredClosed).Succeeded());
    CHECK(fixture.dynamic.Refresh(fixture.interior).Succeeded());
    const auto restored = fixture.dynamic.Snapshot();
    CHECK_EQ(restored->dynamicBoxes.size(), 2u);
    CHECK_APPROX_EPS(DynamicBox(*restored, false)->minimum.x,
                     DynamicBox(*collisionClosed, false)->minimum.x, 1.0e-12);
    CHECK_APPROX_EPS(DynamicBox(*restored, true)->maximum.z,
                     DynamicBox(*collisionClosed, true)->maximum.z, 1.0e-12);
}

TEST_CASE(DynamicInteriorCollision_RotatedPanelMatchesMotionAndStaysAssemblyLocal)
{
    DynamicFixture fixture;
    fixture.assembly->modules[0].transform.positionMeters = { 3.0, 1.0, 4.0 };
    fixture.assembly->modules[0].transform.rotationEulerDegrees =
        { 0.0, 90.0, 0.0 };
    fixture.assembly->modules[0].transform.scale = { 2.0, 1.5, 0.5 };
    fixture.assembly->sockets[0].positionMeters = { 0.25, 0.5, 0.0 };
    fixture.assembly->movingParts[0].motionType =
        asset::AssemblyMotionType::Rotational;
    fixture.assembly->movingParts[0].pivotMeters = { 0.7, 0.0, 0.0 };
    fixture.assembly->movingParts[0].axis = { 0.0, 1.0, 0.0 };
    fixture.assembly->movingParts[0].travel = 90.0;

    ecs::Transform localModule;
    localModule.position = { 3.0, 1.0, 4.0 };
    localModule.rotation = core::Quatf::FromEuler(
        0.0f, 0.5f * core::PI, 0.0f).Normalized();
    localModule.scale = { 2.0f, 1.5f, 0.5f };
    const core::Vec3d rootOffset{ 100.0, 0.0, 0.0 };
    ecs::Transform worldModule = localModule;
    worldModule.position += rootOffset;
    fixture.modules[0].localTransform = worldModule;
    fixture.movingParts[0].localTransform = worldModule;

    CHECK(fixture.Initialize());
    const auto closed = fixture.dynamic.Snapshot();
    const scene::InteriorCollisionBox* guard = DynamicBox(*closed, true);
    CHECK(guard != nullptr);
    const core::Vec3d guardCenter =
        (guard->minimum + guard->maximum) * 0.5;
    const core::Vec3f closedSocket = localModule.rotation.Rotate({
        static_cast<float>(fixture.assembly->sockets[0].positionMeters[0]) *
            localModule.scale.x,
        static_cast<float>(fixture.assembly->sockets[0].positionMeters[1]) *
            localModule.scale.y,
        static_cast<float>(fixture.assembly->sockets[0].positionMeters[2]) *
            localModule.scale.z
    });
    CHECK_APPROX_EPS(guardCenter.x,
                     localModule.position.x + closedSocket.x, 1.0e-6);
    CHECK_APPROX_EPS(guardCenter.y,
                     localModule.position.y + closedSocket.y, 1.0e-6);
    CHECK_APPROX_EPS(guardCenter.z,
                     localModule.position.z + closedSocket.z, 1.0e-6);

    CHECK(fixture.interior.ActivateInteraction(0).Succeeded());
    CHECK(fixture.interior.Advance(1.0).Succeeded());
    CHECK(fixture.dynamic.Refresh(fixture.interior).Succeeded());
    const auto snapshot = fixture.dynamic.Snapshot();
    CHECK_EQ(snapshot->dynamicBoxes.size(), 1u);

    const ecs::Transform* moving =
        fixture.interior.MovingPartLocalTransform(0);
    CHECK(moving != nullptr);
    const core::Vec3f scaledSocket{
        static_cast<float>(fixture.assembly->sockets[0].positionMeters[0]) *
            moving->scale.x,
        static_cast<float>(fixture.assembly->sockets[0].positionMeters[1]) *
            moving->scale.y,
        static_cast<float>(fixture.assembly->sockets[0].positionMeters[2]) *
            moving->scale.z
    };
    const core::Vec3d expectedCenter = moving->position - rootOffset +
        core::Vec3d::FromFloat(moving->rotation.Rotate(scaledSocket));
    const scene::InteriorCollisionBox* panel = DynamicBox(*snapshot, false);
    CHECK(panel != nullptr);
    const core::Vec3d actualCenter =
        (panel->minimum + panel->maximum) * 0.5;
    CHECK_APPROX_EPS(actualCenter.x, expectedCenter.x, 1.0e-6);
    CHECK_APPROX_EPS(actualCenter.y, expectedCenter.y, 1.0e-6);
    CHECK_APPROX_EPS(actualCenter.z, expectedCenter.z, 1.0e-6);
}

TEST_CASE(DynamicInteriorCollision_TopologyMismatchLeavesPublishedSnapshotUntouched)
{
    DynamicFixture fixture;
    DynamicFixture other(2);
    CHECK(fixture.Initialize());
    CHECK(other.InitializeInterior());
    const auto before = fixture.dynamic.Snapshot();
    const auto rejected = fixture.dynamic.Refresh(other.interior);
    CHECK_EQ(rejected.status,
             scene::AssemblyDynamicCollisionStatus::TopologyMismatch);
    CHECK(fixture.dynamic.Snapshot() == before);
    CHECK(fixture.dynamic.IsCurrent(before));
}

TEST_CASE(DynamicInteriorCollision_RejectsReservedIdentityMalformedBasisAndLimits)
{
    DynamicFixture reserved;
    CHECK(reserved.InitializeInterior());
    reserved.staticWorld = StaticWorld({
        Box(uint64_t{ 1 } << 63,
            { -1.0, -1.0, -1.0 }, { 1.0, 1.0, 1.0 })
    });
    CHECK_EQ(reserved.dynamic.Initialize(
                 reserved.assembly, reserved.staticWorld, reserved.interior).status,
             scene::AssemblyDynamicCollisionStatus::InvalidTopology);

    DynamicFixture malformed;
    malformed.assembly->sockets[0].up = { 0.0, 0.0, 2.0 };
    CHECK(malformed.InitializeInterior());
    CHECK_EQ(malformed.dynamic.Initialize(
                 malformed.assembly, malformed.staticWorld,
                 malformed.interior).status,
             scene::AssemblyDynamicCollisionStatus::InvalidTopology);

    DynamicFixture limited;
    CHECK(limited.InitializeInterior());
    scene::AssemblyDynamicCollisionConfig config;
    config.maxDynamicBoxes = 1;
    CHECK_EQ(limited.dynamic.Initialize(
                 limited.assembly, limited.staticWorld,
                 limited.interior, config).status,
             scene::AssemblyDynamicCollisionStatus::ResourceLimitExceeded);

    DynamicFixture badAperture;
    CHECK(badAperture.InitializeInterior());
    badAperture.assembly->minimumDoorWidthMeters = 0.0;
    CHECK_EQ(badAperture.dynamic.Initialize(
                 badAperture.assembly, badAperture.staticWorld,
                 badAperture.interior).status,
             scene::AssemblyDynamicCollisionStatus::InvalidTopology);

    DynamicFixture combinedLimit;
    CHECK(combinedLimit.InitializeInterior());
    config = {};
    config.combinedWorldLimits.maxBoxes = 2;
    CHECK_EQ(combinedLimit.dynamic.Initialize(
                 combinedLimit.assembly, combinedLimit.staticWorld,
                 combinedLimit.interior, config).status,
             scene::AssemblyDynamicCollisionStatus::ResourceLimitExceeded);
}

TEST_CASE(DynamicInteriorCollision_CombinedQueriesKeepStaticTieOrdering)
{
    DynamicFixture fixture;
    fixture.staticWorld = StaticWorld({
        Box(7, { -0.5, -0.5, -0.06 }, { 0.5, 1.5, 0.06 })
    });
    CHECK(fixture.Initialize());
    scene::InteriorCapsule capsule;
    capsule.center = { 0.0, 0.5, -2.0 };
    capsule.radius = 0.35;
    capsule.halfSegment = 0.45;
    const auto hit = fixture.dynamic.Snapshot()->collisionWorld->SweepCapsule(
        capsule, { 0.0, 0.0, 4.0 });
    CHECK(hit.Succeeded());
    CHECK(hit.hit);
    CHECK_EQ(hit.stableId, 7u);
}

TEST_CASE(DynamicInteriorCollision_ShutdownAndRestoreRejectStaleOwnership)
{
    DynamicFixture fixture;
    CHECK(fixture.Initialize());
    const auto live = fixture.dynamic.Snapshot();
    fixture.dynamic.Shutdown();
    CHECK(!fixture.dynamic.IsInitialized());
    CHECK(!fixture.dynamic.IsCurrent(live));
    CHECK_EQ(fixture.dynamic.Refresh(fixture.interior).status,
             scene::AssemblyDynamicCollisionStatus::NotInitialized);
}

TEST_CASE(DynamicInteriorCollision_StatusNamesRemainStable)
{
    CHECK_EQ(std::string(scene::AssemblyDynamicCollisionStatusName(
                 scene::AssemblyDynamicCollisionStatus::TopologyMismatch)),
             std::string("topology_mismatch"));
    CHECK_EQ(std::string(scene::AssemblyDynamicCollisionStatusName(
                 static_cast<scene::AssemblyDynamicCollisionStatus>(255))),
             std::string("unknown"));
}
