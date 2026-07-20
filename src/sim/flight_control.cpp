// =============================================================================
// sim/flight_control.cpp — Control allocation + flight-assist law (GPU-free)
// =============================================================================

#include "flight_control.h"

#include "rigid_body.h" // InertiaDiagFromInverse — one shared I-from-inverse source

namespace sim
{

void AllocateThrusters(ecs::ThrusterSet& set,
                       const core::Vec3f& linearDemand,
                       const core::Vec3f& angularDemand,
                       const core::Vec3f& centreOfMass)
{
    const uint32_t n = (set.count < ecs::ThrusterSet::kMaxThrusters)
                           ? set.count
                           : ecs::ThrusterSet::kMaxThrusters;
    for (uint32_t i = 0; i < n; ++i)
    {
        ecs::Thruster& t = set.thrusters[i];

        // Linear alignment: how much this thruster's push serves the demanded
        // acceleration direction. dir is unit by contract.
        const float linAlign = linearDemand.Dot(t.localDirection);

        // Angular alignment: how much this thruster's torque axis serves the
        // demanded rotation. The torque this thruster makes about the CoM is
        // (pos-com) × (dir·maxForce); its AXIS is normalize((pos-com) × dir).
        // A thruster whose line of action runs through the CoM makes no torque
        // (near-zero cross product) and contributes nothing angular.
        const core::Vec3f lever   = t.localPosition - centreOfMass;
        const core::Vec3f torque  = lever.Cross(t.localDirection);
        float angAlign = 0.0f;
        if (torque.LengthSq() > 1e-12f)
            angAlign = angularDemand.Dot(torque.Normalized());

        // Forward-only: saturate to [0,1]. Opposing demand yields a negative sum
        // that clamps to zero, so it is served by the opposing thruster instead.
        // Zero demand -> both alignments 0 -> throttle EXACTLY 0 (negative control).
        t.throttle = core::Saturate(linAlign + angAlign);
    }
}

AssistWrench ComputeFlightAssist(const ecs::RigidBody& body,
                                 const core::Quatf& orientation,
                                 const core::Vec3f& linearDemand,
                                 const core::Vec3f& angularDemand,
                                 const FlightAssistParams& params)
{
    AssistWrench out;

    // ---- Linear: WORLD-frame proportional velocity controller ----------------
    // mass = 1/invMass. A static body (invMass <= 0) receives no linear assist.
    const double mass = (body.invMass > 0.0) ? 1.0 / body.invMass : 0.0;

    // Target body-frame velocity from the demand, rotated into world. At identity
    // orientation Rotate() is exact, so body == world for the closed-form tests.
    const core::Vec3f vTargetBody  = linearDemand * params.maxLinearSpeed;
    const core::Vec3d vTargetWorld =
        core::Vec3d::FromFloat(orientation.Normalized().Rotate(vTargetBody));
    const core::Vec3d vError = vTargetWorld - body.linearVelocity;

    // F = mass·gain·error  ->  a = F·invMass = gain·error (mass cancels): the
    // property the geometric closed form depends on.
    out.worldForce = vError * (mass * static_cast<double>(params.linearGain));

    // ---- Angular: BODY-frame proportional rate controller --------------------
    // T = I·gain·error  ->  alpha = Iinv·T = gain·error (I cancels). A locked axis
    // (invInertia == 0) has I == 0 here, so it receives no angular assist.
    const core::Vec3f inertiaDiag = InertiaDiagFromInverse(body.invInertiaDiag);
    const core::Vec3f wTarget = angularDemand * params.maxAngularRate;
    const core::Vec3f wError  = wTarget - body.angularVelocity;
    out.bodyTorque = {
        inertiaDiag.x * params.angularGain * wError.x,
        inertiaDiag.y * params.angularGain * wError.y,
        inertiaDiag.z * params.angularGain * wError.z,
    };

    return out;
}

} // namespace sim
