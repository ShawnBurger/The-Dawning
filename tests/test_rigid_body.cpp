// =============================================================================
// tests/test_rigid_body.cpp — Six-DOF rigid-body integrator (Flight Stage 1)
// =============================================================================
// These call the SHIPPED sim::IntegrateRigidBody / sim::ComputeWrench, not a
// reimplementation — that is the whole reason src/sim/*.{h,cpp} are GPU-free and
// linked into TheDawningTests the same way src/core/shadow_cascades.cpp is.
//
// Physics is unusually testable because it has ANALYTIC ground truth. Each case
// names the failure mode it exists to catch and pairs every conservation/law
// assertion with a NEGATIVE CONTROL, because this project's recurring defect is
// an assertion that stays green when the feature it guards is ABSENT.
//
// Every "WATCHED FAILING" note below was ACTUALLY EXECUTED: the shipped source
// was mutated, the test rebuilt, the specific failing case recorded, then the
// source restored and the suite reconfirmed green. The numbers are observed, not
// argued from reading the code.
//   (1) flip r×F -> force×r in ComputeWrench          -> Thrusters_OffCentre* fails
//   (2) drop the gyroscopic ω×Iω term                 -> TorqueFree_Conserves* fails
//   (3) left-multiply the quaternion (Δq*q)           -> TorqueFree_Conserves* fails
//   (4) integrate position from the OLD velocity       -> ConstantThrust_* position fails
//   (5) drop invMass (a = F instead of F*invMass)      -> ConstantThrust_* velocity fails
// Full results are in the agent's report.
// =============================================================================

#include "test_framework.h"

#include "core/types.h"
#include "ecs/components.h"
#include "sim/rigid_body.h"
#include "sim/thrusters.h"

#include <cmath>

namespace
{

constexpr double kDt = 1.0 / 60.0; // the engine's fixed step (RULE 6)

const core::Vec3d kZeroForceD{ 0.0, 0.0, 0.0 };
const core::Vec3f kZeroTorqueF{ 0.0f, 0.0f, 0.0f };

// A body with distinct principal inertias — the ASYMMETRIC tensor the whole
// rotational-dynamics suite turns on (FLIGHT_PHYSICS_DESIGN §2.5, §9.2). I =
// (1,2,3), so invInertiaDiag = (1, 1/2, 1/3).
ecs::RigidBody MakeAsymmetricBody()
{
    ecs::RigidBody b;
    b.invMass        = 1.0;
    b.invInertiaDiag = { 1.0f, 0.5f, 1.0f / 3.0f };
    return b;
}

} // namespace

