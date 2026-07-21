#include "cooked_assembly.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <new>
#include <queue>
#include <unordered_set>

namespace asset
{
namespace
{

constexpr std::array<uint8_t, 8> kMagic = {
    'T', 'D', 'A', 'S', 'M', 'B', 0, 0
};
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kHeaderBytes = 72;
constexpr uint32_t kSchemaVersion = 1;
constexpr uint8_t kRequiredContractFlags = 0x07;

CookedAssemblyResult Failure(CookedAssemblyStatus status, std::string error)
{
    CookedAssemblyResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

bool IsValidUtf8(std::string_view text)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    size_t index = 0;
    while (index < text.size())
    {
        const uint8_t first = bytes[index++];
        if (first <= 0x7f)
            continue;
        uint32_t codePoint = 0;
        uint32_t continuationCount = 0;
        if (first >= 0xc2 && first <= 0xdf)
        {
            codePoint = first & 0x1f;
            continuationCount = 1;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            codePoint = first & 0x0f;
            continuationCount = 2;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            codePoint = first & 0x07;
            continuationCount = 3;
        }
        else
        {
            return false;
        }
        if (continuationCount > text.size() - index)
            return false;
        for (uint32_t i = 0; i < continuationCount; ++i)
        {
            const uint8_t next = bytes[index++];
            if ((next & 0xc0) != 0x80)
                return false;
            codePoint = (codePoint << 6) | (next & 0x3f);
        }
        if ((continuationCount == 2 && codePoint < 0x800) ||
            (continuationCount == 3 && codePoint < 0x10000) ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff) || codePoint > 0x10ffff)
        {
            return false;
        }
    }
    return true;
}

bool IsSafeText(std::string_view text)
{
    if (text.empty() || !IsValidUtf8(text))
        return false;
    return std::none_of(text.begin(), text.end(), [](unsigned char value) {
        return value < 0x20 || value == 0x7f;
    });
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
        return IsValidUtf8(value);
    }

    size_t Remaining() const { return m_bytes.size() - m_offset; }
    bool Done() const { return m_offset == m_bytes.size(); }

private:
    std::span<const std::byte> m_bytes;
    size_t m_offset = 0;
};

bool ReadVector3(Reader& reader, std::array<double, 3>& value)
{
    return reader.Double(value[0]) && reader.Double(value[1]) &&
           reader.Double(value[2]);
}

bool IsFinite(double value)
{
    return std::isfinite(value);
}

bool IsFinite(const std::array<double, 3>& value)
{
    return IsFinite(value[0]) && IsFinite(value[1]) && IsFinite(value[2]);
}

double LengthSquared(const std::array<double, 3>& value)
{
    return value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
}

bool IsId(std::string_view value)
{
    if (value.empty())
        return false;
    bool separator = true;
    for (const unsigned char c : value)
    {
        const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        const bool currentSeparator = c == '.' || c == '_' || c == '-';
        if (!alphanumeric && !currentSeparator)
            return false;
        if (currentSeparator && separator)
            return false;
        separator = currentSeparator;
    }
    return !separator;
}

template <typename Value>
bool HasUniqueIds(const std::vector<Value>& values)
{
    std::unordered_set<std::string_view> ids;
    ids.reserve(values.size());
    for (const Value& value : values)
    {
        if (!IsId(value.id) || !ids.insert(value.id).second)
            return false;
    }
    return true;
}

bool IsMovingInteraction(AssemblyInteractionType type)
{
    return type == AssemblyInteractionType::Airlock ||
           type == AssemblyInteractionType::Door ||
           type == AssemblyInteractionType::Elevator ||
           type == AssemblyInteractionType::Hatch;
}

bool ReadCount(
    Reader& reader,
    uint32_t limit,
    uint32_t& count,
    bool& limitExceeded)
{
    if (!reader.U32(count))
        return false;
    if (count > limit)
    {
        limitExceeded = true;
        return false;
    }
    return true;
}

bool ChargeCount(
    uint32_t count,
    uint64_t& aggregate,
    uint64_t limit,
    bool& limitExceeded)
{
    if (aggregate > limit || count > limit - aggregate)
    {
        limitExceeded = true;
        return false;
    }
    aggregate += count;
    return true;
}

