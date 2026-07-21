#include "sim_serialize.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace sim
{

// =============================================================================
// Format constants
// =============================================================================
namespace
{
constexpr uint8_t  kMagic[4]      = { 'D', 'W', 'N', 'S' };
constexpr uint16_t kFormatMajor   = 1;
constexpr uint16_t kFormatMinor   = 1;
constexpr uint32_t kHeaderBytes   = 32;
constexpr uint32_t kSectionHeader = 20;
constexpr size_t   kCrcFieldOffset = 12;
constexpr size_t   kMinFileBytes  = kHeaderBytes; // header alone is a legal (0-section) file? no - EPOK+FRMS required
constexpr size_t   kMaxBufferBytes = 512ull * 1024ull * 1024ull;
constexpr uint32_t kFlagRequired  = 1u;

// FourCC as a little-endian u32 (bytes a,b,c,d in ascending address order).
constexpr uint32_t FourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a))
         | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
         | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}
constexpr uint32_t kTagEPOK = FourCC('E', 'P', 'O', 'K');
constexpr uint32_t kTagFRMS = FourCC('F', 'R', 'M', 'S');
constexpr uint32_t kTagBODY = FourCC('B', 'O', 'D', 'Y');
constexpr uint32_t kTagGRAV = FourCC('G', 'R', 'A', 'V');
constexpr uint32_t kTagCLKS = FourCC('C', 'L', 'K', 'S');

// Minimum bytes per record, to bound `count` before any reserve.
constexpr uint32_t kMinFrameBytes = 76;  // u32 parent + 3 i64 + 6 f64
constexpr uint32_t kMinBodyBytes  = 124; // fixed
constexpr uint32_t kMinGravBytes  = 28;  // no-rails minimum: u64+2*f64+4*u8 (variable record)
constexpr uint32_t kMinClkBytes   = 56;  // fixed

// ---- CRC-32 (IEEE reflected 0xEDB88320), the shipped asset-pipeline idiom ----
uint32_t Crc32Byte(uint32_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u)); // branchless mask
    return crc;
}
} // namespace

uint32_t ComputeSimCrc32(const uint8_t* data, size_t size, size_t crcFieldOffset)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
    {
        const uint8_t b = (i >= crcFieldOffset && i < crcFieldOffset + 4) ? 0u : data[i];
        crc = Crc32Byte(crc, b);
    }
    return crc ^ 0xFFFFFFFFu;
}

// =============================================================================
// Little-endian writer (field-by-field via bit_cast; NO struct memcpy)
// =============================================================================
namespace
{
struct Writer
{
    std::vector<uint8_t>& out;
    void U8(uint8_t v) { out.push_back(v); }
    void U16(uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); }
    void U32(uint32_t v)
    {
        out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8));
        out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 24));
    }
    void U64(uint64_t v) { U32(uint32_t(v)); U32(uint32_t(v >> 32)); }
    void I64(int64_t v) { U64(std::bit_cast<uint64_t>(v)); }
    void F32(float v) { U32(std::bit_cast<uint32_t>(v)); }
    void F64(double v) { U64(std::bit_cast<uint64_t>(v)); }
    void V3d(const Vec3d& v) { F64(v.x); F64(v.y); F64(v.z); }
    void V3f(const Vec3f& v) { F32(v.x); F32(v.y); F32(v.z); }
    void Quat(const Quatf& q) { F32(q.x); F32(q.y); F32(q.z); F32(q.w); }
};

// Patch a little-endian u32 already written at `offset`.
void PatchU32(std::vector<uint8_t>& buf, size_t offset, uint32_t v)
{
    buf[offset + 0] = uint8_t(v); buf[offset + 1] = uint8_t(v >> 8);
    buf[offset + 2] = uint8_t(v >> 16); buf[offset + 3] = uint8_t(v >> 24);
}