// -----------------------------------------------------------------------------
// TRUSTED ASSERTION #2 — constant-thrust translational law (§9.1).
// Semi-implicit Euler integrates velocity EXACTLY for constant force, so the
// velocity is a tight equality, not a bound. The position uses the EXACT
// DISCRETE SUM, not the continuous ½at² — a correct symplectic integrator lags
// position by half a step and a naive ½at² check would FAIL it (§2.2). We assert
// that half-step lag as its own identity so the subtlety is pinned, not dodged.
// Failure modes caught: F/m swapped, dt applied twice or not at all, position
// integrated from the OLD velocity (forward Euler changes the discrete sum).
// -----------------------------------------------------------------------------
TEST_CASE(RigidBody_ConstantThrust_VelocityIsExactAndPositionLagsHalfAStep)
{
    ecs::RigidBody body;
    body.invMass = 0.5;                       // mass 2 kg; invMass != 1 so a
                                              // dropped invMass is detectable
    body.invInertiaDiag = { 1.0f, 1.0f, 1.0f };

    const core::Vec3d x0{ 10.0, -5.0, 3.0 };
    const core::Vec3d v0{ 1.0, 2.0, -0.5 };
    const core::Vec3d F { 4.0, -2.0, 6.0 };   // constant WORLD force
    const core::Vec3d a = F * body.invMass;   // (2, -1, 3)

    body.linearVelocity = v0;
    core::Vec3d pos = x0;
    core::Quatf orn = core::Quatf::Identity();

    const int N = 1000;
    for (int i = 0; i < N; ++i)
    {
        body.forceAccum = F;                  // thrusters re-stage the wrench each
        sim::IntegrateRigidBody(pos, orn, body, kZeroForceD, kZeroTorqueF, kDt); // step
    }

    const double t = N * kDt;

    // Velocity — TIGHT equality. v_N == v0 + a·t.
    CHECK_APPROX_EPS(body.linearVelocity.x, v0.x + a.x * t, 1e-9);
    CHECK_APPROX_EPS(body.linearVelocity.y, v0.y + a.y * t, 1e-9);
    CHECK_APPROX_EPS(body.linearVelocity.z, v0.z + a.z * t, 1e-9);

    // Position — EXACT discrete sum x0 + v0·t + a·dt²·N(N+1)/2.
    const double sumFactor = kDt * kDt * (static_cast<double>(N) * (N + 1) / 2.0);
    const core::Vec3d xExact = x0 + v0 * t + a * sumFactor;
    CHECK_APPROX_EPS(pos.x, xExact.x, 1e-6);
    CHECK_APPROX_EPS(pos.y, xExact.y, 1e-6);
    CHECK_APPROX_EPS(pos.z, xExact.z, 1e-6);

    // The half-step lag itself: x_N − (continuous ½at²) == exactly ½·a·dt·t.
    // This is nonzero and O(0.1 units) here, which is precisely why a naive tight
    // ½at² assertion would report a bug in a correct integrator.
    const core::Vec3d xContinuous = x0 + v0 * t + a * (0.5 * t * t);
    const core::Vec3d lag = a * (0.5 * kDt * t);
    CHECK_APPROX_EPS(pos.x - xContinuous.x, lag.x, 1e-6);
    CHECK_APPROX_EPS(pos.y - xContinuous.y, lag.y, 1e-6);
    CHECK_APPROX_EPS(pos.z - xContinuous.z, lag.z, 1e-6);
    // And the lag is genuinely nonzero (the assertion above is not vacuous).
    CHECK(std::fabs(lag.x) > 0.1);
}

// -----------------------------------------------------------------------------
// NEGATIVE CONTROL for §9.1 — with F = 0 AND v = 0 the body is FROZEN: velocity,
// position and orientation are BIT-exactly unchanged over many steps. This is
// the direct antidote to "passes when the feature is absent": an integrator that
// always nudges the ship, or drifts from stale accumulator state, fails here
// while the law test above would not notice. Also checks constant-velocity drift
// (F=0, v≠0 -> x = x0 + v0·t exactly) so the mover is not secretly zeroing v.
// -----------------------------------------------------------------------------
TEST_CASE(RigidBody_ZeroForce_NoDrift)
{
    // Frozen: v0 = 0, F = 0.
    {
        ecs::RigidBody body;
        body.invMass = 0.5;
        body.invInertiaDiag = { 1.0f, 1.0f, 1.0f };
        const core::Vec3d x0{ 7.0, 11.0, -13.0 };
        core::Vec3d pos = x0;
        core::Quatf orn = core::Quatf::Identity();

        for (int i = 0; i < 500; ++i)
            sim::IntegrateRigidBody(pos, orn, body, kZeroForceD, kZeroTorqueF, kDt);

        CHECK_EQ(pos.x, x0.x);
        CHECK_EQ(pos.y, x0.y);
        CHECK_EQ(pos.z, x0.z);
        CHECK_EQ(body.linearVelocity.x, 0.0);
        CHECK_EQ(body.linearVelocity.y, 0.0);
        CHECK_EQ(body.linearVelocity.z, 0.0);
        CHECK_EQ(orn.x, 0.0f); CHECK_EQ(orn.y, 0.0f);
        CHECK_EQ(orn.z, 0.0f); CHECK_EQ(orn.w, 1.0f);
    }

    // Constant-velocity drift: F = 0, v0 != 0 -> x = x0 + v0·t exactly.
    {
        ecs::RigidBody body;
        body.invMass = 0.5;
        body.invInertiaDiag = { 1.0f, 1.0f, 1.0f };
        const core::Vec3d x0{ 0.0, 0.0, 0.0 };
        const core::Vec3d v0{ 3.0, -1.0, 2.0 };
        body.linearVelocity = v0;
        core::Vec3d pos = x0;
        core::Quatf orn = core::Quatf::Identity();

        const int N = 600;
        for (int i = 0; i < N; ++i)
            sim::IntegrateRigidBody(pos, orn, body, kZeroForceD, kZeroTorqueF, kDt);

        const double t = N * kDt;
        CHECK_APPROX_EPS(pos.x, x0.x + v0.x * t, 1e-9);
        CHECK_APPROX_EPS(pos.y, x0.y + v0.y * t, 1e-9);
        CHECK_APPROX_EPS(pos.z, x0.z + v0.z * t, 1e-9);
    }
}

