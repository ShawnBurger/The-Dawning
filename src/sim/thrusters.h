#pragma once
// =============================================================================
// sim/thrusters.h — Thruster wrench accumulation (GPU-free)
// =============================================================================
// Converts a ThrusterSet's per-nozzle throttles into the net wrench (force +
// torque) they produce, per the Ship deep-dive PART 2 and FLIGHT_PHYSICS_DESIGN
// §1: each thruster contributes force = dir·maxForce·throttle and torque about
// the centre of mass = (localPosition − com) × force.
//
// The returned wrench is entirely in the BODY frame, because every thruster
// datum (offset, direction) is body-local. Applying it to a RigidBody therefore
// requires rotating the FORCE into the world frame (the linear dynamics are
// world-frame) while the TORQUE stays body-frame — that bridge is
// AccumulateBodyWrench below.
//
// GPU-free for the same reason rigid_body.h is: includes only core/types.h and
// ecs/components.h, so it compiles into TheDawningTests and the shipped code is
// what the tests drive.
// =============================================================================

#include "../core/types.h"
#include "../ecs/components.h"

namespace sim
{

// Net force and torque produced by a set of thrusters, both in the BODY frame.
struct Wrench
{
    core::Vec3f force  = { 0.0f, 0.0f, 0.0f }; // body-frame, newtons
    core::Vec3f torque = { 0.0f, 0.0f, 0.0f }; // body-frame about the CoM, N·m
};

// Sum the body-frame wrench over all live thrusters in `set`.
//   force  = Σ dir_i · maxForce_i · throttle_i
//   torque = Σ (position_i − centreOfMass) × force_i
// centreOfMass is the body-local CoM the lever arms are measured from (the
// origin for a body modelled with its origin at the CoM).
Wrench ComputeWrench(const ecs::ThrusterSet& set, const core::Vec3f& centreOfMass);

// Bridge a BODY-frame wrench onto a RigidBody's accumulators. The force is
// rotated into the WORLD frame by the body's current orientation (world-frame
// linear dynamics); the torque is added body-frame unchanged. This is the one
// place body→world frame conversion happens for thrust, so the accumulators keep
// their documented frames (RigidBody in ecs/components.h).
void AccumulateBodyWrench(ecs::RigidBody& body,
                          const core::Quatf& orientation,
                          const Wrench& wrench);

} // namespace sim
