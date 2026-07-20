// =============================================================================
// sim/physics_system.cpp — Fixed-step flight physics system (ECS glue, GPU-free)
// =============================================================================

#include "physics_system.h"

#include "rigid_body.h"
#include "thrusters.h"

#include <cmath>

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
                    // Flight assist: proportional velocity controller applied as an
                    // idealised reaction wrench (world force + body torque). Kept a
                    // direct wrench rather than re-quantised through the clamped
                    // thruster bank precisely so it has the exact closed form the
                    // tests check (flight_control.h). Thruster-limited coupled mode
                    // is a follow-on.
                    const AssistWrench a = ComputeFlightAssist(
                        body, transform.rotation, fc.linearDemand, fc.angularDemand, params);
                    body.forceAccum  += a.worldForce;
                    body.torqueAccum += a.bodyTorque;
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
