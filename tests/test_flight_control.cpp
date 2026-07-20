// =============================================================================
// tests/test_flight_control.cpp — Control allocation + flight-assist law
// =============================================================================
// Flight Stage 2. These call the SHIPPED sim::AllocateThrusters /
// sim::ComputeFlightAssist / sim::ComputeWrench / sim::IntegrateRigidBody, not a
// reimplementation — sim/*.{h,cpp} are GPU-free and linked into TheDawningTests.
//
// Every law is checked against ANALYTIC ground truth and paired with a NEGATIVE
// CONTROL, because this project's recurring defect is an assertion that stays
// green when the feature it guards is ABSENT.
//
// WATCHED FAILING (each mutation actually executed: source broken, suite rebuilt,
// the specific case observed failing, source restored, suite reconfirmed green):
//   (1) flip the linear-alignment sign in AllocateThrusters (linAlign -> -linAlign)
//         -> Allocation_PureLinearDemand* net force points -X: that case fails.
//   (2) flip the assist error (v_target - v -> v - v_target) so damping amplifies
//         -> Assist_LinearZeroDemandDecays* |v| grows: that case fails.
// Full results are in the agent's report.
// =============================================================================

#include "test_framework.h"

#include "core/types.h"
#include "ecs/components.h"
#include "sim/flight_control.h"
#include "sim/rigid_body.h"
#include "sim/thrusters.h"

#include <cmath>

namespace
{

constexpr double kDt = 1.0 / 60.0; // the engine's fixed step (RULE 6)

const core::Vec3d kZeroForceD{ 0.0, 0.0, 0.0 };
const core::Vec3f kZeroF{ 0.0f, 0.0f, 0.0f };

// A symmetric reaction-control layout: six thrusters, one per body axis-direction,
// each mounted BEHIND the CoM on the axis it pushes along. Every thruster's line
// of action runs through the origin, so each makes ZERO torque — pure translation
// demand therefore yields a pure force with no parasitic torque.
ecs::ThrusterSet MakeSymmetricLinearRig(float maxForce)
{
    ecs::ThrusterSet s;
    s.count = 6;
    const core::Vec3f dirs[6] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    for (uint32_t i = 0; i < 6; ++i)
    {
        s.thrusters[i].localDirection = dirs[i];
        s.thrusters[i].localPosition  = dirs[i] * -1.0f; // mounted opposite its push
        s.thrusters[i].maxForce       = maxForce;
        s.thrusters[i].throttle       = 0.0f;
    }
    return s;
}

// A pure couple about +X: two thrusters whose forces cancel but whose torques add.
ecs::ThrusterSet MakePitchCouple(float maxForce)
{
    ecs::ThrusterSet s;
    s.count = 2;
    s.thrusters[0].localPosition  = { 0.0f, 1.0f, 0.0f };
    s.thrusters[0].localDirection = { 0.0f, 0.0f, 1.0f };
    s.thrusters[0].maxForce       = maxForce;
    s.thrusters[1].localPosition  = { 0.0f, -1.0f, 0.0f };
    s.thrusters[1].localDirection = { 0.0f, 0.0f, -1.0f };
    s.thrusters[1].maxForce       = maxForce;
    return s;
}

} // namespace