bool DecodePayload(
    std::span<const std::byte> payload,
    const CookedAssemblyLimits& limits,
    CookedAssembly& assembly,
    bool& limitExceeded)
{
    Reader reader(payload);
    uint64_t totalRecords = 0;
    uint64_t totalNestedItems = 0;
    uint8_t assetKind = 0;
    uint8_t contractFlags = 0;
    uint16_t reserved = 0;
    if (!reader.Bytes(assembly.sourceManifestSha256.bytes) ||
        !reader.U32(assembly.schemaVersion) || !reader.U8(assetKind) ||
        !reader.U8(contractFlags) || !reader.U16(reserved) ||
        !reader.String(assembly.assetId, limits.maxStringBytes) ||
        !reader.String(assembly.assemblyRevision, limits.maxStringBytes) ||
        !reader.Double(assembly.minimumClearanceMeters) ||
        !reader.Double(assembly.minimumDoorWidthMeters))
    {
        return false;
    }
    if (assembly.schemaVersion != kSchemaVersion || reserved != 0 ||
        contractFlags != kRequiredContractFlags ||
        (assetKind != static_cast<uint8_t>(AssemblyAssetKind::Ship) &&
         assetKind != static_cast<uint8_t>(AssemblyAssetKind::Structure)))
    {
        return false;
    }
    assembly.assetKind = static_cast<AssemblyAssetKind>(assetKind);

    uint32_t count = 0;
    if (!ReadCount(reader, limits.maxProvenance, count, limitExceeded) ||
        !ChargeCount(count, totalRecords, limits.maxTotalRecords, limitExceeded))
        return false;
    assembly.provenance.resize(count);
    for (AssemblyProvenance& source : assembly.provenance)
    {
        if (!reader.String(source.id, limits.maxStringBytes) ||
            !reader.String(source.provider, limits.maxStringBytes) ||
            !reader.Bytes(source.requestSha256.bytes))
        {
            return false;
        }
    }

    if (!ReadCount(reader, limits.maxModules, count, limitExceeded) ||
        !ChargeCount(count, totalRecords, limits.maxTotalRecords, limitExceeded))
        return false;
    assembly.modules.resize(count);
    for (AssemblyModule& module : assembly.modules)
    {
        uint8_t role = 0;
        uint8_t collision = 0;
        uint16_t moduleReserved = 0;
        if (!reader.String(module.id, limits.maxStringBytes) ||
            !reader.U8(role) || !reader.U8(collision) || !reader.U16(moduleReserved) ||
            !reader.U32(module.provenanceIndex) ||
            !reader.String(module.visualSource, limits.maxStringBytes) ||
            !ReadVector3(reader, module.transform.positionMeters) ||
            !ReadVector3(reader, module.transform.rotationEulerDegrees) ||
            !ReadVector3(reader, module.transform.scale) ||
            !reader.String(module.collisionSource, limits.maxStringBytes))
        {
            return false;
        }
        if (moduleReserved != 0 || role < 1 || role > 2 || collision < 1 || collision > 3)
            return false;
        module.role = static_cast<AssemblyModuleRole>(role);
        module.collisionType = static_cast<AssemblyCollisionType>(collision);
        uint32_t lodCount = 0;
        if (!ReadCount(reader, limits.maxLodsPerModule, lodCount, limitExceeded) ||
            !ChargeCount(
                lodCount, totalNestedItems, limits.maxTotalNestedItems, limitExceeded))
            return false;
        module.lods.resize(lodCount);
        for (AssemblyLod& lod : module.lods)
        {
            if (!reader.U32(lod.level) ||
                !reader.String(lod.source, limits.maxStringBytes) ||
                !reader.Double(lod.maxDistanceMeters))
            {
                return false;
            }
        }
    }

    if (!ReadCount(reader, limits.maxSockets, count, limitExceeded) ||
        !ChargeCount(count, totalRecords, limits.maxTotalRecords, limitExceeded))
        return false;
    assembly.sockets.resize(count);
    for (AssemblySocket& socket : assembly.sockets)
    {
        uint8_t type = 0;
        uint8_t reserved8 = 0;
        uint16_t reserved16 = 0;
        if (!reader.String(socket.id, limits.maxStringBytes) ||
            !reader.U32(socket.moduleIndex) || !reader.U8(type) ||
            !reader.U8(reserved8) || !reader.U16(reserved16) ||
            !ReadVector3(reader, socket.positionMeters) ||
            !ReadVector3(reader, socket.forward) || !ReadVector3(reader, socket.up))
        {
            return false;
        }
        if (type < 1 || type > 4 || reserved8 != 0 || reserved16 != 0)
            return false;
        socket.type = static_cast<AssemblySocketType>(type);
    }

    if (!ReadCount(reader, limits.maxZones, count, limitExceeded) ||
        !ChargeCount(count, totalRecords, limits.maxTotalRecords, limitExceeded))
        return false;
    assembly.zones.resize(count);
    for (AssemblyZone& zone : assembly.zones)
    {
        uint8_t pressure = 0;
        uint8_t reserved8 = 0;
        uint16_t reserved16 = 0;
        if (!reader.String(zone.id, limits.maxStringBytes) ||
            !reader.U32(zone.moduleIndex) || !reader.U8(pressure) ||
            !reader.U8(reserved8) || !reader.U16(reserved16) ||
            !reader.String(zone.navmeshSource, limits.maxStringBytes) ||
            !reader.String(zone.walkableSurface, limits.maxStringBytes))
        {
            return false;
        }
        if (pressure < 1 || pressure > 3 || reserved8 != 0 || reserved16 != 0)
            return false;
        zone.pressure = static_cast<AssemblyPressureClass>(pressure);
    }

    if (!ReadCount(reader, limits.maxPortals, count, limitExceeded) ||
        !ChargeCount(count, totalRecords, limits.maxTotalRecords, limitExceeded))
        return false;
    assembly.portals.resize(count);
    for (AssemblyPortal& portal : assembly.portals)
    {
        uint8_t flags = 0;
        uint8_t reserved8 = 0;
        uint16_t reserved16 = 0;
        if (!reader.String(portal.id, limits.maxStringBytes) ||
            !reader.U32(portal.endpointA) || !reader.U32(portal.endpointB) ||
            !reader.U32(portal.socketA) || !reader.U32(portal.socketB) ||
            !reader.U32(portal.closureInteraction) || !reader.U8(flags) ||
            !reader.U8(reserved8) || !reader.U16(reserved16))
        {
            return false;
        }
        if ((flags & ~0x03u) != 0 || reserved8 != 0 || reserved16 != 0)
            return false;
        portal.sealable = (flags & 0x01u) != 0;
        portal.navLink = (flags & 0x02u) != 0;
    }

    if (!ReadCount(reader, limits.maxInteractions, count, limitExceeded) ||
        !ChargeCount(count, totalRecords, limits.maxTotalRecords, limitExceeded))
        return false;
    assembly.interactions.resize(count);
    for (AssemblyInteraction& interaction : assembly.interactions)
    {
        uint8_t type = 0;
        uint8_t reserved8 = 0;
        uint16_t reserved16 = 0;
        if (!reader.String(interaction.id, limits.maxStringBytes) ||
            !reader.U8(type) || !reader.U8(reserved8) || !reader.U16(reserved16) ||
            !reader.U32(interaction.moduleIndex) ||
            !reader.U32(interaction.socketIndex))
        {
            return false;
        }
        if (type < 1 || type > 7 || reserved8 != 0 || reserved16 != 0)
            return false;
        interaction.type = static_cast<AssemblyInteractionType>(type);
        uint32_t stateCount = 0;
        if (!ReadCount(
                reader, limits.maxStatesPerInteraction, stateCount, limitExceeded) ||
            !ChargeCount(
                stateCount, totalNestedItems, limits.maxTotalNestedItems, limitExceeded))
            return false;
        interaction.states.resize(stateCount);
        for (std::string& state : interaction.states)
        {
            if (!reader.String(state, limits.maxStringBytes))
                return false;
        }
        if (!reader.U32(interaction.initialStateIndex) ||
            !reader.U32(interaction.movingPartIndex) ||
            !reader.U32(interaction.portalIndex))
        {
            return false;
        }
    }

    if (!ReadCount(reader, limits.maxMovingParts, count, limitExceeded) ||
        !ChargeCount(count, totalRecords, limits.maxTotalRecords, limitExceeded))
        return false;
    assembly.movingParts.resize(count);
    for (AssemblyMovingPart& part : assembly.movingParts)
    {
        uint8_t type = 0;
        uint8_t reserved8 = 0;
        uint16_t reserved16 = 0;
        if (!reader.String(part.id, limits.maxStringBytes) ||
            !reader.U32(part.moduleIndex) || !reader.U32(part.interactionIndex) ||
            !reader.String(part.visualSource, limits.maxStringBytes) ||
            !reader.U8(type) || !reader.U8(reserved8) || !reader.U16(reserved16) ||
            !ReadVector3(reader, part.pivotMeters) ||
            !ReadVector3(reader, part.axis) || !reader.Double(part.travel))
        {
            return false;
        }
        if (type < 1 || type > 2 || reserved8 != 0 || reserved16 != 0)
            return false;
        part.motionType = static_cast<AssemblyMotionType>(type);
    }

    uint32_t requiredCount = 0;
    if (!reader.U32(assembly.entryZone) ||
        !ReadCount(reader, limits.maxZones, requiredCount, limitExceeded) ||
        !ChargeCount(
            requiredCount, totalNestedItems, limits.maxTotalNestedItems, limitExceeded))
        return false;
    assembly.requiredReachableZones.resize(requiredCount);
    for (uint32_t& zone : assembly.requiredReachableZones)
    {
        if (!reader.U32(zone))
            return false;
    }
    return reader.Done();
}

