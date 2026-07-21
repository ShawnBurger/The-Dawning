#pragma once
// =============================================================================
// sim/physics_system.h — Fixed-step flight physics system (ECS glue, GPU-free)
// =============================================================================
// The one place the pure sim laws meet the ECS. StepFlightPhysics is the system
// Scene::UpdateSystems runs once per RULE-6 fixed step (app.cpp drives it from
// `while (m_timer.ConsumeFixedStep()) m_scene.UpdateSystems(m_timer.GetFixedDt())`,
// so it advances by a CONSTANT dt per call, decoupled from the render frame).
//
// Per eligible entity with Transform + RigidBody it runs the Stage-2 pipeline
// (FLIGHT_PHYSICS_DESIGN §6):
//     control demand  ->  allocation / flight-assist  ->  wrench
//                     ->  IntegrateRigidBody          ->  write Transform
// Mode selection (FlightControl.mode):
//   - Decoupled: pilot demand -> AllocateThrusters -> ComputeWrench ->
//     AccumulateBodyWrench. Direct thrust; zero demand -> zero throttle -> coast.
//   - Coupled:   ComputeFlightAssist produces the desired wrench, then bounded
//     allocation through ThrusterSet realizes it. No bank -> no assist force;
//     zero demand brakes only within installed actuator authority.
// An entity with a ThrusterSet but no FlightControl fires whatever throttles are
// already set (scripted thrusters). Bodies with neither just integrate.
//
// GPU-free: includes only ecs/registry.h + ecs/components.h + the sim headers,
// all device-free, so it links into TheDawningTests and the wiring is exercised
// over the REAL registry (tests/test_physics_system.cpp), not a stand-in.
//
// COEXISTENCE WITH THE KINEMATIC MOVER. ecs::systems::IntegrateVelocity (driven by
// Scene::SystemVelocity) still moves Transform+Velocity entities — spinners,
// debris, toy movers. It and this system key on DISJOINT component sets (Velocity
// vs RigidBody), so a body is driven by exactly one of them; they never fight over
// the same Transform. Both run each fixed step.
// =============================================================================

#include "../ecs/registry.h"
#include "../ecs/components.h"
#include "flight_control.h"
#include "reference_frame.h"

#include <cstdint>

namespace sim
{

struct GravityAccumulationResult
{
    bool accepted = false;
    uint32_t sourceCount = 0;
    uint32_t targetCount = 0;
};

// Stage the softened gravity of every valid massive source into every
// ForceIntegrated body's world-force accumulator. Source and target positions
// are expressed in each target's frame before calling GravityAccelerationAt.
// Validation is whole-system atomic: any malformed gravitational entity rejects
// without changing any accumulator. NBodyActive and OnRails bodies are sources
// only here; their motion remains owned by their respective integrators.
GravityAccumulationResult AccumulateForceIntegratedGravity(
    ecs::Registry& registry, const FrameGraph& frames);

// Advance every RigidBody entity in `registry` by one fixed step `dt`.
// dt <= 0 or non-finite is a whole-system no-op (matches IntegrateRigidBody).
// Gravitational bodies advance here only when owner == ForceIntegrated; the
// other ownership states are exact no-ops so two movers cannot update one pose.
void StepFlightPhysics(ecs::Registry& registry,
                       double dt,
                       const FlightAssistParams& params = FlightAssistParams{});

} // namespace sim