// -----------------------------------------------------------------------------
// ALLOCATION — a pure +X linear demand fires only the +X thruster; the net BODY
// wrench is a pure +X force with (to tolerance) zero torque for the symmetric rig.
// Failure mode caught: a flipped allocation axis sign (fires the -X thruster, net
// force -X), or leaking torque from an asymmetric fire.
// -----------------------------------------------------------------------------
TEST_CASE(Allocation_PureLinearDemand_TracksAxisWithNoTorque)
{
    ecs::ThrusterSet rig = MakeSymmetricLinearRig(10.0f);

    sim::AllocateThrusters(rig, core::Vec3f{ 1.0f, 0.0f, 0.0f }, kZeroF, kZeroF);

    // Only the +X thruster (index 0, dir +X) is at full throttle; the opposing -X
    // and all off-axis thrusters are OFF (their alignment is <= 0).
    CHECK_APPROX(rig.thrusters[0].throttle, 1.0f);
    CHECK_EQ(rig.thrusters[1].throttle, 0.0f); // -X opposes the demand
    for (uint32_t i = 2; i < 6; ++i)
        CHECK_EQ(rig.thrusters[i].throttle, 0.0f);

    const sim::Wrench w = sim::ComputeWrench(rig, kZeroF);
    CHECK_APPROX(w.force.x, 10.0f); // net force along +X — the load-bearing sign
    CHECK_APPROX(w.force.y, 0.0f);
    CHECK_APPROX(w.force.z, 0.0f);
    CHECK_APPROX(w.torque.x, 0.0f); // symmetric rig: no parasitic torque
    CHECK_APPROX(w.torque.y, 0.0f);
    CHECK_APPROX(w.torque.z, 0.0f);

    // Opposing demand is served by the OPPOSING thruster (task requirement), not by
    // a negative throttle: a -X demand fires -X and produces -X force.
    ecs::ThrusterSet rig2 = MakeSymmetricLinearRig(10.0f);
    sim::AllocateThrusters(rig2, core::Vec3f{ -1.0f, 0.0f, 0.0f }, kZeroF, kZeroF);
    CHECK_EQ(rig2.thrusters[0].throttle, 0.0f); // +X now opposes
    CHECK_APPROX(rig2.thrusters[1].throttle, 1.0f);
    const sim::Wrench w2 = sim::ComputeWrench(rig2, kZeroF);
    CHECK_APPROX(w2.force.x, -10.0f);

    // Half demand -> half throttle -> half force (proportional, not bang-bang).
    ecs::ThrusterSet rig3 = MakeSymmetricLinearRig(10.0f);
    sim::AllocateThrusters(rig3, core::Vec3f{ 0.5f, 0.0f, 0.0f }, kZeroF, kZeroF);
    CHECK_APPROX(rig3.thrusters[0].throttle, 0.5f);
    CHECK_APPROX(sim::ComputeWrench(rig3, kZeroF).force.x, 5.0f);
}

// -----------------------------------------------------------------------------
// NEGATIVE CONTROL for allocation — ZERO demand produces EXACTLY 0.0f throttle on
// every thruster (not "some small value") and therefore an EXACTLY zero wrench.
// This is the direct antidote to "passes when the feature is absent": an allocator
// that idles thrusters at a bias, or always nudges the body, fails here.
// -----------------------------------------------------------------------------
TEST_CASE(Allocation_ZeroDemand_ProducesExactlyZeroThrottle)
{
    ecs::ThrusterSet rig = MakeSymmetricLinearRig(10.0f);
    // Pre-load nonzero throttles so the test proves allocation WROTE zero, not that
    // the field merely started at zero.
    for (uint32_t i = 0; i < rig.count; ++i)
        rig.thrusters[i].throttle = 0.7f;

    sim::AllocateThrusters(rig, kZeroF, kZeroF, kZeroF);

    for (uint32_t i = 0; i < rig.count; ++i)
        CHECK_EQ(rig.thrusters[i].throttle, 0.0f); // EXACT zero, every thruster

    const sim::Wrench w = sim::ComputeWrench(rig, kZeroF);
    CHECK_EQ(w.force.x, 0.0f);  CHECK_EQ(w.force.y, 0.0f);  CHECK_EQ(w.force.z, 0.0f);
    CHECK_EQ(w.torque.x, 0.0f); CHECK_EQ(w.torque.y, 0.0f); CHECK_EQ(w.torque.z, 0.0f);
}

