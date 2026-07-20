// =============================================================================
// sim/rigid_body.cpp — Six-DOF rigid-body integrator (GPU-free)
// =============================================================================

#include "rigid_body.h"

#include <cmath>

namespace sim
{

core::Vec3f InertiaDiagFromInverse(const core::Vec3f& invInertiaDiag)
{
    return {
        invInertiaDiag.x != 0.0f ? 1.0f / invInertiaDiag.x : 0.0f,
        invInertiaDiag.y != 0.0f ? 1.0f / invInertiaDiag.y : 0.0f,
        invInertiaDiag.z != 0.0f ? 1.0f / invInertiaDiag.z : 0.0f,
    };
}

namespace
{

// I · ω, component-wise, in the body frame (I diagonal).
core::Vec3f InertiaTimes(const core::Vec3f& inertiaDiag, const core::Vec3f& w)
{
    return { inertiaDiag.x * w.x, inertiaDiag.y * w.y, inertiaDiag.z * w.z };
}

// -----------------------------------------------------------------------------
// The gyroscopic coupling ω × (I ω), solved IMPLICITLY (backward Euler, one
// Newton step from ω_n) and returned as the increment dω with ω_{n+1} = ω_n − dω.
//
// WHY IMPLICIT. The explicit form ω += Iinv·(−ω×Iω)·dt is what
// FLIGHT_PHYSICS_DESIGN §2.1 literally writes, but it is numerically UNSTABLE for
// the free asymmetric top: it gains energy and angular momentum secularly and
// diverges to NaN within ~10⁴ steps at flight angular rates (this was MEASURED —
// relL grew 0.14 → 0.91 → NaN over N = 1e3 → 4e3 → 1e4 at ω = (2,1.5,1)). That
// contradicts the design's "bounded energy error" claim, which holds only for
// the SEPARABLE translational half, not the non-canonical rotational one. The
// implicit solve is the standard game-physics fix (matches Bullet's implicit
// gyroscopic torque): it is unconditionally stable and does not pump energy, so
// world-frame L stays conserved over 10⁴+ steps with no secular drift.
//
// The gyroscopic PHYSICS is still explicit and droppable: rhs below is exactly
// dt·(ω × Iω). Removing this whole function's contribution reproduces the
// "dropped gyroscopic term" failure mode of §9.2, and the conservation test
// catches it.
//
// Solved with an inline 3×3 Cramer's-rule solve rather than adding a Mat3x3 to
// core/types.h, keeping the design's "no Mat3x3 yet" decision (§2.5) intact.
// Jacobian J = diag(I) + dt·(skew(ω)·diag(I) − skew(Iω)).
// -----------------------------------------------------------------------------
core::Vec3f GyroscopicDeltaImplicit(const core::Vec3f& I, const core::Vec3f& w, float dt)
{
    const core::Vec3f L   = { I.x * w.x, I.y * w.y, I.z * w.z };  // I ω
    const core::Vec3f rhs = w.Cross(L) * dt;                       // dt (ω × Iω)

    const float j00 = I.x;
    const float j01 = dt * (-w.z * I.y + L.z);
    const float j02 = dt * ( w.y * I.z - L.y);
    const float j10 = dt * ( w.z * I.x - L.z);
    const float j11 = I.y;
    const float j12 = dt * (-w.x * I.z + L.x);
    const float j20 = dt * (-w.y * I.x + L.y);
    const float j21 = dt * ( w.x * I.y - L.x);
    const float j22 = I.z;

    const float det = j00 * (j11 * j22 - j12 * j21)
                    - j01 * (j10 * j22 - j12 * j20)
                    + j02 * (j10 * j21 - j11 * j20);
    if (std::fabs(det) < 1e-20f)
        return { 0.0f, 0.0f, 0.0f };
    const float inv = 1.0f / det;

    const float dx = (rhs.x * (j11 * j22 - j12 * j21)
                    - j01 * (rhs.y * j22 - j12 * rhs.z)
                    + j02 * (rhs.y * j21 - j11 * rhs.z)) * inv;
    const float dy = (j00 * (rhs.y * j22 - j12 * rhs.z)
                    - rhs.x * (j10 * j22 - j12 * j20)
                    + j02 * (j10 * rhs.z - rhs.y * j20)) * inv;
    const float dz = (j00 * (j11 * rhs.z - rhs.y * j21)
                    - j01 * (j10 * rhs.z - rhs.y * j20)
                    + rhs.x * (j10 * j21 - j11 * j20)) * inv;
    return { dx, dy, dz };
}

} // namespace

core::Vec3d WorldAngularMomentum(const core::Quatf& orientation,
                                 const ecs::RigidBody& body)
{
    const core::Vec3f inertiaDiag = InertiaDiagFromInverse(body.invInertiaDiag);
    const core::Vec3f Lbody = InertiaTimes(inertiaDiag, body.angularVelocity);
    // R maps body-frame vectors into world; L is a (pseudo)vector so this is the
    // correct transform of the body-frame angular momentum into the world frame.
    const core::Vec3f Lworld = orientation.Normalized().Rotate(Lbody);
    return core::Vec3d::FromFloat(Lworld);
}

double RotationalKineticEnergy(const ecs::RigidBody& body)
{
    const core::Vec3f inertiaDiag = InertiaDiagFromInverse(body.invInertiaDiag);
    const core::Vec3f Iw = InertiaTimes(inertiaDiag, body.angularVelocity);
    return 0.5 * static_cast<double>(body.angularVelocity.Dot(Iw));
}

void IntegrateRigidBody(core::Vec3d& position,
                        core::Quatf& orientation,
                        ecs::RigidBody& body,
                        const core::Vec3d& externalForce,
                        const core::Vec3f& externalTorque,
                        double dt)
{
    // Same guard as ecs::systems::IntegrateVelocity: a non-positive or non-finite
    // step is a no-op, and it leaves the accumulators intact so the caller can
    // retry the step without losing the wrench it staged.
    if (!(dt > 0.0) || !std::isfinite(dt))
        return;

    // -------------------------------------------------------------------------
    // Linear half — WORLD frame. Semi-implicit Euler: velocity first, then
    // position from the UPDATED velocity.
    // -------------------------------------------------------------------------
    const core::Vec3d totalForce = body.forceAccum + externalForce;
    const core::Vec3d linearAccel = totalForce * body.invMass;   // a = F * invMass
    body.linearVelocity += linearAccel * dt;                     // v_{n+1}
    position += body.linearVelocity * dt;                        // x from NEW v

    // -------------------------------------------------------------------------
    // Angular half — BODY frame. Euler's equation, split into (1) the gyroscopic
    // coupling solved IMPLICITLY for stability and (2) the applied torque, which
    // is small and controlled, applied explicitly. Velocity is updated first,
    // then the orientation from the NEW ω.
    // -------------------------------------------------------------------------
    const core::Vec3f inertiaDiag = InertiaDiagFromInverse(body.invInertiaDiag);
    const float       dtf = static_cast<float>(dt);

    // (1) Gyroscopic coupling ω × (I ω), implicit (see GyroscopicDeltaImplicit).
    const core::Vec3f dOmega = GyroscopicDeltaImplicit(inertiaDiag, body.angularVelocity, dtf);
    body.angularVelocity -= dOmega;

    // (2) Applied torque: α = Iinv · τ, component-wise (I diagonal).
    const core::Vec3f totalTorque = body.torqueAccum + externalTorque;
    const core::Vec3f angAccel = {
        body.invInertiaDiag.x * totalTorque.x,
        body.invInertiaDiag.y * totalTorque.y,
        body.invInertiaDiag.z * totalTorque.z,
    };
    body.angularVelocity += angAccel * dtf;                      // ω_{n+1}

    // Orientation via the exponential map of the NEW body-frame rate, RIGHT-
    // multiplied and renormalised — identical in form to IntegrateVelocity.
    const float angularSpeed = body.angularVelocity.Length();
    if (angularSpeed > 1e-8f && std::isfinite(angularSpeed))
    {
        const core::Vec3f axis  = body.angularVelocity / angularSpeed;
        const float       angle = angularSpeed * static_cast<float>(dt);
        const core::Quatf delta = core::Quatf::FromAxisAngle(axis, angle);
        orientation = (orientation * delta).Normalized();        // body frame
    }

    // Accumulators are consumed once per step — clear them for the next one.
    body.forceAccum  = { 0.0, 0.0, 0.0 };
    body.torqueAccum = { 0.0f, 0.0f, 0.0f };
}

} // namespace sim
