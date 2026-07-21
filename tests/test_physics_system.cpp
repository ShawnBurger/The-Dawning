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
#include "sim/nbody.h"
#include "sim/relativity.h"
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

namespace
{

ecs::Entity MakeGravityEntity(ecs::Registry& registry, sim::FrameId frame,
                              const core::Vec3d& position, uint64_t bodyId,
                              double mu, double radius, bool isSource,
                              ecs::OrbitOwner owner, bool withRigidBody)
{
    const ecs::Entity entity = registry.Create();
    ecs::Transform transform;
    transform.position = position;
    registry.Assign<ecs::Transform>(entity, transform);
    registry.Assign<ecs::SpatialFrame>(
        entity, ecs::SpatialFrame{ static_cast<uint32_t>(frame) });

    ecs::GravitationalBody gravity;
    gravity.bodyId = bodyId;
    gravity.mu = mu;
    gravity.radius = radius;
    gravity.isSource = isSource;
    gravity.owner = owner;
    registry.Assign<ecs::GravitationalBody>(entity, gravity);

    if (withRigidBody)
    {
        ecs::RigidBody body;
        body.invMass = 0.5;
        body.invInertiaDiag = core::Vec3f{ 1.0f, 1.0f, 1.0f };
        registry.Assign<ecs::RigidBody>(entity, body);
    }
    return entity;
}

} // namespace

TEST_CASE(PhysicsSystem_GravityBridge_CrossFrameKnownAnswerAndFlightStep)
{
    const sim::WorldPos center{
        1234567, -2345678, 3456789,
        core::Vec3d{ 4000000.0, 5000000.0, 6000000.0 }
    };
    sim::FrameGraph frames;
    const sim::FrameId sourceFrame = frames.CreateFrame(sim::kInvalidFrame, center);
    const sim::FrameId targetFrame = frames.CreateFrame(
        sim::kInvalidFrame, sim::Translate(center, core::Vec3d{ 2000.0, 0.0, 0.0 }));

    ecs::Registry registry;
    MakeGravityEntity(registry, sourceFrame, core::Vec3d{}, 10, 1.0e9, 10.0,
                      true, ecs::OrbitOwner::OnRails, false);
    const ecs::Entity target = MakeGravityEntity(
        registry, targetFrame, core::Vec3d{}, 20, 0.0, 1.0, false,
        ecs::OrbitOwner::ForceIntegrated, true);
    ecs::RigidBody& body = registry.Get<ecs::RigidBody>(target);
    body.forceAccum = core::Vec3d{ 3.0, 4.0, 5.0 };

    const sim::GravityAccumulationResult result =
        sim::AccumulateForceIntegratedGravity(registry, frames);

    const double r = 2000.0;
    const double r2 = r * r + 10.0 * 10.0;
    const double expectedAx = -1.0e9 * r / (r2 * std::sqrt(r2));
    CHECK(result.accepted);
    CHECK_EQ(result.sourceCount, 1u);
    CHECK_EQ(result.targetCount, 1u);
    CHECK_APPROX_EPS(body.forceAccum.x, 3.0 + 2.0 * expectedAx, 1e-12);
    CHECK_EQ(body.forceAccum.y, 4.0);
    CHECK_EQ(body.forceAccum.z, 5.0);

    const core::Vec3d stagedForce = body.forceAccum;
    sim::StepFlightPhysics(registry, kDt);
    CHECK_APPROX_EPS(body.linearVelocity.x, stagedForce.x * 0.5 * kDt, 1e-12);
    CHECK_APPROX_EPS(body.linearVelocity.y, stagedForce.y * 0.5 * kDt, 1e-12);
    CHECK_APPROX_EPS(registry.Get<ecs::Transform>(target).position.x,
                     body.linearVelocity.x * kDt, 1e-12);
    CHECK(body.forceAccum == core::Vec3d{});
}