// -----------------------------------------------------------------------------
// ALLOCATION — a pure +X angular (pitch) demand fires the couple, producing a pure
// +X torque with zero net force. Exercises the torque-axis half of the allocator.
// -----------------------------------------------------------------------------
TEST_CASE(Allocation_PureAngularDemand_TracksTorqueAxisWithNoForce)
{
    ecs::ThrusterSet couple = MakePitchCouple(10.0f);

    sim::AllocateThrusters(couple, kZeroF, core::Vec3f{ 1.0f, 0.0f, 0.0f }, kZeroF);

    CHECK_APPROX(couple.thrusters[0].throttle, 1.0f);
    CHECK_APPROX(couple.thrusters[1].throttle, 1.0f);

    const sim::Wrench w = sim::ComputeWrench(couple, kZeroF);
    CHECK_APPROX(w.force.x, 0.0f);   // forces cancel
    CHECK_APPROX(w.force.y, 0.0f);
    CHECK_APPROX(w.force.z, 0.0f);
    CHECK_APPROX(w.torque.x, 20.0f); // torques add: pure +X torque
    CHECK_APPROX(w.torque.y, 0.0f);
    CHECK_APPROX(w.torque.z, 0.0f);

    // A pitch demand about -X fires neither thruster (both align +X): forward-only.
    ecs::ThrusterSet couple2 = MakePitchCouple(10.0f);
    sim::AllocateThrusters(couple2, kZeroF, core::Vec3f{ -1.0f, 0.0f, 0.0f }, kZeroF);
    CHECK_EQ(couple2.thrusters[0].throttle, 0.0f);
    CHECK_EQ(couple2.thrusters[1].throttle, 0.0f);
}

// -----------------------------------------------------------------------------
// FLIGHT ASSIST — linear zero-demand decay against the EXACT geometric closed
// form. Because the assist commands a = linearGain·(v_target − v) (mass cancels)
// and v_target = 0, semi-implicit Euler gives v_n = v_0·(1 − linearGain·dt)^n
// EXACTLY (all double). Asserted as a tight equality, plus strict monotonic decay
// and a small bound within a known time.
// Failure mode caught: sign-flipped damping (amplifies), damping applied to the
// wrong quantity, a gain that does not scale with dt.
// -----------------------------------------------------------------------------
TEST_CASE(Assist_LinearZeroDemandDecays_MatchesGeometricClosedForm)
{
    sim::FlightAssistParams params;
    params.linearGain = 3.0f; // linearGain·dt = 0.05 -> factor 0.95

    ecs::RigidBody body;
    body.invMass        = 0.5;                       // mass 2; invMass != 1 detects a
                                                     // dropped mass in the a=gain·err law
    body.invInertiaDiag = { 0.5f, 0.5f, 0.5f };      // symmetric: gyro stays zero
    const core::Vec3d v0{ 10.0, -6.0, 4.0 };
    body.linearVelocity = v0;

    core::Vec3d pos{};
    core::Quatf orn = core::Quatf::Identity();

    const double factor = 1.0 - static_cast<double>(params.linearGain) * kDt;
    double prevSpeed = body.linearVelocity.Length();

    const int N = 120;
    for (int i = 0; i < N; ++i)
    {
        const sim::AssistWrench a = sim::ComputeFlightAssist(
            body, orn, kZeroF, kZeroF, params); // zero demand
        body.forceAccum  += a.worldForce;
        body.torqueAccum += a.bodyTorque;
        sim::IntegrateRigidBody(pos, orn, body, kZeroForceD, kZeroF, kDt);

        const double speed = body.linearVelocity.Length();
        CHECK(speed < prevSpeed); // STRICT monotonic decay, every step
        prevSpeed = speed;
    }

    // Exact closed form v_N = v0 · factor^N (double, tight).
    const double pw = std::pow(factor, N);
    CHECK_APPROX_EPS(body.linearVelocity.x, v0.x * pw, 1e-9);
    CHECK_APPROX_EPS(body.linearVelocity.y, v0.y * pw, 1e-9);
    CHECK_APPROX_EPS(body.linearVelocity.z, v0.z * pw, 1e-9);

    // Reaches a small bound within a known time (N steps ~= 2 s): 0.95^120 ~= 2e-3,
    // times |v0| ~= 12.4 -> ~0.025.
    CHECK(body.linearVelocity.Length() < 0.05);
    // ...and the decay was real, not a body that started slow.
    CHECK(v0.Length() > 10.0);
}