// ---- section-body builders ----
void BuildEPOK(std::vector<uint8_t>& body, const SimSnapshot& s)
{
    Writer w{ body };
    w.F64(s.coordinateTimeEpoch); w.F64(s.fixedDt); w.U64(s.simTick);
    w.U32(s.masterFrame); w.U32(0u);
}
void BuildFRMS(std::vector<uint8_t>& body, const SimSnapshot& s)
{
    Writer w{ body };
    w.U32(static_cast<uint32_t>(s.frames.size()));
    for (const Frame& f : s.frames)
    {
        w.U32(f.parent);
        w.I64(f.origin.sx); w.I64(f.origin.sy); w.I64(f.origin.sz);
        w.V3d(f.origin.offset);
        w.V3d(f.velocity);
    }
}
void BuildBODY(std::vector<uint8_t>& body, const SimSnapshot& s)
{
    Writer w{ body };
    w.U32(static_cast<uint32_t>(s.bodies.size()));
    for (const BodyRecord& b : s.bodies)
    {
        w.U64(b.bodyId); w.U32(b.frame); w.V3d(b.position); w.Quat(b.rotation);
        w.V3f(b.scale); w.U32(b.velocityFrame); w.V3d(b.linearVelocity);
        w.V3f(b.angularVelocity); w.F64(b.invMass); w.V3f(b.invInertiaDiag);
    }
}
void BuildGRAV(std::vector<uint8_t>& body, const SimSnapshot& s)
{
    Writer w{ body };
    w.U32(static_cast<uint32_t>(s.gravity.size()));
    for (const GravRecord& g : s.gravity)
    {
        w.U64(g.bodyId); w.F64(g.mu); w.F64(g.radius);
        w.U8(g.isSource); w.U8(g.owner); w.U8(g.hasRails); w.U8(0u);
        if (g.hasRails)
        {
            w.F64(g.semiMajorAxis); w.F64(g.eccentricity); w.F64(g.inclination);
            w.F64(g.longitudeAscNode); w.F64(g.argPeriapsis); w.F64(g.trueAnomaly);
            w.F64(g.primaryMu); w.U64(g.primaryBodyId); w.F64(g.railsEpoch);
        }
    }
}
void BuildCLKS(std::vector<uint8_t>& body, const SimSnapshot& s)
{
    Writer w{ body };
    w.U32(static_cast<uint32_t>(s.clocks.size()));
    for (const ClockRecord& c : s.clocks)
    {
        w.U64(c.bodyId); w.V3d(c.momentum); w.F64(c.restMass);
        w.F64(c.coordinateTime); w.F64(c.properTimeDeviation);
    }
}
} // namespace

// =============================================================================
// Serialize
// =============================================================================
std::vector<uint8_t> Serialize(const SimSnapshot& input)
{
    SimSnapshot s = input;
    CanonicalizeSnapshot(s);

    // Build each section body first, so its length is known before framing.
    struct Sec { uint32_t tag; uint32_t flags; std::vector<uint8_t> body; };
    std::vector<Sec> secs;
    { std::vector<uint8_t> b; BuildEPOK(b, s); secs.push_back({ kTagEPOK, kFlagRequired, std::move(b) }); }
    { std::vector<uint8_t> b; BuildFRMS(b, s); secs.push_back({ kTagFRMS, kFlagRequired, std::move(b) }); }
    if (!s.bodies.empty())  { std::vector<uint8_t> b; BuildBODY(b, s); secs.push_back({ kTagBODY, 0u, std::move(b) }); }
    if (!s.gravity.empty()) { std::vector<uint8_t> b; BuildGRAV(b, s); secs.push_back({ kTagGRAV, 0u, std::move(b) }); }
    if (!s.clocks.empty())  { std::vector<uint8_t> b; BuildCLKS(b, s); secs.push_back({ kTagCLKS, 0u, std::move(b) }); }

    size_t total = kHeaderBytes;
    for (const Sec& sec : secs) total += kSectionHeader + sec.body.size();

    std::vector<uint8_t> buf;
    buf.reserve(total);
    Writer w{ buf };
    // Header (crc field written 0, patched last).
    for (uint8_t m : kMagic) w.U8(m);
    w.U16(kFormatMajor); w.U16(kFormatMinor); w.U32(kHeaderBytes);
    w.U32(0u);                                  // crc32 placeholder @12
    w.U64(static_cast<uint64_t>(total));        // fileBytes
    w.U32(static_cast<uint32_t>(secs.size()));  // sectionCount
    w.U32(0u);                                  // reserved
    // Sections.
    for (const Sec& sec : secs)
    {
        w.U32(sec.tag); w.U16(1u); w.U16(0u); w.U32(sec.flags);
        w.U64(static_cast<uint64_t>(sec.body.size()));
        buf.insert(buf.end(), sec.body.begin(), sec.body.end());
    }

    const uint32_t crc = ComputeSimCrc32(buf.data(), buf.size(), kCrcFieldOffset);
    PatchU32(buf, kCrcFieldOffset, crc);
    return buf;
}

