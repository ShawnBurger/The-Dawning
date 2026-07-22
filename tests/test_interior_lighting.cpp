#include "test_framework.h"

#include "ship/interior_lighting.h"

#include <limits>
#include <memory>

namespace
{

std::shared_ptr<const asset::CookedAssembly> MakeLightingAssembly()
{
    auto assembly = std::make_shared<asset::CookedAssembly>();
    assembly->schemaVersion = 2;
    assembly->sourceManifestSha256 = asset::ComputeSha256({});
    asset::AssemblyModule module;
    module.id = "cockpit";
    module.role = asset::AssemblyModuleRole::Interior;
    assembly->modules.push_back(module);

    asset::AssemblyLightFixture cool;
    cool.id = "cool_point";
    cool.moduleIndex = 0;
    cool.type = asset::AssemblyLightType::Point;
    cool.shadowPolicy = asset::AssemblyLightShadowPolicy::None;
    cool.emergencyBehavior =
        asset::AssemblyLightEmergencyBehavior::Unchanged;
    cool.positionMeters = { 0.0, 1.0, 0.0 };
    cool.direction = { 0.0, 0.0, 1.0 };
    cool.colorTemperatureKelvin = 10000.0;
    cool.intensityLumensOrCandela = 800.0;
    cool.rangeMeters = 6.0;
    cool.innerConeDegrees = 180.0;
    cool.outerConeDegrees = 180.0;
    cool.importance = 0.25;
    cool.emergencyColorTemperatureKelvin = 1800.0;
    cool.emergencyIntensityScale = 1.0;
    cool.groupId = "primary";
    cool.circuitId = "main";
    assembly->lightFixtures.push_back(cool);

    asset::AssemblyLightFixture warm = cool;
    warm.id = "warm_spot";
    warm.type = asset::AssemblyLightType::Spot;
    warm.shadowPolicy = asset::AssemblyLightShadowPolicy::Dynamic;
    warm.positionMeters = { 2.0, 3.0, 4.0 };
    warm.direction = { 0.0, 0.0, 1.0 };
    warm.colorTemperatureKelvin = 2200.0;
    warm.intensityLumensOrCandela = 1200.0;
    warm.innerConeDegrees = 20.0;
    warm.outerConeDegrees = 45.0;
    warm.importance = 1.0;
    assembly->lightFixtures.push_back(warm);
    return assembly;
}

} // namespace

TEST_CASE(InteriorLighting_PreservesLocalDetailAtAstronomicalWorldCoordinates)
{
    ship::InteriorStateCoordinator coordinator;
    CHECK(coordinator.Initialize(MakeLightingAssembly()).Succeeded());

    ecs::Transform module;
    module.position = { 1.0e15 + 16.0, -2.0e15 + 8.0, 3.0e15 - 32.0 };
    module.rotation = core::Quatf::FromAxisAngle(
        { 0.0f, 1.0f, 0.0f }, 1.57079632679f);
    const core::Vec3d camera{ 1.0e15, -2.0e15, 3.0e15 };
    const auto snapshot = coordinator.Snapshot();
    const ship::InteriorModulePoseView poses{
        snapshot->topologySha256,
        snapshot->topologyRevision,
        std::span<const ecs::Transform>(&module, 1),
    };
    ship::InteriorLightFrame frame;
    const ship::InteriorLightingResult result = ship::BuildInteriorLightFrame(
        *snapshot,
        poses,
        camera,
        frame);
    CHECK(result.Succeeded());
    CHECK_EQ(frame.lights.size(), 2u);
    CHECK_EQ(frame.stateRevision, coordinator.Snapshot()->stateRevision);

    const ship::InteriorGpuLight& point = frame.lights[0];
    CHECK_APPROX_EPS(point.positionCameraRelative.x, 16.0f, 1.0e-4f);
    CHECK_APPROX_EPS(point.positionCameraRelative.y, 9.0f, 1.0e-4f);
    CHECK_APPROX_EPS(point.positionCameraRelative.z, -32.0f, 1.0e-4f);
    CHECK(point.colorLinear.z > point.colorLinear.x);
    CHECK_EQ(point.type, asset::AssemblyLightType::Point);
    CHECK_APPROX_EPS(point.innerConeCosine, -1.0f, 1.0e-6f);

    const ship::InteriorGpuLight& spot = frame.lights[1];
    CHECK_APPROX_EPS(spot.positionCameraRelative.x, 20.0f, 1.0e-4f);
    CHECK_APPROX_EPS(spot.positionCameraRelative.y, 11.0f, 1.0e-4f);
    CHECK_APPROX_EPS(spot.positionCameraRelative.z, -34.0f, 1.0e-4f);
    CHECK_APPROX_EPS(spot.direction.x, 1.0f, 1.0e-5f);
    CHECK_APPROX_EPS(spot.direction.z, 0.0f, 1.0e-5f);
    CHECK(spot.colorLinear.x > spot.colorLinear.z);
    CHECK(spot.innerConeCosine > spot.outerConeCosine);
    CHECK_EQ(spot.shadowPolicy, asset::AssemblyLightShadowPolicy::Dynamic);
}

