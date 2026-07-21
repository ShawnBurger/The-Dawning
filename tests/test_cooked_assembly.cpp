#include "test_framework.h"
#include "asset/cooked_assembly.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

class Writer
{
public:
    void U8(uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }

    void U16(uint16_t value)
    {
        U8(static_cast<uint8_t>(value));
        U8(static_cast<uint8_t>(value >> 8));
    }

    void U32(uint32_t value)
    {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            U8(static_cast<uint8_t>(value >> shift));
    }

    void U64(uint64_t value)
    {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            U8(static_cast<uint8_t>(value >> shift));
    }

    void Double(double value) { U64(std::bit_cast<uint64_t>(value)); }

    void Bytes(std::span<const uint8_t> value)
    {
        for (uint8_t byte : value)
            U8(byte);
    }

    void Bytes(std::string_view value)
    {
        for (const char byte : value)
            U8(static_cast<uint8_t>(byte));
    }

    void String(std::string_view value)
    {
        U32(static_cast<uint32_t>(value.size()));
        Bytes(value);
    }

    void Vec3(double x, double y, double z)
    {
        Double(x);
        Double(y);
        Double(z);
    }

    std::vector<std::byte> bytes;
};

std::vector<std::byte> ToBytes(std::string_view text)
{
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

void WriteModule(
    Writer& writer,
    std::string_view id,
    uint8_t role,
    bool unsafeVisualSource = false)
{
    writer.String(id);
    writer.U8(role);
    writer.U8(2); // compound collision
    writer.U16(0);
    writer.U32(0); // provenance
    if (unsafeVisualSource)
    {
        constexpr char unsafe[] = "visual://cockpit\0hidden";
        writer.String(std::string_view(unsafe, sizeof(unsafe) - 1));
    }
    else
    {
        writer.String(role == 1 ? "visual://hull" : "visual://cockpit");
    }
    writer.Vec3(0.0, 0.0, 0.0);
    writer.Vec3(0.0, 0.0, 0.0);
    writer.Vec3(1.0, 1.0, 1.0);
    writer.String(role == 1 ? "collision://hull" : "collision://cockpit");
    writer.U32(3);
    for (uint32_t level = 0; level < 3; ++level)
    {
        writer.U32(level);
        writer.String(level == 0 ? "visual://lod0" :
                      (level == 1 ? "visual://lod1" : "visual://lod2"));
        writer.Double(level == 0 ? 30.0 : (level == 1 ? 100.0 : 500.0));
    }
}

struct FixtureOptions
{
    bool duplicateModuleId = false;
    bool danglingPortalSocket = false;
    bool trailingPayloadByte = false;
    bool unsafeControlCharacter = false;
};

std::vector<std::byte> BuildFixture(const FixtureOptions& options = {})
{
    Writer payload;
    payload.Bytes(asset::ComputeSha256(ToBytes("canonical manifest")).bytes);
    payload.U32(1); // schema
    payload.U8(1); // ship
    payload.U8(7); // continuous, loading-screen-free, interactive
    payload.U16(0);
    payload.String("ship.test.fixture");
    payload.String("fixture-1");
    payload.Double(2.1);
    payload.Double(0.9);

    payload.U32(1); // provenance
    payload.String("source");
    payload.String("Meshy");
    payload.Bytes(asset::ComputeSha256(ToBytes("request")).bytes);

    payload.U32(2); // modules: canonical ID order
    WriteModule(payload, "cockpit", 2, options.unsafeControlCharacter);
    WriteModule(payload, options.duplicateModuleId ? "cockpit" : "hull", 1);

    payload.U32(2); // sockets
    payload.String("cockpit_entry");
    payload.U32(0);
    payload.U8(1);
    payload.U8(0);
    payload.U16(0);
    payload.Vec3(0.0, 0.0, -1.0);
    payload.Vec3(0.0, 0.0, -1.0);
    payload.Vec3(0.0, 1.0, 0.0);
    payload.String("hull_entry");
    payload.U32(1);
    payload.U8(1);
    payload.U8(0);
    payload.U16(0);
    payload.Vec3(0.0, 0.0, -1.0);
    payload.Vec3(0.0, 0.0, -1.0);
    payload.Vec3(0.0, 1.0, 0.0);

    payload.U32(1); // zones
    payload.String("cockpit");
    payload.U32(0);
    payload.U8(1); // pressurized
    payload.U8(0);
    payload.U16(0);
    payload.String("nav://cockpit");
    payload.String("walk://cockpit");

    payload.U32(1); // portals
    payload.String("entry");
    payload.U32(asset::kAssemblyOutsideIndex);
    payload.U32(0);
    payload.U32(1);
    payload.U32(options.danglingPortalSocket ? 99u : 0u);
    payload.U32(0);
    payload.U8(3);
    payload.U8(0);
    payload.U16(0);

    payload.U32(1); // interactions
    payload.String("hatch");
    payload.U8(5); // hatch
    payload.U8(0);
    payload.U16(0);
    payload.U32(0);
    payload.U32(0);
    payload.U32(2);
    payload.String("closed");
    payload.String("open");
    payload.U32(0);
    payload.U32(0);
    payload.U32(0);

    payload.U32(1); // moving parts
    payload.String("hatch_panel");
    payload.U32(0);
    payload.U32(0);
    payload.String("visual://cockpit/hatch_panel");
    payload.U8(2); // rotational
    payload.U8(0);
    payload.U16(0);
    payload.Vec3(0.5, 0.0, -1.0);
    payload.Vec3(0.0, 1.0, 0.0);
    payload.Double(100.0);

    payload.U32(0); // entry zone
    payload.U32(1); // required zones
    payload.U32(0);
    if (options.trailingPayloadByte)
        payload.U8(0xff);

    Writer file;
    constexpr std::array<uint8_t, 8> magic = {
        'T', 'D', 'A', 'S', 'M', 'B', 0, 0
    };
    file.Bytes(magic);
    file.U32(1);
    file.U32(72);
    file.U64(72 + payload.bytes.size());
    file.U64(payload.bytes.size());
    file.Bytes(asset::ComputeSha256(payload.bytes).bytes);
    file.U64(0);
    file.bytes.insert(file.bytes.end(), payload.bytes.begin(), payload.bytes.end());
    return file.bytes;
}

void WriteU32(std::vector<std::byte>& bytes, size_t offset, uint32_t value)
{
    for (uint32_t shift = 0; shift < 32; shift += 8)
        bytes[offset++] = static_cast<std::byte>(value >> shift);
}

static_assert(std::is_const_v<std::remove_reference_t<decltype(
    *std::declval<asset::CookedAssemblyResult>().assembly)>>);

} // namespace