// -----------------------------------------------------------------------------
// FLIGHT ASSIST — angular zero-demand decay. With SYMMETRIC inertia the gyroscopic
// coupling is exactly zero, so ω follows the same geometric law ω_n =
// ω_0·(1 − angularGain·dt)^n. ω is Vec3f, so this is checked to a float tolerance
// plus strict monotonic decay and a small bound.
// -----------------------------------------------------------------------------
TEST_CASE(Assist_AngularZeroDemandDecays_TowardZero)
{
    sim::FlightAssistParams params;
    params.angularGain = 3.0f;

    ecs::RigidBody body;
    body.invMass        = 1.0;
    body.invInertiaDiag = { 0.5f, 0.5f, 0.5f }; // I = 2, symmetric -> gyro == 0
    const core::Vec3f w0{ 1.0f, -0.5f, 0.3f };
    body.angularVelocity = w0;

    core::Vec3d pos{};
    core::Quatf orn = core::Quatf::Identity();

    const float factor = 1.0f - params.angularGain * static_cast<float>(kDt);
    float prevRate = body.angularVelocity.Length();

    const int N = 120;
    for (int i = 0; i < N; ++i)
    {
        const sim::AssistWrench a = sim::ComputeFlightAssist(
            body, orn, kZeroF, kZeroF, params);
        body.forceAccum  += a.worldForce;
        body.torqueAccum += a.bodyTorque;
        sim::IntegrateRigidBody(pos, orn, body, kZeroForceD, kZeroF, kDt);

        const float rate = body.angularVelocity.Length();
        CHECK(rate < prevRate); // strict monotonic decay
        prevRate = rate;
    }

    const float pw = std::pow(factor, static_cast<float>(N));
    CHECK_APPROX_EPS(body.angularVelocity.x, w0.x * pw, 1e-4f);
    CHECK_APPROX_EPS(body.angularVelocity.y, w0.y * pw, 1e-4f);
    CHECK_APPROX_EPS(body.angularVelocity.z, w0.z * pw, 1e-4f);
    CHECK(body.angularVelocity.Length() < 0.02f);
    CHECK(w0.Length() > 1.0f);
}

// -----------------------------------------------------------------------------
// FLIGHT ASSIST — with a NONZERO velocity demand the body converges TO the demanded
// velocity, NOT to zero. Closed form v_n = v_target·(1 − factor^n). This is the
// discriminator the zero-demand decay test cannot make: a controller that simply
// drained energy would send v -> 0 here and fail.
// -----------------------------------------------------------------------------
TEST_CASE(Assist_VelocityDemand_ConvergesToDemandNotZero)
{
    sim::FlightAssistParams params;
    params.linearGain     = 3.0f;
    params.maxLinearSpeed = 100.0f;

    ecs::RigidBody body;
    body.invMass        = 0.5;
    body.invInertiaDiag = { 0.5f, 0.5f, 0.5f };
    // Starts at rest.

    core::Vec3d pos{};
    core::Quatf orn = core::Quatf::Identity();

    const core::Vec3f demand{ 1.0f, 0.0f, 0.0f }; // full +X: target = 100 m/s +X
    const double vTarget = static_cast<double>(params.maxLinearSpeed);
    const double factor  = 1.0 - static_cast<double>(params.linearGain) * kDt;

    const int N = 240;
    for (int i = 0; i < N; ++i)
    {
        const sim::AssistWrench a = sim::ComputeFlightAssist(
            body, orn, demand, kZeroF, params);
        body.forceAccum  += a.worldForce;
        body.torqueAccum += a.bodyTorque;
        sim::IntegrateRigidBody(pos, orn, body, kZeroForceD, kZeroF, kDt);
    }

    // v_n = vTarget·(1 − factor^N): converges to the demand, tight closed form.
    const double expected = vTarget * (1.0 - std::pow(factor, N));
    CHECK_APPROX_EPS(body.linearVelocity.x, expected, 1e-7);
    CHECK_APPROX_EPS(body.linearVelocity.y, 0.0, 1e-9);
    CHECK_APPROX_EPS(body.linearVelocity.z, 0.0, 1e-9);

    // The discriminator: it settled NEAR the demand (>= 99 m/s), NOT near zero.
    CHECK(body.linearVelocity.x > 99.0);
}
