// =============================================================================
// tests/test_star_system_seed.cpp — seeding the reference star system into a
// live scene (Prototype Phase B).
//
// Proves the three things SeedStarSystem must get right, GPU-free:
//   * bodyId de-collision — the builder's star is bodyId 1, the same id the
//     player ship uses; the N-body gravity pass rejects the WHOLE step on any
//     duplicate bodyId, so the offset is load-bearing. Witnessed BOTH ways in
//     one test: seeded WITH the offset -> the step is accepted and SOI runs;
//     seeded WITHOUT it (base 0) -> the step is rejected (the duplicate abort).
//   * visuals — every seeded body gets the shared sphere mesh + a material + a
//     true-radius scale, and the star (the only seeded body with no OrbitState)
//     is the emitter.
//   * SOI live — an SOI-enabled step over the seeded system evaluates the
//     on-rails bodies.
// =============================================================================

#include "test_framework.h"
#include "scene/star_system_seed.h"
#include "sim/star_system.h"
#include "sim/system_instantiate.h"
#include "sim/simulation_system.h"
#include "sim/reference_frame.h"

#include "ecs/registry.h"
#include "ecs/components.h"

#include <cmath>

namespace
{
using core::Vec3d;
using namespace sim;

ecs::Entity FindBody(ecs::Registry& r, uint64_t id)
{
    auto* pool = r.GetPool<ecs::GravitationalBody>();
    if (!pool) return ecs::NullEntity;
    for (uint32_t i = 0; i < pool->Count(); ++i)
        if (pool->DataAt(i).bodyId == id)
            return r.EntityAtIndex(pool->EntityAt(i));
    return ecs::NullEntity;
}

// A minimal force-integrated "ship" at bodyId 1 — the id the seeded star collides
// with unless the seed is offset.
ecs::Entity MakeShip(ecs::Registry& r, FrameId frame)
{
    const ecs::Entity e = r.Create();
    ecs::Transform t; t.position = { 1.5e11, 0.0, 0.0 };
    r.Assign<ecs::Transform>(e, t);
    ecs::RigidBody b; b.invMass = 1.0; b.prevPosition = t.position;
    r.Assign<ecs::RigidBody>(e, b);
    r.Assign<ecs::SpatialFrame>(e, ecs::SpatialFrame{ static_cast<uint32_t>(frame) });
    ecs::GravitationalBody g;
    g.bodyId = 1; g.isSource = false; g.owner = ecs::OrbitOwner::ForceIntegrated;
    g.mu = 0.0; g.radius = 1.0;
    r.Assign<ecs::GravitationalBody>(e, g);
    return e;
}

// Seed the reference system into a registry the way Scene::SeedStarSystem does,
// with a caller-chosen bodyId base (so a test can force a collision at base 0).
uint32_t SeedWith(ecs::Registry& r, FrameId frame, uint64_t base,
                  uint32_t meshHandle, float meshRadius)
{
    const StarSystem sys =
        scene::OffsetStarSystemBodyIds(BuildReferenceSystem(), base);
    const uint32_t created = InstantiateStarSystem(r, frame, sys);
    scene::AttachStarSystemVisuals(r, base, meshHandle, meshRadius);
    return created;
}

} // namespace

TEST_CASE(StarSystemSeed_OffsetShiftsIdsAndKeepsPrimaryGraphConsistent)
{
    const StarSystem base = BuildReferenceSystem();
    const uint64_t K = scene::kStarSystemBodyIdBase;
    const StarSystem off = scene::OffsetStarSystemBodyIds(base, K);

    CHECK_EQ(off.bodies.size(), base.bodies.size());

    for (size_t i = 0; i < base.bodies.size(); ++i)
    {
        const SystemBody& b = base.bodies[i];
        const SystemBody& o = off.bodies[i];
        CHECK_EQ(o.bodyId, b.bodyId + K);
        // The central star's primary is 0 and MUST stay 0; every other primary
        // is shifted so the child still points at its (shifted) parent.
        if (b.primaryBodyId == 0)
            CHECK_EQ(o.primaryBodyId, 0ull);
        else
            CHECK_EQ(o.primaryBodyId, b.primaryBodyId + K);
        if (b.hasOrbit)
            CHECK_EQ(o.orbit.primaryBodyId, b.orbit.primaryBodyId + K);
    }

    // Referential integrity: every non-star primary id resolves to a body in the
    // shifted system (no dangling parent introduced by the shift).
    for (const SystemBody& o : off.bodies)
    {
        if (o.primaryBodyId == 0) continue;
        bool found = false;
        for (const SystemBody& q : off.bodies)
            if (q.bodyId == o.primaryBodyId) { found = true; break; }
        CHECK(found);
    }
}

