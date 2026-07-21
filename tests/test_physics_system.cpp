// =============================================================================
// tests/test_physics_system.cpp — Fixed-step flight physics over the real ECS
// =============================================================================
// Drives the SHIPPED sim::StepFlightPhysics over a real ecs::Registry — the same
// function Scene::UpdateSystems calls every fixed step — so the WIRING (mode
// selection, per-entity dispatch, writing Transform, honouring the passed dt) is
// what is under test, not a stand-in.
//
// The defining pairing is CoupledUsesThrusters_DecoupledCoasts: the SAME moving
// body and actuator bank brake under flight assist and COAST with assist off. A
// test where the body decays in both modes would prove nothing; the contrast is
// the evidence.
//
// WATCHED FAILING (each mutation executed, the specific case observed failing,
// then restored and reconfirmed green):
//   (2) flip the assist error sign (v_target - v -> v - v_target)
//         -> Coupled* body diverges instead of decaying: that case fails.
//   (3) make StepFlightPhysics ignore the passed dt (integrate with a hardcoded
//       1.0/30.0), i.e. run the step OFF the fixed timestep
//         -> CoupledUsesThrusters* and StepAdvancesByExactlyOneFixedStep fail; the
//            body no longer advances by the dt the accumulator handed it.
// Full results are in the agent's report.
// =============================================================================

#include "test_framework.h"

#include "core/types.h"
#include "ecs/components.h"
#include "ecs/registry.h"
#include "sim/physics_system.h"
#include "sim/rigid_body.h"
#include "sim/flight_control.h"
#include "sim/thrusters.h"

#include <cmath>
#include <cstdint>

namespace
{

constexpr double kDt = 1.0 / 60.0;
const core::Vec3f kZeroF{ 0.0f, 0.0f, 0.0f };

// Assist params with a round decay factor: linearGain·dt = angularGain·dt = 0.05.
sim::FlightAssistParams TestParams()
{
    sim::FlightAssistParams p;
    p.linearGain     = 3.0f;
    p.angularGain    = 3.0f;
    p.maxLinearSpeed = 100.0f;
    p.maxAngularRate = 2.0f;
    return p;
}

// A flyable body: mass 2 (invMass 0.5, non-unit so a dropped mass shows), symmetric
// inertia so the gyroscopic term stays exactly zero and the assist law is clean.
ecs::Entity MakeShip(ecs::Registry& reg, ecs::FlightMode mode,
                     const core::Vec3d& v0, const core::Vec3f& w0)
{
    ecs::Entity e = reg.Create();
    ecs::Transform t;
    reg.Assign<ecs::Transform>(e, t);

    ecs::RigidBody b;
    b.invMass         = 0.5;
    b.invInertiaDiag  = { 0.5f, 0.5f, 0.5f };
    b.linearVelocity  = v0;
    b.angularVelocity = w0;
    reg.Assign<ecs::RigidBody>(e, b);

    ecs::FlightControl fc;
    fc.mode = mode;
    reg.Assign<ecs::FlightControl>(e, fc);
    return e;
}

// Symmetric reaction-control rig (one thruster per body axis, each through the CoM).
ecs::ThrusterSet SymmetricRig(float maxForce)
{
    ecs::ThrusterSet s;
    s.count = 6;
    const core::Vec3f dirs[6] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    for (uint32_t i = 0; i < 6; ++i)
    {
        s.thrusters[i].localDirection = dirs[i];
        s.thrusters[i].localPosition  = dirs[i] * -1.0f;
        s.thrusters[i].maxForce       = maxForce;
    }
    return s;
}

uint64_t HashRigidState(ecs::Registry& reg, ecs::Entity e)
{
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](const void* data, size_t n)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
    };
    const ecs::Transform& t = reg.Get<ecs::Transform>(e);
    const ecs::RigidBody&  b = reg.Get<ecs::RigidBody>(e);
    mix(&t.position, sizeof(t.position));
    mix(&t.rotation, sizeof(t.rotation));
    mix(&b.linearVelocity, sizeof(b.linearVelocity));
    mix(&b.angularVelocity, sizeof(b.angularVelocity));
    return h;
}

} // namespace