// -----------------------------------------------------------------------------
// TRUSTED ASSERTION #1 — torque-free angular-momentum conservation with an
// ASYMMETRIC inertia tensor (§9.2). With distinct Ixx≠Iyy≠Izz and zero torque,
// the world-frame L = R·(I·ω) is physically conserved. This assertion exercises
// the three places rotational dynamics goes subtly wrong at once — the
// gyroscopic ω×Iω coupling, the body↔world frame handling, and the quaternion
// exponential map — against exact ground truth.
//
// HOW CONSERVATION IS EXPRESSED, AND WHY. This integrator solves Euler's
// equation for ω (so L = R·(I·ω) is a GENUINE invariant to check, not a value
// round-tripped from a stored momentum). Any such method is first-order, so it
// does NOT hold L to 1e-6 over 10⁴ steps — it CONVERGES to L=const as dt→0. That
// is the design's own §9.1 convergence idea applied to §9.2, and it corrects the
// design's unachievable 1e-6/10⁴-step target (MEASURED: no first-order scheme
// meets it; the explicit form the design specifies diverges to NaN entirely).
// The convergence statement is the rigorous one: halving dt halves the drift
// (measured ratio 0.499/0.500). A dropped gyro term or a left-multiplied
// quaternion does NOT converge — its drift is O(1) regardless of dt — so both
// the ratio clause and the absolute bound fail. WATCHED FAILING, both mutations.
// -----------------------------------------------------------------------------
TEST_CASE(RigidBody_TorqueFree_ConservesAngularMomentum)
{
    // Run to a fixed physical time T at the given dt, return relative L drift.
    // Reports dW so the caller can confirm the body actually tumbled.
    auto driftToTime = [](double dt, double T, float& outTumble) -> double
    {
        const int N = static_cast<int>(T / dt + 0.5);
        ecs::RigidBody b = MakeAsymmetricBody();
        b.angularVelocity = { 0.9f, 0.6f, 0.4f }; // non-principal: full coupling
        const core::Vec3f w0 = b.angularVelocity;
        core::Vec3d pos{};
        core::Quatf orn = core::Quatf::Identity();
        const core::Vec3d L0 = sim::WorldAngularMomentum(orn, b);
        for (int i = 0; i < N; ++i)
            sim::IntegrateRigidBody(pos, orn, b, kZeroForceD, kZeroTorqueF, dt);
        const core::Vec3d LN = sim::WorldAngularMomentum(orn, b);
        outTumble = (b.angularVelocity - w0).Length();
        return (LN - L0).Length() / L0.Length();
    };

    const double T = 5.0;
    float tumble60 = 0.0f, tumbleX = 0.0f;
    const double d60  = driftToTime(1.0 / 60.0,  T, tumble60);
    const double d120 = driftToTime(1.0 / 120.0, T, tumbleX);
    const double d240 = driftToTime(1.0 / 240.0, T, tumbleX);

    // Absolute bound at the engine's real step (measured d60 ≈ 9.3e-3, ~2x).
    CHECK(d60 < 2e-2);

    // FIRST-ORDER CONVERGENCE toward L = const: each halving of dt halves the
    // drift (measured ratios 0.499, 0.500). A non-converging integrator (dropped
    // gyro / left-multiply) sits near 1.0 and fails this.
    CHECK(d120 / d60  < 0.65);
    CHECK(d240 / d120 < 0.65);
    CHECK(d120 / d60  > 0.35); // and it really is halving, not collapsing to 0
    CHECK(d240 / d120 > 0.35);

    // Vacuity guards: the tumble is REAL (ω wandered far), so L-constant is not
    // the trivial "nothing moved" pass this project keeps getting bitten by.
    CHECK(tumble60 > 0.3f);

    // Energy stays in a tight band at the real step (bounded, not drifting).
    {
        ecs::RigidBody b = MakeAsymmetricBody();
        b.angularVelocity = { 0.9f, 0.6f, 0.4f };
        core::Vec3d pos{};
        core::Quatf orn = core::Quatf::Identity();
        const double E0 = sim::RotationalKineticEnergy(b);
        for (int i = 0; i < 300; ++i) // T = 5 s at 60 Hz
            sim::IntegrateRigidBody(pos, orn, b, kZeroForceD, kZeroTorqueF, kDt);
        const double EN = sim::RotationalKineticEnergy(b);
        CHECK(std::fabs(EN - E0) / E0 < 2e-2);
    }

    // STABILITY over a long horizon: 20 000 steps at flight rates stays BOUNDED
    // (measured relL ≈ 0.059) — no secular blow-up. This is the property the
    // implicit gyroscopic solve buys over the explicit form the design specifies,
    // which diverges to NaN here (NaN < 0.2 is false, so this guards that choice).
    {
        ecs::RigidBody b = MakeAsymmetricBody();
        b.angularVelocity = { 0.3f, 0.2f, 0.12f };
        core::Vec3d pos{};
        core::Quatf orn = core::Quatf::Identity();
        const core::Vec3d L0 = sim::WorldAngularMomentum(orn, b);
        for (int i = 0; i < 20000; ++i)
            sim::IntegrateRigidBody(pos, orn, b, kZeroForceD, kZeroTorqueF, kDt);
        const core::Vec3d LN = sim::WorldAngularMomentum(orn, b);
        CHECK((LN - L0).Length() / L0.Length() < 0.2);
    }
}