TEST_CASE(InteriorLighting_BlackoutPublishesAnEmptyButRevisionedFrame)
{
    ship::InteriorStateCoordinator coordinator;
    CHECK(coordinator.Initialize(MakeLightingAssembly()).Succeeded());
    CHECK(coordinator.SetAlertState(
        ship::InteriorAlertState::Blackout, 4).Succeeded());
    ecs::Transform module;
    const auto snapshot = coordinator.Snapshot();
    const ship::InteriorModulePoseView poses{
        snapshot->topologySha256,
        snapshot->topologyRevision,
        std::span<const ecs::Transform>(&module, 1),
    };
    ship::InteriorLightFrame frame;
    CHECK(ship::BuildInteriorLightFrame(
        *snapshot,
        poses,
        {},
        frame).Succeeded());
    CHECK(frame.lights.empty());
    CHECK_EQ(frame.simulationTick, 4ull);
    CHECK_EQ(frame.stateRevision, coordinator.Snapshot()->stateRevision);
}

TEST_CASE(InteriorLighting_FailuresDoNotPartiallyReplacePublishedOutput)
{
    ship::InteriorStateCoordinator coordinator;
    CHECK(coordinator.Initialize(MakeLightingAssembly()).Succeeded());
    ecs::Transform module;
    const auto snapshot = coordinator.Snapshot();
    ship::InteriorModulePoseView poses{
        snapshot->topologySha256,
        snapshot->topologyRevision,
        std::span<const ecs::Transform>(&module, 1),
    };
    ship::InteriorLightFrame output;
    output.stateRevision = 777;
    output.lights.resize(1);
    output.lights[0].stableIndex = 99;

    core::Vec3d invalidCamera;
    invalidCamera.x = (std::numeric_limits<double>::quiet_NaN)();
    CHECK_EQ(
        ship::BuildInteriorLightFrame(
            *snapshot,
            poses,
            invalidCamera,
            output).status,
        ship::InteriorLightingStatus::InvalidArgument);
    CHECK_EQ(output.stateRevision, 777ull);
    CHECK_EQ(output.lights[0].stableIndex, 99u);

    module.scale = { 1.0f, 2.0f, 1.0f };
    CHECK_EQ(
        ship::BuildInteriorLightFrame(
            *snapshot,
            poses,
            {},
            output).status,
        ship::InteriorLightingStatus::InvalidTransform);
    CHECK_EQ(output.stateRevision, 777ull);

    module = {};
    ship::InteriorLightFrameConfig limits;
    limits.maxLights = 1;
    CHECK_EQ(
        ship::BuildInteriorLightFrame(
            *snapshot,
            poses,
            {},
            output,
            limits).status,
        ship::InteriorLightingStatus::ResourceLimitExceeded);
    CHECK_EQ(output.stateRevision, 777ull);

    limits = {};
    limits.maximumCameraRelativeMagnitude =
        (std::numeric_limits<double>::max)();
    CHECK_EQ(
        ship::BuildInteriorLightFrame(
            *snapshot,
            poses,
            {},
            output,
            limits).status,
        ship::InteriorLightingStatus::InvalidArgument);
    CHECK_EQ(output.stateRevision, 777ull);
}

