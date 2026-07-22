#include "test_framework.h"

#include "ship/interior_state.h"

#include <limits>
#include <memory>
#include <utility>

namespace
{

asset::AssemblyLightFixture Fixture(
    std::string id,
    std::string group,
    std::string circuit,
    asset::AssemblyLightEmergencyBehavior emergency,
    double intensity)
{
    asset::AssemblyLightFixture fixture;
    fixture.id = std::move(id);
    fixture.moduleIndex = 0;
    fixture.type = asset::AssemblyLightType::Spot;
    fixture.shadowPolicy = asset::AssemblyLightShadowPolicy::None;
    fixture.emergencyBehavior = emergency;
    fixture.positionMeters = { 0.0, 1.8, 0.0 };
    fixture.direction = { 0.0, -1.0, 0.0 };
    fixture.colorTemperatureKelvin = 4300.0;
    fixture.intensityLumensOrCandela = intensity;
    fixture.rangeMeters = 8.0;
    fixture.innerConeDegrees = 25.0;
    fixture.outerConeDegrees = 45.0;
    fixture.importance = 0.75;
    fixture.emergencyColorTemperatureKelvin = 1800.0;
    fixture.emergencyIntensityScale = 2.0;
    fixture.groupId = std::move(group);
    fixture.circuitId = std::move(circuit);
    return fixture;
}

std::shared_ptr<const asset::CookedAssembly> MakeAssembly()
{
    auto assembly = std::make_shared<asset::CookedAssembly>();
    assembly->schemaVersion = 2;
    assembly->sourceManifestSha256 = asset::ComputeSha256({});
    asset::AssemblyModule module;
    module.id = "cockpit";
    module.role = asset::AssemblyModuleRole::Interior;
    assembly->modules.push_back(std::move(module));
    assembly->lightFixtures.push_back(Fixture(
        "cockpit_emergency",
        "emergency",
        "battery",
        asset::AssemblyLightEmergencyBehavior::EmergencyOnly,
        100.0));
    assembly->lightFixtures.push_back(Fixture(
        "cockpit_key",
        "primary",
        "main",
        asset::AssemblyLightEmergencyBehavior::Unchanged,
        1000.0));
    assembly->lightFixtures.push_back(Fixture(
        "cockpit_task",
        "primary",
        "main",
        asset::AssemblyLightEmergencyBehavior::Off,
        500.0));
    return assembly;
}

} // namespace

TEST_CASE(InteriorState_ResolvesNormalEmergencyAndBlackoutFromOneSnapshot)
{
    ship::InteriorStateCoordinator coordinator;
    CHECK(coordinator.Initialize(MakeAssembly()).Succeeded());
    const auto initial = coordinator.Snapshot();
    CHECK(initial != nullptr);
    CHECK_EQ(initial->stateRevision, 1ull);
    CHECK_EQ(initial->simulationTick, 0ull);
    CHECK_EQ(initial->resolvedLights.size(), 3u);
    CHECK_APPROX_EPS(initial->resolvedLights[0].intensityLumensOrCandela, 0.0, 1.0e-9);
    CHECK_APPROX_EPS(initial->resolvedLights[1].intensityLumensOrCandela, 1000.0, 1.0e-9);
    CHECK_APPROX_EPS(initial->resolvedLights[2].intensityLumensOrCandela, 500.0, 1.0e-9);

    CHECK(coordinator.SetAlertState(
        ship::InteriorAlertState::Emergency, 10).Succeeded());
    const auto emergency = coordinator.Snapshot();
    CHECK_EQ(emergency->alert, ship::InteriorAlertState::Emergency);
    CHECK_EQ(emergency->simulationTick, 10ull);
    CHECK_APPROX_EPS(emergency->resolvedLights[0].intensityLumensOrCandela, 200.0, 1.0e-9);
    CHECK_APPROX_EPS(emergency->resolvedLights[0].colorTemperatureKelvin, 1800.0, 1.0e-9);
    CHECK_APPROX_EPS(emergency->resolvedLights[1].intensityLumensOrCandela, 1000.0, 1.0e-9);
    CHECK_APPROX_EPS(emergency->resolvedLights[2].intensityLumensOrCandela, 0.0, 1.0e-9);

    CHECK(coordinator.SetLightGroupState("primary", true, 0.5, 11).Succeeded());
    CHECK_APPROX_EPS(
        coordinator.Snapshot()->resolvedLights[1].intensityLumensOrCandela,
        500.0,
        1.0e-9);
    CHECK(coordinator.SetCircuitState("main", false, 1.0, 12).Succeeded());
    CHECK_APPROX_EPS(
        coordinator.Snapshot()->resolvedLights[1].intensityLumensOrCandela,
        0.0,
        1.0e-9);
    CHECK(coordinator.SetFixtureState(
        "cockpit_emergency", true, 0.25, 13).Succeeded());
    CHECK_APPROX_EPS(
        coordinator.Snapshot()->resolvedLights[0].intensityLumensOrCandela,
        50.0,
        1.0e-9);

    CHECK(coordinator.SetAlertState(
        ship::InteriorAlertState::Blackout, 14).Succeeded());
    for (const ship::ResolvedInteriorLight& light :
         coordinator.Snapshot()->resolvedLights)
    {
        CHECK_APPROX_EPS(light.intensityLumensOrCandela, 0.0, 1.0e-9);
    }
}

