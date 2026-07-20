#pragma once
// =============================================================================
// sim/flight_control.h — Control allocation + flight-assist law (GPU-free)
// =============================================================================
// Flight Stage 2 (FLIGHT_PHYSICS_DESIGN.md §8.2 stages 2-3): the two PURE control
// laws that sit between the pilot's six-axis FlightControl demand and the rigid-
// body wrench the integrator consumes.
//
//   1. AllocateThrusters — DIRECT/greedy control allocation. Maps the six-axis
//      demand to a per-thruster throttle in [0,1] so the produced net BODY wrench
//      tracks the demand (§1: "Thruster allocation is direct, not solved").
//   2. ComputeFlightAssist — the COUPLED-mode proportional velocity controller.
//      Interprets the demand as a TARGET velocity and returns the wrench that
//      drives the body toward it (§1 "flight-assist … auto-brakes toward zero",
//      §8.2 Stage 3). This is a well-defined LAW with an exact closed form, not a
//      feel knob — see tests/test_flight_control.cpp for the geometric-decay
//      ground truth it is checked against.
//
// GPU-free for the same reason rigid_body.h / thrusters.h are: includes only
// core/types.h and ecs/components.h, so it links into TheDawningTests and the
// analytic tests drive the SHIPPED laws, not a paraphrase.
//
// FRAMES (load-bearing, matching ecs::RigidBody):
//   - linearDemand / angularDemand are BODY-frame pilot intent in [-1,1] per axis
//     (strafe X / lift Y / thrust Z, and pitch X / yaw Y / roll Z).
//   - AllocateThrusters works entirely in the BODY frame (thruster geometry is
//     body-local); ComputeWrench then produces a body-frame wrench.
//   - ComputeFlightAssist returns force in the WORLD frame and torque in the BODY
//     frame — exactly the accumulator conventions ecs::RigidBody documents — so
//     the caller can add them straight onto forceAccum / torqueAccum.
// =============================================================================

#include "../core/types.h"
#include "../ecs/components.h"

namespace sim
{

// -----------------------------------------------------------------------------
// DIRECT / GREEDY control allocation.
// -----------------------------------------------------------------------------
// For each live thruster i, writes
//     throttle_i = saturate( linearDemand · dir_i  +  angularDemand · axis_i )
// where dir_i is the unit thrust direction and axis_i is the UNIT axis of the
// torque the thruster makes about the centre of mass (normalize((pos_i-com)×dir_i),
// or zero for a thruster whose line of action passes through the CoM). A thruster
// fires ONLY forward (throttle >= 0): opposing demand is served by the opposing
// thruster, whose alignment with the demand is negative and clamps to zero.
//
// PROPERTIES (the tested ground truth):
//   - ZERO demand -> EXACTLY 0.0f throttle on every thruster (saturate(0)==0). This
//     is the negative control: allocation adds nothing when nothing is asked.
//   - A pure +axis demand fires the thrusters aligned with that axis; for a layout
//     symmetric about the CoM the off-axis torques cancel, so the net wrench is a
//     pure force (or, for an angular demand, a pure torque).
//
// LIMITATIONS (this is the first slice, stated so no one mistakes it for more):
//   - Directional, not magnitude-calibrated: throttle is a cosine alignment, so
//     the produced wrench MAGNITUDE depends on the layout (lever arms, maxForce),
//     not solved to equal the demand. A least-squares / pseudo-inverse allocator
//     that hits an exact target wrench and balances redundant thrusters is later
//     scope (§1, Ship deep-dive PART 2's per-nozzle IFCS).
//   - Greedy per-axis: a thruster serving both a linear and an angular axis sums
//     the two alignments and may saturate, under-serving one. No cross-axis
//     cancellation of oblique thrusters.
void AllocateThrusters(ecs::ThrusterSet& set,
                       const core::Vec3f& linearDemand,
                       const core::Vec3f& angularDemand,
                       const core::Vec3f& centreOfMass);

// -----------------------------------------------------------------------------
// Flight-assist tunables. These are TUNING and live in data, deliberately OUT of
// the tested law (FLIGHT_PHYSICS_DESIGN §10 "keep the tunables in data … out of
// the tested laws"): the tests pass their own params and assert the closed form,
// never "feels right".
// -----------------------------------------------------------------------------
struct FlightAssistParams
{
    // Proportional feedback rates (1/s). The per-step decay factor is (1 - gain*dt);
    // for monotonic, non-overshooting decay keep gain*dt in (0, 1]. At the 60 Hz
    // step 2-4 /s gives gain*dt of 0.03-0.07.
    float linearGain     = 2.0f;
    float angularGain    = 3.0f;

    // Target speed / rate at full (|demand|=1) stick deflection.
    float maxLinearSpeed = 100.0f; // m/s
    float maxAngularRate = 2.0f;   // rad/s
};

// The flight-assist output, split into the two frames RigidBody's accumulators use.
struct AssistWrench
{
    core::Vec3d worldForce = { 0.0, 0.0, 0.0 }; // WORLD frame, newtons
    core::Vec3f bodyTorque = { 0.0f, 0.0f, 0.0f }; // BODY frame, newton-metres
};

// -----------------------------------------------------------------------------
// COUPLED-mode proportional velocity controller.
// -----------------------------------------------------------------------------
// Interprets the pilot demand as a TARGET velocity and returns the wrench that
// closes the velocity error:
//     v_target = R · (linearDemand · maxLinearSpeed)     [demand is body-frame]
//     F_world  = mass · linearGain · (v_target − v)
//     w_target = angularDemand · maxAngularRate          [body frame]
//     T_body   = I · angularGain · (w_target − w)        [component-wise, I diag]
// where mass = 1/invMass and I = InertiaDiagFromInverse(invInertiaDiag).
//
// WHY THIS SHAPE IS TESTABLE. Multiplying the linear error by mass (and the
// angular error by I) makes the resulting ACCELERATION exactly gain·error,
// independent of the body's mass/inertia:
//     a = F·invMass = linearGain·(v_target − v)
// Semi-implicit Euler then gives the exact geometric law
//     v_{n+1} = v_n + a·dt = (1 − linearGain·dt)·v_n + linearGain·dt·v_target
// i.e. v_n = v_target + (v_0 − v_target)·(1 − linearGain·dt)^n. With zero demand
// (v_target = 0) that is monotone decay to rest; with a demand it converges to the
// demanded velocity. The angular half is identical when the gyroscopic coupling
// vanishes (symmetric inertia or a principal-axis spin). A static body (invMass
// <= 0) or a locked axis (invInertia == 0) receives zero assist on that channel.
AssistWrench ComputeFlightAssist(const ecs::RigidBody& body,
                                 const core::Quatf& orientation,
                                 const core::Vec3f& linearDemand,
                                 const core::Vec3f& angularDemand,
                                 const FlightAssistParams& params);

} // namespace sim