// -----------------------------------------------------------------------------
// THE DEFINING PAIR: flight assist ON commands the installed thrusters to brake;
// flight assist OFF (decoupled) with the same bank and zero demand COASTS. Both
// bodies live in one registry and are stepped by the same StepFlightPhysics call,
// so the only behavioral difference is FlightControl.mode.
// -----------------------------------------------------------------------------
TEST_CASE(PhysicsSystem_CoupledUsesThrusters_DecoupledCoasts)
{
    const sim::FlightAssistParams params = TestParams();
    const core::Vec3d v0{ 8.0, -4.0, 5.0 };
    const core::Vec3f w0 = kZeroF;

    ecs::Registry reg;
    ecs::Entity coupled   = MakeShip(reg, ecs::FlightMode::Coupled,   v0, w0);
    reg.Assign<ecs::ThrusterSet>(coupled, SymmetricRig(10.0f));
    ecs::Entity decoupled = MakeShip(reg, ecs::FlightMode::Decoupled, v0, w0);
    // The decoupled ship carries a full thruster rig; with zero demand allocation
    // must idle every thruster, so it coasts regardless of having actuators.
    reg.Assign<ecs::ThrusterSet>(decoupled, SymmetricRig(10.0f));

    const int N = 180;
    double prevCoupledSpeed = v0.Length();
    for (int i = 0; i < N; ++i)
    {
        sim::StepFlightPhysics(reg, kDt, params);
        const double s = reg.Get<ecs::RigidBody>(coupled).linearVelocity.Length();
        CHECK(s < prevCoupledSpeed); // coupled: strict monotonic decay every step
        prevCoupledSpeed = s;
    }

    // Coupled: the physical bank brakes near rest without an ideal force path.
    const ecs::RigidBody& cb = reg.Get<ecs::RigidBody>(coupled);
    CHECK(cb.linearVelocity.Length() < 0.05); // damped near rest
    // This through-CoM bank has no angular authority; no rotation is introduced.
    CHECK_EQ(cb.angularVelocity.Length(), 0.0f);

    // Decoupled negative control: velocity is unchanged; it coasts, no decay.
    const ecs::RigidBody& db = reg.Get<ecs::RigidBody>(decoupled);
    CHECK_APPROX_EPS(db.linearVelocity.x, v0.x, 1e-12);
    CHECK_APPROX_EPS(db.linearVelocity.y, v0.y, 1e-12);
    CHECK_APPROX_EPS(db.linearVelocity.z, v0.z, 1e-12);
    // No initial spin and no torque means angular velocity remains exactly zero.
    CHECK_EQ(db.angularVelocity.Length(), 0.0f);
    // And every thruster was left idle by the zero-demand allocation.
    const ecs::ThrusterSet& ts = reg.Get<ecs::ThrusterSet>(decoupled);
    for (uint32_t i = 0; i < ts.count; ++i)
        CHECK_EQ(ts.thrusters[i].throttle, 0.0f);
}

// -----------------------------------------------------------------------------
// DECOUPLED thrust through the full system pipeline: pilot demand -> greedy
// allocation -> thruster wrench -> IntegrateRigidBody -> Transform. A full +X
// demand accelerates the body along WORLD +X at a = F·invMass with no rotation
// (symmetric rig), and the pose is written back into Transform.
// -----------------------------------------------------------------------------
TEST_CASE(PhysicsSystem_DecoupledDemand_AllocatesThrustAndMovesTransform)
{
    ecs::Registry reg;
    ecs::Entity ship = MakeShip(reg, ecs::FlightMode::Decoupled,
                                core::Vec3d{ 0.0, 0.0, 0.0 }, kZeroF);
    reg.Assign<ecs::ThrusterSet>(ship, SymmetricRig(10.0f));
    reg.Get<ecs::FlightControl>(ship).linearDemand = { 1.0f, 0.0f, 0.0f };

    const int N = 60;
    for (int i = 0; i < N; ++i)
        sim::StepFlightPhysics(reg, kDt); // default params (thrust path ignores them)

    // Net force = 10 N +X, invMass 0.5 -> a = 5 m/s². Velocity is exact for
    // constant force under semi-implicit Euler: v_N = a·N·dt.
    const ecs::RigidBody& b = reg.Get<ecs::RigidBody>(ship);
    CHECK_APPROX_EPS(b.linearVelocity.x, 5.0 * N * kDt, 1e-9);
    CHECK_APPROX_EPS(b.linearVelocity.y, 0.0, 1e-12);
    CHECK_APPROX_EPS(b.linearVelocity.z, 0.0, 1e-12);
    CHECK_APPROX(b.angularVelocity.Length(), 0.0f); // symmetric rig: no torque

    // Transform actually advanced along +X (the pose was written back).
    const ecs::Transform& t = reg.Get<ecs::Transform>(ship);
    CHECK(t.position.x > 0.0);
    CHECK_APPROX_EPS(t.position.y, 0.0, 1e-12);

    // The +X thruster is the one firing; its opposite stays idle.
    const ecs::ThrusterSet& ts = reg.Get<ecs::ThrusterSet>(ship);
    CHECK_APPROX(ts.thrusters[0].throttle, 1.0f); // +X
    CHECK_EQ(ts.thrusters[1].throttle, 0.0f);     // -X
}

