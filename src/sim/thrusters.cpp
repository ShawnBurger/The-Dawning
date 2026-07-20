// =============================================================================
// sim/thrusters.cpp — Thruster wrench accumulation (GPU-free)
// =============================================================================

#include "thrusters.h"

namespace sim
{

Wrench ComputeWrench(const ecs::ThrusterSet& set, const core::Vec3f& centreOfMass)
{
    Wrench w;
    const uint32_t n = (set.count < ecs::ThrusterSet::kMaxThrusters)
                           ? set.count
                           : ecs::ThrusterSet::kMaxThrusters;
    for (uint32_t i = 0; i < n; ++i)
    {
        const ecs::Thruster& t = set.thrusters[i];
        // localDirection is a unit vector by contract; used as-is so the force
        // formula matches the design exactly (no hidden renormalisation).
        const core::Vec3f force = t.localDirection * (t.maxForce * t.throttle);
        const core::Vec3f lever = t.localPosition - centreOfMass;
        w.force  += force;
        w.torque += lever.Cross(force); // r × F: torque about the CoM
    }
    return w;
}

void AccumulateBodyWrench(ecs::RigidBody& body,
                          const core::Quatf& orientation,
                          const Wrench& wrench)
{
    const core::Vec3f worldForce = orientation.Normalized().Rotate(wrench.force);
    body.forceAccum  += core::Vec3d::FromFloat(worldForce);
    body.torqueAccum += wrench.torque;
}

} // namespace sim