TEST_CASE(StarSystemSeed_AttachesMeshMaterialAndTrueRadiusScale)
{
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
    ecs::Registry registry;

    const uint64_t K = scene::kStarSystemBodyIdBase;
    const float meshRadius = 0.5f;
    const uint32_t handle = 7u; // any resource-handle value; not dereferenced here
    const uint32_t created = SeedWith(registry, root, K, handle, meshRadius);
    CHECK(created >= 4u); // Sun + two planets + a moon

    // The star: seeded id K+1, no OrbitState, emissive, mesh attached, scaled to
    // the Sun's true radius.
    const ecs::Entity star = FindBody(registry, K + 1);
    CHECK_FALSE(star.IsNull());
    if (!star.IsNull())
    {
        CHECK_FALSE(registry.Has<ecs::OrbitState>(star));
        CHECK(registry.Has<ecs::MeshInstance>(star));
        CHECK_EQ(registry.Get<ecs::MeshInstance>(star).meshHandle, handle);
        const ecs::Material& m = registry.Get<ecs::Material>(star);
        CHECK(m.emissiveStrength > 0.0f); // the star is the emitter
        const double sunRadius = registry.Get<ecs::GravitationalBody>(star).radius;
        CHECK_APPROX_EPS(registry.Get<ecs::Transform>(star).scale.x,
                         static_cast<float>(sunRadius) / meshRadius,
                         static_cast<float>(sunRadius) / meshRadius * 1.0e-4f);
    }

    // A planet (Earth, seeded id K+10): lit (not emissive), mesh attached.
    const ecs::Entity earth = FindBody(registry, K + 10);
    CHECK_FALSE(earth.IsNull());
    if (!earth.IsNull())
    {
        CHECK(registry.Has<ecs::MeshInstance>(earth));
        CHECK(registry.Has<ecs::OrbitState>(earth));
        CHECK_EQ(registry.Get<ecs::Material>(earth).emissiveStrength, 0.0f);
    }
}

TEST_CASE(StarSystemSeed_LiveStepAcceptedWithOffsetRejectedWithout)
{
    const float meshRadius = 0.5f;
    const uint32_t handle = 3u;

    SimulationStepConfig config;
    config.enableSoiTransitions = true;
    config.soiHysteresis = 0.01;

    // --- WITH the offset: ship (id 1) and the seeded star (id K+1) do NOT
    //     collide, so the full step is accepted and the SOI phase runs. ---
    {
        FrameGraph frames;
        const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
        ecs::Registry registry;
        MakeShip(registry, root);
        SeedWith(registry, root, scene::kStarSystemBodyIdBase, handle, meshRadius);

        config.activeFrame = root; config.masterFrame = root; config.coordinateTime = 0.0;
        const SimulationStepResult r = StepSimulation(registry, frames, 1.0, config);
        CHECK(r.accepted);
        CHECK(r.soiTransitions.evaluated >= 3u); // on-rails planets + moon evaluated
    }

    // --- WITHOUT the offset (base 0): the seeded star keeps bodyId 1, colliding
    //     with the ship. The gravity pass rejects the WHOLE step. This is the
    //     watched failure for the de-collision, asserted in-place. ---
    {
        FrameGraph frames;
        const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
        ecs::Registry registry;
        MakeShip(registry, root);
        SeedWith(registry, root, 0ull, handle, meshRadius);

        config.activeFrame = root; config.masterFrame = root; config.coordinateTime = 0.0;
        const SimulationStepResult r = StepSimulation(registry, frames, 1.0, config);
        CHECK_FALSE(r.accepted); // duplicate bodyId 1 aborts the step
    }
}
