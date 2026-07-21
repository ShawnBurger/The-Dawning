#pragma once
// =============================================================================
// sim/simulation_system.h - deterministic GPU-free fixed-step orchestration
// =============================================================================
// This is the single simulation phase-order contract. The host owns input,
// render-history reset and fixed-accumulator draining; this module owns the
// ordered ECS adapters that consume one already-approved fixed dt.

#include "atmosphere_system.h"
#include "ftl_system.h"
#include "orbit_system.h"
#include "physics_system.h"
#include "relativity_system.h"

#include <span>
#include <vector>

namespace sim
{

struct AtmosphereBinding
{
    ecs::Entity entity = ecs::NullEntity;
    AtmosphereEnvironment environment;
};

struct FtlCommand
{
    ecs::Entity entity = ecs::NullEntity;
    FrameId destinationFrame = kInvalidFrame;
    MouthTransform mouth;
};

struct SimulationStepConfig
{
    FrameId activeFrame = kInvalidFrame;
    FrameId masterFrame = kInvalidFrame;
    double coordinateTime = 0.0;
    FlightAssistParams flightAssist;
    CloseEncounterConfig closeEncounters;
    std::span<const FtlCommand> ftlCommands;
    std::span<const AtmosphereBinding> atmosphereBindings;
    std::span<const ClockGravityBinding> clockGravityBindings;
};

enum class SimulationStepStage : uint32_t
{
    None = 0,
    Ftl,
    Atmosphere,
    PassiveOrbit,
    Gravity,
    Flight,
    Clocks,
    Complete,
};

struct SimulationStepResult
{
    bool accepted = false;
    SimulationStepStage completedStage = SimulationStepStage::None;
    bool resetRenderHistory = false;
    bool drainFixedAccumulator = false;
    std::vector<FtlTransitionResult> ftl;
    std::vector<AtmosphereStepResult> atmosphere;
    PassiveOrbitStepResult passiveOrbit;
    GravityAccumulationResult gravity;
    FlightPhysicsStepResult flight;
    RelativisticClockStepResult clocks;
};

// Execute one fixed coordinate-time step in this load-bearing order:
//   FTL commands -> atmosphere ownership transitions -> passive N-body/rails ->
//   force-integrated gravity -> flight/relativistic momentum -> proper clocks.
// FTL and atmosphere must run before motion-owner dispatch because either may
// promote a passive body to ForceIntegrated. Duplicate per-step commands/bindings
// are rejected before the first write so one operator cannot be applied twice.
SimulationStepResult StepSimulation(
    ecs::Registry& registry,
    const FrameGraph& frames,
    double dt,
    const SimulationStepConfig& config);

} // namespace sim