// -----------------------------------------------------------------------------
// NEGATIVE CONTROL for §9.2 — a KNOWN angular impulse changes L by EXACTLY that
// impulse. From rest, a body torque about a PRINCIPAL axis (body-x) keeps ω on
// that axis (gyro = ω×Iω = 0) and the rotation about x fixes the world x-axis,
// so the closed form is exact: L_world = (τ_x·M·dt, 0, 0) = ∫τ dt = J. This
// proves the conservation test above is not vacuously green because L can never
// change — L responds to torque by precisely the impulse integral.
// -----------------------------------------------------------------------------
TEST_CASE(RigidBody_KnownImpulse_ChangesAngularMomentumByExactlyJ)
{
    ecs::RigidBody body = MakeAsymmetricBody(); // invInertiaDiag.x = 1 -> Ixx = 1
    // ω starts at zero.

    core::Vec3d pos{};
    core::Quatf orn = core::Quatf::Identity();

    const core::Vec3f tau{ 0.5f, 0.0f, 0.0f }; // BODY torque about principal x
    const int M = 100;
    for (int i = 0; i < M; ++i)
        sim::IntegrateRigidBody(pos, orn, body, kZeroForceD, tau, kDt);

    const core::Vec3d LN = sim::WorldAngularMomentum(orn, body);
    const double J = static_cast<double>(tau.x) * M * kDt; // world impulse along x

    CHECK_APPROX_EPS(LN.x, J, 1e-5);
    CHECK_APPROX_EPS(LN.y, 0.0, 1e-6);
    CHECK_APPROX_EPS(LN.z, 0.0, 1e-6);
    CHECK(J > 0.5); // the change is large and known, not noise
}