// -----------------------------------------------------------------------------
// COUPLED CONTROL HAS NO REACTIONLESS DRIVE. Without a ThrusterSet, the desired
// assist wrench cannot be realized and the body coasts unchanged.
// -----------------------------------------------------------------------------
TEST_CASE(PhysicsSystem_CoupledWithoutThrusters_CannotApplyIdealWrench)
{
    ecs::Registry reg;
    const core::Vec3d v0{ 10.0, -4.0, 2.0 };
    const core::Vec3f w0{ 0.5f, -0.25f, 0.1f };
    const ecs::Entity ship = MakeShip(reg, ecs::FlightMode::Coupled, v0, w0);

    sim::StepFlightPhysics(reg, kDt, TestParams());

    const ecs::RigidBody& body = reg.Get<ecs::RigidBody>(ship);
    CHECK_EQ(body.linearVelocity.x, v0.x);
    CHECK_EQ(body.linearVelocity.y, v0.y);
    CHECK_EQ(body.linearVelocity.z, v0.z);
    CHECK_EQ(body.angularVelocity.x, w0.x);
    CHECK_EQ(body.angularVelocity.y, w0.y);
    CHECK_EQ(body.angularVelocity.z, w0.z);
}

// -----------------------------------------------------------------------------
// COUPLED BRAKING IS ACTUATOR-LIMITED and writes the same throttle state used by
// exhaust feedback. The ideal assist asks this 2 kg body for -60 N at 10 m/s,
// but the installed retro thruster can supply only -10 N.
// -----------------------------------------------------------------------------
TEST_CASE(PhysicsSystem_CoupledBraking_IsCappedAndUpdatesThrottleFeedback)
{
    ecs::Registry reg;
    const ecs::Entity ship = MakeShip(reg, ecs::FlightMode::Coupled,
                                      core::Vec3d{ 10.0, 0.0, 0.0 }, kZeroF);
    reg.Assign<ecs::ThrusterSet>(ship, SymmetricRig(10.0f));

    sim::StepFlightPhysics(reg, kDt, TestParams());

    const ecs::RigidBody& body = reg.Get<ecs::RigidBody>(ship);
    const double expected = 10.0 - 10.0 * body.invMass * kDt;
    CHECK_APPROX_EPS(body.linearVelocity.x, expected, 1e-12);

    const ecs::ThrusterSet& thrusters = reg.Get<ecs::ThrusterSet>(ship);
    CHECK_EQ(thrusters.thrusters[0].throttle, 0.0f); // +X would accelerate
    CHECK_EQ(thrusters.thrusters[1].throttle, 1.0f); // -X brakes at full authority
}

