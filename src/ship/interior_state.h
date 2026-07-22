#pragma once

#include "../asset/cooked_assembly.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ship
{

enum class InteriorAlertState : uint8_t
{
    Normal = 1,
    Emergency = 2,
    Blackout = 3
};

enum class InteriorStateStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidArgument,
    InvalidTopology,
    ResourceLimitExceeded,
    NotFound,
    StaleUpdate,
    AllocationFailure,
    InternalError
};

const char* InteriorStateStatusName(InteriorStateStatus status);

struct InteriorStateLimits
{
    uint32_t maxFixtures = 4096;
    uint32_t maxGroups = 1024;
    uint32_t maxCircuits = 1024;
    uint32_t maxIdBytes = 256;
    double maximumIntensityScale = 8.0;
};

struct InteriorLightGroupState
{
    std::string id;
    bool enabled = true;
    double intensityScale = 1.0;
};

struct InteriorCircuitState
{
    std::string id;
    bool energized = true;
    double availablePowerScale = 1.0;
};

struct InteriorFixtureState
{
    std::string id;
    bool enabled = true;
    double health = 1.0;
};

struct ResolvedInteriorLight
{
    std::string id;
    uint32_t stableIndex = asset::kAssemblyNoIndex;
    uint32_t moduleIndex = asset::kAssemblyNoIndex;
    asset::AssemblyLightType type = asset::AssemblyLightType::Point;
    asset::AssemblyLightShadowPolicy shadowPolicy =
        asset::AssemblyLightShadowPolicy::None;
    std::array<double, 3> localPositionMeters{};
    std::array<double, 3> localDirection{ 0.0, 0.0, 1.0 };
    double colorTemperatureKelvin = 6500.0;
    double intensityLumensOrCandela = 0.0;
    double rangeMeters = 0.0;
    double innerConeDegrees = 180.0;
    double outerConeDegrees = 180.0;
    double importance = 0.0;
};

struct ShipInteriorSnapshot
{
    asset::Sha256Digest topologySha256;
    uint64_t topologyRevision = 1;
    uint64_t stateRevision = 0;
    uint64_t simulationTick = 0;
    InteriorAlertState alert = InteriorAlertState::Normal;
    std::vector<InteriorLightGroupState> lightGroups;
    std::vector<InteriorCircuitState> circuits;
    std::vector<InteriorFixtureState> fixtures;
    std::vector<ResolvedInteriorLight> resolvedLights;
};

struct InteriorStateResult
{
    InteriorStateStatus status = InteriorStateStatus::InvalidArgument;
    uint32_t failedStableIndex = asset::kAssemblyNoIndex;
    bool changed = false;
    std::string error;

    bool Succeeded() const
    {
        return status == InteriorStateStatus::Success;
    }
};

class InteriorStateCoordinator
{
public:
    InteriorStateResult Initialize(
        std::shared_ptr<const asset::CookedAssembly> assembly,
        const InteriorStateLimits& limits = {});
    void Shutdown() noexcept;

    bool IsInitialized() const { return m_snapshot != nullptr; }
    std::shared_ptr<const ShipInteriorSnapshot> Snapshot() const
    {
        return m_snapshot;
    }

    InteriorStateResult SetAlertState(
        InteriorAlertState alert,
        uint64_t simulationTick);
    InteriorStateResult SetLightGroupState(
        std::string_view id,
        bool enabled,
        double intensityScale,
        uint64_t simulationTick);
    InteriorStateResult SetCircuitState(
        std::string_view id,
        bool energized,
        double availablePowerScale,
        uint64_t simulationTick);
    InteriorStateResult SetFixtureState(
        std::string_view id,
        bool enabled,
        double health,
        uint64_t simulationTick);
    InteriorStateResult AdvanceSimulationTick(uint64_t simulationTick);

private:
    struct FixtureTopology
    {
        asset::AssemblyLightFixture authored;
        uint32_t groupIndex = asset::kAssemblyNoIndex;
        uint32_t circuitIndex = asset::kAssemblyNoIndex;
    };

    InteriorStateResult BuildSnapshot(
        InteriorAlertState alert,
        uint64_t simulationTick,
        uint64_t stateRevision,
        const std::vector<InteriorLightGroupState>& groups,
        const std::vector<InteriorCircuitState>& circuits,
        const std::vector<InteriorFixtureState>& fixtures,
        std::shared_ptr<const ShipInteriorSnapshot>& output) const;

    std::shared_ptr<const asset::CookedAssembly> m_assembly;
    std::vector<FixtureTopology> m_topology;
    std::vector<InteriorLightGroupState> m_groups;
    std::vector<InteriorCircuitState> m_circuits;
    std::vector<InteriorFixtureState> m_fixtures;
    std::shared_ptr<const ShipInteriorSnapshot> m_snapshot;
    InteriorStateLimits m_limits;
};

} // namespace ship
