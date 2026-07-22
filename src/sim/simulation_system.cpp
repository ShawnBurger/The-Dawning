// =============================================================================
// sim/simulation_system.cpp - deterministic GPU-free fixed-step orchestration
// =============================================================================

#include "simulation_system.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

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

struct FtlRollbackState
{
    ecs::Entity entity = ecs::NullEntity;
    ecs::Transform transform;
    ecs::SpatialFrame spatialFrame;
    ecs::RigidBody rigidBody;
    bool hasGravity = false;
    ecs::GravitationalBody gravity;
    bool hasRelativistic = false;
    ecs::RelativisticBody relativistic;
};

bool CaptureFtlState(ecs::Registry& registry, ecs::Entity entity,
                     FtlRollbackState& state)
{
    const ecs::Transform* transform = registry.TryGet<ecs::Transform>(entity);
    const ecs::SpatialFrame* spatial = registry.TryGet<ecs::SpatialFrame>(entity);
    const ecs::RigidBody* rigid = registry.TryGet<ecs::RigidBody>(entity);
    if (!transform || !spatial || !rigid)
        return false;

    state.entity = entity;
    state.transform = *transform;
    state.spatialFrame = *spatial;
    state.rigidBody = *rigid;
    if (const auto* gravity = registry.TryGet<ecs::GravitationalBody>(entity))
    {
        state.hasGravity = true;
        state.gravity = *gravity;
    }
    if (const auto* relativistic = registry.TryGet<ecs::RelativisticBody>(entity))
    {
        state.hasRelativistic = true;
        state.relativistic = *relativistic;
    }
    return true;
}

void RestoreFtlState(ecs::Registry& registry, const FtlRollbackState& state)
{
    registry.Get<ecs::Transform>(state.entity) = state.transform;
    registry.Get<ecs::SpatialFrame>(state.entity) = state.spatialFrame;
    registry.Get<ecs::RigidBody>(state.entity) = state.rigidBody;
    if (state.hasGravity)
        registry.Get<ecs::GravitationalBody>(state.entity) = state.gravity;
    if (state.hasRelativistic)
        registry.Get<ecs::RelativisticBody>(state.entity) = state.relativistic;
}

struct AtmosphereRollbackState
{
    ecs::Entity entity = ecs::NullEntity;
    ecs::RigidBody rigidBody;
    bool hasGravity = false;
    ecs::GravitationalBody gravity;
    bool hasRelativistic = false;
    ecs::RelativisticBody relativistic;
};

bool CaptureAtmosphereState(ecs::Registry& registry, ecs::Entity entity,
                            AtmosphereRollbackState& state)
{
    if (!registry.Has<ecs::Transform>(entity) ||
        !registry.Has<ecs::SpatialFrame>(entity) ||
        !registry.Has<ecs::AerodynamicBody>(entity))
        return false;
    const ecs::RigidBody* rigid = registry.TryGet<ecs::RigidBody>(entity);
    if (!rigid)
        return false;

    state.entity = entity;
    state.rigidBody = *rigid;
    if (const auto* gravity = registry.TryGet<ecs::GravitationalBody>(entity))
    {
        state.hasGravity = true;
        state.gravity = *gravity;
    }
    if (const auto* relativistic = registry.TryGet<ecs::RelativisticBody>(entity))
    {
        state.hasRelativistic = true;
        state.relativistic = *relativistic;
    }
    return true;
}