bool ValidateAssembly(const CookedAssembly& assembly, std::string& error)
{
    const auto fail = [&error](std::string message) {
        error = std::move(message);
        return false;
    };
    if (!IsId(assembly.assetId) || !IsSafeText(assembly.assemblyRevision) ||
        !IsFinite(assembly.minimumClearanceMeters) ||
        !IsFinite(assembly.minimumDoorWidthMeters) ||
        assembly.minimumClearanceMeters <= 0.0 || assembly.minimumDoorWidthMeters <= 0.0)
    {
        return fail("assembly metadata is invalid");
    }
    if (!HasUniqueIds(assembly.provenance) || !HasUniqueIds(assembly.modules) ||
        !HasUniqueIds(assembly.sockets) || !HasUniqueIds(assembly.zones) ||
        !HasUniqueIds(assembly.portals) || !HasUniqueIds(assembly.interactions) ||
        !HasUniqueIds(assembly.movingParts))
    {
        return fail("assembly IDs must be valid and unique within each table");
    }
    if (assembly.provenance.empty() || assembly.modules.empty() ||
        assembly.sockets.empty() || assembly.zones.empty() || assembly.portals.empty())
    {
        return fail("assembly graph is incomplete");
    }
    bool hasExterior = false;
    bool hasInterior = false;
    for (const AssemblyProvenance& source : assembly.provenance)
    {
        if (!IsSafeText(source.provider))
            return fail("provenance provider is invalid");
    }
    for (const AssemblyModule& module : assembly.modules)
    {
        if (module.provenanceIndex >= assembly.provenance.size() ||
            !IsSafeText(module.visualSource) || !IsSafeText(module.collisionSource) ||
            !IsFinite(module.transform.positionMeters) ||
            !IsFinite(module.transform.rotationEulerDegrees) ||
            !IsFinite(module.transform.scale) ||
            std::any_of(module.transform.scale.begin(), module.transform.scale.end(),
                        [](double value) { return value <= 0.0; }) ||
            module.lods.size() < 3)
        {
            return fail("module data or provenance is invalid");
        }
        hasExterior |= module.role == AssemblyModuleRole::Exterior;
        hasInterior |= module.role == AssemblyModuleRole::Interior;
        double previousDistance = 0.0;
        for (size_t index = 0; index < module.lods.size(); ++index)
        {
            const AssemblyLod& lod = module.lods[index];
            if (lod.level != index || !IsSafeText(lod.source) ||
                !IsFinite(lod.maxDistanceMeters) || lod.maxDistanceMeters <= previousDistance)
            {
                return fail("module LOD chain is invalid");
            }
            previousDistance = lod.maxDistanceMeters;
        }
    }
    if (!hasExterior || !hasInterior)
        return fail("boardable assembly requires exterior and interior modules");

    for (const AssemblySocket& socket : assembly.sockets)
    {
        if (socket.moduleIndex >= assembly.modules.size() ||
            !IsFinite(socket.positionMeters) || !IsFinite(socket.forward) ||
            !IsFinite(socket.up) || LengthSquared(socket.forward) < 1.0e-12 ||
            LengthSquared(socket.up) < 1.0e-12)
        {
            return fail("socket data is invalid");
        }
        const double dot = socket.forward[0] * socket.up[0] +
                           socket.forward[1] * socket.up[1] +
                           socket.forward[2] * socket.up[2];
        const double cosine = dot /
            std::sqrt(LengthSquared(socket.forward) * LengthSquared(socket.up));
        if (!IsFinite(cosine) || std::abs(cosine) > 0.01)
            return fail("socket forward/up axes are not orthogonal");
    }

    std::vector<bool> zonedModules(assembly.modules.size(), false);
    for (const AssemblyZone& zone : assembly.zones)
    {
        if (zone.moduleIndex >= assembly.modules.size() ||
            assembly.modules[zone.moduleIndex].role != AssemblyModuleRole::Interior ||
            !IsSafeText(zone.navmeshSource) || !IsSafeText(zone.walkableSurface))
        {
            return fail("zone data or module ownership is invalid");
        }
        zonedModules[zone.moduleIndex] = true;
    }
    for (size_t index = 0; index < assembly.modules.size(); ++index)
    {
        if (assembly.modules[index].role == AssemblyModuleRole::Interior &&
            !zonedModules[index])
        {
            return fail("interior module has no gameplay zone");
        }
    }

    for (size_t index = 0; index < assembly.interactions.size(); ++index)
    {
        const AssemblyInteraction& interaction = assembly.interactions[index];
        if (interaction.moduleIndex >= assembly.modules.size() ||
            interaction.socketIndex >= assembly.sockets.size() ||
            assembly.sockets[interaction.socketIndex].moduleIndex != interaction.moduleIndex ||
            interaction.states.empty() ||
            interaction.initialStateIndex >= interaction.states.size())
        {
            return fail("interaction identity or module ownership is invalid");
        }
        std::unordered_set<std::string_view> states;
        for (const std::string& state : interaction.states)
        {
            if (!IsId(state) || !states.insert(state).second)
                return fail("interaction states must be valid and unique");
        }
        if (IsMovingInteraction(interaction.type))
        {
            if (interaction.movingPartIndex >= assembly.movingParts.size() ||
                interaction.portalIndex >= assembly.portals.size() ||
                !states.contains("closed") || !states.contains("open"))
            {
                return fail("moving interaction is incomplete");
            }
            const AssemblyMovingPart& part =
                assembly.movingParts[interaction.movingPartIndex];
            if (part.interactionIndex != index ||
                part.moduleIndex != interaction.moduleIndex ||
                assembly.sockets[interaction.socketIndex].type !=
                    AssemblySocketType::Portal)
            {
                return fail("moving interaction ownership is not reciprocal");
            }
        }
        else if (interaction.movingPartIndex != kAssemblyNoIndex ||
                 interaction.portalIndex != kAssemblyNoIndex ||
                 assembly.sockets[interaction.socketIndex].type !=
                    AssemblySocketType::Interaction)
        {
            return fail("nonmoving interaction has invalid socket or closure references");
        }
    }

    for (size_t index = 0; index < assembly.movingParts.size(); ++index)
    {
        const AssemblyMovingPart& part = assembly.movingParts[index];
        if (part.moduleIndex >= assembly.modules.size() ||
            part.interactionIndex >= assembly.interactions.size() ||
            assembly.interactions[part.interactionIndex].movingPartIndex != index ||
            assembly.interactions[part.interactionIndex].moduleIndex != part.moduleIndex ||
            !IsSafeText(part.visualSource) ||
            !IsFinite(part.pivotMeters) || !IsFinite(part.axis) ||
            LengthSquared(part.axis) < 1.0e-12 || !IsFinite(part.travel) ||
            part.travel <= 0.0)
        {
            return fail("moving-part data or reciprocal interaction is invalid");
        }
    }

    const uint32_t outsideNode = static_cast<uint32_t>(assembly.zones.size());
    std::vector<std::vector<uint32_t>> graph(assembly.zones.size() + 1);
    std::unordered_set<uint32_t> usedPortalSockets;
    bool entryConnectedToOutside = false;
    for (size_t index = 0; index < assembly.portals.size(); ++index)
    {
        const AssemblyPortal& portal = assembly.portals[index];
        const auto endpointValid = [&assembly](uint32_t endpoint) {
            return endpoint == kAssemblyOutsideIndex || endpoint < assembly.zones.size();
        };
        if (!endpointValid(portal.endpointA) || !endpointValid(portal.endpointB) ||
            portal.endpointA == portal.endpointB || portal.socketA >= assembly.sockets.size() ||
            portal.socketB >= assembly.sockets.size() ||
            portal.closureInteraction >= assembly.interactions.size() ||
            !portal.sealable || !portal.navLink ||
            assembly.sockets[portal.socketA].type != AssemblySocketType::Portal ||
            assembly.sockets[portal.socketB].type != AssemblySocketType::Portal ||
            assembly.interactions[portal.closureInteraction].portalIndex != index ||
            (assembly.interactions[portal.closureInteraction].socketIndex != portal.socketA &&
             assembly.interactions[portal.closureInteraction].socketIndex != portal.socketB))
        {
            return fail("portal data or reciprocal closure is invalid");
        }
        if (!usedPortalSockets.insert(portal.socketA).second ||
            !usedPortalSockets.insert(portal.socketB).second)
        {
            return fail("portal socket is reused by multiple endpoints");
        }
        const auto validateSide = [&assembly](uint32_t endpoint, uint32_t socketIndex) {
            const AssemblySocket& socket = assembly.sockets[socketIndex];
            if (endpoint == kAssemblyOutsideIndex)
                return assembly.modules[socket.moduleIndex].role == AssemblyModuleRole::Exterior;
            return socket.moduleIndex == assembly.zones[endpoint].moduleIndex;
        };
        if (!validateSide(portal.endpointA, portal.socketA) ||
            !validateSide(portal.endpointB, portal.socketB))
        {
            return fail("portal socket does not belong to its endpoint module");
        }
        const uint32_t a = portal.endpointA == kAssemblyOutsideIndex
            ? outsideNode : portal.endpointA;
        const uint32_t b = portal.endpointB == kAssemblyOutsideIndex
            ? outsideNode : portal.endpointB;
        graph[a].push_back(b);
        graph[b].push_back(a);
        if ((a == outsideNode && b == assembly.entryZone) ||
            (b == outsideNode && a == assembly.entryZone))
        {
            entryConnectedToOutside = true;
        }
    }

    if (assembly.entryZone >= assembly.zones.size() || !entryConnectedToOutside ||
        assembly.requiredReachableZones.size() != assembly.zones.size())
    {
        return fail("navigation entry or required-zone table is invalid");
    }
    std::vector<bool> required(assembly.zones.size(), false);
    for (uint32_t zone : assembly.requiredReachableZones)
    {
        if (zone >= assembly.zones.size() || required[zone])
            return fail("required-zone table contains invalid or duplicate indices");
        required[zone] = true;
    }
    std::vector<bool> reached(graph.size(), false);
    std::queue<uint32_t> pending;
    pending.push(outsideNode);
    reached[outsideNode] = true;
    while (!pending.empty())
    {
        const uint32_t current = pending.front();
        pending.pop();
        for (uint32_t neighbor : graph[current])
        {
            if (!reached[neighbor])
            {
                reached[neighbor] = true;
                pending.push(neighbor);
            }
        }
    }
    for (size_t index = 0; index < assembly.zones.size(); ++index)
    {
        if (!reached[index])
            return fail("interior zone is not reachable from outside");
    }
    return true;
}

} // namespace

