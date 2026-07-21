// =============================================================================
// tests/test_soi_transition.cpp — patched-conics orchestration (Stage 13).
//
// Proves the SOI transition wiring: a probe coasting from the star's domain into
// a planet's sphere of influence has its primary reassigned star -> planet, with
// its orbit re-fit continuously in global position AND velocity; and a pristine
// star system produces NO spurious transitions (planets/moon keep their primaries).
// =============================================================================

#include "test_framework.h"
#include "sim/soi_transition.h"
#include "sim/soi.h"                 // SphereOfInfluenceRadius
#include "sim/star_system.h"
#include "sim/system_instantiate.h"
#include "sim/reference_frame.h"
#include "sim/nbody.h"              // PromoteFromRails
#include "sim/kepler.h"             // StateVector

#include "ecs/registry.h"
#include "ecs/components.h"

#include <cmath>

namespace
{
using core::Vec3d;
using namespace sim;

constexpr double kMuSun   = 1.32712440018e20;
constexpr double kMuEarth = 3.986004418e14;
constexpr double kAU      = 1.495978707e11;

ecs::Entity FindBody(ecs::Registry& r, uint64_t id)
{
    auto* pool = r.GetPool<ecs::GravitationalBody>();
    if (!pool) return ecs::NullEntity;
    for (uint32_t i = 0; i < pool->Count(); ++i)
        if (pool->DataAt(i).bodyId == id)
            return r.EntityAtIndex(pool->EntityAt(i));
    return ecs::NullEntity;
}

// A massless on-rails probe about the star at `pos`, moving at `vel`.
ecs::Entity MakeProbe(ecs::Registry& r, FrameId frame, const Vec3d& pos,
                      const Vec3d& vel)
{
    const ecs::Entity e = r.Create();
    ecs::Transform t; t.position = pos;
    r.Assign<ecs::Transform>(e, t);
    ecs::RigidBody b; b.linearVelocity = vel; b.invMass = 1.0; b.prevPosition = pos;
    r.Assign<ecs::RigidBody>(e, b);
    r.Assign<ecs::SpatialFrame>(e, ecs::SpatialFrame{ static_cast<uint32_t>(frame) });
    ecs::GravitationalBody g;
    g.bodyId = 999; g.owner = ecs::OrbitOwner::OnRails; g.mu = 0.0;
    g.radius = 0.0; g.isSource = false;
    r.Assign<ecs::GravitationalBody>(e, g);
    // Primary = the star (id 1). Elements are irrelevant to the SOI test (membership
    // reads Transform.position); a valid circular placeholder keeps it well-formed.
    r.Assign<ecs::OrbitState>(e, CircularOrbit(kMuSun, 1, pos.Length(), 0.0, 0.0, 0.0, 0.0));
    return e;
}

} // namespace

TEST_CASE(SoiTransition_PristineSystemHasNoSpuriousTransitions)
{
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
    ecs::Registry registry;
    InstantiateStarSystem(registry, root, BuildReferenceSystem());

    // Planets sit in the star's SOI, the moon in its planet's — each already has
    // the correct primary, so a resolve pass moves nothing.
    const SoiTransitionResult r = StepSoiTransitions(registry, 0.0, 0.01);
    CHECK_EQ(r.transitions, 0u);
    CHECK(r.evaluated >= 3u); // two planets + a moon are on-rails candidates

    // The moon is still bound to its planet, the planets still to the star.
    CHECK_EQ(registry.Get<ecs::OrbitState>(FindBody(registry, 11)).primaryBodyId, 10ull);
    CHECK_EQ(registry.Get<ecs::OrbitState>(FindBody(registry, 10)).primaryBodyId, 1ull);
    CHECK_EQ(registry.Get<ecs::OrbitState>(FindBody(registry, 20)).primaryBodyId, 1ull);
}

TEST_CASE(SoiTransition_ProbeCrossingIntoAPlanetSwitchesPrimaryContinuously)
{
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
    ecs::Registry registry;
    InstantiateStarSystem(registry, root, BuildReferenceSystem());

    const ecs::Entity planet = FindBody(registry, 10);
    const Vec3d planetPos = registry.Get<ecs::Transform>(planet).position;
    const Vec3d planetVel = registry.Get<ecs::RigidBody>(planet).linearVelocity;
    const double planetSoi = SphereOfInfluenceRadius(kAU, kMuEarth, kMuSun); // ~9.24e8

    // A probe just OUTSIDE the planet's SOI (offset along z, away from the moon),
    // moving with a small tangential velocity relative to the planet.
    const Vec3d relVel{ 0.0, 500.0, 0.0 };
    const Vec3d posOut = planetPos + Vec3d{ 0.0, 0.0, planetSoi * 1.6 };
    const ecs::Entity probe = MakeProbe(registry, root, posOut, planetVel + relVel);
    CHECK_EQ(registry.Get<ecs::OrbitState>(probe).primaryBodyId, 1ull); // star

    // Outside the SOI: no transition.
    StepSoiTransitions(registry, 0.0, 0.01);
    CHECK_EQ(registry.Get<ecs::OrbitState>(probe).primaryBodyId, 1ull);

    // Move the probe INSIDE the planet's SOI (still clear of the moon's).
    const Vec3d posIn = planetPos + Vec3d{ 0.0, 0.0, planetSoi * 0.5 };
    registry.Get<ecs::Transform>(probe).position = posIn;

    const double now = 100.0;
    const SoiTransitionResult r = StepSoiTransitions(registry, now, 0.01);
    CHECK(r.transitions >= 1u);
    // The probe now orbits the PLANET.
    const ecs::OrbitState& po = registry.Get<ecs::OrbitState>(probe);
    CHECK_EQ(po.primaryBodyId, 10ull);
    CHECK_EQ(po.epoch, now);
    CHECK_EQ(po.primaryMu, kMuEarth);

    // Repatch continuity: the re-fit orbit reproduces the probe's GLOBAL position
    // and velocity when evaluated about the planet's live state (no step elapsed,
    // so the planet is still at planetPos/planetVel).
    const StateVector rel = PromoteFromRails(po, 0.0);
    const Vec3d reconPos = planetPos + rel.position;
    const Vec3d reconVel = planetVel + rel.velocity;
    CHECK((reconPos - posIn).Length() < 5.0);            // metres, over a ~4.6e8 m arm
    CHECK((reconVel - (planetVel + relVel)).Length() < 1.0e-3);

    // The planets themselves did not transition (still bound to the star).
    CHECK_EQ(registry.Get<ecs::OrbitState>(planet).primaryBodyId, 1ull);
}
