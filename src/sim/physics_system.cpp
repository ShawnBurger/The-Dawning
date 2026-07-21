// =============================================================================
// sim/physics_system.cpp — Fixed-step flight physics system (ECS glue, GPU-free)
// =============================================================================

#include "physics_system.h"

#include "rigid_body.h"
#include "thrusters.h"

#include <cmath>
#include <cstdint>

namespace sim
{

void StepFlightPhysics(ecs::Registry& registry, double dt, const FlightAssistParams& params)
{
    // Whole-system no-op on a bad step, so the fixed-step accumulator can retry
    // without any body advancing on stale state (matches IntegrateRigidBody).
    if (!(dt > 0.0) || !std::isfinite(dt))
        return;

    // No external accelerations in Stage 2 (gravity is Stage 5, §4). The body
    // origin is the centre of mass, so lever arms are measured from zero.
    const core::Vec3d kZeroForce{ 0.0, 0.0, 0.0 };
    const core::Vec3f kZeroTorque{ 0.0f, 0.0f, 0.0f };
    const core::Vec3f kCoM{ 0.0f, 0.0f, 0.0f };

    registry.Each<ecs::Transform, ecs::RigidBody>(
        [&](uint32_t entityIndex, ecs::Transform& transform, ecs::RigidBody& body)
        {
            const bool hasControl   = registry.HasByIndex<ecs::FlightControl>(entityIndex);
            const bool hasThrusters = registry.HasByIndex<ecs::ThrusterSet>(entityIndex);

            if (hasControl)
            {
                const ecs::FlightControl& fc =
                    registry.GetByIndex<ecs::FlightControl>(entityIndex);

                if (fc.mode == ecs::FlightMode::Coupled)
                {
                    // Flight assist has no reactionless fallback. Its desired
                    // wrench is realized only through installed nozzles, and those
                    // throttles are also the exhaust/damage feedback state.
                    if (hasThrusters)
                    {
                        ecs::ThrusterSet& ts =
                            registry.GetByIndex<ecs::ThrusterSet>(entityIndex);
                        const AssistWrench desired = ComputeFlightAssist(
                            body, transform.rotation, fc.linearDemand,
                            fc.angularDemand, params);
                        AllocateThrustersForWrench(
                            ts, desired.worldForce, desired.bodyTorque,
                            transform.rotation, kCoM);
                        const Wrench realized = ComputeWrench(ts, kCoM);
                        AccumulateBodyWrench(body, transform.rotation, realized);
                    }
                }
                else if (hasThrusters)
                {
                    // Decoupled: pilot demand -> greedy allocation -> thruster wrench.
                    ecs::ThrusterSet& ts =
                        registry.GetByIndex<ecs::ThrusterSet>(entityIndex);
                    AllocateThrusters(ts, fc.linearDemand, fc.angularDemand, kCoM);
                    const Wrench w = ComputeWrench(ts, kCoM);
                    AccumulateBodyWrench(body, transform.rotation, w);
                }
                // Decoupled with no thrusters has no actuators -> pure coast.
            }
            else if (hasThrusters)
            {
                // Scripted thrusters (no pilot): realise whatever throttles are set.
                ecs::ThrusterSet& ts = registry.GetByIndex<ecs::ThrusterSet>(entityIndex);
                const Wrench w = ComputeWrench(ts, kCoM);
                AccumulateBodyWrench(body, transform.rotation, w);
            }

            // One semi-implicit-Euler step of the PASSED dt, writing the pose back
            // into Transform and zeroing the accumulators.
            IntegrateRigidBody(transform.position, transform.rotation, body,
                               kZeroForce, kZeroTorque, dt);
        });
}

} // namespace sim