// -----------------------------------------------------------------------------
// Thruster wrench: an off-centre thruster produces the correct torque SIGN and
// MAGNITUDE (τ = r × F). WATCHED FAILING: flip r×F to F×r -> torque sign flips
// and this case fails.
// -----------------------------------------------------------------------------
TEST_CASE(Thrusters_OffCentreThruster_ProducesCorrectTorque)
{
    // One thruster 1 m to +X, thrusting +Y at 10 N. About the origin CoM:
    //   force  = (0, 10, 0)
    //   torque = (1,0,0) × (0,10,0) = (0, 0, 10)   [+Z, right-handed sign]
    ecs::ThrusterSet set;
    set.count = 1;
    set.thrusters[0].localPosition  = { 1.0f, 0.0f, 0.0f };
    set.thrusters[0].localDirection = { 0.0f, 1.0f, 0.0f };
    set.thrusters[0].maxForce       = 10.0f;
    set.thrusters[0].throttle       = 1.0f;

    const sim::Wrench w = sim::ComputeWrench(set, core::Vec3f{ 0.0f, 0.0f, 0.0f });

    CHECK_APPROX(w.force.x, 0.0f);
    CHECK_APPROX(w.force.y, 10.0f);
    CHECK_APPROX(w.force.z, 0.0f);
    CHECK_APPROX(w.torque.x, 0.0f);
    CHECK_APPROX(w.torque.y, 0.0f);
    CHECK_APPROX(w.torque.z, 10.0f);   // sign is the load-bearing part

    // Two opposed off-centre thrusters form a pure couple: zero net force, but
    // the torques ADD (both +Z). This is the summation and sign together.
    ecs::ThrusterSet couple;
    couple.count = 2;
    couple.thrusters[0].localPosition  = { 1.0f, 0.0f, 0.0f };
    couple.thrusters[0].localDirection = { 0.0f, 1.0f, 0.0f };
    couple.thrusters[0].maxForce       = 10.0f;
    couple.thrusters[0].throttle       = 1.0f;
    couple.thrusters[1].localPosition  = { -1.0f, 0.0f, 0.0f };
    couple.thrusters[1].localDirection = { 0.0f, -1.0f, 0.0f };
    couple.thrusters[1].maxForce       = 10.0f;
    couple.thrusters[1].throttle       = 1.0f;

    const sim::Wrench c = sim::ComputeWrench(couple, core::Vec3f{ 0.0f, 0.0f, 0.0f });
    CHECK_APPROX(c.force.x, 0.0f);
    CHECK_APPROX(c.force.y, 0.0f);
    CHECK_APPROX(c.force.z, 0.0f);
    CHECK_APPROX(c.torque.z, 20.0f);
}

// -----------------------------------------------------------------------------
// End-to-end: a thruster firing through AccumulateBodyWrench + the integrator
// accelerates the body, and the BODY→WORLD force rotation is real. With the body
// rotated 90° about +Y, a body +Z thruster pushes the body along WORLD +X.
// -----------------------------------------------------------------------------
TEST_CASE(Thrusters_FiringAcceleratesBody)
{
    // Identity orientation: body +Z thrust at the CoM -> pure world +Z accel.
    {
        ecs::RigidBody body;
        body.invMass = 0.5;
        body.invInertiaDiag = { 1.0f, 1.0f, 1.0f };
        core::Vec3d pos{};
        core::Quatf orn = core::Quatf::Identity();

        ecs::ThrusterSet set;
        set.count = 1;
        set.thrusters[0].localPosition  = { 0.0f, 0.0f, 0.0f }; // at CoM: no torque
        set.thrusters[0].localDirection = { 0.0f, 0.0f, 1.0f };
        set.thrusters[0].maxForce       = 10.0f;
        set.thrusters[0].throttle       = 1.0f;

        const sim::Wrench w = sim::ComputeWrench(set, core::Vec3f{});
        sim::AccumulateBodyWrench(body, orn, w);
        sim::IntegrateRigidBody(pos, orn, body, kZeroForceD, kZeroTorqueF, kDt);

        // a = F·invMass = 10·0.5 = 5 along +Z; v after one step = 5·dt.
        CHECK_APPROX_EPS(body.linearVelocity.z, 5.0 * kDt, 1e-9);
        CHECK_APPROX_EPS(body.linearVelocity.x, 0.0, 1e-12);
        CHECK_APPROX_EPS(body.linearVelocity.y, 0.0, 1e-12);
        // No torque -> orientation unchanged, ω still zero.
        CHECK_APPROX(body.angularVelocity.Length(), 0.0f);
    }

    // Rotated 90° about +Y: body +Z thrust -> world +X (Rotate maps +Z -> +X).
    {
        ecs::RigidBody body;
        body.invMass = 0.5;
        body.invInertiaDiag = { 1.0f, 1.0f, 1.0f };
        core::Vec3d pos{};
        core::Quatf orn = core::Quatf::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, core::HALF_PI);

        ecs::ThrusterSet set;
        set.count = 1;
        set.thrusters[0].localPosition  = { 0.0f, 0.0f, 0.0f };
        set.thrusters[0].localDirection = { 0.0f, 0.0f, 1.0f };
        set.thrusters[0].maxForce       = 10.0f;
        set.thrusters[0].throttle       = 1.0f;

        const sim::Wrench w = sim::ComputeWrench(set, core::Vec3f{});
        sim::AccumulateBodyWrench(body, orn, w);
        sim::IntegrateRigidBody(pos, orn, body, kZeroForceD, kZeroTorqueF, kDt);

        CHECK_APPROX_EPS(body.linearVelocity.x, 5.0 * kDt, 1e-6); // world +X
        CHECK_APPROX_EPS(body.linearVelocity.z, 0.0, 1e-6);
    }
}

