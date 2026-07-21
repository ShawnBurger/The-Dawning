#include "test_framework.h"
#include "asset/cooked_collision.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace
{

class Writer
{
public:
    void U8(uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
    void U16(uint16_t value) { U8(static_cast<uint8_t>(value)); U8(static_cast<uint8_t>(value >> 8)); }
    void U32(uint32_t value) { for (uint32_t s = 0; s < 32; s += 8) U8(static_cast<uint8_t>(value >> s)); }
    void U64(uint64_t value) { for (uint32_t s = 0; s < 64; s += 8) U8(static_cast<uint8_t>(value >> s)); }
    void Double(double value) { U64(std::bit_cast<uint64_t>(value)); }
    void Bytes(std::span<const uint8_t> value) { for (uint8_t v : value) U8(v); }
    void Bytes(std::string_view value) { for (char v : value) U8(static_cast<uint8_t>(v)); }
    void String(std::string_view value) { U32(static_cast<uint32_t>(value.size())); Bytes(value); }
    void Vec3(double x, double y, double z) { Double(x); Double(y); Double(z); }
    std::vector<std::byte> bytes;
};

struct FixtureOptions
{
    bool duplicateId = false;
    bool unknownFlags = false;
    bool zeroExtent = false;
    bool nonFinite = false;
    bool trailing = false;
};

std::vector<std::byte> BuildFixture(const FixtureOptions& options = {})
{
    Writer payload;
    payload.U32(1);
    payload.String("collision.fixture");
    const std::array<uint8_t, 32> source{};
    payload.Bytes(source);
    payload.U32(2);
    for (uint32_t i = 0; i < 2; ++i)
    {
        payload.U8(1);
        payload.U8(options.unknownFlags && i == 0 ? 0x80 : (i == 0 ? 1 : 0));
        payload.U16(0);
        payload.String(options.duplicateId ? "floor" : (i == 0 ? "floor" : "wall"));
        payload.Vec3(0.0, i == 0 ? -0.1 : 1.0, 0.0);
        const double x = options.nonFinite && i == 0
            ? (std::numeric_limits<double>::quiet_NaN)()
            : 2.0;
        payload.Vec3(x, options.zeroExtent && i == 0 ? 0.0 : 0.1, 2.0);
    }
    if (options.trailing)
        payload.U8(0);

    const asset::Sha256Digest digest = asset::ComputeSha256(payload.bytes);
    Writer file;
    constexpr std::array<uint8_t, 8> magic = {
        'T', 'D', 'C', 'O', 'L', 'L', 0, 0
    };
    file.Bytes(magic);
    file.U32(1);
    file.U32(72);
    file.U64(72 + payload.bytes.size());
    file.U64(payload.bytes.size());
    file.Bytes(digest.bytes);
    file.U64(0);
    file.bytes.insert(file.bytes.end(), payload.bytes.begin(), payload.bytes.end());
    return file.bytes;
}

} // namespace

TEST_CASE(CookedCollision_RepresentativeBoxesLoadWithAuthenticatedIdentity)
{
    const asset::CookedCollisionResult result =
        asset::LoadCookedCollisionMemory(BuildFixture());
    CHECK(result.Succeeded());
    if (!result.Succeeded())
        return;
    CHECK_EQ(result.collision->schemaVersion, 1u);
    CHECK_EQ(result.collision->collisionId, std::string("collision.fixture"));
    CHECK_EQ(result.collision->boxes.size(), 2u);
    CHECK_EQ(result.collision->boxes[0].id, std::string("floor"));
    CHECK(asset::HasCollisionSurfaceFlag(
        result.collision->boxes[0].surfaceFlags,
        asset::CollisionSurfaceFlags::Walkable));
    CHECK(!asset::HasCollisionSurfaceFlag(
        result.collision->boxes[1].surfaceFlags,
        asset::CollisionSurfaceFlags::Walkable));
}

TEST_CASE(CookedCollision_HeaderIntegrityAndLimitsFailClosed)
{
    std::vector<std::byte> bytes = BuildFixture();
    bytes[0] = std::byte{ 'X' };
    CHECK_EQ(asset::LoadCookedCollisionMemory(bytes).status,
             asset::CookedCollisionStatus::InvalidMagic);

    bytes = BuildFixture();
    bytes[8] = std::byte{ 2 };
    CHECK_EQ(asset::LoadCookedCollisionMemory(bytes).status,
             asset::CookedCollisionStatus::UnsupportedVersion);

    bytes = BuildFixture();
    bytes.back() ^= std::byte{ 1 };
    CHECK_EQ(asset::LoadCookedCollisionMemory(bytes).status,
             asset::CookedCollisionStatus::IntegrityMismatch);

    asset::CookedCollisionLimits limits;
    limits.maxBoxes = 1;
    CHECK_EQ(asset::LoadCookedCollisionMemory(BuildFixture(), limits).status,
             asset::CookedCollisionStatus::ResourceLimitExceeded);
}

TEST_CASE(CookedCollision_RehashedMalformedShapesAreRejected)
{
    CHECK_EQ(asset::LoadCookedCollisionMemory(BuildFixture({ .duplicateId = true })).status,
             asset::CookedCollisionStatus::InvalidData);
    CHECK_EQ(asset::LoadCookedCollisionMemory(BuildFixture({ .unknownFlags = true })).status,
             asset::CookedCollisionStatus::InvalidData);
    CHECK_EQ(asset::LoadCookedCollisionMemory(BuildFixture({ .zeroExtent = true })).status,
             asset::CookedCollisionStatus::InvalidData);
    CHECK_EQ(asset::LoadCookedCollisionMemory(BuildFixture({ .nonFinite = true })).status,
             asset::CookedCollisionStatus::InvalidData);
    CHECK_EQ(asset::LoadCookedCollisionMemory(BuildFixture({ .trailing = true })).status,
             asset::CookedCollisionStatus::InvalidData);
}