const char* CookedAssemblyStatusName(CookedAssemblyStatus status)
{
    switch (status)
    {
    case CookedAssemblyStatus::Success: return "success";
    case CookedAssemblyStatus::FileNotFound: return "file_not_found";
    case CookedAssemblyStatus::IoError: return "io_error";
    case CookedAssemblyStatus::InvalidMagic: return "invalid_magic";
    case CookedAssemblyStatus::UnsupportedVersion: return "unsupported_version";
    case CookedAssemblyStatus::InvalidLayout: return "invalid_layout";
    case CookedAssemblyStatus::IntegrityMismatch: return "integrity_mismatch";
    case CookedAssemblyStatus::ResourceLimitExceeded: return "resource_limit_exceeded";
    case CookedAssemblyStatus::InvalidData: return "invalid_data";
    }
    return "unknown";
}

CookedAssemblyResult LoadCookedAssemblyMemory(
    std::span<const std::byte> bytes,
    const CookedAssemblyLimits& limits)
{
    try
    {
        if (bytes.size() > limits.maxFileBytes)
            return Failure(CookedAssemblyStatus::ResourceLimitExceeded,
                           "cooked assembly exceeds the file-size limit");
        if (bytes.size() < kHeaderBytes)
            return Failure(CookedAssemblyStatus::InvalidLayout,
                           "cooked assembly header is truncated");

        Reader reader(bytes.first(kHeaderBytes));
        std::array<uint8_t, 8> magic{};
        uint32_t version = 0;
        uint32_t headerBytes = 0;
        uint64_t fileBytes = 0;
        uint64_t payloadBytes = 0;
        Sha256Digest payloadSha;
        uint64_t reserved = 0;
        if (!reader.Bytes(magic) || magic != kMagic)
            return Failure(CookedAssemblyStatus::InvalidMagic,
                           "cooked assembly magic is invalid");
        if (!reader.U32(version))
            return Failure(CookedAssemblyStatus::InvalidLayout,
                           "cooked assembly header is truncated");
        if (version != kFormatVersion)
            return Failure(CookedAssemblyStatus::UnsupportedVersion,
                           "cooked assembly version is unsupported");
        if (!reader.U32(headerBytes) || !reader.U64(fileBytes) ||
            !reader.U64(payloadBytes) || !reader.Bytes(payloadSha.bytes) ||
            !reader.U64(reserved))
        {
            return Failure(CookedAssemblyStatus::InvalidLayout,
                           "cooked assembly header is truncated");
        }
        if (!reader.Done() || headerBytes != kHeaderBytes || reserved != 0 ||
            fileBytes != bytes.size() || payloadBytes > limits.maxPayloadBytes ||
            payloadBytes != bytes.size() - kHeaderBytes)
        {
            return Failure(
                payloadBytes > limits.maxPayloadBytes
                    ? CookedAssemblyStatus::ResourceLimitExceeded
                    : CookedAssemblyStatus::InvalidLayout,
                "cooked assembly header layout is invalid");
        }

        const std::span<const std::byte> payload = bytes.subspan(kHeaderBytes);
        if (ComputeSha256(payload) != payloadSha)
            return Failure(CookedAssemblyStatus::IntegrityMismatch,
                           "cooked assembly payload SHA-256 does not match");

        CookedAssembly decoded;
        bool limitExceeded = false;
        if (!DecodePayload(payload, limits, decoded, limitExceeded))
        {
            return Failure(
                limitExceeded ? CookedAssemblyStatus::ResourceLimitExceeded
                              : CookedAssemblyStatus::InvalidData,
                limitExceeded ? "cooked assembly exceeds a resource limit"
                              : "cooked assembly payload is invalid");
        }
        std::string validationError;
        if (!ValidateAssembly(decoded, validationError))
            return Failure(CookedAssemblyStatus::InvalidData, std::move(validationError));

        CookedAssemblyResult result;
        result.status = CookedAssemblyStatus::Success;
        result.assembly = std::make_shared<const CookedAssembly>(std::move(decoded));
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return Failure(CookedAssemblyStatus::ResourceLimitExceeded,
                       "out of memory while loading cooked assembly");
    }
}

