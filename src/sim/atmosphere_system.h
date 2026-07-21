#pragma once
// =============================================================================
// sim/atmosphere_system.h - frame-aware ECS atmospheric-flight adapter
// =============================================================================
// GPU-free glue between the reviewed atmosphere math and live rigid-body state.
// Call exactly once before StepFlightPhysics in the same fixed step. Drag is an
// operator-split velocity update; it must not also be accumulated as force.

#include "atmosphere.h"
#include "reference_frame.h"
#include "../ecs/registry.h"
#include "../ecs/components.h"

namespace sim
{

struct AtmosphereEnvironment
{
    WorldPos center;
    Vec3d linearVelocity{ 0.0, 0.0, 0.0 };  // global translation velocity
    Vec3d angularVelocity{ 0.0, 0.0, 0.0 }; // global rotation vector, rad/s
    double radius = 1.0;                     // geometric surface radius, m
    AtmosphereModel model = AtmosphereModel::Vacuum();
};

struct AtmosphereStepResult
{
    bool accepted = false;
    bool inAtmosphere = false;
    double density = 0.0;
    double dynamicPressure = 0.0;
    double mach = 0.0;
    double angleOfAttack = 0.0;
    double heatFlux = 0.0;
};

// Apply one fixed atmospheric substep to an entity with Transform + SpatialFrame
// + RigidBody + AerodynamicBody. Optional GravitationalBody ownership is promoted
// to NBodyActive only when the sampled density is positive. Invalid input is a
// component-preserving rejection; vacuum and ceiling samples are accepted no-ops.
AtmosphereStepResult ApplyAtmosphereToEntity(ecs::Registry& registry,
                                             ecs::Entity entity,
                                             const FrameGraph& frames,
                                             const AtmosphereEnvironment& environment,
                                             double dt);

} // namespace sim
