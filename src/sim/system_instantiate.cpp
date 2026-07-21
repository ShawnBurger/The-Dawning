// =============================================================================
// sim/system_instantiate.cpp — instantiate a StarSystem into ECS entities.
// =============================================================================

#include "system_instantiate.h"

#include "nbody.h"           // PromoteFromRails
#include "kepler.h"          // StateVector
#include "../ecs/components.h"

#include <cstdint>
#include <unordered_map>

namespace sim {

using core::Vec3d;

uint32_t InstantiateStarSystem(ecs::Registry& registry, FrameId frame,
                               const StarSystem& system)
{
    // In-frame (star-relative) state of each already-created body, so a rails body
    // can be placed relative to its primary. Parent-before-child guarantees the
    // primary is present when a child is processed.
    struct LocalState { Vec3d position; Vec3d velocity; };
    std::unordered_map<uint64_t, LocalState> placed;
    placed.reserve(system.bodies.size() * 2u);

    uint32_t created = 0;
    for (const SystemBody& b : system.bodies)
    {
        Vec3d localPos{ 0.0, 0.0, 0.0 };
        Vec3d localVel{ 0.0, 0.0, 0.0 };
        if (b.hasOrbit)
        {
            // The primary must already be placed (parent-before-child). Skip a body
            // whose primary is absent (malformed input) rather than default a child
            // onto the star — a partial world beats a wrong one.
            const auto primary = placed.find(b.primaryBodyId);
            if (primary == placed.end())
                continue;
            const StateVector rel = PromoteFromRails(b.orbit, 0.0);
            localPos = primary->second.position + rel.position;
            localVel = primary->second.velocity + rel.velocity;
        }
        placed.emplace(b.bodyId, LocalState{ localPos, localVel });

        const ecs::Entity entity = registry.Create();

        ecs::Transform transform;
        transform.position = localPos;
        registry.Assign<ecs::Transform>(entity, transform);

        ecs::RigidBody body;
        body.linearVelocity = localVel;
        body.invMass        = b.invMass;
        body.prevPosition   = localPos;
        registry.Assign<ecs::RigidBody>(entity, body);

        registry.Assign<ecs::SpatialFrame>(
            entity, ecs::SpatialFrame{ static_cast<uint32_t>(frame) });

        ecs::GravitationalBody gravity;
        gravity.bodyId   = b.bodyId;
        gravity.owner    = b.owner;
        gravity.mu       = b.mu;
        gravity.radius   = b.radius;
        gravity.isSource = b.isSource;
        registry.Assign<ecs::GravitationalBody>(entity, gravity);

        if (b.hasOrbit)
            registry.Assign<ecs::OrbitState>(entity, b.orbit);

        ++created;
    }
    return created;
}

} // namespace sim
