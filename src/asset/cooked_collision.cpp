#include "cooked_collision.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <new>
#include <unordered_set>

namespace asset
{
namespace
{

constexpr std::array<uint8_t, 8> kMagic = {
    'T', 'D', 'C', 'O', 'L', 'L', 0, 0
};
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kHeaderBytes = 72;
constexpr uint32_t kSchemaVersion = 1;
constexpr uint8_t kKnownSurfaceFlags =
    static_cast<uint8_t>(CollisionSurfaceFlags::Walkable);
constexpr double kMinimumHalfExtentMeters = 1.0e-5;
constexpr double kMaximumCoordinateMeters = 1.0e9;

CookedCollisionResult Failure(CookedCollisionStatus status, std::string error)
{
    CookedCollisionResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

    bool U8(uint8_t& value)
    {
        if (m_offset >= m_bytes.size())
            return false;
        value = std::to_integer<uint8_t>(m_bytes[m_offset++]);
        return true;
    }

    bool U16(uint16_t& value)
    {
        uint8_t low = 0;
        uint8_t high = 0;
        if (!U8(low) || !U8(high))
            return false;
        value = static_cast<uint16_t>(low | (static_cast<uint16_t>(high) << 8));
        return true;
    }

    bool U32(uint32_t& value)
    {
        value = 0;
        for (uint32_t shift = 0; shift < 32; shift += 8)
        {
            uint8_t byte = 0;
            if (!U8(byte))
                return false;
            value |= static_cast<uint32_t>(byte) << shift;
        }
        return true;
    }

    bool U64(uint64_t& value)
    {
        value = 0;
        for (uint32_t shift = 0; shift < 64; shift += 8)
        {
            uint8_t byte = 0;
            if (!U8(byte))
                return false;
            value |= static_cast<uint64_t>(byte) << shift;
        }
        return true;
    }

    bool Double(double& value)
    {
        uint64_t bits = 0;
        if (!U64(bits))
            return false;
        value = std::bit_cast<double>(bits);
        return true;
    }

    bool Bytes(std::span<uint8_t> output)
    {
        if (output.size() > Remaining())
            return false;
        for (uint8_t& byte : output)
            byte = std::to_integer<uint8_t>(m_bytes[m_offset++]);
        return true;
    }

    bool String(std::string& value, uint32_t maxBytes)
    {
        uint32_t size = 0;
        if (!U32(size) || size > maxBytes || size > Remaining())
            return false;
        const char* data = reinterpret_cast<const char*>(m_bytes.data() + m_offset);
        value.assign(data, size);
        m_offset += size;
        return true;
    }

    size_t Remaining() const { return m_bytes.size() - m_offset; }
    bool Done() const { return m_offset == m_bytes.size(); }

private:
    std::span<const std::byte> m_bytes;
    size_t m_offset = 0;
};

bool IsId(std::string_view value)
{
    if (value.empty())
        return false;
    bool separator = true;
    for (const unsigned char c : value)
    {
        const bool alphanumeric =
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        const bool currentSeparator = c == '.' || c == '_' || c == '-';
        if (!alphanumeric && !currentSeparator)
            return false;
        if (currentSeparator && separator)
            return false;
        separator = currentSeparator;
    }
    return !separator;
}

bool ReadVec3(Reader& reader, std::array<double, 3>& value)
{
    return reader.Double(value[0]) && reader.Double(value[1]) &&
           reader.Double(value[2]);
}

bool IsFiniteCoordinate(double value)
{
    return std::isfinite(value) && std::abs(value) <= kMaximumCoordinateMeters;
}

bool ValidateCollision(const CookedCollision& collision, std::string& error)
{
    if (collision.schemaVersion != kSchemaVersion ||
        !IsId(collision.collisionId) || collision.boxes.empty())
    {
        error = "collision metadata is invalid";
        return false;
    }

    std::unordered_set<std::string_view> ids;
    ids.reserve(collision.boxes.size());
    for (const CookedCollisionBox& box : collision.boxes)
    {
        if (!IsId(box.id) || !ids.insert(box.id).second)
        {
            error = "collision shape IDs must be unique canonical IDs";
            return false;
        }
        for (double value : box.centerMeters)
        {
            if (!IsFiniteCoordinate(value))
            {
                error = "collision box center is non-finite or out of range";
                return false;
            }
        }
        for (double value : box.halfExtentsMeters)
        {
            if (!IsFiniteCoordinate(value) || value < kMinimumHalfExtentMeters)
            {
                error = "collision box half extent is invalid";
                return false;
            }
        }
        if ((static_cast<uint8_t>(box.surfaceFlags) & ~kKnownSurfaceFlags) != 0)
        {
            error = "collision box uses unknown surface flags";
            return false;
        }
    }
    return true;
}

bool DecodePayload(
    std::span<const std::byte> payload,
    const CookedCollisionLimits& limits,
    CookedCollision& collision,
    bool& limitExceeded)
{
    Reader reader(payload);
    uint32_t boxCount = 0;
    if (!reader.U32(collision.schemaVersion) ||
        !reader.String(collision.collisionId, limits.maxStringBytes) ||
        !reader.Bytes(collision.sourceSha256.bytes) || !reader.U32(boxCount))
    {
        return false;
    }
    if (boxCount > limits.maxBoxes)
    {
        limitExceeded = true;
        return false;
    }

    collision.boxes.reserve(boxCount);
    for (uint32_t i = 0; i < boxCount; ++i)
    {
        CookedCollisionBox box;
        uint8_t kind = 0;
        uint8_t flags = 0;
        uint16_t reserved = 0;
        if (!reader.U8(kind) || !reader.U8(flags) || !reader.U16(reserved) ||
            kind != 1 || reserved != 0 ||
            !reader.String(box.id, limits.maxStringBytes) ||
            !ReadVec3(reader, box.centerMeters) ||
            !ReadVec3(reader, box.halfExtentsMeters))
        {
            return false;
        }
        box.surfaceFlags = static_cast<CollisionSurfaceFlags>(flags);
        collision.boxes.push_back(std::move(box));
    }
    return reader.Done();
}

} // namespace

const char* CookedCollisionStatusName(CookedCollisionStatus status)
{
    switch (status)
    {
    case CookedCollisionStatus::Success: return "success";
    case CookedCollisionStatus::FileNotFound: return "file_not_found";
    case CookedCollisionStatus::IoError: return "io_error";
    case CookedCollisionStatus::InvalidMagic: return "invalid_magic";
    case CookedCollisionStatus::UnsupportedVersion: return "unsupported_version";
    case CookedCollisionStatus::InvalidLayout: return "invalid_layout";
    case CookedCollisionStatus::IntegrityMismatch: return "integrity_mismatch";
    case CookedCollisionStatus::ResourceLimitExceeded: return "resource_limit_exceeded";
    case CookedCollisionStatus::InvalidData: return "invalid_data";
    }
    return "unknown";
}

CookedCollisionResult LoadCookedCollisionMemory(
    std::span<const std::byte> bytes,
    const CookedCollisionLimits& limits)
{
    try
    {
        if (bytes.size() > limits.maxFileBytes)
            return Failure(CookedCollisionStatus::ResourceLimitExceeded,
                           "cooked collision exceeds the file-size limit");
        if (bytes.size() < kHeaderBytes)
            return Failure(CookedCollisionStatus::InvalidLayout,
                           "cooked collision header is truncated");

        Reader reader(bytes.first(kHeaderBytes));
        std::array<uint8_t, 8> magic{};
        uint32_t version = 0;
        uint32_t headerBytes = 0;
        uint64_t fileBytes = 0;
        uint64_t payloadBytes = 0;
        Sha256Digest payloadSha;
        uint64_t reserved = 0;
        if (!reader.Bytes(magic) || magic != kMagic)
            return Failure(CookedCollisionStatus::InvalidMagic,
                           "cooked collision magic is invalid");
        if (!reader.U32(version))
            return Failure(CookedCollisionStatus::InvalidLayout,
                           "cooked collision header is truncated");
        if (version != kFormatVersion)
            return Failure(CookedCollisionStatus::UnsupportedVersion,
                           "cooked collision version is unsupported");
        if (!reader.U32(headerBytes) || !reader.U64(fileBytes) ||
            !reader.U64(payloadBytes) || !reader.Bytes(payloadSha.bytes) ||
            !reader.U64(reserved))
        {
            return Failure(CookedCollisionStatus::InvalidLayout,
                           "cooked collision header is truncated");
        }
        if (!reader.Done() || headerBytes != kHeaderBytes || reserved != 0 ||
            fileBytes != bytes.size() || payloadBytes > limits.maxPayloadBytes ||
            payloadBytes != bytes.size() - kHeaderBytes)
        {
            return Failure(
                payloadBytes > limits.maxPayloadBytes
                    ? CookedCollisionStatus::ResourceLimitExceeded
                    : CookedCollisionStatus::InvalidLayout,
                "cooked collision header layout is invalid");
        }

        const std::span<const std::byte> payload = bytes.subspan(kHeaderBytes);
        if (ComputeSha256(payload) != payloadSha)
            return Failure(CookedCollisionStatus::IntegrityMismatch,
                           "cooked collision payload SHA-256 does not match");

        CookedCollision decoded;
        decoded.payloadSha256 = payloadSha;
        bool limitExceeded = false;
        if (!DecodePayload(payload, limits, decoded, limitExceeded))
        {
            return Failure(
                limitExceeded ? CookedCollisionStatus::ResourceLimitExceeded
                              : CookedCollisionStatus::InvalidData,
                limitExceeded ? "cooked collision exceeds a resource limit"
                              : "cooked collision payload is invalid");
        }
        std::string validationError;
        if (!ValidateCollision(decoded, validationError))
            return Failure(CookedCollisionStatus::InvalidData, std::move(validationError));

        CookedCollisionResult result;
        result.status = CookedCollisionStatus::Success;
        result.collision =
            std::make_shared<const CookedCollision>(std::move(decoded));
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return Failure(CookedCollisionStatus::ResourceLimitExceeded,
                       "out of memory while loading cooked collision");
    }
}

CookedCollisionResult LoadCookedCollisionFile(
    const std::filesystem::path& path,
    const CookedCollisionLimits& limits)
{
    try
    {
        std::error_code errorCode;
        if (!std::filesystem::exists(path, errorCode))
        {
            return Failure(errorCode ? CookedCollisionStatus::IoError
                                     : CookedCollisionStatus::FileNotFound,
                           errorCode ? "could not inspect cooked collision file"
                                     : "cooked collision file does not exist");
        }
        const uint64_t size = std::filesystem::file_size(path, errorCode);
        if (errorCode)
            return Failure(CookedCollisionStatus::IoError,
                           "could not determine cooked collision file size");
        if (size > limits.maxFileBytes)
            return Failure(CookedCollisionStatus::ResourceLimitExceeded,
                           "cooked collision exceeds the file-size limit");
        if (size > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
            return Failure(CookedCollisionStatus::ResourceLimitExceeded,
                           "cooked collision cannot fit in memory");

        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return Failure(CookedCollisionStatus::IoError,
                           "could not open cooked collision file");
        std::vector<std::byte> bytes(static_cast<size_t>(size));
        if (size > 0 && !stream.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(size)))
        {
            return Failure(CookedCollisionStatus::IoError,
                           "could not read cooked collision file");
        }
        return LoadCookedCollisionMemory(bytes, limits);
    }
    catch (const std::bad_alloc&)
    {
        return Failure(CookedCollisionStatus::ResourceLimitExceeded,
                       "out of memory while reading cooked collision");
    }
}

} // namespace asset