// A damaged/weakened bank must produce proportionally less braking authority;
// the controller cannot silently retain the ideal acceleration.
TEST_CASE(PhysicsSystem_CoupledWeakenedThruster_DegradesBraking)
{
    ecs::Registry reg;
    const ecs::Entity strong = MakeShip(reg, ecs::FlightMode::Coupled,
                                        core::Vec3d{ 10.0, 0.0, 0.0 }, kZeroF);
    const ecs::Entity weak = MakeShip(reg, ecs::FlightMode::Coupled,
                                      core::Vec3d{ 10.0, 0.0, 0.0 }, kZeroF);
    reg.Assign<ecs::ThrusterSet>(strong, SymmetricRig(10.0f));
    reg.Assign<ecs::ThrusterSet>(weak, SymmetricRig(2.0f));

    sim::StepFlightPhysics(reg, kDt, TestParams());

    const double strongSpeed = reg.Get<ecs::RigidBody>(strong).linearVelocity.x;
    const double weakSpeed = reg.Get<ecs::RigidBody>(weak).linearVelocity.x;
    CHECK(strongSpeed < weakSpeed);
    CHECK_APPROX_EPS(strongSpeed, 10.0 - 5.0 * kDt, 1e-12);
    CHECK_APPROX_EPS(weakSpeed, 10.0 - 1.0 * kDt, 1e-12);
}

// -----------------------------------------------------------------------------
// FIXED-TIMESTEP FAITHFULNESS — StepFlightPhysics advances each body by EXACTLY
// one semi-implicit-Euler step of the PASSED dt: one system call is bit-identical
// to desired-wrench computation, bounded allocation, realized wrench, and one
// IntegrateRigidBody call by hand. This pins the actuator pipeline and proves the
// step honours the accumulator's dt.
// -----------------------------------------------------------------------------
TEST_CASE(PhysicsSystem_StepAdvancesByExactlyOneFixedStep)
{
    const sim::FlightAssistParams params = TestParams();
    const core::Vec3d v0{ 7.0, 2.0, -3.0 };
    const core::Vec3f w0{ 0.2f, 0.5f, -0.1f };

    // Through the system.
    ecs::Registry reg;
    ecs::Entity e = MakeShip(reg, ecs::FlightMode::Coupled, v0, w0);
    reg.Assign<ecs::ThrusterSet>(e, SymmetricRig(10.0f));
    sim::StepFlightPhysics(reg, kDt, params);
    const ecs::RigidBody& sysB = reg.Get<ecs::RigidBody>(e);
    const ecs::Transform& sysT = reg.Get<ecs::Transform>(e);
    const ecs::ThrusterSet& sysThrusters = reg.Get<ecs::ThrusterSet>(e);

    // The same single step, by hand, with the same dt.
    ecs::RigidBody manB;
    manB.invMass         = 0.5;
    manB.invInertiaDiag  = { 0.5f, 0.5f, 0.5f };
    manB.linearVelocity  = v0;
    manB.angularVelocity = w0;
    core::Vec3d manPos{};
    core::Quatf manOrn = core::Quatf::Identity();
    ecs::ThrusterSet manThrusters = SymmetricRig(10.0f);
    const sim::AssistWrench desired =
        sim::ComputeFlightAssist(manB, manOrn, kZeroF, kZeroF, params);
    sim::AllocateThrustersForWrench(manThrusters, desired.worldForce,
                                    desired.bodyTorque, manOrn, kZeroF);
    const sim::Wrench realized = sim::ComputeWrench(manThrusters, kZeroF);
    sim::AccumulateBodyWrench(manB, manOrn, realized);
    sim::IntegrateRigidBody(manPos, manOrn, manB, core::Vec3d{ 0, 0, 0 }, kZeroF, kDt);

    CHECK_EQ(sysB.linearVelocity.x, manB.linearVelocity.x);
    CHECK_EQ(sysB.linearVelocity.y, manB.linearVelocity.y);
    CHECK_EQ(sysB.linearVelocity.z, manB.linearVelocity.z);
    CHECK_EQ(sysB.angularVelocity.x, manB.angularVelocity.x);
    CHECK_EQ(sysB.angularVelocity.z, manB.angularVelocity.z);
    CHECK_EQ(sysT.position.x, manPos.x);
    CHECK_EQ(sysT.position.z, manPos.z);
    for (uint32_t i = 0; i < sysThrusters.count; ++i)
        CHECK_EQ(sysThrusters.thrusters[i].throttle, manThrusters.thrusters[i].throttle);

    // A non-positive or non-finite dt is a whole-system no-op.
    ecs::Registry reg2;
    ecs::Entity e2 = MakeShip(reg2, ecs::FlightMode::Coupled, v0, w0);
    sim::StepFlightPhysics(reg2, 0.0, params);
    sim::StepFlightPhysics(reg2, -1.0, params);
    const ecs::RigidBody& b2 = reg2.Get<ecs::RigidBody>(e2);
    CHECK_EQ(b2.linearVelocity.x, v0.x);
    CHECK_EQ(b2.linearVelocity.z, v0.z);
}

