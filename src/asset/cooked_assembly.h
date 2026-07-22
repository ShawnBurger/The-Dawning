#pragma once

#include "cooked_model.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace asset
{

constexpr uint32_t kAssemblyOutsideIndex = (std::numeric_limits<uint32_t>::max)();
constexpr uint32_t kAssemblyNoIndex = (std::numeric_limits<uint32_t>::max)();

enum class AssemblyAssetKind : uint8_t
{
    Ship = 1,
    Structure = 2
};

enum class AssemblyModuleRole : uint8_t
{
    Exterior = 1,
    Interior = 2
};

enum class AssemblyCollisionType : uint8_t
{
    Convex = 1,
    Compound = 2,
    Mesh = 3
};

enum class AssemblySocketType : uint8_t
{
    Portal = 1,
    Interaction = 2,
    Attachment = 3,
    Spawn = 4
};

enum class AssemblyPressureClass : uint8_t
{
    Pressurized = 1,
    Unpressurized = 2,
    Airlock = 3
};

enum class AssemblyInteractionType : uint8_t
{
    Airlock = 1,
    Console = 2,
    Door = 3,
    Elevator = 4,
    Hatch = 5,
    Ladder = 6,
    Seat = 7
};

enum class AssemblyMotionType : uint8_t
{
    Linear = 1,
    Rotational = 2
};

enum class AssemblyLightType : uint8_t
{
    Point = 1,
    Spot = 2
};

enum class AssemblyLightShadowPolicy : uint8_t
{
    None = 1,
    Static = 2,
    Dynamic = 3
};

enum class AssemblyLightEmergencyBehavior : uint8_t
{
    Unchanged = 1,
    Off = 2,
    EmergencyOnly = 3,
    Override = 4
};

struct AssemblyProvenance
{
    std::string id;
    std::string provider;
    Sha256Digest requestSha256;
};

struct AssemblyTransform
{
    std::array<double, 3> positionMeters{};
    std::array<double, 3> rotationEulerDegrees{};
    std::array<double, 3> scale{ 1.0, 1.0, 1.0 };
};

struct AssemblyLod
{
    uint32_t level = 0;
    std::string source;
    double maxDistanceMeters = 0.0;
};

struct AssemblyModule
{
    std::string id;
    AssemblyModuleRole role = AssemblyModuleRole::Exterior;
    AssemblyCollisionType collisionType = AssemblyCollisionType::Convex;
    uint32_t provenanceIndex = kAssemblyNoIndex;
    std::string visualSource;
    AssemblyTransform transform;
    std::string collisionSource;
    std::vector<AssemblyLod> lods;
};

struct AssemblySocket
{
    std::string id;
    uint32_t moduleIndex = kAssemblyNoIndex;
    AssemblySocketType type = AssemblySocketType::Portal;
    std::array<double, 3> positionMeters{};
    std::array<double, 3> forward{};
    std::array<double, 3> up{};
};

struct AssemblyZone
{
    std::string id;
    uint32_t moduleIndex = kAssemblyNoIndex;
    AssemblyPressureClass pressure = AssemblyPressureClass::Unpressurized;
    std::string navmeshSource;
    std::string walkableSurface;
};

struct AssemblyPortal
{
    std::string id;
    uint32_t endpointA = kAssemblyOutsideIndex;
    uint32_t endpointB = kAssemblyOutsideIndex;
    uint32_t socketA = kAssemblyNoIndex;
    uint32_t socketB = kAssemblyNoIndex;
    uint32_t closureInteraction = kAssemblyNoIndex;
    bool sealable = false;
    bool navLink = false;
};

struct AssemblyInteraction
{
    std::string id;
    AssemblyInteractionType type = AssemblyInteractionType::Console;
    uint32_t moduleIndex = kAssemblyNoIndex;
    uint32_t socketIndex = kAssemblyNoIndex;
    std::vector<std::string> states;
    uint32_t initialStateIndex = kAssemblyNoIndex;
    uint32_t movingPartIndex = kAssemblyNoIndex;
    uint32_t portalIndex = kAssemblyNoIndex;
};

struct AssemblyMovingPart
{
    std::string id;
    uint32_t moduleIndex = kAssemblyNoIndex;
    uint32_t interactionIndex = kAssemblyNoIndex;
    std::string visualSource;
    AssemblyMotionType motionType = AssemblyMotionType::Linear;
    std::array<double, 3> pivotMeters{};
    std::array<double, 3> axis{};
    double travel = 0.0;
};

struct AssemblyLightFixture
{
    std::string id;
    uint32_t moduleIndex = kAssemblyNoIndex;
    AssemblyLightType type = AssemblyLightType::Point;
    AssemblyLightShadowPolicy shadowPolicy =
        AssemblyLightShadowPolicy::None;
    AssemblyLightEmergencyBehavior emergencyBehavior =
        AssemblyLightEmergencyBehavior::Unchanged;
    std::array<double, 3> positionMeters{};
    std::array<double, 3> direction{ 0.0, 0.0, 1.0 };
    double colorTemperatureKelvin = 6500.0;
    double intensityLumensOrCandela = 0.0;
    double rangeMeters = 0.0;
    double innerConeDegrees = 180.0;
    double outerConeDegrees = 180.0;
    double importance = 0.0;
    double emergencyColorTemperatureKelvin = 6500.0;
    double emergencyIntensityScale = 1.0;
    std::string groupId;
    std::string circuitId;
};

struct CookedAssembly
{
    uint32_t schemaVersion = 0;
    AssemblyAssetKind assetKind = AssemblyAssetKind::Ship;
    std::string assetId;
    std::string assemblyRevision;
    double minimumClearanceMeters = 0.0;
    double minimumDoorWidthMeters = 0.0;
    Sha256Digest sourceManifestSha256;
    std::vector<AssemblyProvenance> provenance;
    std::vector<AssemblyModule> modules;
    std::vector<AssemblySocket> sockets;
    std::vector<AssemblyZone> zones;
    std::vector<AssemblyPortal> portals;
    std::vector<AssemblyInteraction> interactions;
    std::vector<AssemblyMovingPart> movingParts;
    uint32_t entryZone = kAssemblyNoIndex;
    std::vector<uint32_t> requiredReachableZones;
    std::vector<AssemblyLightFixture> lightFixtures;
};

enum class CookedAssemblyStatus : uint8_t
{
    Success,
    FileNotFound,
    IoError,
    InvalidMagic,
    UnsupportedVersion,
    InvalidLayout,
    IntegrityMismatch,
    ResourceLimitExceeded,
    InvalidData
};

const char* CookedAssemblyStatusName(CookedAssemblyStatus status);

struct CookedAssemblyLimits
{
    uint64_t maxFileBytes = 64ull * 1024ull * 1024ull;
    uint64_t maxPayloadBytes = 64ull * 1024ull * 1024ull;
    uint32_t maxStringBytes = 1024u * 1024u;
    uint32_t maxProvenance = 100'000u;
    uint32_t maxModules = 100'000u;
    uint32_t maxSockets = 200'000u;
    uint32_t maxZones = 100'000u;
    uint32_t maxPortals = 200'000u;
    uint32_t maxInteractions = 200'000u;
    uint32_t maxMovingParts = 200'000u;
    uint32_t maxLightFixtures = 100'000u;
    uint32_t maxLodsPerModule = 32u;
    uint32_t maxStatesPerInteraction = 256u;
    uint64_t maxTotalRecords = 250'000ull;
    uint64_t maxTotalNestedItems = 1'000'000ull;
};

struct CookedAssemblyResult
{
    CookedAssemblyStatus status = CookedAssemblyStatus::InvalidData;
    std::shared_ptr<const CookedAssembly> assembly;
    std::string error;

    bool Succeeded() const
    {
        return status == CookedAssemblyStatus::Success && assembly != nullptr;
    }
};

CookedAssemblyResult LoadCookedAssemblyMemory(
    std::span<const std::byte> bytes,
    const CookedAssemblyLimits& limits = {});

CookedAssemblyResult LoadCookedAssemblyFile(
    const std::filesystem::path& path,
    const CookedAssemblyLimits& limits = {});

} // namespace asset