TEST_CASE(CookedAssembly_RepresentativeGraphLoadsWithResolvedIdentity)
{
    const asset::CookedAssemblyResult result =
        asset::LoadCookedAssemblyMemory(BuildFixture());
    CHECK(result.Succeeded());
    if (!result.Succeeded())
        return;

    const asset::CookedAssembly& assembly = *result.assembly;
    CHECK_EQ(assembly.assetId, "ship.test.fixture");
    CHECK_EQ(assembly.schemaVersion, 1u);
    CHECK_EQ(assembly.modules.size(), 2u);
    CHECK_EQ(assembly.sockets.size(), 2u);
    CHECK_EQ(assembly.zones.size(), 1u);
    CHECK_EQ(assembly.portals.size(), 1u);
    CHECK_EQ(assembly.interactions.size(), 1u);
    CHECK_EQ(assembly.movingParts.size(), 1u);
    CHECK_EQ(assembly.modules[assembly.zones[0].moduleIndex].id, "cockpit");
    CHECK_EQ(assembly.interactions[assembly.portals[0].closureInteraction].id, "hatch");
    CHECK_EQ(assembly.movingParts[assembly.interactions[0].movingPartIndex].id,
             "hatch_panel");
    CHECK_EQ(assembly.zones[assembly.entryZone].id, "cockpit");
}