TEST_CASE(InteriorState_UpdatesAreIdempotentMonotonicAndTransactional)
{
    ship::InteriorStateCoordinator coordinator;
    CHECK(coordinator.Initialize(MakeAssembly()).Succeeded());
    const ship::InteriorStateResult updated =
        coordinator.SetLightGroupState("primary", true, 0.5, 7);
    CHECK(updated.Succeeded());
    CHECK_EQ(updated.failedStableIndex, asset::kAssemblyNoIndex);
    const auto published = coordinator.Snapshot();

    const ship::InteriorStateResult idempotent =
        coordinator.SetLightGroupState("primary", true, 0.5, 7);
    CHECK(idempotent.Succeeded());
    CHECK(!idempotent.changed);
    CHECK(coordinator.Snapshot() == published);

    const ship::InteriorStateResult stale =
        coordinator.SetCircuitState("main", false, 1.0, 6);
    CHECK_EQ(stale.status, ship::InteriorStateStatus::StaleUpdate);
    CHECK(coordinator.Snapshot() == published);

    const ship::InteriorStateResult missing =
        coordinator.SetCircuitState("missing", false, 1.0, 8);
    CHECK_EQ(missing.status, ship::InteriorStateStatus::NotFound);
    CHECK(coordinator.Snapshot() == published);

    const ship::InteriorStateResult invalid = coordinator.SetFixtureState(
        "cockpit_key",
        true,
        (std::numeric_limits<double>::quiet_NaN)(),
        8);
    CHECK_EQ(invalid.status, ship::InteriorStateStatus::InvalidArgument);
    CHECK(coordinator.Snapshot() == published);

    const ship::InteriorStateResult advanced =
        coordinator.AdvanceSimulationTick(8);
    CHECK(advanced.Succeeded());
    CHECK(advanced.changed);
    CHECK_EQ(coordinator.Snapshot()->simulationTick, 8ull);
    CHECK_EQ(coordinator.Snapshot()->stateRevision, published->stateRevision + 1);
}

TEST_CASE(InteriorState_EmergencyOverrideChangesOnlyTheEmergencySnapshot)
{
    auto assembly = std::const_pointer_cast<asset::CookedAssembly>(MakeAssembly());
    assembly->lightFixtures[1].emergencyBehavior =
        asset::AssemblyLightEmergencyBehavior::Override;
    ship::InteriorStateCoordinator coordinator;
    CHECK(coordinator.Initialize(assembly).Succeeded());
    CHECK_APPROX_EPS(
        coordinator.Snapshot()->resolvedLights[1].intensityLumensOrCandela,
        1000.0,
        1.0e-9);
    CHECK_APPROX_EPS(
        coordinator.Snapshot()->resolvedLights[1].colorTemperatureKelvin,
        4300.0,
        1.0e-9);

    CHECK(coordinator.SetAlertState(
        ship::InteriorAlertState::Emergency, 1).Succeeded());
    CHECK_APPROX_EPS(
        coordinator.Snapshot()->resolvedLights[1].intensityLumensOrCandela,
        2000.0,
        1.0e-9);
    CHECK_APPROX_EPS(
        coordinator.Snapshot()->resolvedLights[1].colorTemperatureKelvin,
        1800.0,
        1.0e-9);
}

