#pragma once

#include "cooked_model.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace asset
{

enum class CollisionSurfaceFlags : uint8_t
{
    None = 0,
    Walkable = 1u << 0
};

constexpr bool HasCollisionSurfaceFlag(
    CollisionSurfaceFlags flags,
    CollisionSurfaceFlags flag)
{
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
}

struct CookedCollisionBox
{
    std::string id;
    std::array<double, 3> centerMeters{};
    std::array<double, 3> halfExtentsMeters{};
    CollisionSurfaceFlags surfaceFlags = CollisionSurfaceFlags::None;
};

struct CookedCollision
{
    uint32_t schemaVersion = 0;
    std::string collisionId;
    Sha256Digest sourceSha256;
    Sha256Digest payloadSha256;
    std::vector<CookedCollisionBox> boxes;
};

enum class CookedCollisionStatus : uint8_t
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

const char* CookedCollisionStatusName(CookedCollisionStatus status);

struct CookedCollisionLimits
{
    uint64_t maxFileBytes = 64ull * 1024ull * 1024ull;
    uint64_t maxPayloadBytes = 64ull * 1024ull * 1024ull;
    uint32_t maxStringBytes = 1024u * 1024u;
    uint32_t maxBoxes = 1'000'000u;
};

struct CookedCollisionResult
{
    CookedCollisionStatus status = CookedCollisionStatus::InvalidData;
    std::shared_ptr<const CookedCollision> collision;
    std::string error;

    bool Succeeded() const
    {
        return status == CookedCollisionStatus::Success && collision != nullptr;
    }
};

CookedCollisionResult LoadCookedCollisionMemory(
    std::span<const std::byte> bytes,
    const CookedCollisionLimits& limits = {});

CookedCollisionResult LoadCookedCollisionFile(
    const std::filesystem::path& path,
    const CookedCollisionLimits& limits = {});

} // namespace asset