void RestoreAtmosphereState(ecs::Registry& registry,
                            const AtmosphereRollbackState& state)
{
    registry.Get<ecs::RigidBody>(state.entity) = state.rigidBody;
    if (state.hasGravity)
        registry.Get<ecs::GravitationalBody>(state.entity) = state.gravity;
    if (state.hasRelativistic)
        registry.Get<ecs::RelativisticBody>(state.entity) = state.relativistic;
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
        !UniqueLiveEntities(registry, config.atmosphereBindings) ||
        !UniqueLiveEntities(registry, config.clockGravityBindings))
        return result;

    result.ftl.reserve(config.ftlCommands.size());
    std::vector<FtlRollbackState> ftlRollback(config.ftlCommands.size());
    for (size_t i = 0; i < config.ftlCommands.size(); ++i)
        if (!CaptureFtlState(registry, config.ftlCommands[i].entity,
                             ftlRollback[i]))
            return result;
    for (const FtlCommand& command : config.ftlCommands)
    {
        const FtlTransitionResult transition = TryTeleportEntity(
            registry, command.entity, frames,
            command.destinationFrame, command.mouth);
        result.ftl.push_back(transition);
        if (!transition.accepted)
        {
            for (const FtlRollbackState& state : ftlRollback)
                RestoreFtlState(registry, state);
            return result;
        }
        result.resetRenderHistory |= transition.resetRenderHistory;
        result.drainFixedAccumulator |= transition.drainFixedAccumulator;
    }
    result.completedStage = SimulationStepStage::Ftl;

    result.atmosphere.reserve(config.atmosphereBindings.size());
    std::vector<AtmosphereRollbackState> atmosphereRollback(
        config.atmosphereBindings.size());
    for (size_t i = 0; i < config.atmosphereBindings.size(); ++i)
        if (!CaptureAtmosphereState(
                registry, config.atmosphereBindings[i].entity,
                atmosphereRollback[i]))
            return result;
    for (const AtmosphereBinding& binding : config.atmosphereBindings)
    {
        const AtmosphereStepResult atmosphere = ApplyAtmosphereToEntity(
            registry, binding.entity, frames, binding.environment, dt);
        result.atmosphere.push_back(atmosphere);
        if (!atmosphere.accepted)
        {
            for (const AtmosphereRollbackState& state : atmosphereRollback)
                RestoreAtmosphereState(registry, state);
            return result;
        }
    }
    result.completedStage = SimulationStepStage::Atmosphere;

    result.passiveOrbit = StepPassiveOrbits(
        registry, frames, config.activeFrame,
        config.coordinateTime, dt, config.closeEncounters);
    if (!result.passiveOrbit.accepted)
        return result;

    for (const CollisionEvent& event : result.passiveOrbit.collisions.events)
    {
        if (!event.merged)
            continue;
        for (uint64_t absorbedId : event.absorbedIds)
            result.bodyIdRemaps.emplace_back(absorbedId, event.survivorId);
    }
    // One survivor may itself be absorbed by a later micro-step. Normalize all
    // mappings to the terminal body ID so persistent host bindings need only one
    // deterministic lookup.
    for (auto& remap : result.bodyIdRemaps)
    {
        for (size_t hop = 0; hop < result.bodyIdRemaps.size(); ++hop)
        {
            const auto next = std::find_if(
                result.bodyIdRemaps.begin(), result.bodyIdRemaps.end(),
                [&](const auto& candidate)
                { return candidate.first == remap.second; });
            if (next == result.bodyIdRemaps.end())
                break;
            remap.second = next->second;
        }
    }
    result.completedStage = SimulationStepStage::PassiveOrbit;

    // Patched conics: now that positions reflect this step (coordinateTime + dt),
    // reassign any on-rails body's primary if it crossed a sphere of influence.
    // Best-effort and non-gating — it never rejects the step. Off unless a scene
    // running a star system opts in, so scenarios with no celestial bodies pay
    // nothing and see no behaviour change.
    if (config.enableSoiTransitions)
        result.soiTransitions = StepSoiTransitions(
            registry, config.coordinateTime + dt, config.soiHysteresis);

    result.gravity = AccumulateForceIntegratedGravity(registry, frames);
    if (!result.gravity.accepted)
        return result;
    result.completedStage = SimulationStepStage::Gravity;

    result.flight = StepFlightPhysics(registry, dt, config.flightAssist);
    if (!result.flight.accepted)
        return result;
    result.completedStage = SimulationStepStage::Flight;

    // Passive collision reconciliation may have absorbed an entity that owned
    // a clock binding. Every binding was live during the preflight above, so a
    // dead entry here can only be a body intentionally destroyed by this step.
    // Do not turn that valid merge into a late partial-step rejection.
    std::vector<ClockGravityBinding> survivingClockBindings;
    survivingClockBindings.reserve(config.clockGravityBindings.size());
    for (const ClockGravityBinding& binding : config.clockGravityBindings)
    {
        if (registry.IsAlive(binding.entity))
        {
            ClockGravityBinding remapped = binding;
            const auto bodyRemap = std::find_if(
                result.bodyIdRemaps.begin(), result.bodyIdRemaps.end(),
                [&](const auto& candidate)
                { return candidate.first == remapped.primaryBodyId; });
            if (bodyRemap != result.bodyIdRemaps.end())
                remapped.primaryBodyId = bodyRemap->second;
            survivingClockBindings.push_back(remapped);
        }
    }

    result.clocks = StepRelativisticClocks(
        registry, frames, config.masterFrame, dt,
        survivingClockBindings);
    if (!result.clocks.accepted)
        return result;
    result.completedStage = SimulationStepStage::Clocks;

    result.accepted = true;
    result.completedStage = SimulationStepStage::Complete;
    return result;
}

} // namespace sim