// =============================================================================
// Bounded cursor (total, crash-free reads)
// =============================================================================
namespace
{
struct Cursor
{
    const uint8_t* p; size_t n; size_t off = 0; bool ok = true; // invariant: off <= n
    bool Need(size_t k) { if (!ok || n - off < k) { ok = false; return false; } return true; }
    uint8_t  U8() { if (!Need(1)) return 0; return p[off++]; }
    uint16_t U16() { if (!Need(2)) return 0; uint16_t v = uint16_t(p[off]) | (uint16_t(p[off + 1]) << 8); off += 2; return v; }
    uint32_t U32()
    {
        if (!Need(4)) return 0;
        uint32_t v = uint32_t(p[off]) | (uint32_t(p[off + 1]) << 8)
                   | (uint32_t(p[off + 2]) << 16) | (uint32_t(p[off + 3]) << 24);
        off += 4; return v;
    }
    uint64_t U64() { uint64_t lo = U32(), hi = U32(); return lo | (hi << 32); }
    int64_t  I64() { return std::bit_cast<int64_t>(U64()); }
    float    F32() { return std::bit_cast<float>(U32()); }
    double   F64() { return std::bit_cast<double>(U64()); }
    Vec3d    V3d() { double x = F64(), y = F64(), z = F64(); return { x, y, z }; }
    Vec3f    V3f() { float x = F32(), y = F32(), z = F32(); return { x, y, z }; }
    Quatf    Quat() { float x = F32(), y = F32(), z = F32(), w = F32(); return { x, y, z, w }; }
    size_t   Remaining() const { return ok ? n - off : 0; }
    void     Seek(size_t o) { off = o; }
};

bool Finite(double v) { return std::isfinite(v); }
bool Finite(const Vec3d& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }
bool Finite(const Vec3f& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool Finite(const Quatf& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

// WorldPos load-validation, UB-free (no float->int64 cast on an out-of-band value,
// no llabs(INT64_MIN)). Returns false on an invalid position.
bool ValidatePos(WorldPos& wp)
{
    const int64_t s[3] = { wp.sx, wp.sy, wp.sz };
    for (int64_t v : s)
        if (!(v >= -kMaxSectorCoord && v <= kMaxSectorCoord)) return false;
    const double o[3] = { wp.offset.x, wp.offset.y, wp.offset.z };
    for (double v : o)
        if (!Finite(v) || !(v >= -kSectorSize && v < 2.0 * kSectorSize)) return false;
    wp = Canonicalize(wp); // now provably safe: |off/kSectorSize| < 2, carry in {-1,0,1}
    return IsCanonical(wp);
}
} // namespace

// =============================================================================
// Semantic validation on load
// =============================================================================
namespace
{
SimLoadStatus ValidateSnapshot(SimSnapshot& s, std::string& err)
{
    if (!Finite(s.coordinateTimeEpoch) || !Finite(s.fixedDt)) { err = "non-finite global"; return SimLoadStatus::InvalidData; }
    if (!(s.fixedDt > 0.0)) { err = "fixedDt <= 0"; return SimLoadStatus::InvalidData; }

    const uint32_t nFrames = static_cast<uint32_t>(s.frames.size());
    if (!(s.masterFrame == kInvalidFrame || s.masterFrame < nFrames)) { err = "masterFrame OOB"; return SimLoadStatus::InvalidData; }

    for (uint32_t i = 0; i < nFrames; ++i)
    {
        Frame& f = s.frames[i];
        // parent strictly lower index (or invalid) => acyclic forest, NCA cannot loop.
        if (!(f.parent == kInvalidFrame || f.parent < i)) { err = "frame parent not < index"; return SimLoadStatus::InvalidData; }
        if (!Finite(f.velocity)) { err = "frame velocity non-finite"; return SimLoadStatus::InvalidData; }
        if (!ValidatePos(f.origin)) { err = "frame origin invalid"; return SimLoadStatus::InvalidData; }
    }

    std::unordered_set<uint64_t> ids;
    ids.reserve(s.bodies.size() * 2);
    for (const BodyRecord& b : s.bodies)
    {
        if (!(b.frame == kInvalidFrame || b.frame < nFrames)) { err = "body.frame OOB"; return SimLoadStatus::InvalidData; }
        if (!(b.velocityFrame == kInvalidFrame || b.velocityFrame < nFrames)) { err = "body.velocityFrame OOB"; return SimLoadStatus::InvalidData; }
        if (!Finite(b.position) || !Finite(b.linearVelocity) || !Finite(b.angularVelocity)
            || !Finite(b.invInertiaDiag) || !Finite(b.scale) || !Finite(b.invMass)) { err = "body non-finite"; return SimLoadStatus::InvalidData; }
        if (!Finite(b.rotation)) { err = "body rotation non-finite"; return SimLoadStatus::InvalidData; }
        const double qn = double(b.rotation.x) * b.rotation.x + double(b.rotation.y) * b.rotation.y
                        + double(b.rotation.z) * b.rotation.z + double(b.rotation.w) * b.rotation.w;
        if (!(std::fabs(qn - 1.0) <= 1e-3)) { err = "body rotation not unit"; return SimLoadStatus::InvalidData; }
        if (!ids.insert(b.bodyId).second) { err = "duplicate body bodyId"; return SimLoadStatus::InvalidData; }
    }

    ids.clear();
    for (const GravRecord& g : s.gravity)
    {
        if (!Finite(g.mu) || !Finite(g.radius)) { err = "grav non-finite"; return SimLoadStatus::InvalidData; }
        if (g.isSource > 1 || g.owner > 1 || g.hasRails > 1) { err = "grav enum out of range"; return SimLoadStatus::InvalidData; }
        if (g.hasRails)
        {
            const double e[8] = { g.semiMajorAxis, g.eccentricity, g.inclination, g.longitudeAscNode,
                                  g.argPeriapsis, g.trueAnomaly, g.primaryMu, g.railsEpoch };
            for (double v : e) if (!Finite(v)) { err = "rails non-finite"; return SimLoadStatus::InvalidData; }
        }
        if (!ids.insert(g.bodyId).second) { err = "duplicate grav bodyId"; return SimLoadStatus::InvalidData; }
    }

    ids.clear();
    for (const ClockRecord& c : s.clocks)
    {
        if (!Finite(c.momentum) || !Finite(c.restMass) || !Finite(c.coordinateTime) || !Finite(c.properTimeDeviation))
        { err = "clock non-finite"; return SimLoadStatus::InvalidData; }
        if (!(c.restMass > 0.0)) { err = "restMass <= 0"; return SimLoadStatus::InvalidData; }
        if (!(c.properTimeDeviation <= 1e-6)) { err = "properTimeDeviation > 0"; return SimLoadStatus::InvalidData; }
        if (!ids.insert(c.bodyId).second) { err = "duplicate clock bodyId"; return SimLoadStatus::InvalidData; }
    }
    return SimLoadStatus::Ok;
}

SimLoadResult Fail(SimLoadStatus st, std::string msg)
{
    SimLoadResult r; r.status = st; r.error = std::move(msg); return r;
}
} // namespace

// =============================================================================
// Deserialize
// =============================================================================
SimLoadResult Deserialize(const uint8_t* data, size_t size)
{
    if (size > kMaxBufferBytes) return Fail(SimLoadStatus::TooLarge, "buffer over cap");
    if (size < kHeaderBytes + kSectionHeader) return Fail(SimLoadStatus::ShortBuffer, "below minimum size");

    Cursor c{ data, size };
    uint8_t magic[4] = { c.U8(), c.U8(), c.U8(), c.U8() };
    if (std::memcmp(magic, kMagic, 4) != 0) return Fail(SimLoadStatus::BadMagic, "bad magic");
    const uint16_t major = c.U16();
    c.U16(); // minor - any accepted
    if (major != kFormatMajor) return Fail(SimLoadStatus::UnsupportedVersion, "format major mismatch");
    const uint32_t headerBytes = c.U32();
    const uint32_t storedCrc = c.U32();
    const uint64_t fileBytes = c.U64();
    const uint32_t sectionCount = c.U32();
    const uint32_t reserved = c.U32();
    if (!c.ok) return Fail(SimLoadStatus::ShortBuffer, "header truncated");
    if (reserved != 0 || headerBytes != kHeaderBytes || fileBytes != size)
        return Fail(SimLoadStatus::BadLayout, "header self-inconsistent");
    if (ComputeSimCrc32(data, size, kCrcFieldOffset) != storedCrc)
        return Fail(SimLoadStatus::BadChecksum, "crc mismatch");

    SimSnapshot snap;
    bool seenEPOK = false, seenFRMS = false, seenBODY = false, seenGRAV = false, seenCLKS = false;

    c.Seek(kHeaderBytes);
    try
    {
        for (uint32_t si = 0; si < sectionCount; ++si)
        {
            if (!c.Need(kSectionHeader)) return Fail(SimLoadStatus::ShortBuffer, "section header truncated");
            const uint32_t tag = c.U32();
            const uint16_t secMajor = c.U16();
            c.U16(); // secMinor
            const uint32_t flags = c.U32();
            const uint64_t bodyBytes = c.U64();
            if (bodyBytes > c.Remaining()) return Fail(SimLoadStatus::BadSectionLength, "section body past end");
            const size_t bodyEnd = c.off + static_cast<size_t>(bodyBytes);

            auto skip = [&]() { c.Seek(bodyEnd); };
            const bool supported = (secMajor == 1);
            const bool required = (flags & kFlagRequired) != 0;

            // Count bound helper (division form, no multiply overflow).
            auto countFits = [&](uint32_t count, uint32_t minRec) {
                return static_cast<uint64_t>(count) <= (bodyEnd - c.off) / (minRec ? minRec : 1);
            };

            if (tag == kTagEPOK && supported)
            {
                if (seenEPOK) return Fail(SimLoadStatus::DuplicateSection, "duplicate EPOK");
                seenEPOK = true;
                snap.coordinateTimeEpoch = c.F64(); snap.fixedDt = c.F64();
                snap.simTick = c.U64(); snap.masterFrame = c.U32(); c.U32();
            }
            else if (tag == kTagFRMS && supported)
            {
                if (seenFRMS) return Fail(SimLoadStatus::DuplicateSection, "duplicate FRMS");
                seenFRMS = true;
                const uint32_t count = c.U32();
                if (!countFits(count, kMinFrameBytes)) return Fail(SimLoadStatus::BadSectionLength, "FRMS count overflow");
                snap.frames.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    Frame f;
                    f.parent = c.U32();
                    f.origin.sx = c.I64(); f.origin.sy = c.I64(); f.origin.sz = c.I64();
                    f.origin.offset = c.V3d();
                    f.velocity = c.V3d();
                    snap.frames.push_back(f);
                }
            }
            else if (tag == kTagBODY && supported)
            {
                if (seenBODY) return Fail(SimLoadStatus::DuplicateSection, "duplicate BODY");
                seenBODY = true;
                const uint32_t count = c.U32();
                if (!countFits(count, kMinBodyBytes)) return Fail(SimLoadStatus::BadSectionLength, "BODY count overflow");
                snap.bodies.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    BodyRecord b;
                    b.bodyId = c.U64(); b.frame = c.U32(); b.position = c.V3d(); b.rotation = c.Quat();
                    b.scale = c.V3f(); b.velocityFrame = c.U32(); b.linearVelocity = c.V3d();
                    b.angularVelocity = c.V3f(); b.invMass = c.F64(); b.invInertiaDiag = c.V3f();
                    snap.bodies.push_back(b);
                }
            }
            else if (tag == kTagGRAV && supported)
            {
                if (seenGRAV) return Fail(SimLoadStatus::DuplicateSection, "duplicate GRAV");
                seenGRAV = true;
                const uint32_t count = c.U32();
                if (!countFits(count, kMinGravBytes)) return Fail(SimLoadStatus::BadSectionLength, "GRAV count overflow");
                snap.gravity.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    GravRecord g;
                    g.bodyId = c.U64(); g.mu = c.F64(); g.radius = c.F64();
                    g.isSource = c.U8(); g.owner = c.U8(); g.hasRails = c.U8(); c.U8();
                    if (g.hasRails)
                    {
                        g.semiMajorAxis = c.F64(); g.eccentricity = c.F64(); g.inclination = c.F64();
                        g.longitudeAscNode = c.F64(); g.argPeriapsis = c.F64(); g.trueAnomaly = c.F64();
                        g.primaryMu = c.F64(); g.primaryBodyId = c.U64(); g.railsEpoch = c.F64();
                    }
                    snap.gravity.push_back(g);
                }
            }
            else if (tag == kTagCLKS && supported)
            {
                if (seenCLKS) return Fail(SimLoadStatus::DuplicateSection, "duplicate CLKS");
                seenCLKS = true;
                const uint32_t count = c.U32();
                if (!countFits(count, kMinClkBytes)) return Fail(SimLoadStatus::BadSectionLength, "CLKS count overflow");
                snap.clocks.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    ClockRecord r;
                    r.bodyId = c.U64(); r.momentum = c.V3d(); r.restMass = c.F64();
                    r.coordinateTime = c.F64(); r.properTimeDeviation = c.F64();
                    snap.clocks.push_back(r);
                }
            }
            else
            {
                // Unknown tag or unsupported secMajor.
                if (required) return Fail(SimLoadStatus::UnknownRequired, "unknown required section");
                skip();
            }

