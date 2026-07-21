// =============================================================================
// tests/test_snapshot_system.cpp - live ECS snapshot transaction
// =============================================================================

#include "test_framework.h"
#include "sim/snapshot_system.h"

#include <limits>

namespace
{

ecs::Entity MakeBody(ecs::Registry& registry, sim::FrameId frame,
                     uint64_t bodyId, ecs::OrbitOwner owner)
{
    const ecs::Entity entity = registry.Create();
    ecs::Transform transform;
    transform.position = { static_cast<double>(bodyId), 2.0, 3.0 };
    registry.Assign<ecs::Transform>(entity, transform);
    ecs::RigidBody rigid;
    rigid.linearVelocity = { 4.0, 5.0, 6.0 };
    rigid.angularVelocity = { 0.1f, 0.2f, 0.3f };
    rigid.invMass = 0.5;
    rigid.invInertiaDiag = { 1.0f, 2.0f, 3.0f };
    registry.Assign<ecs::RigidBody>(entity, rigid);
    registry.Assign<ecs::SpatialFrame>(
        entity, ecs::SpatialFrame{ static_cast<uint32_t>(frame) });
    ecs::GravitationalBody gravity;
    gravity.bodyId = bodyId;
    gravity.owner = owner;
    gravity.mu = owner == ecs::OrbitOwner::ForceIntegrated ? 0.0 : 1.0e6;
    gravity.radius = 1.0;
    gravity.isSource = owner != ecs::OrbitOwner::ForceIntegrated;
    registry.Assign<ecs::GravitationalBody>(entity, gravity);
    return entity;
}

} // namespace

TEST_CASE(SnapshotSystem_RoundTripRestoresSimulationAndPreservesGameplay)
{
    sim::FrameGraph frames;
    const sim::FrameId root = frames.CreateFrame(sim::kInvalidFrame, sim::WorldPos{});
    const sim::FrameId child = frames.CreateFrame(
        root, sim::WorldPos::FromOffset({ 1000.0, 0.0, 0.0 }),
        { 10.0, 0.0, 0.0 });
    ecs::Registry registry;
    const ecs::Entity ship = MakeBody(
        registry, root, 10, ecs::OrbitOwner::ForceIntegrated);
    const ecs::Entity satellite = MakeBody(
        registry, child, 20, ecs::OrbitOwner::OnRails);
    ecs::OrbitState orbit;
    orbit.elements.semiMajorAxis = 100.0;
    orbit.elements.eccentricity = 0.1;
    orbit.primaryMu = 1.0e6;
    orbit.primaryBodyId = 10;
    orbit.epoch = 12.0;
    registry.Assign<ecs::OrbitState>(satellite, orbit);
    ecs::RelativisticBody relativistic;
    relativistic.restMass = 2.0;
    relativistic.momentum = { 8.0, 10.0, 12.0 };
    registry.Assign<ecs::RelativisticBody>(ship, relativistic);
    registry.Assign<ecs::RelativisticClock>(
        ship, ecs::RelativisticClock{ 7.0, -0.25 });
    ecs::Name name;
    name.Set("Persistent ship");
    registry.Assign<ecs::Name>(ship, name);

    const sim::SnapshotBuildResult saved = sim::BuildSnapshot(
        registry, frames, 42.0, 1.0 / 60.0, 2520, root);
    CHECK(saved.accepted);
    if (!saved.accepted) return;

    registry.Get<ecs::Transform>(ship).position = { 999.0, 999.0, 999.0 };
    registry.Get<ecs::RigidBody>(ship).linearVelocity = {};
    registry.Get<ecs::GravitationalBody>(ship).mu = 123.0;
    registry.Get<ecs::RelativisticClock>(ship).coordinateTime = 99.0;
    frames.CreateFrame(root, sim::WorldPos::FromOffset({ 55.0, 0.0, 0.0 }));

    const sim::SnapshotApplyResult applied =
        sim::ApplySnapshot(registry, frames, saved.snapshot);
    CHECK(applied.accepted);
    CHECK_EQ(frames.FrameCount(), 2u);
    CHECK_EQ(registry.Get<ecs::Transform>(ship).position.x, 10.0);
    CHECK_EQ(registry.Get<ecs::RigidBody>(ship).linearVelocity.x, 4.0);
    CHECK_EQ(registry.Get<ecs::RelativisticClock>(ship).coordinateTime, 7.0);
    CHECK_EQ(registry.Get<ecs::Name>(ship).text[0], 'P');

    const sim::SnapshotBuildResult rebuilt = sim::BuildSnapshot(
        registry, frames, 42.0, 1.0 / 60.0, 2520, root);
    CHECK(rebuilt.accepted);
    CHECK(rebuilt.snapshot == saved.snapshot);
}

