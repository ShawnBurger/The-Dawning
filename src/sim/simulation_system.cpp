// =============================================================================
// sim/simulation_system.cpp - deterministic GPU-free fixed-step orchestration
// =============================================================================

#include "simulation_system.h"

#include <cmath>
#include <unordered_set>

namespace sim
{

namespace
{

template<typename T>
bool UniqueLiveEntities(ecs::Registry& registry, std::span<const T> entries)
{
    std::unordered_set<uint32_t> seen;
    seen.reserve(entries.size() * 2u);
    for (const T& entry : entries)
        if (!registry.IsAlive(entry.entity) ||
            !seen.insert(entry.entity.id).second)
            return false;
    return true;
}

} // namespace

SimulationStepResult StepSimulation(
    ecs::Registry& registry,
    const FrameGraph& frames,
    double dt,
    const SimulationStepConfig& config)
{
    SimulationStepResult result;
    if (!(dt > 0.0) || !std::isfinite(dt) ||
        !std::isfinite(config.coordinateTime) ||
        !std::isfinite(config.coordinateTime + dt) ||
        !UniqueLiveEntities(registry, config.ftlCommands) ||
        !UniqueLiveEntities(registry, config.atmosphereBindings))
        return result;

    result.ftl.reserve(config.ftlCommands.size());
    for (const FtlCommand& command : config.ftlCommands)
    {
        const FtlTransitionResult transition = TryTeleportEntity(
            registry, command.entity, frames,
            command.destinationFrame, command.mouth);
        result.ftl.push_back(transition);
        if (!transition.accepted)
            return result;
        result.resetRenderHistory |= transition.resetRenderHistory;
        result.drainFixedAccumulator |= transition.drainFixedAccumulator;
    }
    result.completedStage = SimulationStepStage::Ftl;

    result.atmosphere.reserve(config.atmosphereBindings.size());
    for (const AtmosphereBinding& binding : config.atmosphereBindings)
    {
        const AtmosphereStepResult atmosphere = ApplyAtmosphereToEntity(
            registry, binding.entity, frames, binding.environment, dt);
        result.atmosphere.push_back(atmosphere);
        if (!atmosphere.accepted)
            return result;
    }
    result.completedStage = SimulationStepStage::Atmosphere;

    result.passiveOrbit = StepPassiveOrbits(
        registry, frames, config.activeFrame,
        config.coordinateTime, dt, config.closeEncounters);
    if (!result.passiveOrbit.accepted)
        return result;
    result.completedStage = SimulationStepStage::PassiveOrbit;

    result.gravity = AccumulateForceIntegratedGravity(registry, frames);
    if (!result.gravity.accepted)
        return result;
    result.completedStage = SimulationStepStage::Gravity;

    result.flight = StepFlightPhysics(registry, dt, config.flightAssist);
    if (!result.flight.accepted)
        return result;
    result.completedStage = SimulationStepStage::Flight;

    result.clocks = StepRelativisticClocks(
        registry, frames, config.masterFrame, dt,
        config.clockGravityBindings);
    if (!result.clocks.accepted)
        return result;
    result.completedStage = SimulationStepStage::Clocks;

    result.accepted = true;
    result.completedStage = SimulationStepStage::Complete;
    return result;
}

} // namespace sim