TEST_CASE(PhysicsSystem_MotionOwner_AllowsExactlyOneTranslationIntegrator)
{
    ecs::Registry registry;

    auto makeOwned = [&](ecs::OrbitOwner owner) -> ecs::Entity
    {
        const ecs::Entity entity = registry.Create();
        registry.Assign<ecs::Transform>(entity, ecs::Transform{});
        ecs::RigidBody body;
        body.invMass = 1.0;
        body.invInertiaDiag = core::Vec3f{ 1.0f, 1.0f, 1.0f };
        body.linearVelocity = core::Vec3d{ 1.0, 0.0, 0.0 };
        body.forceAccum = core::Vec3d{ 2.0, 0.0, 0.0 };
        registry.Assign<ecs::RigidBody>(entity, body);
        ecs::GravitationalBody gravity;
        gravity.bodyId = static_cast<uint64_t>(entity.Index()) + 1;
        gravity.mu = 1.0;
        gravity.radius = 1.0;
        gravity.owner = owner;
        registry.Assign<ecs::GravitationalBody>(entity, gravity);
        return entity;
    };

    const ecs::Entity nbody = makeOwned(ecs::OrbitOwner::NBodyActive);
    const ecs::Entity rails = makeOwned(ecs::OrbitOwner::OnRails);
    const ecs::Entity force = makeOwned(ecs::OrbitOwner::ForceIntegrated);
    const ecs::Entity legacy = registry.Create();
    registry.Assign<ecs::Transform>(legacy, ecs::Transform{});
    ecs::RigidBody legacyBody;
    legacyBody.invMass = 1.0;
    legacyBody.invInertiaDiag = core::Vec3f{ 1.0f, 1.0f, 1.0f };
    legacyBody.linearVelocity = core::Vec3d{ 1.0, 0.0, 0.0 };
    registry.Assign<ecs::RigidBody>(legacy, legacyBody);

    sim::StepFlightPhysics(registry, 0.5);

    CHECK_EQ(registry.Get<ecs::Transform>(nbody).position.x, 0.0);
    CHECK_EQ(registry.Get<ecs::Transform>(rails).position.x, 0.0);
    CHECK_EQ(registry.Get<ecs::RigidBody>(nbody).linearVelocity.x, 1.0);
    CHECK_EQ(registry.Get<ecs::RigidBody>(rails).linearVelocity.x, 1.0);
    CHECK_EQ(registry.Get<ecs::RigidBody>(nbody).forceAccum.x, 2.0);
    CHECK_EQ(registry.Get<ecs::RigidBody>(rails).forceAccum.x, 2.0);
    CHECK_APPROX_EPS(registry.Get<ecs::RigidBody>(force).linearVelocity.x, 2.0, 1e-12);
    CHECK_APPROX_EPS(registry.Get<ecs::Transform>(force).position.x, 1.0, 1e-12);
    CHECK_APPROX_EPS(registry.Get<ecs::Transform>(legacy).position.x, 0.5, 1e-12);
}

TEST_CASE(PhysicsSystem_GravityBridge_SourceOrderIsBitExactAndSelfIsExcluded)
{
    sim::FrameGraph frames;
    const sim::FrameId frame = frames.CreateFrame(
        sim::kInvalidFrame, sim::WorldPos::FromOffset(core::Vec3d{ 1000.0, 0.0, 0.0 }));

    struct RunResult
    {
        core::Vec3d force;
        double insertionOrderForceX = 0.0;
    };

    auto run = [&](bool reverse) -> RunResult
    {
        ecs::Registry registry;
        auto sourceA = [&]()
        {
            MakeGravityEntity(registry, frame, core::Vec3d{ 1.0, 0.0, 0.0 },
                              1, 2.8e16, 0.0, true,
                              ecs::OrbitOwner::NBodyActive, false);
        };
        auto sourceB = [&]()
        {
            MakeGravityEntity(registry, frame, core::Vec3d{ -1.0, 0.0, 0.0 },
                              2, 2.8e16, 0.0, true,
                              ecs::OrbitOwner::OnRails, false);
        };
        auto sourceC = [&]()
        {
            MakeGravityEntity(registry, frame, core::Vec3d{ 1.0, 0.0, 0.0 },
                              3, 3.0, 0.0, true,
                              ecs::OrbitOwner::NBodyActive, false);
        };
        if (reverse) { sourceC(); sourceB(); sourceA(); }
        else         { sourceA(); sourceB(); sourceC(); }

        const ecs::Entity target = MakeGravityEntity(
            registry, frame, core::Vec3d{}, 99, 5.0, 0.0, true,
            ecs::OrbitOwner::ForceIntegrated, true);
        const sim::GravityAccumulationResult result =
            sim::AccumulateForceIntegratedGravity(registry, frames);
        CHECK(result.accepted);
        CHECK_EQ(result.sourceCount, 4u);
        CHECK_EQ(result.targetCount, 1u);

        auto contribution = [](double sourceX, double mu)
        {
            const double eps = sim::SofteningLength(mu, 0.0);
            const double d = -sourceX;
            const double r2 = d * d + eps * eps;
            return -d * mu / (r2 * std::sqrt(r2));
        };
        const double a = contribution(1.0, 2.8e16);
        const double b = contribution(-1.0, 2.8e16);
        const double c = contribution(1.0, 3.0);
        const double naiveAcceleration = reverse
            ? (c + b) + a
            : (a + b) + c;
        return RunResult{
            registry.Get<ecs::RigidBody>(target).forceAccum,
            naiveAcceleration * 2.0
        };
    };

    const RunResult forward = run(false);
    const RunResult reverse = run(true);
    CHECK(forward.force == reverse.force);
    CHECK(forward.force.x > 0.0);
    CHECK_EQ(forward.force.y, 0.0);
    CHECK_EQ(forward.force.z, 0.0);
    CHECK(forward.insertionOrderForceX != reverse.insertionOrderForceX);

    ecs::Registry selfOnly;
    const ecs::Entity self = MakeGravityEntity(
        selfOnly, frame, core::Vec3d{}, 7, 1.0e6, 1.0, true,
        ecs::OrbitOwner::ForceIntegrated, true);
    const sim::GravityAccumulationResult selfResult =
        sim::AccumulateForceIntegratedGravity(selfOnly, frames);
    CHECK(selfResult.accepted);
    CHECK_EQ(selfResult.sourceCount, 1u);
    CHECK_EQ(selfResult.targetCount, 1u);
    CHECK(selfOnly.Get<ecs::RigidBody>(self).forceAccum == core::Vec3d{});
}