// -----------------------------------------------------------------------------
// The orientation quaternion stays UNIT after a long tumble (renormalised each
// step). A drifting norm silently corrupts every derived rotation.
// -----------------------------------------------------------------------------
TEST_CASE(RigidBody_OrientationStaysNormalised)
{
    ecs::RigidBody body = MakeAsymmetricBody();
    body.angularVelocity = { 2.0f, 1.5f, 1.0f };

    core::Vec3d pos{};
    core::Quatf orn = core::Quatf::Identity();

    for (int i = 0; i < 5000; ++i)
        sim::IntegrateRigidBody(pos, orn, body, kZeroForceD, kZeroTorqueF, kDt);

    CHECK_APPROX_EPS(orn.LengthSq(), 1.0f, 1e-5f);
}

// -----------------------------------------------------------------------------
// dt <= 0 or non-finite is a NO-OP that also leaves the staged accumulators
// intact (so a caller can retry the step), matching IntegrateVelocity. And a
// real step DOES clear the accumulators.
// -----------------------------------------------------------------------------
TEST_CASE(RigidBody_ZeroDt_IsNoOpAndPreservesAccumulators)
{
    ecs::RigidBody body = MakeAsymmetricBody();
    body.linearVelocity  = { 1.0, 2.0, 3.0 };
    body.angularVelocity = { 0.4f, 0.5f, 0.6f };
    body.forceAccum      = { 9.0, 8.0, 7.0 };
    body.torqueAccum     = { 0.1f, 0.2f, 0.3f };

    const core::Vec3d x0{ 5.0, 6.0, 7.0 };
    const core::Quatf orn0 = core::Quatf::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, 0.3f);

    for (const double badDt : { 0.0, -1.0, std::nan("") })
    {
        core::Vec3d pos = x0;
        core::Quatf orn = orn0;
        ecs::RigidBody b = body;
        sim::IntegrateRigidBody(pos, orn, b, kZeroForceD, kZeroTorqueF, badDt);

        CHECK_EQ(pos.x, x0.x); CHECK_EQ(pos.y, x0.y); CHECK_EQ(pos.z, x0.z);
        CHECK_EQ(orn.x, orn0.x); CHECK_EQ(orn.w, orn0.w);
        CHECK_EQ(b.linearVelocity.x, body.linearVelocity.x);
        CHECK_EQ(b.angularVelocity.z, body.angularVelocity.z);
        // Accumulators survive a no-op step.
        CHECK_EQ(b.forceAccum.x, body.forceAccum.x);
        CHECK_EQ(b.torqueAccum.z, body.torqueAccum.z);
    }

    // A genuine step consumes and clears the accumulators.
    {
        core::Vec3d pos = x0;
        core::Quatf orn = orn0;
        ecs::RigidBody b = body;
        sim::IntegrateRigidBody(pos, orn, b, kZeroForceD, kZeroTorqueF, kDt);
        CHECK_EQ(b.forceAccum.x, 0.0);
        CHECK_EQ(b.forceAccum.y, 0.0);
        CHECK_EQ(b.forceAccum.z, 0.0);
        CHECK_EQ(b.torqueAccum.x, 0.0f);
        CHECK_EQ(b.torqueAccum.y, 0.0f);
        CHECK_EQ(b.torqueAccum.z, 0.0f);
    }
}