TEST_CASE(SnapshotSystem_TopologyMismatchIsAtomic)
{
    sim::FrameGraph frames;
    const sim::FrameId root = frames.CreateFrame(sim::kInvalidFrame, sim::WorldPos{});
    ecs::Registry registry;
    const ecs::Entity ship = MakeBody(
        registry, root, 10, ecs::OrbitOwner::ForceIntegrated);
    const sim::SnapshotBuildResult saved = sim::BuildSnapshot(
        registry, frames, 1.0, 0.1, 10, root);
    CHECK(saved.accepted);
    if (!saved.accepted) return;

    sim::SimSnapshot wrong = saved.snapshot;
    wrong.bodies[0].bodyId = 99;
    const ecs::Transform before = registry.Get<ecs::Transform>(ship);
    const std::vector<sim::Frame> framesBefore = frames.Frames();
    const sim::SnapshotApplyResult result =
        sim::ApplySnapshot(registry, frames, wrong);
    CHECK_FALSE(result.accepted);
    CHECK(registry.Get<ecs::Transform>(ship).position == before.position);
    CHECK_EQ(frames.FrameCount(), static_cast<uint32_t>(framesBefore.size()));
    CHECK(frames.GetFrame(root).origin == framesBefore[root].origin);
    CHECK(frames.GetFrame(root).velocity == framesBefore[root].velocity);

    wrong = saved.snapshot;
    wrong.bodies[0].position.x =
        (std::numeric_limits<double>::quiet_NaN)();
    CHECK_FALSE(sim::ApplySnapshot(registry, frames, wrong).accepted);
    CHECK(registry.Get<ecs::Transform>(ship).position == before.position);
    CHECK_EQ(frames.FrameCount(), static_cast<uint32_t>(framesBefore.size()));
    CHECK(frames.GetFrame(root).origin == framesBefore[root].origin);
    CHECK(frames.GetFrame(root).velocity == framesBefore[root].velocity);
}

TEST_CASE(SnapshotSystem_RejectsIncompleteTopologyAndPendingWrench)
{
    sim::FrameGraph frames;
    const sim::FrameId root = frames.CreateFrame(sim::kInvalidFrame, sim::WorldPos{});
    ecs::Registry registry;
    const ecs::Entity ship = MakeBody(
        registry, root, 10, ecs::OrbitOwner::ForceIntegrated);
    registry.Get<ecs::RigidBody>(ship).forceAccum = { 1.0, 0.0, 0.0 };
    CHECK_FALSE(sim::BuildSnapshot(
        registry, frames, 0.0, 0.1, 0, root).accepted);

    registry.Get<ecs::RigidBody>(ship).forceAccum = {};
    const ecs::Entity incomplete = registry.Create();
    registry.Assign<ecs::RigidBody>(incomplete, ecs::RigidBody{});
    CHECK_FALSE(sim::BuildSnapshot(
        registry, frames, 0.0, 0.1, 0, root).accepted);
}

TEST_CASE(SnapshotSystem_RejectsOrphanedSimulationSidecars)
{
    sim::FrameGraph frames;
    const sim::FrameId root = frames.CreateFrame(
        sim::kInvalidFrame, sim::WorldPos{});
    ecs::Registry registry;
    MakeBody(registry, root, 10, ecs::OrbitOwner::ForceIntegrated);

    const ecs::Entity orphan = registry.Create();
    registry.Assign<ecs::RelativisticBody>(orphan, ecs::RelativisticBody{});
    registry.Assign<ecs::RelativisticClock>(orphan, ecs::RelativisticClock{});
    CHECK_FALSE(sim::BuildSnapshot(
        registry, frames, 0.0, 0.1, 0, root).accepted);

    registry.Destroy(orphan);
    const ecs::Entity orphanOrbit = registry.Create();
    registry.Assign<ecs::OrbitState>(orphanOrbit, ecs::OrbitState{});
    CHECK_FALSE(sim::BuildSnapshot(
        registry, frames, 0.0, 0.1, 0, root).accepted);
}
