// =============================================================================
// sim/flight_control.cpp — Control allocation + flight-assist law (GPU-free)
// =============================================================================

#include "flight_control.h"

#include "rigid_body.h" // InertiaDiagFromInverse — one shared I-from-inverse source

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>

namespace sim
{

namespace
{

constexpr size_t kWrenchAxes = 6;
constexpr int kAllocationPasses = 32;

bool IsFinite(const core::Vec3d& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool IsFinite(const core::Vec3f& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool TryWorldToBody(const core::Quatf& orientation,
                    const core::Vec3d& world,
                    core::Vec3d& body)
{
    const double x = static_cast<double>(orientation.x);
    const double y = static_cast<double>(orientation.y);
    const double z = static_cast<double>(orientation.z);
    const double w = static_cast<double>(orientation.w);
    const double lengthSq = x * x + y * y + z * z + w * w;
    if (!std::isfinite(lengthSq) || !(lengthSq > 0.0) || !IsFinite(world))
        return false;

    const double invLength = 1.0 / std::sqrt(lengthSq);
    const core::Vec3d qv{ -x * invLength, -y * invLength, -z * invLength };
    const double qw = w * invLength;

    const double scale = (std::max)({ std::fabs(world.x), std::fabs(world.y),
                                      std::fabs(world.z) });
    core::Vec3d operand = world;
    constexpr double kDirectLimit = 1.0e150;
    if (scale > kDirectLimit)
        operand = world / scale;

    const core::Vec3d uv = qv.Cross(operand);
    const core::Vec3d uuv = qv.Cross(uv);
    body = operand + (uv * qw + uuv) * 2.0;
    if (scale > kDirectLimit)
        body *= scale;
    return IsFinite(body);
}

double NormalizeTarget(double target, double authority)
{
    if (!(authority > 0.0) || !std::isfinite(authority))
        return 0.0;
    if (target >= authority)
        return 1.0;
    if (target <= -authority)
        return -1.0;
    return target / authority;
}

struct AllocationColumn
{
    std::array<double, kWrenchAxes> value{};
    bool usable = false;
};

} // namespace

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

void AllocateThrustersForWrench(ecs::ThrusterSet& set,
                                const core::Vec3d& desiredWorldForce,
                                const core::Vec3f& desiredBodyTorque,
                                const core::Quatf& orientation,
                                const core::Vec3f& centreOfMass)
{
    const uint32_t n = (set.count < ecs::ThrusterSet::kMaxThrusters)
                           ? set.count
                           : ecs::ThrusterSet::kMaxThrusters;

    // Start from zero every step so stale throttle cannot leak across mode
    // switches or impossible requests.
    for (uint32_t i = 0; i < n; ++i)
        set.thrusters[i].throttle = 0.0f;

    core::Vec3d desiredBodyForce;
    if (!IsFinite(desiredBodyTorque) || !IsFinite(centreOfMass) ||
        !TryWorldToBody(orientation, desiredWorldForce, desiredBodyForce))
        return;

    std::array<AllocationColumn, ecs::ThrusterSet::kMaxThrusters> columns{};
    std::array<double, kWrenchAxes> authority{};

    for (uint32_t i = 0; i < n; ++i)
    {
        const ecs::Thruster& thruster = set.thrusters[i];
        const double maxForce = static_cast<double>(thruster.maxForce);
        if (!(maxForce > 0.0) || !std::isfinite(maxForce) ||
            !IsFinite(thruster.localDirection) ||
            !IsFinite(thruster.localPosition))
            continue;

        const core::Vec3d direction = core::Vec3d::FromFloat(thruster.localDirection);
        const core::Vec3d lever = core::Vec3d::FromFloat(
            thruster.localPosition - centreOfMass);
        const core::Vec3d force = direction * maxForce;
        const core::Vec3d torque = lever.Cross(force);
        if (!IsFinite(force) || !IsFinite(torque))
            continue;

        AllocationColumn& column = columns[i];
        column.value = { force.x, force.y, force.z,
                         torque.x, torque.y, torque.z };
        column.usable = true;
        for (size_t axis = 0; axis < kWrenchAxes; ++axis)
            authority[axis] += std::fabs(column.value[axis]);
    }

    const std::array<double, kWrenchAxes> rawTarget = {
        desiredBodyForce.x, desiredBodyForce.y, desiredBodyForce.z,
        static_cast<double>(desiredBodyTorque.x),
        static_cast<double>(desiredBodyTorque.y),
        static_cast<double>(desiredBodyTorque.z),
    };
    std::array<double, kWrenchAxes> target{};
    for (size_t axis = 0; axis < kWrenchAxes; ++axis)
        target[axis] = NormalizeTarget(rawTarget[axis], authority[axis]);

    // Normalize each column by installed per-axis authority. Projected
    // Gauss-Seidel coordinate descent minimizes the six-axis residual under
    // 0 <= throttle <= 1. Fixed passes and nozzle order make it deterministic.
    for (uint32_t i = 0; i < n; ++i)
    {
        if (!columns[i].usable)
            continue;
        for (size_t axis = 0; axis < kWrenchAxes; ++axis)
        {
            if (authority[axis] > 0.0 && std::isfinite(authority[axis]))
                columns[i].value[axis] /= authority[axis];
            else
                columns[i].value[axis] = 0.0;
        }
    }

    std::array<double, ecs::ThrusterSet::kMaxThrusters> throttle{};
    std::array<double, kWrenchAxes> realized{};
    for (int pass = 0; pass < kAllocationPasses; ++pass)
    {
        bool changed = false;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (!columns[i].usable)
                continue;

            double numerator = 0.0;
            double denominator = 0.0;
            for (size_t axis = 0; axis < kWrenchAxes; ++axis)
            {
                const double column = columns[i].value[axis];
                numerator += column * (target[axis] - realized[axis]);
                denominator += column * column;
            }
            if (!(denominator > 0.0) || !std::isfinite(denominator))
                continue;

            const double next = (std::clamp)(throttle[i] + numerator / denominator,
                                              0.0, 1.0);
            const double delta = next - throttle[i];
            if (std::fabs(delta) <= 1.0e-12)
                continue;

            throttle[i] = next;
            changed = true;
            for (size_t axis = 0; axis < kWrenchAxes; ++axis)
                realized[axis] += columns[i].value[axis] * delta;
        }
        if (!changed)
            break;
    }

    for (uint32_t i = 0; i < n; ++i)
        set.thrusters[i].throttle = static_cast<float>(throttle[i]);
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
