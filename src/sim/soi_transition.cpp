// =============================================================================
// sim/soi_transition.cpp — patched-conics orchestration. See soi_transition.h.
// =============================================================================

#include "soi_transition.h"

#include "soi.h"               // SoiWell, ResolveSoiWithHysteresis, Repatch, SphereOfInfluenceRadius, kInvalidSoi
#include "reference_frame.h"   // WorldPos::FromOffset
#include "../ecs/components.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace sim {

using core::Vec3d;

namespace {
struct Candidate { uint64_t bodyId; uint32_t entityIndex; };

// A re-fit conic is safe to commit only if it is well-formed. Repatch's documented
// precondition is a non-degenerate conic (r×v ≠ 0, r > 0); a bit-exact radial /
// co-moving crossing violates it and DemoteToRails then yields NaN inclination and
// e == 1.0. Such an OrbitState would fail IsValidOrbit in the very next
// StepPassiveOrbits and — because that step commits nothing — freeze propagation
// permanently and silently. Reject it here (keep the current valid orbit) instead.
bool IsWellFormedOrbit(const ecs::OrbitState& o)
{
    const ecs::OrbitalElements& e = o.elements;
    return std::isfinite(e.semiMajorAxis) && std::isfinite(e.eccentricity) &&
           std::isfinite(e.inclination) && std::isfinite(e.longitudeAscNode) &&
           std::isfinite(e.argPeriapsis) && std::isfinite(e.trueAnomaly) &&
           std::isfinite(o.primaryMu) && o.primaryMu > 0.0 &&
           e.eccentricity >= 0.0 && e.eccentricity != 1.0;
}
} // namespace

SoiTransitionResult StepSoiTransitions(ecs::Registry& registry, double now,
                                       double hysteresis)
{
    SoiTransitionResult result;

    auto* gravPool = registry.GetPool<ecs::GravitationalBody>();
    if (!gravPool)
        return result;

    // Gather gravity-source wells and an id->entityIndex map for primary lookup,
    // plus the on-rails transition candidates. One pass over the gravity pool.
    std::vector<SoiWell> wells;
    std::unordered_map<uint64_t, uint32_t> indexById;
    std::vector<Candidate> candidates;
    wells.reserve(gravPool->Count());
    indexById.reserve(gravPool->Count() * 2u);

    for (uint32_t i = 0; i < gravPool->Count(); ++i)
    {
        const uint32_t entIdx = gravPool->EntityAt(i);
        const ecs::GravitationalBody& g = gravPool->DataAt(i);
        indexById[g.bodyId] = entIdx;

        const Vec3d pos = registry.GetByIndex<ecs::Transform>(entIdx).position;

        if (g.isSource && g.mu > 0.0)
        {
            // SOI radius from the source's OWN orbit; a source with no orbit is the
            // unbounded root (the central star), which contains all interplanetary
            // space.
            double soiR = std::numeric_limits<double>::infinity();
            if (registry.HasByIndex<ecs::OrbitState>(entIdx))
            {
                const ecs::OrbitState& o = registry.GetByIndex<ecs::OrbitState>(entIdx);
                soiR = SphereOfInfluenceRadius(o.elements.semiMajorAxis, g.mu,
                                               o.primaryMu);
            }
            wells.push_back(SoiWell{ g.bodyId, WorldPos::FromOffset(pos), soiR });
        }

        if (g.owner == ecs::OrbitOwner::OnRails &&
            registry.HasByIndex<ecs::OrbitState>(entIdx))
            candidates.push_back(Candidate{ g.bodyId, entIdx });
    }

    // Deterministic processing order (ascending bodyId), independent of pool layout.
    std::sort(wells.begin(), wells.end(),
              [](const SoiWell& a, const SoiWell& b) { return a.bodyId < b.bodyId; });
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.bodyId < b.bodyId; });

    for (const Candidate& c : candidates)
    {
        ++result.evaluated;
        ecs::OrbitState& orbit = registry.GetByIndex<ecs::OrbitState>(c.entityIndex);
        const Vec3d bodyPos = registry.GetByIndex<ecs::Transform>(c.entityIndex).position;
        const WorldPos bodyWorld = WorldPos::FromOffset(bodyPos);
        const uint64_t current = orbit.primaryBodyId;

        // A body is never its own primary: resolve against wells excluding itself
        // (a source at distance 0 from its own centre would always win otherwise).
        std::vector<SoiWell> others;
        others.reserve(wells.size());
        for (const SoiWell& w : wells)
            if (w.bodyId != c.bodyId)
                others.push_back(w);

        const uint64_t dominant =
            ResolveSoiWithHysteresis(bodyWorld, others, current, hysteresis);
        if (dominant == kInvalidSoi || dominant == current)
            continue; // no primary, or unchanged

        const auto pit = indexById.find(dominant);
        if (pit == indexById.end())
            continue; // dominant well has no entity (should not happen)

        const uint32_t pIdx = pit->second;
        const Vec3d primaryPos = registry.GetByIndex<ecs::Transform>(pIdx).position;
        const Vec3d primaryVel = registry.GetByIndex<ecs::RigidBody>(pIdx).linearVelocity;
        const double primaryMu = registry.GetByIndex<ecs::GravitationalBody>(pIdx).mu;
        const Vec3d bodyVel = registry.GetByIndex<ecs::RigidBody>(c.entityIndex).linearVelocity;

        // Re-fit the orbit about the new primary, continuous in global (pos, vel).
        // A degenerate crossing (relative state exactly radial / co-moving, or the
        // body exactly at the primary) yields a NaN conic; committing it would
        // freeze StepPassiveOrbits forever, so skip the transition and keep the
        // current valid orbit. A real, non-radial crossing never hits this.
        const ecs::OrbitState refit =
            Repatch(bodyWorld, bodyVel, WorldPos::FromOffset(primaryPos),
                    primaryVel, primaryMu, dominant, now);
        if (!IsWellFormedOrbit(refit))
            continue;
        orbit = refit;
        ++result.transitions;
    }

    return result;
}

} // namespace sim
