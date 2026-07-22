// =============================================================================
// scene/star_system_seed.cpp — see star_system_seed.h.
// =============================================================================

#include "star_system_seed.h"

#include "../ecs/components.h"
#include "../core/types.h"

#include <vector>

namespace scene
{

using core::Color;

sim::StarSystem OffsetStarSystemBodyIds(const sim::StarSystem& sys, uint64_t base)
{
    sim::StarSystem out = sys;
    for (sim::SystemBody& b : out.bodies)
    {
        b.bodyId += base;
        if (b.primaryBodyId != 0)          // the central star keeps primary 0
            b.primaryBodyId += base;
        if (b.hasOrbit)                    // the orbit references the (shifted) primary
            b.orbit.primaryBodyId += base;
    }
    return out;
}

namespace
{

struct Visual
{
    Color albedo;
    float roughness;
    float metallic;
    Color emissive;
    float emissiveStrength;
};

// Recognisable materials for the reference system, keyed by the body's ORIGINAL
// (un-offset) id. The star is the emitter; the rest are lit surfaces. Unknown
// ids fall back to a neutral rock so a larger system still renders.
Visual VisualFor(uint64_t localId, bool isStar)
{
    if (isStar)
        return { Color{ 0.05f, 0.05f, 0.05f, 1.0f }, 1.0f, 0.0f,
                 Color{ 1.0f, 0.95f, 0.85f, 1.0f }, 4.0f };
    switch (localId)
    {
        case 10: return { Color{ 0.20f, 0.42f, 0.85f, 1.0f }, 0.85f, 0.0f,
                          Color{ 0, 0, 0, 1 }, 0.0f }; // Earth — blue
        case 11: return { Color{ 0.55f, 0.55f, 0.55f, 1.0f }, 0.95f, 0.0f,
                          Color{ 0, 0, 0, 1 }, 0.0f }; // Moon — grey
        case 20: return { Color{ 0.72f, 0.32f, 0.16f, 1.0f }, 0.90f, 0.0f,
                          Color{ 0, 0, 0, 1 }, 0.0f }; // Mars — rust
        default: return { Color{ 0.75f, 0.75f, 0.78f, 1.0f }, 0.85f, 0.0f,
                          Color{ 0, 0, 0, 1 }, 0.0f };
    }
}

} // namespace

uint32_t AttachStarSystemVisuals(ecs::Registry& registry, uint64_t bodyIdBase,
                                 uint32_t sphereMeshHandle, float meshRadius)
{
    if (!(meshRadius > 0.0f))
        return 0;

    auto* pool = registry.GetPool<ecs::GravitationalBody>();
    if (!pool)
        return 0;

    // Collect first, mutate second: attaching MeshInstance/Material/Transform
    // touches OTHER pools, but gathering the targets up front keeps this robust to
    // any pool growth during the attach.
    struct Seeded { ecs::Entity entity; double radius; uint64_t bodyId; bool isStar; };
    std::vector<Seeded> seeded;
    seeded.reserve(pool->Count());
    for (uint32_t i = 0; i < pool->Count(); ++i)
    {
        const ecs::GravitationalBody& g = pool->DataAt(i);
        if (g.bodyId < bodyIdBase)
            continue; // a gameplay body (e.g. the ship), not part of the seed
        const ecs::Entity e = registry.EntityAtIndex(pool->EntityAt(i));
        const bool isStar = !registry.Has<ecs::OrbitState>(e);
        seeded.push_back({ e, g.radius, g.bodyId, isStar });
    }

    for (const Seeded& s : seeded)
    {
        registry.Assign<ecs::MeshInstance>(
            s.entity, ecs::MeshInstance{ sphereMeshHandle, true });

        // True-radius render size: the sphere mesh spans meshRadius, so scale it
        // by bodyRadius/meshRadius. (The per-mode render scale K, added later,
        // multiplies position AND size uniformly on top of this.)
        const float scale = static_cast<float>(s.radius) / meshRadius;
        if (ecs::Transform* t = registry.TryGet<ecs::Transform>(s.entity))
            t->scale = { scale, scale, scale };

        const Visual v = VisualFor(s.bodyId - bodyIdBase, s.isStar);
        ecs::Material m;
        m.albedo           = v.albedo;
        m.roughness        = v.roughness;
        m.metallic         = v.metallic;
        m.emissive         = v.emissive;
        m.emissiveStrength = v.emissiveStrength;
        registry.Assign<ecs::Material>(s.entity, m);
    }

    return static_cast<uint32_t>(seeded.size());
}

} // namespace scene