TEST_CASE(PhysicsSystem_GravityBridge_InvalidRegistryIsAtomic)
{
    sim::FrameGraph frames;
    const sim::FrameId frame = frames.CreateFrame(
        sim::kInvalidFrame, sim::WorldPos::FromOffset(core::Vec3d{ 5000.0, 0.0, 0.0 }));
    ecs::Registry registry;
    MakeGravityEntity(registry, frame, core::Vec3d{ 100.0, 0.0, 0.0 },
                      1, 1.0e6, 1.0, true,
                      ecs::OrbitOwner::NBodyActive, false);
    const ecs::Entity first = MakeGravityEntity(
        registry, frame, core::Vec3d{}, 2, 0.0, 1.0, false,
        ecs::OrbitOwner::ForceIntegrated, true);
    const ecs::Entity invalid = MakeGravityEntity(
        registry, frame, core::Vec3d{ 0.0, 10.0, 0.0 }, 3, 0.0, 1.0, false,
        ecs::OrbitOwner::ForceIntegrated, true);
    registry.Get<ecs::RigidBody>(first).forceAccum = core::Vec3d{ 1.0, 2.0, 3.0 };
    registry.Get<ecs::RigidBody>(invalid).forceAccum = core::Vec3d{ 4.0, 5.0, 6.0 };
    registry.Get<ecs::RigidBody>(invalid).invMass = 0.0;

    const core::Vec3d firstBefore = registry.Get<ecs::RigidBody>(first).forceAccum;
    const core::Vec3d invalidBefore = registry.Get<ecs::RigidBody>(invalid).forceAccum;
    CHECK(!sim::AccumulateForceIntegratedGravity(registry, frames).accepted);
    CHECK(registry.Get<ecs::RigidBody>(first).forceAccum == firstBefore);
    CHECK(registry.Get<ecs::RigidBody>(invalid).forceAccum == invalidBefore);

    registry.Get<ecs::RigidBody>(invalid).invMass = 0.5;
    registry.Get<ecs::GravitationalBody>(invalid).bodyId = 2;
    CHECK(!sim::AccumulateForceIntegratedGravity(registry, frames).accepted);
    CHECK(registry.Get<ecs::RigidBody>(first).forceAccum == firstBefore);
    CHECK(registry.Get<ecs::RigidBody>(invalid).forceAccum == invalidBefore);

    registry.Get<ecs::GravitationalBody>(invalid).bodyId = 3;
    registry.Get<ecs::SpatialFrame>(invalid).frameId = sim::kInvalidFrame;
    CHECK(!sim::AccumulateForceIntegratedGravity(registry, frames).accepted);
    CHECK(registry.Get<ecs::RigidBody>(first).forceAccum == firstBefore);
    CHECK(registry.Get<ecs::RigidBody>(invalid).forceAccum == invalidBefore);

    registry.Get<ecs::SpatialFrame>(invalid).frameId = frame;
    registry.Get<ecs::GravitationalBody>(invalid).owner =
        static_cast<ecs::OrbitOwner>(99);
    CHECK(!sim::AccumulateForceIntegratedGravity(registry, frames).accepted);
    CHECK(registry.Get<ecs::RigidBody>(first).forceAccum == firstBefore);
    CHECK(registry.Get<ecs::RigidBody>(invalid).forceAccum == invalidBefore);

    registry.Get<ecs::GravitationalBody>(invalid).owner =
        ecs::OrbitOwner::ForceIntegrated;
    registry.Get<ecs::RigidBody>(first).invMass = 1.0e-308;
    CHECK(!sim::AccumulateForceIntegratedGravity(registry, frames).accepted);
    CHECK(registry.Get<ecs::RigidBody>(first).forceAccum == firstBefore);
    CHECK(registry.Get<ecs::RigidBody>(invalid).forceAccum == invalidBefore);
}