TEST_CASE(CookedAssembly_HeaderAndIntegrityFailuresAreDistinctAndFailClosed)
{
    std::vector<std::byte> bytes = BuildFixture();

    const asset::CookedAssemblyResult truncated =
        asset::LoadCookedAssemblyMemory(std::span(bytes).first(20));
    CHECK_EQ(truncated.status, asset::CookedAssemblyStatus::InvalidLayout);
    CHECK(truncated.assembly == nullptr);

    std::vector<std::byte> badMagic = bytes;
    badMagic[0] = std::byte{ 'X' };
    const asset::CookedAssemblyResult magic =
        asset::LoadCookedAssemblyMemory(badMagic);
    CHECK_EQ(magic.status, asset::CookedAssemblyStatus::InvalidMagic);
    CHECK(magic.assembly == nullptr);

    std::vector<std::byte> badVersion = bytes;
    WriteU32(badVersion, 8, 2);
    const asset::CookedAssemblyResult version =
        asset::LoadCookedAssemblyMemory(badVersion);
    CHECK_EQ(version.status, asset::CookedAssemblyStatus::UnsupportedVersion);
    CHECK(version.assembly == nullptr);

    bytes.back() ^= std::byte{ 0x01 };
    const asset::CookedAssemblyResult integrity =
        asset::LoadCookedAssemblyMemory(bytes);
    CHECK_EQ(integrity.status, asset::CookedAssemblyStatus::IntegrityMismatch);
    CHECK(integrity.assembly == nullptr);
}

TEST_CASE(CookedAssembly_ResourceAndPayloadLimitsRejectBeforePublication)
{
    const std::vector<std::byte> bytes = BuildFixture();
    asset::CookedAssemblyLimits limits;
    limits.maxFileBytes = bytes.size() - 1;
    const asset::CookedAssemblyResult fileLimit =
        asset::LoadCookedAssemblyMemory(bytes, limits);
    CHECK_EQ(fileLimit.status, asset::CookedAssemblyStatus::ResourceLimitExceeded);
    CHECK(fileLimit.assembly == nullptr);

    limits = {};
    limits.maxModules = 1;
    const asset::CookedAssemblyResult countLimit =
        asset::LoadCookedAssemblyMemory(bytes, limits);
    CHECK_EQ(countLimit.status, asset::CookedAssemblyStatus::ResourceLimitExceeded);
    CHECK(countLimit.assembly == nullptr);

    limits = {};
    limits.maxTotalRecords = 8;
    const asset::CookedAssemblyResult aggregateLimit =
        asset::LoadCookedAssemblyMemory(bytes, limits);
    CHECK_EQ(aggregateLimit.status, asset::CookedAssemblyStatus::ResourceLimitExceeded);
    CHECK(aggregateLimit.assembly == nullptr);
}

TEST_CASE(CookedAssembly_RehashedMalformedGraphsStillFailSemanticValidation)
{
    const asset::CookedAssemblyResult duplicate =
        asset::LoadCookedAssemblyMemory(BuildFixture({ .duplicateModuleId = true }));
    CHECK_EQ(duplicate.status, asset::CookedAssemblyStatus::InvalidData);
    CHECK(duplicate.assembly == nullptr);

    const asset::CookedAssemblyResult dangling =
        asset::LoadCookedAssemblyMemory(BuildFixture({ .danglingPortalSocket = true }));
    CHECK_EQ(dangling.status, asset::CookedAssemblyStatus::InvalidData);
    CHECK(dangling.assembly == nullptr);

    const asset::CookedAssemblyResult trailing =
        asset::LoadCookedAssemblyMemory(BuildFixture({ .trailingPayloadByte = true }));
    CHECK_EQ(trailing.status, asset::CookedAssemblyStatus::InvalidData);
    CHECK(trailing.assembly == nullptr);

    const asset::CookedAssemblyResult unsafeText =
        asset::LoadCookedAssemblyMemory(BuildFixture({ .unsafeControlCharacter = true }));
    CHECK_EQ(unsafeText.status, asset::CookedAssemblyStatus::InvalidData);
    CHECK(unsafeText.assembly == nullptr);
}