TEST_CASE(InteriorLighting_RejectsForgedSnapshotTopology)
{
    ship::InteriorStateCoordinator coordinator;
    CHECK(coordinator.Initialize(MakeLightingAssembly()).Succeeded());
    ship::ShipInteriorSnapshot forged = *coordinator.Snapshot();
    forged.resolvedLights[0].stableIndex = 1;
    ecs::Transform module;
    const ship::InteriorModulePoseView poses{
        coordinator.Snapshot()->topologySha256,
        coordinator.Snapshot()->topologyRevision,
        std::span<const ecs::Transform>(&module, 1),
    };
    ship::InteriorLightFrame output;
    CHECK_EQ(
        ship::BuildInteriorLightFrame(
            forged,
            poses,
            {},
            output).status,
        ship::InteriorLightingStatus::InvalidSnapshot);
    CHECK(output.lights.empty());

    forged = *coordinator.Snapshot();
    forged.fixtures[0].id = "other_fixture";
    CHECK_EQ(
        ship::BuildInteriorLightFrame(
            forged,
            poses,
            {},
            output).status,
        ship::InteriorLightingStatus::InvalidSnapshot);
    CHECK(output.lights.empty());

    forged = *coordinator.Snapshot();
    forged.fixtures[0].id = "Bad Fixture";
    forged.resolvedLights[0].id = "Bad Fixture";
    CHECK_EQ(
        ship::BuildInteriorLightFrame(
            forged,
            poses,
            {},
            output).status,
        ship::InteriorLightingStatus::InvalidSnapshot);
    CHECK(output.lights.empty());

    forged = *coordinator.Snapshot();
    ship::InteriorModulePoseView wrongPoses = poses;
    wrongPoses.topologySha256.bytes[0] ^= 0xff;
    CHECK_EQ(
        ship::BuildInteriorLightFrame(
            forged,
            wrongPoses,
            {},
            output).status,
        ship::InteriorLightingStatus::InvalidSnapshot);
    CHECK(output.lights.empty());
}

TEST_CASE(InteriorLighting_DefaultCeilingAcceptsMaximumDefaultStateScaling)
{
    auto assembly = std::const_pointer_cast<asset::CookedAssembly>(
        MakeLightingAssembly());
    asset::AssemblyLightFixture& light = assembly->lightFixtures[0];
    light.emergencyBehavior =
        asset::AssemblyLightEmergencyBehavior::EmergencyOnly;
    light.intensityLumensOrCandela = 1.0e9;
    light.emergencyIntensityScale = 8.0;

    ship::InteriorStateCoordinator coordinator;
    CHECK(coordinator.Initialize(assembly).Succeeded());
    CHECK(coordinator.SetLightGroupState(
        "primary", true, 8.0, 1).Succeeded());
    CHECK(coordinator.SetCircuitState("main", true, 8.0, 2).Succeeded());
    CHECK(coordinator.SetAlertState(
        ship::InteriorAlertState::Emergency, 3).Succeeded());

    ecs::Transform module;
    const auto snapshot = coordinator.Snapshot();
    const ship::InteriorModulePoseView poses{
        snapshot->topologySha256,
        snapshot->topologyRevision,
        std::span<const ecs::Transform>(&module, 1),
    };
    ship::InteriorLightFrame frame;
    CHECK(ship::BuildInteriorLightFrame(
        *snapshot,
        poses,
        {},
        frame).Succeeded());
    CHECK_EQ(frame.lights.size(), 2u);
    CHECK_APPROX_EPS(
        frame.lights[0].intensityLumensOrCandela,
        5.12e11f,
        1.0e5f);
}