            if (!c.ok) return Fail(SimLoadStatus::ShortBuffer, "section body truncated");
            if (c.off != bodyEnd) return Fail(SimLoadStatus::BadSectionLength, "section record stream desync");
        }
    }
    catch (const std::bad_alloc&)
    {
        return Fail(SimLoadStatus::TooLarge, "allocation failed");
    }

    if (c.off != size) return Fail(SimLoadStatus::BadSectionLength, "trailing gap/overlap");
    if (!seenEPOK || !seenFRMS) return Fail(SimLoadStatus::MissingRequired, "EPOK or FRMS absent");

    std::string err;
    const SimLoadStatus vs = ValidateSnapshot(snap, err);
    if (vs != SimLoadStatus::Ok) return Fail(vs, std::move(err));

    SimLoadResult r; r.status = SimLoadStatus::Ok; r.snapshot = std::move(snap);
    return r;
}

SimLoadResult Deserialize(std::span<const uint8_t> bytes)
{
    return Deserialize(bytes.data(), bytes.size());
}

// =============================================================================
// Canonicalization + equality + status strings
// =============================================================================
void CanonicalizeSnapshot(SimSnapshot& s)
{
    for (Frame& f : s.frames) f.origin = Canonicalize(f.origin);
    std::sort(s.bodies.begin(), s.bodies.end(),
              [](const BodyRecord& a, const BodyRecord& b) { return a.bodyId < b.bodyId; });
    std::sort(s.gravity.begin(), s.gravity.end(),
              [](const GravRecord& a, const GravRecord& b) { return a.bodyId < b.bodyId; });
    std::sort(s.clocks.begin(), s.clocks.end(),
              [](const ClockRecord& a, const ClockRecord& b) { return a.bodyId < b.bodyId; });
}