// -----------------------------------------------------------------------------
// DETERMINISM — the step is a pure function of (state, dt): two identical runs
// produce bit-identical state (same FNV hash). NEGATIVE CONTROL: a single changed
// input (one perturbed initial velocity) yields a DIFFERENT hash, so "identical"
// is not vacuously true because nothing depends on the input. No wall-clock, no
// RNG in the step.
// -----------------------------------------------------------------------------
TEST_CASE(PhysicsSystem_DeterministicReplay)
{
    const sim::FlightAssistParams params = TestParams();
    const core::Vec3d v0{ 6.0, -2.0, 3.0 };
    const core::Vec3f w0{ 0.3f, 0.1f, -0.2f };

    auto run = [&](const core::Vec3d& startV) -> uint64_t
    {
        ecs::Registry reg;
        ecs::Entity e = MakeShip(reg, ecs::FlightMode::Coupled, startV, w0);
        reg.Assign<ecs::ThrusterSet>(e, SymmetricRig(10.0f));
        reg.Get<ecs::FlightControl>(e).linearDemand = { 0.5f, 0.0f, -0.25f };
        for (int i = 0; i < 200; ++i)
            sim::StepFlightPhysics(reg, kDt, params);
        return HashRigidState(reg, e);
    };

    const uint64_t a = run(v0);
    const uint64_t b = run(v0);                              // identical inputs
    const uint64_t c = run(v0 + core::Vec3d{ 1e-6, 0, 0 });  // one perturbed input

    CHECK_EQ(a, b);   // deterministic: same inputs -> bit-identical trajectory
    CHECK(a != c);    // and the hash actually depends on the input (not vacuous)
}

// -----------------------------------------------------------------------------
// A scripted ThrusterSet with NO FlightControl fires whatever throttles are set
// (supports non-piloted thruster entities); and a body with neither control nor
// thrusters simply integrates (here: coasts). Confirms the dispatch branches.
// -----------------------------------------------------------------------------
TEST_CASE(PhysicsSystem_ScriptedThrusters_AndPlainBodyCoasts)
{
    ecs::Registry reg;

    // Scripted: no FlightControl, a single +Z thruster held open at half throttle.
    ecs::Entity scripted = reg.Create();
    reg.Assign<ecs::Transform>(scripted, ecs::Transform{});
    ecs::RigidBody sb;
    sb.invMass = 0.5; sb.invInertiaDiag = { 1.0f, 1.0f, 1.0f };
    reg.Assign<ecs::RigidBody>(scripted, sb);
    ecs::ThrusterSet ts;
    ts.count = 1;
    ts.thrusters[0].localDirection = { 0.0f, 0.0f, 1.0f };
    ts.thrusters[0].maxForce       = 10.0f;
    ts.thrusters[0].throttle       = 0.5f;
    reg.Assign<ecs::ThrusterSet>(scripted, ts);

    // Plain: RigidBody only, moving; must coast unchanged.
    ecs::Entity plain = reg.Create();
    reg.Assign<ecs::Transform>(plain, ecs::Transform{});
    ecs::RigidBody pb;
    pb.invMass = 1.0; pb.invInertiaDiag = { 1.0f, 1.0f, 1.0f };
    pb.linearVelocity = { 2.0, 0.0, 0.0 };
    reg.Assign<ecs::RigidBody>(plain, pb);

    sim::StepFlightPhysics(reg, kDt);

    // Scripted thruster: force 0.5·10 = 5 N +Z, a = 5·0.5 = 2.5, v_z = 2.5·dt.
    CHECK_APPROX_EPS(reg.Get<ecs::RigidBody>(scripted).linearVelocity.z, 2.5 * kDt, 1e-9);
    // Plain body: unchanged velocity (coasts).
    CHECK_APPROX_EPS(reg.Get<ecs::RigidBody>(plain).linearVelocity.x, 2.0, 1e-12);
}
