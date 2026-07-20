#pragma once
// =============================================================================
// ecs/systems.h - Pure ECS simulation systems
// =============================================================================

#include "components.h"
#include "registry.h"

#include <cmath>

namespace ecs::systems
{

inline void IntegrateVelocity(Transform& transform, const Velocity& velocity, double dt)
{
    if (!(dt > 0.0) || !std::isfinite(dt))
        return;

    transform.position += core::Vec3d::FromFloat(velocity.linear) * dt;

    const float angularSpeed = velocity.angular.Length();
    if (angularSpeed <= 1e-8f || !std::isfinite(angularSpeed))
        return;

    const core::Vec3f axis = velocity.angular / angularSpeed;
    const float angle = angularSpeed * static_cast<float>(dt);
    const core::Quatf delta = core::Quatf::FromAxisAngle(axis, angle);
    transform.rotation = (transform.rotation * delta).Normalized();
}

inline void IntegrateVelocities(Registry& registry, double dt)
{
    if (!(dt > 0.0) || !std::isfinite(dt))
        return;

    registry.Each<Transform, Velocity>(
        [&registry, dt](uint32_t entityIndex, Transform& transform, Velocity& velocity)
        {
            // RigidBody is the authoritative pose owner for dynamic entities.
            // Legacy Velocity may remain during an ownership transition, but it
            // must not advance the same Transform before flight physics does.
            if (registry.HasByIndex<RigidBody>(entityIndex))
                return;

            IntegrateVelocity(transform, velocity, dt);
        });
}

inline void IntegrateRotation(Transform& transform, const RotationSpeed& spin, double dt)
{
    if (!(dt > 0.0) || !std::isfinite(dt))
        return;

    const float angle = spin.radiansPerSecond * static_cast<float>(dt);
    const core::Quatf delta = core::Quatf::FromAxisAngle(spin.axis, angle);
    transform.rotation = (transform.rotation * delta).Normalized();
}

inline void IntegrateRotations(Registry& registry, double dt)
{
    if (!(dt > 0.0) || !std::isfinite(dt))
        return;

    registry.Each<Transform, RotationSpeed>(
        [&registry, dt](uint32_t entityIndex, Transform& transform, RotationSpeed& spin)
        {
            // As above, decorative spin cannot also own a rigid body's pose.
            if (registry.HasByIndex<RigidBody>(entityIndex))
                return;

            IntegrateRotation(transform, spin, dt);
        });
}

} // namespace ecs::systems