namespace
{
bool QuatEq(const Quatf& a, const Quatf& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
bool V3fEq(const Vec3f& a, const Vec3f& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool V3dEq(const Vec3d& a, const Vec3d& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
} // namespace

bool BodyRecord::operator==(const BodyRecord& o) const
{
    return bodyId == o.bodyId && frame == o.frame && V3dEq(position, o.position)
        && QuatEq(rotation, o.rotation) && V3fEq(scale, o.scale) && velocityFrame == o.velocityFrame
        && V3dEq(linearVelocity, o.linearVelocity) && V3fEq(angularVelocity, o.angularVelocity)
        && invMass == o.invMass && V3fEq(invInertiaDiag, o.invInertiaDiag);
}
bool GravRecord::operator==(const GravRecord& o) const
{
    return bodyId == o.bodyId && mu == o.mu && radius == o.radius && isSource == o.isSource
        && owner == o.owner && hasRails == o.hasRails && semiMajorAxis == o.semiMajorAxis
        && eccentricity == o.eccentricity && inclination == o.inclination
        && longitudeAscNode == o.longitudeAscNode && argPeriapsis == o.argPeriapsis
        && trueAnomaly == o.trueAnomaly && primaryMu == o.primaryMu
        && primaryBodyId == o.primaryBodyId && railsEpoch == o.railsEpoch;
}
bool ClockRecord::operator==(const ClockRecord& o) const
{
    return bodyId == o.bodyId && V3dEq(momentum, o.momentum) && restMass == o.restMass
        && coordinateTime == o.coordinateTime && properTimeDeviation == o.properTimeDeviation;
}
bool SimSnapshot::operator==(const SimSnapshot& o) const
{
    if (coordinateTimeEpoch != o.coordinateTimeEpoch || fixedDt != o.fixedDt
        || simTick != o.simTick || masterFrame != o.masterFrame) return false;
    if (frames.size() != o.frames.size() || bodies != o.bodies
        || gravity != o.gravity || clocks != o.clocks) return false;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const Frame& a = frames[i]; const Frame& b = o.frames[i];
        if (a.parent != b.parent || a.origin != b.origin || !V3dEq(a.velocity, b.velocity)) return false;
    }
    return true;
}

const char* ToString(SimLoadStatus s)
{
    switch (s)
    {
    case SimLoadStatus::Ok: return "Ok";
    case SimLoadStatus::ShortBuffer: return "ShortBuffer";
    case SimLoadStatus::BadMagic: return "BadMagic";
    case SimLoadStatus::UnsupportedVersion: return "UnsupportedVersion";
    case SimLoadStatus::BadChecksum: return "BadChecksum";
    case SimLoadStatus::BadLayout: return "BadLayout";
    case SimLoadStatus::BadSectionLength: return "BadSectionLength";
    case SimLoadStatus::UnknownRequired: return "UnknownRequired";
    case SimLoadStatus::MissingRequired: return "MissingRequired";
    case SimLoadStatus::DuplicateSection: return "DuplicateSection";
    case SimLoadStatus::TooLarge: return "TooLarge";
    case SimLoadStatus::InvalidData: return "InvalidData";
    }
    return "Unknown";
}

} // namespace sim