CookedAssemblyResult LoadCookedAssemblyFile(
    const std::filesystem::path& path,
    const CookedAssemblyLimits& limits)
{
    try
    {
        std::error_code errorCode;
        if (!std::filesystem::exists(path, errorCode))
        {
            return Failure(errorCode ? CookedAssemblyStatus::IoError
                                     : CookedAssemblyStatus::FileNotFound,
                           errorCode ? "could not inspect cooked assembly file"
                                     : "cooked assembly file does not exist");
        }
        const uint64_t size = std::filesystem::file_size(path, errorCode);
        if (errorCode)
            return Failure(CookedAssemblyStatus::IoError,
                           "could not determine cooked assembly size");
        if (size > limits.maxFileBytes ||
            size > (std::numeric_limits<size_t>::max)() ||
            size > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)()))
        {
            return Failure(CookedAssemblyStatus::ResourceLimitExceeded,
                           "cooked assembly exceeds the file-size limit");
        }
        std::vector<std::byte> bytes(static_cast<size_t>(size));
        std::ifstream stream(path, std::ios::binary);
        if (!stream || !stream.read(reinterpret_cast<char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size())))
        {
            return Failure(CookedAssemblyStatus::IoError,
                           "could not read cooked assembly file");
        }
        return LoadCookedAssemblyMemory(bytes, limits);
    }
    catch (const std::bad_alloc&)
    {
        return Failure(CookedAssemblyStatus::ResourceLimitExceeded,
                       "out of memory while reading cooked assembly file");
    }
}

} // namespace asset