TEST_CASE(InteriorState_RejectsUnstableTopologyAndHardLimitOverflow)
{
    auto unsorted = std::const_pointer_cast<asset::CookedAssembly>(MakeAssembly());
    std::swap(unsorted->lightFixtures[0], unsorted->lightFixtures[1]);
    ship::InteriorStateCoordinator coordinator;
    CHECK_EQ(
        coordinator.Initialize(unsorted).status,
        ship::InteriorStateStatus::InvalidTopology);
    CHECK(!coordinator.IsInitialized());

    ship::InteriorStateLimits limits;
    limits.maxFixtures = 2;
    CHECK_EQ(
        coordinator.Initialize(MakeAssembly(), limits).status,
        ship::InteriorStateStatus::ResourceLimitExceeded);
    CHECK(!coordinator.IsInitialized());

    limits = {};
    limits.maxGroups = 1;
    CHECK_EQ(
        coordinator.Initialize(MakeAssembly(), limits).status,
        ship::InteriorStateStatus::ResourceLimitExceeded);
    CHECK(!coordinator.IsInitialized());

    limits = {};
    limits.maxCircuits = 1;
    CHECK_EQ(
        coordinator.Initialize(MakeAssembly(), limits).status,
        ship::InteriorStateStatus::ResourceLimitExceeded);
    CHECK(!coordinator.IsInitialized());

    limits = {};
    limits.maxIdBytes = 4;
    CHECK_EQ(
        coordinator.Initialize(MakeAssembly(), limits).status,
        ship::InteriorStateStatus::InvalidTopology);
    CHECK(!coordinator.IsInitialized());

    auto legacy = std::make_shared<asset::CookedAssembly>();
    legacy->schemaVersion = 1;
    CHECK(coordinator.Initialize(legacy).Succeeded());
    CHECK(coordinator.Snapshot()->resolvedLights.empty());

    auto emptyV2 = std::make_shared<asset::CookedAssembly>();
    emptyV2->schemaVersion = 2;
    CHECK_EQ(
        coordinator.Initialize(emptyV2).status,
        ship::InteriorStateStatus::InvalidTopology);

    auto litV1 = std::const_pointer_cast<asset::CookedAssembly>(MakeAssembly());
    litV1->schemaVersion = 1;
    CHECK_EQ(
        coordinator.Initialize(litV1).status,
        ship::InteriorStateStatus::InvalidTopology);
}

TEST_CASE(InteriorState_RejectsBypassedCookerFixtureCorruptionTransactionally)
{
    ship::InteriorStateCoordinator coordinator;
    CHECK(coordinator.Initialize(MakeAssembly()).Succeeded());
    const auto published = coordinator.Snapshot();

    auto badEnum = std::const_pointer_cast<asset::CookedAssembly>(MakeAssembly());
    badEnum->lightFixtures[0].type =
        static_cast<asset::AssemblyLightType>(255);
    CHECK_EQ(
        coordinator.Initialize(badEnum).status,
        ship::InteriorStateStatus::InvalidTopology);
    CHECK(coordinator.Snapshot() == published);

    auto badCone = std::const_pointer_cast<asset::CookedAssembly>(MakeAssembly());
    badCone->lightFixtures[0].innerConeDegrees = 50.0;
    badCone->lightFixtures[0].outerConeDegrees = 20.0;
    CHECK_EQ(
        coordinator.Initialize(badCone).status,
        ship::InteriorStateStatus::InvalidTopology);
    CHECK(coordinator.Snapshot() == published);

    auto badId = std::const_pointer_cast<asset::CookedAssembly>(MakeAssembly());
    badId->lightFixtures[0].groupId = "Bad Group";
    CHECK_EQ(
        coordinator.Initialize(badId).status,
        ship::InteriorStateStatus::InvalidTopology);
    CHECK(coordinator.Snapshot() == published);

    auto badScale = std::const_pointer_cast<asset::CookedAssembly>(MakeAssembly());
    badScale->lightFixtures[0].emergencyIntensityScale = 9.0;
    CHECK_EQ(
        coordinator.Initialize(badScale).status,
        ship::InteriorStateStatus::InvalidTopology);
    CHECK(coordinator.Snapshot() == published);
}