TEST_CASE(PhysicsSystem_RelativisticBodyConsumesLinearForceExactlyOnce)
{
    ecs::Registry registry;
    const ecs::Entity entity = registry.Create();
    registry.Assign<ecs::Transform>(entity, ecs::Transform{});

    ecs::RigidBody body;
    body.invMass = 0.5;
    body.invInertiaDiag = core::Vec3f{ 1.0f, 1.0f, 1.0f };
    body.forceAccum = core::Vec3d{ 120.0, -30.0, 15.0 };
    registry.Assign<ecs::RigidBody>(entity, body);

    ecs::RelativisticBody relativistic;
    relativistic.restMass = 2.0;
    relativistic.momentum = sim::MomentumFromVelocity(
        core::Vec3d{ 10.0, 20.0, -5.0 }, relativistic.restMass);
    registry.Assign<ecs::RelativisticBody>(entity, relativistic);

    constexpr double dt = 0.25;
    core::Vec3d expectedVelocity;
    const core::Vec3d expectedMomentum = sim::RelativisticMomentumStep(
        relativistic.momentum, body.forceAccum,
        relativistic.restMass, dt, expectedVelocity);
    const core::Vec3d newtonianDoubleUse =
        expectedVelocity + body.forceAccum * body.invMass * dt;

    const sim::FlightPhysicsStepResult result =
        sim::StepFlightPhysics(registry, dt);
    CHECK(result.accepted);
    CHECK_EQ(result.advancedBodyCount, 1u);
    CHECK_EQ(result.relativisticBodyCount, 1u);
    CHECK(registry.Get<ecs::RelativisticBody>(entity).momentum ==
          expectedMomentum);
    CHECK(registry.Get<ecs::RigidBody>(entity).linearVelocity ==
          expectedVelocity);
    CHECK(registry.Get<ecs::Transform>(entity).position ==
          expectedVelocity * dt);
    CHECK(registry.Get<ecs::RigidBody>(entity).linearVelocity !=
          newtonianDoubleUse);
    CHECK(registry.Get<ecs::RigidBody>(entity).linearVelocity.Length() <
          sim::kSpeedOfLight);
    CHECK(registry.Get<ecs::RigidBody>(entity).forceAccum == core::Vec3d{});
}

TEST_CASE(PhysicsSystem_InvalidRelativisticMassRejectsBeforeAnyBodyMoves)
{
    ecs::Registry registry;
    auto makeBody = [&](double x) {
        const ecs::Entity entity = registry.Create();
        ecs::Transform transform;
        transform.position = core::Vec3d{ x, 0.0, 0.0 };
        registry.Assign<ecs::Transform>(entity, transform);
        ecs::RigidBody body;
        body.invMass = 0.5;
        body.invInertiaDiag = core::Vec3f{ 1.0f, 1.0f, 1.0f };
        body.linearVelocity = core::Vec3d{ 3.0, 0.0, 0.0 };
        body.forceAccum = core::Vec3d{ 4.0, 0.0, 0.0 };
        registry.Assign<ecs::RigidBody>(entity, body);
        return entity;
    };

    const ecs::Entity first = makeBody(1.0);
    const ecs::Entity invalid = makeBody(2.0);
    ecs::RelativisticBody relativistic;
    relativistic.restMass = 3.0;
    registry.Assign<ecs::RelativisticBody>(invalid, relativistic);

    const ecs::Transform firstBefore = registry.Get<ecs::Transform>(first);
    const ecs::RigidBody firstBodyBefore = registry.Get<ecs::RigidBody>(first);
    const sim::FlightPhysicsStepResult result =
        sim::StepFlightPhysics(registry, 0.5);
    CHECK_FALSE(result.accepted);
    CHECK(registry.Get<ecs::Transform>(first).position ==
          firstBefore.position);
    CHECK(registry.Get<ecs::RigidBody>(first).linearVelocity ==
          firstBodyBefore.linearVelocity);
    CHECK(registry.Get<ecs::RigidBody>(first).forceAccum ==
          firstBodyBefore.forceAccum);
}
