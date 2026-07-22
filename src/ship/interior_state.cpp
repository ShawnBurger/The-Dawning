#include "interior_state.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <span>
#include <utility>

namespace ship
{
namespace
{

bool Finite(double value)
{
    return std::isfinite(value);
}

bool ValidId(std::string_view value)
{
    if (value.empty())
        return false;
    bool separator = true;
    for (const unsigned char character : value)
    {
        const bool alphanumeric =
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
        const bool currentSeparator =
            character == '.' || character == '_' || character == '-';
        if ((!alphanumeric && !currentSeparator) ||
            (separator && currentSeparator))
        {
            return false;
        }
        separator = currentSeparator;
    }
    return !separator;
}

bool KnownLightType(asset::AssemblyLightType type)
{
    return type == asset::AssemblyLightType::Point ||
           type == asset::AssemblyLightType::Spot;
}

bool KnownShadowPolicy(asset::AssemblyLightShadowPolicy policy)
{
    return policy == asset::AssemblyLightShadowPolicy::None ||
           policy == asset::AssemblyLightShadowPolicy::Static ||
           policy == asset::AssemblyLightShadowPolicy::Dynamic;
}

bool KnownEmergencyBehavior(asset::AssemblyLightEmergencyBehavior behavior)
{
    return behavior == asset::AssemblyLightEmergencyBehavior::Unchanged ||
           behavior == asset::AssemblyLightEmergencyBehavior::Off ||
           behavior == asset::AssemblyLightEmergencyBehavior::EmergencyOnly ||
           behavior == asset::AssemblyLightEmergencyBehavior::Override;
}

bool KnownAlert(InteriorAlertState alert)
{
    switch (alert)
    {
    case InteriorAlertState::Normal:
    case InteriorAlertState::Emergency:
    case InteriorAlertState::Blackout:
        return true;
    }
    return false;
}

bool ValidLimits(const InteriorStateLimits& limits)
{
    return limits.maxFixtures > 0 && limits.maxGroups > 0 &&
           limits.maxCircuits > 0 && limits.maxIdBytes > 0 &&
           Finite(limits.maximumIntensityScale) &&
           limits.maximumIntensityScale > 0.0;
}

InteriorStateResult Failure(
    InteriorStateStatus status,
    std::string error,
    uint32_t stableIndex = asset::kAssemblyNoIndex)
{
    InteriorStateResult result;
    result.status = status;
    result.failedStableIndex = stableIndex;
    result.error = std::move(error);
    return result;
}

template <typename State>
uint32_t FindState(std::span<const State> states, std::string_view id)
{
    const auto found = std::lower_bound(
        states.begin(),
        states.end(),
        id,
        [](const State& state, std::string_view value) {
            return state.id < value;
        });
    if (found == states.end() || found->id != id)
        return asset::kAssemblyNoIndex;
    return static_cast<uint32_t>(found - states.begin());
}

bool ValidFixtureTopology(
    const asset::CookedAssembly& assembly,
    const asset::AssemblyLightFixture& fixture,
    const InteriorStateLimits& limits)
{
    if (!ValidId(fixture.id) || fixture.id.size() > limits.maxIdBytes ||
        !ValidId(fixture.groupId) || fixture.groupId.size() > limits.maxIdBytes ||
        !ValidId(fixture.circuitId) ||
        fixture.circuitId.size() > limits.maxIdBytes ||
        !KnownLightType(fixture.type) ||
        !KnownShadowPolicy(fixture.shadowPolicy) ||
        !KnownEmergencyBehavior(fixture.emergencyBehavior) ||
        fixture.moduleIndex >= assembly.modules.size() ||
        assembly.modules[fixture.moduleIndex].role !=
            asset::AssemblyModuleRole::Interior)
    {
        return false;
    }
    const double directionLengthSquared =
        fixture.direction[0] * fixture.direction[0] +
        fixture.direction[1] * fixture.direction[1] +
        fixture.direction[2] * fixture.direction[2];
    const bool pointCones = fixture.innerConeDegrees == 180.0 &&
                            fixture.outerConeDegrees == 180.0;
    const bool spotCones = fixture.innerConeDegrees >= 0.0 &&
                           fixture.innerConeDegrees < fixture.outerConeDegrees &&
                           fixture.outerConeDegrees <= 89.9;
    return Finite(fixture.positionMeters[0]) &&
           Finite(fixture.positionMeters[1]) &&
           Finite(fixture.positionMeters[2]) &&
           Finite(fixture.direction[0]) && Finite(fixture.direction[1]) &&
           Finite(fixture.direction[2]) &&
           Finite(directionLengthSquared) &&
           std::abs(directionLengthSquared - 1.0) <= 2.0e-4 &&
           Finite(fixture.colorTemperatureKelvin) &&
           fixture.colorTemperatureKelvin >= 1000.0 &&
           fixture.colorTemperatureKelvin <= 20000.0 &&
           Finite(fixture.intensityLumensOrCandela) &&
           fixture.intensityLumensOrCandela > 0.0 &&
           fixture.intensityLumensOrCandela <= 1.0e9 &&
           Finite(fixture.rangeMeters) && fixture.rangeMeters > 0.0 &&
           fixture.rangeMeters <= 10000.0 &&
           Finite(fixture.innerConeDegrees) &&
           Finite(fixture.outerConeDegrees) &&
           (fixture.type == asset::AssemblyLightType::Point
                ? pointCones
                : spotCones) &&
           Finite(fixture.importance) && fixture.importance >= 0.0 &&
           fixture.importance <= 1.0 &&
           Finite(fixture.emergencyColorTemperatureKelvin) &&
           fixture.emergencyColorTemperatureKelvin >= 1000.0 &&
           fixture.emergencyColorTemperatureKelvin <= 20000.0 &&
           Finite(fixture.emergencyIntensityScale) &&
           fixture.emergencyIntensityScale >= 0.0 &&
           fixture.emergencyIntensityScale <= 8.0 &&
           ((fixture.emergencyBehavior !=
                 asset::AssemblyLightEmergencyBehavior::EmergencyOnly &&
             fixture.emergencyBehavior !=
                 asset::AssemblyLightEmergencyBehavior::Override) ||
            fixture.emergencyIntensityScale > 0.0);
}

InteriorStateResult ValidateUpdate(
    const std::shared_ptr<const ShipInteriorSnapshot>& snapshot,
    uint64_t simulationTick)
{
    if (!snapshot)
    {
        return Failure(
            InteriorStateStatus::NotInitialized,
            "interior state coordinator is not initialized");
    }
    if (simulationTick < snapshot->simulationTick)
    {
        return Failure(
            InteriorStateStatus::StaleUpdate,
            "interior state update is older than the published snapshot");
    }
    if (snapshot->stateRevision == (std::numeric_limits<uint64_t>::max)())
    {
        return Failure(
            InteriorStateStatus::InternalError,
            "interior state revision is exhausted");
    }
    return { InteriorStateStatus::Success };
}

} // namespace

const char* InteriorStateStatusName(InteriorStateStatus status)
{
    switch (status)
    {
    case InteriorStateStatus::Success: return "success";
    case InteriorStateStatus::NotInitialized: return "not_initialized";
    case InteriorStateStatus::InvalidArgument: return "invalid_argument";
    case InteriorStateStatus::InvalidTopology: return "invalid_topology";
    case InteriorStateStatus::ResourceLimitExceeded:
        return "resource_limit_exceeded";
    case InteriorStateStatus::NotFound: return "not_found";
    case InteriorStateStatus::StaleUpdate: return "stale_update";
    case InteriorStateStatus::AllocationFailure: return "allocation_failure";
    case InteriorStateStatus::InternalError: return "internal_error";
    }
    return "unknown";
}

InteriorStateResult InteriorStateCoordinator::Initialize(
    std::shared_ptr<const asset::CookedAssembly> assembly,
    const InteriorStateLimits& limits)
{
    if (!assembly || !ValidLimits(limits))
    {
        return Failure(
            InteriorStateStatus::InvalidArgument,
            "assembly or interior state limits are invalid");
    }
    if (assembly->schemaVersion < 1 || assembly->schemaVersion > 2 ||
        (assembly->schemaVersion == 1 && !assembly->lightFixtures.empty()) ||
        (assembly->schemaVersion == 2 && assembly->lightFixtures.empty()))
    {
        return Failure(
            InteriorStateStatus::InvalidTopology,
            "assembly schema and authored light fixture table disagree");
    }
    if (assembly->lightFixtures.size() > limits.maxFixtures)
    {
        return Failure(
            InteriorStateStatus::ResourceLimitExceeded,
            "authored light fixture count exceeds the runtime limit");
    }

    try
    {
        std::vector<std::string> groupIds;
        std::vector<std::string> circuitIds;
        groupIds.reserve(assembly->lightFixtures.size());
        circuitIds.reserve(assembly->lightFixtures.size());
        std::string_view previousId;
        for (size_t index = 0; index < assembly->lightFixtures.size(); ++index)
        {
            const asset::AssemblyLightFixture& fixture =
                assembly->lightFixtures[index];
            if (!ValidFixtureTopology(*assembly, fixture, limits) ||
                (!previousId.empty() && previousId >= fixture.id))
            {
                return Failure(
                    InteriorStateStatus::InvalidTopology,
                    "authored fixtures are malformed or not in stable ID order",
                    static_cast<uint32_t>(index));
            }
            previousId = fixture.id;
            groupIds.push_back(fixture.groupId);
            circuitIds.push_back(fixture.circuitId);
        }
        std::sort(groupIds.begin(), groupIds.end());
        groupIds.erase(std::unique(groupIds.begin(), groupIds.end()), groupIds.end());
        std::sort(circuitIds.begin(), circuitIds.end());
        circuitIds.erase(
            std::unique(circuitIds.begin(), circuitIds.end()), circuitIds.end());
        if (groupIds.size() > limits.maxGroups ||
            circuitIds.size() > limits.maxCircuits)
        {
            return Failure(
                InteriorStateStatus::ResourceLimitExceeded,
                "fixture groups or circuits exceed the runtime limit");
        }

        std::vector<InteriorLightGroupState> groups;
        groups.reserve(groupIds.size());
        for (std::string& id : groupIds)
            groups.push_back({ std::move(id), true, 1.0 });
        std::vector<InteriorCircuitState> circuits;
        circuits.reserve(circuitIds.size());
        for (std::string& id : circuitIds)
            circuits.push_back({ std::move(id), true, 1.0 });

        std::vector<FixtureTopology> topology;
        std::vector<InteriorFixtureState> fixtures;
        topology.reserve(assembly->lightFixtures.size());
        fixtures.reserve(assembly->lightFixtures.size());
        for (const asset::AssemblyLightFixture& fixture : assembly->lightFixtures)
        {
            FixtureTopology entry;
            entry.authored = fixture;
            entry.groupIndex = FindState<InteriorLightGroupState>(
                groups, fixture.groupId);
            entry.circuitIndex = FindState<InteriorCircuitState>(
                circuits, fixture.circuitId);
            if (entry.groupIndex == asset::kAssemblyNoIndex ||
                entry.circuitIndex == asset::kAssemblyNoIndex)
            {
                return Failure(
                    InteriorStateStatus::InternalError,
                    "fixture state indices could not be resolved");
            }
            topology.push_back(std::move(entry));
            fixtures.push_back({ fixture.id, true, 1.0 });
        }

        InteriorStateCoordinator staged;
        staged.m_assembly = std::move(assembly);
        staged.m_topology = std::move(topology);
        staged.m_groups = std::move(groups);
        staged.m_circuits = std::move(circuits);
        staged.m_fixtures = std::move(fixtures);
        staged.m_limits = limits;
        std::shared_ptr<const ShipInteriorSnapshot> snapshot;
        const InteriorStateResult built = staged.BuildSnapshot(
            InteriorAlertState::Normal,
            0,
            1,
            staged.m_groups,
            staged.m_circuits,
            staged.m_fixtures,
            snapshot);
        if (!built.Succeeded())
            return built;
        staged.m_snapshot = std::move(snapshot);

        *this = std::move(staged);
        return { InteriorStateStatus::Success, asset::kAssemblyNoIndex, true };
    }
    catch (const std::bad_alloc&)
    {
        return Failure(
            InteriorStateStatus::AllocationFailure,
            "allocation failure while initializing interior state");
    }
    catch (...)
    {
        return Failure(
            InteriorStateStatus::InternalError,
            "unexpected failure while initializing interior state");
    }
}

void InteriorStateCoordinator::Shutdown() noexcept
{
    m_snapshot.reset();
    m_fixtures.clear();
    m_circuits.clear();
    m_groups.clear();
    m_topology.clear();
    m_assembly.reset();
    m_limits = {};
}

InteriorStateResult InteriorStateCoordinator::BuildSnapshot(
    InteriorAlertState alert,
    uint64_t simulationTick,
    uint64_t stateRevision,
    const std::vector<InteriorLightGroupState>& groups,
    const std::vector<InteriorCircuitState>& circuits,
    const std::vector<InteriorFixtureState>& fixtures,
    std::shared_ptr<const ShipInteriorSnapshot>& output) const
{
    if (!m_assembly || !KnownAlert(alert) || groups.size() != m_groups.size() ||
        circuits.size() != m_circuits.size() ||
        fixtures.size() != m_topology.size())
    {
        return Failure(
            InteriorStateStatus::InvalidArgument,
            "interior snapshot inputs do not match immutable topology");
    }
    try
    {
        auto snapshot = std::make_shared<ShipInteriorSnapshot>();
        snapshot->topologySha256 = m_assembly->sourceManifestSha256;
        snapshot->topologyRevision = 1;
        snapshot->stateRevision = stateRevision;
        snapshot->simulationTick = simulationTick;
        snapshot->alert = alert;
        snapshot->lightGroups = groups;
        snapshot->circuits = circuits;
        snapshot->fixtures = fixtures;
        snapshot->resolvedLights.reserve(m_topology.size());

        for (size_t index = 0; index < m_topology.size(); ++index)
        {
            const FixtureTopology& topology = m_topology[index];
            if (topology.groupIndex >= groups.size() ||
                topology.circuitIndex >= circuits.size() ||
                fixtures[index].id != topology.authored.id)
            {
                return Failure(
                    InteriorStateStatus::InvalidTopology,
                    "interior snapshot state no longer matches fixture topology",
                    static_cast<uint32_t>(index));
            }
            const InteriorLightGroupState& group = groups[topology.groupIndex];
            const InteriorCircuitState& circuit = circuits[topology.circuitIndex];
            const InteriorFixtureState& fixture = fixtures[index];
            if (!Finite(group.intensityScale) || group.intensityScale < 0.0 ||
                group.intensityScale > m_limits.maximumIntensityScale ||
                !Finite(circuit.availablePowerScale) ||
                circuit.availablePowerScale < 0.0 ||
                circuit.availablePowerScale > m_limits.maximumIntensityScale ||
                !Finite(fixture.health) || fixture.health < 0.0 ||
                fixture.health > 1.0)
            {
                return Failure(
                    InteriorStateStatus::InvalidArgument,
                    "interior light control state is outside bounded ranges",
                    static_cast<uint32_t>(index));
            }

            bool active = fixture.enabled && fixture.health > 0.0 &&
                          group.enabled && group.intensityScale > 0.0 &&
                          circuit.energized &&
                          circuit.availablePowerScale > 0.0;
            double temperature = topology.authored.colorTemperatureKelvin;
            double emergencyScale = 1.0;
            if (alert == InteriorAlertState::Blackout)
            {
                active = false;
            }
            else if (alert == InteriorAlertState::Normal)
            {
                if (topology.authored.emergencyBehavior ==
                    asset::AssemblyLightEmergencyBehavior::EmergencyOnly)
                {
                    active = false;
                }
            }
            else
            {
                if (topology.authored.emergencyBehavior ==
                    asset::AssemblyLightEmergencyBehavior::Off)
                {
                    active = false;
                }
                else if (topology.authored.emergencyBehavior ==
                             asset::AssemblyLightEmergencyBehavior::EmergencyOnly ||
                         topology.authored.emergencyBehavior ==
                             asset::AssemblyLightEmergencyBehavior::Override)
                {
                    temperature =
                        topology.authored.emergencyColorTemperatureKelvin;
                    emergencyScale =
                        topology.authored.emergencyIntensityScale;
                }
            }

            ResolvedInteriorLight resolved;
            resolved.id = topology.authored.id;
            resolved.stableIndex = static_cast<uint32_t>(index);
            resolved.moduleIndex = topology.authored.moduleIndex;
            resolved.type = topology.authored.type;
            resolved.shadowPolicy = topology.authored.shadowPolicy;
            resolved.localPositionMeters = topology.authored.positionMeters;
            resolved.localDirection = topology.authored.direction;
            resolved.colorTemperatureKelvin = temperature;
            resolved.intensityLumensOrCandela = active
                ? topology.authored.intensityLumensOrCandela * fixture.health *
                    group.intensityScale * circuit.availablePowerScale *
                    emergencyScale
                : 0.0;
            resolved.rangeMeters = topology.authored.rangeMeters;
            resolved.innerConeDegrees = topology.authored.innerConeDegrees;
            resolved.outerConeDegrees = topology.authored.outerConeDegrees;
            resolved.importance = topology.authored.importance;
            if (!Finite(resolved.intensityLumensOrCandela))
            {
                return Failure(
                    InteriorStateStatus::InvalidArgument,
                    "resolved light intensity is not finite",
                    static_cast<uint32_t>(index));
            }
            snapshot->resolvedLights.push_back(std::move(resolved));
        }
        output = std::move(snapshot);
        return { InteriorStateStatus::Success };
    }
    catch (const std::bad_alloc&)
    {
        return Failure(
            InteriorStateStatus::AllocationFailure,
            "allocation failure while publishing interior snapshot");
    }
    catch (...)
    {
        return Failure(
            InteriorStateStatus::InternalError,
            "unexpected failure while publishing interior snapshot");
    }
}

InteriorStateResult InteriorStateCoordinator::SetAlertState(
    InteriorAlertState alert,
    uint64_t simulationTick)
{
    const InteriorStateResult valid = ValidateUpdate(m_snapshot, simulationTick);
    if (!valid.Succeeded())
        return valid;
    if (!KnownAlert(alert))
        return Failure(InteriorStateStatus::InvalidArgument, "unknown alert state");
    if (m_snapshot->alert == alert &&
        m_snapshot->simulationTick == simulationTick)
    {
        return { InteriorStateStatus::Success };
    }
    std::shared_ptr<const ShipInteriorSnapshot> snapshot;
    const InteriorStateResult built = BuildSnapshot(
        alert,
        simulationTick,
        m_snapshot->stateRevision + 1,
        m_groups,
        m_circuits,
        m_fixtures,
        snapshot);
    if (!built.Succeeded())
        return built;
    m_snapshot = std::move(snapshot);
    return { InteriorStateStatus::Success, asset::kAssemblyNoIndex, true };
}

InteriorStateResult InteriorStateCoordinator::SetLightGroupState(
    std::string_view id,
    bool enabled,
    double intensityScale,
    uint64_t simulationTick)
{
    const InteriorStateResult valid = ValidateUpdate(m_snapshot, simulationTick);
    if (!valid.Succeeded())
        return valid;
    if (!Finite(intensityScale) || intensityScale < 0.0 ||
        intensityScale > m_limits.maximumIntensityScale)
    {
        return Failure(
            InteriorStateStatus::InvalidArgument,
            "light group intensity scale is outside the configured range");
    }
    const uint32_t index = FindState<InteriorLightGroupState>(m_groups, id);
    if (index == asset::kAssemblyNoIndex)
        return Failure(InteriorStateStatus::NotFound, "light group was not found");
    if (m_groups[index].enabled == enabled &&
        m_groups[index].intensityScale == intensityScale &&
        m_snapshot->simulationTick == simulationTick)
    {
        return { InteriorStateStatus::Success };
    }
    try
    {
        std::vector<InteriorLightGroupState> groups = m_groups;
        groups[index].enabled = enabled;
        groups[index].intensityScale = intensityScale;
        std::shared_ptr<const ShipInteriorSnapshot> snapshot;
        const InteriorStateResult built = BuildSnapshot(
            m_snapshot->alert,
            simulationTick,
            m_snapshot->stateRevision + 1,
            groups,
            m_circuits,
            m_fixtures,
            snapshot);
        if (!built.Succeeded())
            return built;
        m_groups = std::move(groups);
        m_snapshot = std::move(snapshot);
        return {
            InteriorStateStatus::Success,
            asset::kAssemblyNoIndex,
            true,
        };
    }
    catch (const std::bad_alloc&)
    {
        return Failure(
            InteriorStateStatus::AllocationFailure,
            "allocation failure while updating a light group",
            index);
    }
}

InteriorStateResult InteriorStateCoordinator::SetCircuitState(
    std::string_view id,
    bool energized,
    double availablePowerScale,
    uint64_t simulationTick)
{
    const InteriorStateResult valid = ValidateUpdate(m_snapshot, simulationTick);
    if (!valid.Succeeded())
        return valid;
    if (!Finite(availablePowerScale) || availablePowerScale < 0.0 ||
        availablePowerScale > m_limits.maximumIntensityScale)
    {
        return Failure(
            InteriorStateStatus::InvalidArgument,
            "circuit power scale is outside the configured range");
    }
    const uint32_t index = FindState<InteriorCircuitState>(m_circuits, id);
    if (index == asset::kAssemblyNoIndex)
        return Failure(InteriorStateStatus::NotFound, "circuit was not found");
    if (m_circuits[index].energized == energized &&
        m_circuits[index].availablePowerScale == availablePowerScale &&
        m_snapshot->simulationTick == simulationTick)
    {
        return { InteriorStateStatus::Success };
    }
    try
    {
        std::vector<InteriorCircuitState> circuits = m_circuits;
        circuits[index].energized = energized;
        circuits[index].availablePowerScale = availablePowerScale;
        std::shared_ptr<const ShipInteriorSnapshot> snapshot;
        const InteriorStateResult built = BuildSnapshot(
            m_snapshot->alert,
            simulationTick,
            m_snapshot->stateRevision + 1,
            m_groups,
            circuits,
            m_fixtures,
            snapshot);
        if (!built.Succeeded())
            return built;
        m_circuits = std::move(circuits);
        m_snapshot = std::move(snapshot);
        return {
            InteriorStateStatus::Success,
            asset::kAssemblyNoIndex,
            true,
        };
    }
    catch (const std::bad_alloc&)
    {
        return Failure(
            InteriorStateStatus::AllocationFailure,
            "allocation failure while updating a circuit",
            index);
    }
}

InteriorStateResult InteriorStateCoordinator::SetFixtureState(
    std::string_view id,
    bool enabled,
    double health,
    uint64_t simulationTick)
{
    const InteriorStateResult valid = ValidateUpdate(m_snapshot, simulationTick);
    if (!valid.Succeeded())
        return valid;
    if (!Finite(health) || health < 0.0 || health > 1.0)
    {
        return Failure(
            InteriorStateStatus::InvalidArgument,
            "fixture health must be finite and in [0, 1]");
    }
    const uint32_t index = FindState<InteriorFixtureState>(m_fixtures, id);
    if (index == asset::kAssemblyNoIndex)
        return Failure(InteriorStateStatus::NotFound, "fixture was not found");
    if (m_fixtures[index].enabled == enabled &&
        m_fixtures[index].health == health &&
        m_snapshot->simulationTick == simulationTick)
    {
        return { InteriorStateStatus::Success };
    }
    try
    {
        std::vector<InteriorFixtureState> fixtures = m_fixtures;
        fixtures[index].enabled = enabled;
        fixtures[index].health = health;
        std::shared_ptr<const ShipInteriorSnapshot> snapshot;
        const InteriorStateResult built = BuildSnapshot(
            m_snapshot->alert,
            simulationTick,
            m_snapshot->stateRevision + 1,
            m_groups,
            m_circuits,
            fixtures,
            snapshot);
        if (!built.Succeeded())
            return built;
        m_fixtures = std::move(fixtures);
        m_snapshot = std::move(snapshot);
        return {
            InteriorStateStatus::Success,
            asset::kAssemblyNoIndex,
            true,
        };
    }
    catch (const std::bad_alloc&)
    {
        return Failure(
            InteriorStateStatus::AllocationFailure,
            "allocation failure while updating a fixture",
            index);
    }
}

InteriorStateResult InteriorStateCoordinator::AdvanceSimulationTick(
    uint64_t simulationTick)
{
    const InteriorStateResult valid = ValidateUpdate(m_snapshot, simulationTick);
    if (!valid.Succeeded())
        return valid;
    if (simulationTick == m_snapshot->simulationTick)
        return { InteriorStateStatus::Success };
    std::shared_ptr<const ShipInteriorSnapshot> snapshot;
    const InteriorStateResult built = BuildSnapshot(
        m_snapshot->alert,
        simulationTick,
        m_snapshot->stateRevision + 1,
        m_groups,
        m_circuits,
        m_fixtures,
        snapshot);
    if (!built.Succeeded())
        return built;
    m_snapshot = std::move(snapshot);
    return { InteriorStateStatus::Success, asset::kAssemblyNoIndex, true };
}

} // namespace ship
