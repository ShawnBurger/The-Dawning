#pragma once
// =============================================================================
// sim/rigid_body.h — Six-DOF rigid-body integrator (GPU-free)
// =============================================================================
// The pure dynamics core of the flight model, per docs/research/
// FLIGHT_PHYSICS_DESIGN.md §2. Deliberately GPU-free (it includes only
// core/types.h and ecs/components.h, both of which are themselves GPU-free —
// core/types.h is pure math and ecs/components.h includes only core/types.h and
// <cstdint>), so it links into BOTH TheDawningV3 and TheDawningTests exactly the
// way src/core/shadow_cascades.cpp does. The unit tests therefore drive the
// SHIPPED integrator, not a paraphrase of it — which is the whole point of this
// module living here (see tests/test_shadow_cascades.cpp for the same rationale).
//
// NOTE ON INCLUDES: the design §8.1 says this header includes "only
// core/types.h". It must also see the RigidBody struct, which lives in
// ecs/components.h (the design's own IntegrateRigidBody signature takes a
// RigidBody&). components.h is GPU-free, so pulling it in keeps this module
// device-free and preserves the dependency direction core ← ecs ← sim: sim
// includes ecs, ecs never includes sim. No cycle.
//
// -----------------------------------------------------------------------------
// The step (semi-implicit / symplectic Euler) — FLIGHT_PHYSICS_DESIGN §2.1
// -----------------------------------------------------------------------------
// Linear (WORLD frame):
//     a          = F * invMass                       // F = accum + external
//     v_{n+1}    = v_n + a * dt                       // VELOCITY first
//     x_{n+1}    = x_n + v_{n+1} * dt                 // position from the NEW v
//
// Angular (BODY frame, diagonal inertia I):
//     Iω         = (Ixx ωx, Iyy ωy, Izz ωz)
//     gyro       = ω × Iω                             // Euler's-equation coupling
//     α          = Iinv · (τ − gyro)                  // component-wise
//     ω_{n+1}    = ω_n + α * dt                       // VELOCITY first
//     Δq         = FromAxisAngle(ω̂_{n+1}, |ω_{n+1}| dt)
//     q_{n+1}    = normalize(q_n · Δq)                // RIGHT-multiply (body frame)
//
// The orientation update mirrors ecs::systems::IntegrateVelocity element for
// element: FromAxisAngle of the (new) body-frame rate, RIGHT-multiplied onto the
// current orientation, renormalised. That is the shipped, correct body-frame
// convention; the deep-dive's left-multiply is wrong (FLIGHT_PHYSICS_DESIGN §2.4).
// =============================================================================

#include "../core/types.h"
#include "../ecs/components.h"

namespace sim
{

// Recover the forward inertia diagonal (Ixx, Iyy, Izz) from the stored inverse.
// A zero inverse entry means an infinitely-heavy (locked) axis; it maps to a
// zero forward inertia here, which is exact only while that axis's ω is held at
// zero. The flight slice uses finite, positive principal inertias throughout, so
// this guard never bites in practice — it exists so the math is total, not UB.
core::Vec3f InertiaDiagFromInverse(const core::Vec3f& invInertiaDiag);

// World-frame angular momentum L = R · (I · ω_body), returned in double so a
// conservation test can measure drift below float epsilon. Shares this module's
// I-from-inverse derivation so the test and the integrator agree by construction
// rather than by a re-typed formula.
core::Vec3d WorldAngularMomentum(const core::Quatf& orientation,
                                 const ecs::RigidBody& body);

// Rotational kinetic energy ½ ω · (I ω), body frame (frame-invariant scalar).
double RotationalKineticEnergy(const ecs::RigidBody& body);

// Advance one body by one fixed step.
//   position     — WORLD position (Transform.position), read and written.
//   orientation  — Quatf orientation (Transform.rotation), read and written.
//   body         — dynamics state; forceAccum/torqueAccum are consumed and then
//                  ZEROED (so the next step starts clean, matching the ECS wiring
//                  in FLIGHT_PHYSICS_DESIGN §6 step 4).
//   externalForce  — extra WORLD-frame force applied this step (e.g. gravity,
//                    later). Not stored, not cleared — it is a per-call input.
//   externalTorque — extra BODY-frame torque applied this step.
//   dt           — fixed timestep, seconds. dt <= 0 or non-finite is a no-op
//                  (accumulators are left untouched, matching IntegrateVelocity).
void IntegrateRigidBody(core::Vec3d& position,
                        core::Quatf& orientation,
                        ecs::RigidBody& body,
                        const core::Vec3d& externalForce,
                        const core::Vec3f& externalTorque,
                        double dt);

} // namespace sim
