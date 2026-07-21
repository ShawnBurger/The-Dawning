// =============================================================================
// tests/test_sim_serialize.cpp - sim save/load codec
// =============================================================================
// SIM STAGE 6. Drives the SHIPPED sim/sim_serialize.{h,cpp} against
// docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md section 8. Pure CPU; no D3D12.
//
// What is asserted: a save->load round-trip is BIT-EXACT (every f64/f32/i64 bit
// pattern, no load-side mutation); Deserialize is TOTAL (truncation, corruption,
// wrong magic/version never crash and never return a wrong snapshot as Ok); the
// CRC catches a bit-flip; and save/load is TRANSPARENT across a deterministic step
// (run N -> save -> load -> run M reproduces run N+M on the same binary). Each
// check reads the LOADED value, never the input.
// =============================================================================

#include "test_framework.h"
#include "sim/sim_serialize.h"
#include "sim/reference_frame.h"
#include "sim/nbody.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
using core::Vec3d;
using core::Vec3f;
using core::Quatf;
using namespace sim;

// A representative, VALID snapshot exercising every section and adversarial bit
// patterns (subnormal, -0.0, nextafter, far sector, non-axis quaternion).
SimSnapshot MakeSnapshot()
{
    SimSnapshot s;
    s.coordinateTimeEpoch = 123456.789;
    s.fixedDt = 1.0 / 60.0;
    s.simTick = 4242;
    s.masterFrame = 0;

    Frame root; root.parent = kInvalidFrame; root.origin = WorldPos(0, 0, 0, Vec3d{ 0, 0, 0 });
    root.velocity = Vec3d{ 0, 0, 0 };
    Frame child; child.parent = 0; // strictly lower index
    child.origin = WorldPos(950, -3, 7, Vec3d{ 1234.5, 6.0, 7.0 }); // ~1 ly out
    child.velocity = Vec3d{ 1.0, -2.0, 3.0 };
    s.frames = { root, child };

    BodyRecord b0;
    b0.bodyId = 10; b0.frame = 1; b0.velocityFrame = 0; // distinct frames
    b0.position = Vec3d{ std::numeric_limits<double>::denorm_min(), -0.0, std::nextafter(1.0, 2.0) };
    // Deliberately NON-unit-exact (scaled ~1.0004): still inside the 1e-3 load
    // tolerance, but NOT normalize-idempotent - so a load-side .Normalized() would
    // change its bits and break bit-exactness (the T1 quaternion check + WF2).
    b0.rotation = Quatf::FromAxisAngle(Vec3f{ 0.3f, 0.8f, -0.5f }.Normalized(), 0.9f);
    b0.rotation = Quatf{ b0.rotation.x * 1.0004f, b0.rotation.y * 1.0004f,
                         b0.rotation.z * 1.0004f, b0.rotation.w * 1.0004f };
    b0.scale = Vec3f{ 1, 1, 1 };
    b0.linearVelocity = Vec3d{ 12.0, -3.5, 5.0 };
    b0.angularVelocity = Vec3f{ 0.1f, 0.2f, -0.3f };
    b0.invMass = 1.0 / 500.0;
    b0.invInertiaDiag = Vec3f{ 0.01f, 0.02f, 0.03f };
    BodyRecord b1 = b0; b1.bodyId = 20; b1.frame = 0; b1.velocityFrame = 0;
    b1.position = Vec3d{ 5.0, 6.0, 7.0 };
    s.bodies = { b1, b0 }; // out of order on purpose (canonicalize sorts)

    GravRecord g0; g0.bodyId = 10; g0.mu = 3.986e14; g0.radius = 6.371e6; g0.isSource = 1; g0.owner = 2;
    GravRecord g1; g1.bodyId = 20; g1.mu = 0.0; g1.radius = 1.0; g1.isSource = 0; g1.owner = 1;
    g1.hasRails = 1; g1.semiMajorAxis = 7.0e6; g1.eccentricity = 0.1; g1.inclination = 0.5;
    g1.longitudeAscNode = 1.0; g1.argPeriapsis = 2.0; g1.trueAnomaly = 3.0;
    g1.primaryMu = 3.986e14; g1.primaryBodyId = 10; g1.railsEpoch = 100.0;
    s.gravity = { g1, g0 };

    ClockRecord c0; c0.bodyId = 10; c0.momentum = Vec3d{ 1e3, -2e3, 3e3 }; c0.restMass = 500.0;
    c0.coordinateTime = 1.0e6; c0.properTimeDeviation = -1e-9; // tiny negative residual
    s.clocks = { c0 };
    return s;
}

uint64_t Bits(double v) { return std::bit_cast<uint64_t>(v); }
} // namespace

// =============================================================================
// T1 - ROUND-TRIP BIT-EXACTNESS across every field type. No load-side mutation.
// =============================================================================
TEST_CASE(SimSerialize_RoundTrip_IsBitExact)
{
    SimSnapshot s = MakeSnapshot();
    const std::vector<uint8_t> bytes = Serialize(s);
    const SimLoadResult res = Deserialize(bytes);
    CHECK(res.Ok());
    if (!res.Ok()) return;

    // Loaded == the canonical form of the input, field-wise (exact ==).
    SimSnapshot canon = s; CanonicalizeSnapshot(canon);
    CHECK(res.snapshot == canon);

    // Re-serializing the loaded snapshot yields identical bytes (idempotent codec).
    CHECK(Serialize(res.snapshot) == bytes);

    // Bit-exact spot checks on the tricky values (the loaded doubles' bit patterns).
    const BodyRecord& lb = res.snapshot.bodies[0]; // bodyId 10 after sort
    CHECK_EQ(lb.bodyId, 10ull);
    CHECK_EQ(Bits(lb.position.x), Bits(std::numeric_limits<double>::denorm_min())); // subnormal survived
    CHECK_EQ(Bits(lb.position.y), Bits(-0.0));                                       // -0.0 not flattened
    CHECK_EQ(Bits(lb.position.z), Bits(std::nextafter(1.0, 2.0)));
    // Quaternion is NOT renormalized on load: its (non-unit-exact) float bits survive.
    CHECK_EQ(std::bit_cast<uint32_t>(lb.rotation.x), std::bit_cast<uint32_t>(s.bodies[1].rotation.x));
    CHECK_EQ(std::bit_cast<uint32_t>(lb.rotation.w), std::bit_cast<uint32_t>(s.bodies[1].rotation.w));
    // properTimeDeviation residual survived bit-exact (never collapsed into tau).
    CHECK_EQ(Bits(res.snapshot.clocks[0].properTimeDeviation), Bits(-1e-9));
    // Far sector preserved exactly (no int->double round-trip).
    CHECK_EQ(res.snapshot.frames[1].origin.sx, static_cast<int64_t>(950));
}

// =============================================================================
// T2 - TRUNCATION AT EVERY PREFIX is total and crash-free (never Ok, no throw).
// =============================================================================
TEST_CASE(SimSerialize_TruncationAtEveryPrefix_NeverOkNeverCrashes)
{
    const std::vector<uint8_t> bytes = Serialize(MakeSnapshot());
    bool anyOk = false;
    for (size_t k = 0; k < bytes.size(); ++k)
    {
        const SimLoadResult res = Deserialize(bytes.data(), k);
        if (res.Ok()) anyOk = true;
    }
    CHECK_FALSE(anyOk);                         // no prefix ever decodes as Ok
    // The full buffer still loads.
    CHECK(Deserialize(bytes).Ok());
    // A zero-length and a tiny buffer are ShortBuffer, not a crash.
    CHECK_EQ(static_cast<int>(Deserialize(nullptr, 0).status), static_cast<int>(SimLoadStatus::ShortBuffer));
    CHECK_EQ(static_cast<int>(Deserialize(nullptr, bytes.size()).status),
             static_cast<int>(SimLoadStatus::ShortBuffer));
}

// =============================================================================
// T3 - A CORRUPT INTERNAL SECTION LENGTH is rejected by the bounded cursor, not
//      just by fileBytes. Overwrite a section bodyBytes with a huge value and fix
//      the CRC so it is the LENGTH guard, not the checksum, that catches it.
// =============================================================================
TEST_CASE(SimSerialize_CorruptSectionLength_Rejected)
{
    std::vector<uint8_t> bytes = Serialize(MakeSnapshot());
    // First section header starts at offset 32; its bodyBytes u64 is at +12 => 44.
    const size_t off = 32 + 12;
    for (int i = 0; i < 8; ++i) bytes[off + i] = 0xFF; // enormous length
    // Recompute CRC (field at offset 12) so the checksum passes.
    const uint32_t crc = ComputeSimCrc32(bytes.data(), bytes.size(), 12);
    bytes[12] = uint8_t(crc); bytes[13] = uint8_t(crc >> 8);
    bytes[14] = uint8_t(crc >> 16); bytes[15] = uint8_t(crc >> 24);

    const SimLoadResult res = Deserialize(bytes);
    CHECK_FALSE(res.Ok());
    CHECK_EQ(static_cast<int>(res.status), static_cast<int>(SimLoadStatus::BadSectionLength));
}

// =============================================================================
// T4 - CHECKSUM CATCHES A BIT FLIP (data, header, and a length field).
// =============================================================================
TEST_CASE(SimSerialize_Checksum_CatchesBitFlip)
{
    const std::vector<uint8_t> good = Serialize(MakeSnapshot());
    auto flip = [&](size_t byte, int bit) {
        std::vector<uint8_t> b = good; b[byte] ^= uint8_t(1u << bit); return b;
    };
    // Flip a byte in the section data (well past the 32-byte header).
    CHECK_EQ(static_cast<int>(Deserialize(flip(good.size() - 5, 3)).status),
             static_cast<int>(SimLoadStatus::BadChecksum));
    // Flip a byte inside a section header length region.
    CHECK_EQ(static_cast<int>(Deserialize(flip(46, 0)).status),
             static_cast<int>(SimLoadStatus::BadChecksum));
    // Flip fileBytes (offset 16) - caught as BadLayout OR BadChecksum, never Ok.
    CHECK_FALSE(Deserialize(flip(16, 0)).Ok());
}

// =============================================================================
// T5 - WRONG MAGIC and WRONG VERSION give distinct statuses; version is checked
//      BEFORE any section body is decoded (a too-new file reports "too new").
// =============================================================================
TEST_CASE(SimSerialize_MagicAndVersion_DistinctStatuses)
{
    std::vector<uint8_t> b = Serialize(MakeSnapshot());
    std::vector<uint8_t> badMagic = b; badMagic[0] = 'X';
    CHECK_EQ(static_cast<int>(Deserialize(badMagic).status), static_cast<int>(SimLoadStatus::BadMagic));

    std::vector<uint8_t> badVer = b; badVer[4] = 2; badVer[5] = 0; // formatMajor = 2
    const SimLoadResult res = Deserialize(badVer);
    CHECK_EQ(static_cast<int>(res.status), static_cast<int>(SimLoadStatus::UnsupportedVersion));
    CHECK(res.snapshot.frames.empty()); // nothing decoded
}

// =============================================================================
// T6 - SAVE/LOAD TRANSPARENCY: run N -> save -> load -> run M == run N+M, on the
//      same binary. Steps a 2-body N-body system through the SHIPPED StepNBody.
// =============================================================================
namespace
{
std::vector<NBodyParticle> ToParticles(const SimSnapshot& s)
{
    std::vector<NBodyParticle> ps;
    for (const GravRecord& g : s.gravity)
    {
        const BodyRecord* b = nullptr;
        for (const BodyRecord& br : s.bodies) if (br.bodyId == g.bodyId) { b = &br; break; }
        if (!b) continue;
        NBodyParticle p;
        p.bodyId = g.bodyId; p.mu = g.mu; p.radius = g.radius; p.isSource = (g.isSource != 0);
        p.softening = SofteningLength(g.mu, g.radius);
        p.position = b->position; p.velocity = b->linearVelocity;
        ps.push_back(p);
    }
    return ps;
}
void WriteBack(SimSnapshot& s, const std::vector<NBodyParticle>& ps)
{
    for (const NBodyParticle& p : ps)
        for (BodyRecord& b : s.bodies)
            if (b.bodyId == p.bodyId) { b.position = p.position; b.linearVelocity = p.velocity; }
}
SimSnapshot TwoBodyScene()
{
    SimSnapshot s;
    s.coordinateTimeEpoch = 0.0; s.fixedDt = 0.01; s.masterFrame = 0;
    Frame root; root.parent = kInvalidFrame; root.origin = WorldPos(0, 0, 0, Vec3d{ 0, 0, 0 });
    s.frames = { root };
    BodyRecord a; a.bodyId = 1; a.frame = 0; a.velocityFrame = 0;
    a.position = Vec3d{ 0, 0, 0 }; a.linearVelocity = Vec3d{ 0, 0, 0 };
    BodyRecord b; b.bodyId = 2; b.frame = 0; b.velocityFrame = 0;
    b.position = Vec3d{ 100.0, 0, 0 }; b.linearVelocity = Vec3d{ 0, 63.0, 0 };
    s.bodies = { a, b };
    GravRecord ga; ga.bodyId = 1; ga.mu = 4.0e5; ga.radius = 1.0; ga.isSource = 1;
    GravRecord gb; gb.bodyId = 2; gb.mu = 1.0; gb.radius = 1.0; gb.isSource = 1;
    s.gravity = { ga, gb };
    return s;
}
} // namespace
TEST_CASE(SimSerialize_SaveLoadTransparency_AcrossStepping)
{
    const double dt = 0.01;
    const int N = 300, M = 400;

    // Reference: N+M continuous steps.
    SimSnapshot cont = TwoBodyScene();
    {
        std::vector<NBodyParticle> ps = ToParticles(cont);
        for (int i = 0; i < N + M; ++i) StepNBody(ps, dt);
        WriteBack(cont, ps);
    }

    // Split: N steps, save, load, M steps.
    SimSnapshot split = TwoBodyScene();
    std::vector<NBodyParticle> ps = ToParticles(split);
    for (int i = 0; i < N; ++i) StepNBody(ps, dt);
    WriteBack(split, ps);
    const std::vector<uint8_t> saved = Serialize(split);
    const SimLoadResult loaded = Deserialize(saved);
    CHECK(loaded.Ok());
    if (!loaded.Ok()) return;
    SimSnapshot resumed = loaded.snapshot;
    std::vector<NBodyParticle> ps2 = ToParticles(resumed);
    for (int i = 0; i < M; ++i) StepNBody(ps2, dt);
    WriteBack(resumed, ps2);

    // The save/load run reproduces the continuous run, byte-for-byte (canonical).
    double maxPosDiff = 0.0, maxVelDiff = 0.0;
    for (const BodyRecord& rb : resumed.bodies)
        for (const BodyRecord& cb : cont.bodies)
            if (rb.bodyId == cb.bodyId)
            {
                maxPosDiff = std::max(maxPosDiff, (rb.position - cb.position).Length());
                maxVelDiff = std::max(maxVelDiff, (rb.linearVelocity - cb.linearVelocity).Length());
            }
    CHECK_APPROX_EPS(maxPosDiff, 0.0, 1e-12); // MEASURE the divergence
    CHECK_APPROX_EPS(maxVelDiff, 0.0, 1e-12);
    CHECK(Serialize(resumed) == Serialize(cont));

    // NON-VACUITY: the step must have MOVED body 2 macroscopically, else this test
    // would pass even with a no-op integrator (nothing to preserve is not transparency).
    for (const BodyRecord& cb : cont.bodies)
        if (cb.bodyId == 2)
            CHECK((cb.position - Vec3d{ 100.0, 0, 0 }).Length() > 50.0);
}

// =============================================================================
// T7 - FrameId IS LOAD-BEARING: a body far out in a child frame resolves to the
//      same world position after save/load; and a cyclic frame graph is rejected.
// =============================================================================
TEST_CASE(SimSerialize_FrameId_LoadBearing_AndAcyclic)
{
    SimSnapshot s;
    s.fixedDt = 1.0 / 60.0; s.masterFrame = 0;
    Frame root; root.parent = kInvalidFrame; root.origin = WorldPos(0, 0, 0, Vec3d{ 0, 0, 0 });
    Frame child; child.parent = 0; child.origin = WorldPos(950, 0, 0, Vec3d{ 500.0, 0, 0 }); // ~1 ly
    s.frames = { root, child };

    // A FrameGraph built from these frames resolves a small-localPos body far out.
    FrameGraph fgPre; fgPre.RebuildFromFrames(s.frames);
    Body probe; probe.frame = 1; probe.localPos = Vec3d{ 0.25, 0, 0 };
    const WorldPos wpPre = fgPre.ResolveWorldPos(probe);

    const SimLoadResult res = Deserialize(Serialize(s));
    CHECK(res.Ok());
    if (!res.Ok()) return;
    FrameGraph fgPost; fgPost.RebuildFromFrames(res.snapshot.frames);
    const WorldPos wpPost = fgPost.ResolveWorldPos(probe);
    CHECK(wpPost == wpPre);                       // frame graph round-tripped exactly
    CHECK_EQ(res.snapshot.frames[1].origin.sx, static_cast<int64_t>(950)); // its frame survived

    // A CRC-valid file with a CYCLE (frames[0].parent = 1) must be rejected, not hang.
    std::vector<uint8_t> bytes = Serialize(s);
    // FRMS is the 2nd section: header@32(EPOK,20+bodyEPOK). Rather than compute the
    // offset, rebuild a cyclic snapshot's bytes by hand-patching is fragile; instead
    // assert via the validator on a directly-constructed cyclic snapshot's bytes:
    SimSnapshot cyc = s; cyc.frames[0].parent = 1; // root points at its child => cycle
    // Serialize() does not validate (it is the writer); Deserialize() must reject it.
    const SimLoadResult cres = Deserialize(Serialize(cyc));
    CHECK_FALSE(cres.Ok());
    CHECK_EQ(static_cast<int>(cres.status), static_cast<int>(SimLoadStatus::InvalidData));
}

// =============================================================================
// T8 - LOAD VALIDATION, one witness per guard (each via a CRC-valid buffer that
//      decodes structurally but violates a domain invariant).
// =============================================================================
namespace
{
// Serialize a snapshot, keeping it CRC-valid, for validation tests. (Serialize
// itself does not validate, so an "invalid" snapshot still produces a well-framed,
// checksummed buffer that only Deserialize's validator rejects.)
SimLoadResult LoadOf(const SimSnapshot& s) { return Deserialize(Serialize(s)); }
} // namespace
TEST_CASE(SimSerialize_LoadValidation_RejectsDomainViolations)
{
    // (a) an offset just past a sector edge is ACCEPTED and canonicalized in place.
    {
        SimSnapshot s = MakeSnapshot();
        s.frames[0].origin = WorldPos(5, 0, 0, Vec3d{ kSectorSize + 5.0, 0, 0 });
        const SimLoadResult r = LoadOf(s);
        CHECK(r.Ok());
        if (r.Ok()) CHECK(IsCanonical(r.snapshot.frames[0].origin));
    }
    // (b) a wildly out-of-band offset is InvalidData with NO float->int64 UB.
    {
        SimSnapshot s = MakeSnapshot();
        s.frames[0].origin = WorldPos(0, 0, 0, Vec3d{ 1e300, 0, 0 });
        CHECK_EQ(static_cast<int>(LoadOf(s).status), static_cast<int>(SimLoadStatus::InvalidData));
    }
    // (c) a sector index past kMaxSectorCoord is InvalidData.
    {
        SimSnapshot s = MakeSnapshot();
        s.frames[0].origin = WorldPos(kMaxSectorCoord + 1, 0, 0, Vec3d{ 0, 0, 0 });
        CHECK_EQ(static_cast<int>(LoadOf(s).status), static_cast<int>(SimLoadStatus::InvalidData));
    }
    // (d) restMass <= 0 is InvalidData.
    {
        SimSnapshot s = MakeSnapshot(); s.clocks[0].restMass = 0.0;
        CHECK_EQ(static_cast<int>(LoadOf(s).status), static_cast<int>(SimLoadStatus::InvalidData));
    }
    // (e) a POSITIVE proper-time deviation is InvalidData (invariant <= 0).
    {
        SimSnapshot s = MakeSnapshot(); s.clocks[0].properTimeDeviation = 5.0;
        CHECK_EQ(static_cast<int>(LoadOf(s).status), static_cast<int>(SimLoadStatus::InvalidData));
    }
    // (f) duplicate bodyId within BODY is InvalidData.
    {
        SimSnapshot s = MakeSnapshot(); s.bodies[1].bodyId = s.bodies[0].bodyId;
        CHECK_EQ(static_cast<int>(LoadOf(s).status), static_cast<int>(SimLoadStatus::InvalidData));
    }
    // (g) a NaN in a body position is InvalidData (finiteness guard, incl. rotation).
    {
        SimSnapshot s = MakeSnapshot();
        s.bodies[0].position.x = std::numeric_limits<double>::quiet_NaN();
        CHECK_EQ(static_cast<int>(LoadOf(s).status), static_cast<int>(SimLoadStatus::InvalidData));
    }
    // (h) fixedDt <= 0 is InvalidData (a reload cannot be replayed at a bad rate).
    {
        SimSnapshot s = MakeSnapshot(); s.fixedDt = 0.0;
        CHECK_EQ(static_cast<int>(LoadOf(s).status), static_cast<int>(SimLoadStatus::InvalidData));
    }
}

// =============================================================================
// Raw-buffer assembler: build an arbitrary well-framed, CRC-correct container from
// explicit sections, so the STRUCTURAL guards (missing/duplicate/unknown sections,
// secMajor, BadLayout, raw offsets) can be exercised WITHOUT laundering through
// Serialize()'s canonicalization - what the codec's forward-compat and corruption
// paths need to be witnessed, per the adversarial review.
// =============================================================================
namespace
{
struct RawSection { uint32_t tag; uint16_t secMajor; uint16_t secMinor; uint32_t flags; std::vector<uint8_t> body; };

void PutU16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void PutU32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void PutU64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void PutF64(std::vector<uint8_t>& b, double v) { PutU64(b, std::bit_cast<uint64_t>(v)); }
constexpr uint32_t FourCC(char a, char b, char c, char d)
{ return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24); }
const uint32_t TAG_EPOK = FourCC('E', 'P', 'O', 'K');
const uint32_t TAG_FRMS = FourCC('F', 'R', 'M', 'S');
const uint32_t TAG_BODY = FourCC('B', 'O', 'D', 'Y');

// A valid EPOK body (fixedDt>0, masterFrame invalid so it is valid with 0 frames).
std::vector<uint8_t> EpokBody()
{ std::vector<uint8_t> b; PutF64(b, 0.0); PutF64(b, 1.0 / 60.0); PutU64(b, 0); PutU32(b, kInvalidFrame); PutU32(b, 0); return b; }
// A valid FRMS body with zero frames.
std::vector<uint8_t> FrmsBody0() { std::vector<uint8_t> b; PutU32(b, 0); return b; }

// Assemble header + sections, then fix the CRC. reserved/headerBytes overridable for BadLayout tests.
std::vector<uint8_t> BuildRaw(const std::vector<RawSection>& secs, uint32_t reservedField = 0, uint32_t headerBytesField = 32)
{
    size_t total = 32; for (const auto& s : secs) total += 20 + s.body.size();
    std::vector<uint8_t> b;
    for (char m : { 'D', 'W', 'N', 'S' }) b.push_back(uint8_t(m));
    PutU16(b, 1); PutU16(b, 1); PutU32(b, headerBytesField); PutU32(b, 0); // crc placeholder
    PutU64(b, total); PutU32(b, static_cast<uint32_t>(secs.size())); PutU32(b, reservedField);
    for (const auto& s : secs)
    {
        PutU32(b, s.tag); PutU16(b, s.secMajor); PutU16(b, s.secMinor); PutU32(b, s.flags);
        PutU64(b, s.body.size()); b.insert(b.end(), s.body.begin(), s.body.end());
    }
    const uint32_t crc = ComputeSimCrc32(b.data(), b.size(), 12);
    b[12] = uint8_t(crc); b[13] = uint8_t(crc >> 8); b[14] = uint8_t(crc >> 16); b[15] = uint8_t(crc >> 24);
    return b;
}
int St(const std::vector<uint8_t>& b) { return static_cast<int>(Deserialize(b).status); }
} // namespace

// =============================================================================
// T9 - STRUCTURAL guards via hand-assembled buffers (forward-compat + framing).
// =============================================================================
TEST_CASE(SimSerialize_StructuralGuards)
{
    const RawSection epok{ TAG_EPOK, 1, 0, 1u, EpokBody() };
    const RawSection frms{ TAG_FRMS, 1, 0, 1u, FrmsBody0() };

    // Baseline: EPOK + FRMS alone is a valid (empty) snapshot.
    CHECK_EQ(St(BuildRaw({ epok, frms })), static_cast<int>(SimLoadStatus::Ok));
    // MissingRequired: FRMS absent.
    CHECK_EQ(St(BuildRaw({ epok })), static_cast<int>(SimLoadStatus::MissingRequired));
    // DuplicateSection: EPOK twice.
    CHECK_EQ(St(BuildRaw({ epok, epok, frms })), static_cast<int>(SimLoadStatus::DuplicateSection));
    // UnknownRequired: an unknown tag carrying the REQUIRED flag.
    const RawSection unkReq{ FourCC('Z', 'Z', 'Z', 'Z'), 1, 0, 1u, { 1, 2, 3, 4 } };
    CHECK_EQ(St(BuildRaw({ epok, frms, unkReq })), static_cast<int>(SimLoadStatus::UnknownRequired));
    // Unknown OPTIONAL section is SKIPPED and the rest loads Ok (forward compat).
    const RawSection unkOpt{ FourCC('X', 'T', 'R', 'A'), 1, 0, 0u, { 9, 9, 9, 9, 9, 9, 9, 9 } };
    CHECK_EQ(St(BuildRaw({ epok, frms, unkOpt })), static_cast<int>(SimLoadStatus::Ok));
    // Unsupported secMajor on a REQUIRED section => UnknownRequired (never decoded at old stride).
    const RawSection epok2{ TAG_EPOK, 2, 0, 1u, EpokBody() };
    CHECK_EQ(St(BuildRaw({ epok2, frms })), static_cast<int>(SimLoadStatus::UnknownRequired));
    // Unsupported secMajor on an OPTIONAL section => skipped, Ok.
    const RawSection bodyV2{ TAG_BODY, 2, 0, 0u, { 0, 0, 0, 0 } };
    CHECK_EQ(St(BuildRaw({ epok, frms, bodyV2 })), static_cast<int>(SimLoadStatus::Ok));
    // BadLayout: reserved != 0, and headerBytes != 32 (both checked before CRC).
    CHECK_EQ(St(BuildRaw({ epok, frms }, 7u)), static_cast<int>(SimLoadStatus::BadLayout));
    CHECK_EQ(St(BuildRaw({ epok, frms }, 0u, 40u)), static_cast<int>(SimLoadStatus::BadLayout));
}

// =============================================================================
// T10 - INTERNAL length guards: a huge record count and a sub-4-byte section body
//       must be rejected BEFORE any reserve (the HIGH-severity underflow fix), and
//       a raw out-of-band offset must hit the offset-band gate, not the sector path.
// =============================================================================
TEST_CASE(SimSerialize_InternalLengthGuards)
{
    // BODY section whose internal count is enormous but body is tiny: countFits must
    // reject (no giant reserve, no crash), returning BadSectionLength.
    {
        std::vector<uint8_t> body; PutU32(body, 0xFFFFFFFFu); // count = 4 billion, no records
        const RawSection epok{ TAG_EPOK, 1, 0, 1u, EpokBody() };
        const RawSection frms{ TAG_FRMS, 1, 0, 1u, FrmsBody0() };
        const RawSection bigBody{ TAG_BODY, 1, 0, 0u, body };
        CHECK_EQ(St(BuildRaw({ epok, frms, bigBody })), static_cast<int>(SimLoadStatus::BadSectionLength));
    }
    // THE underflow witness (HIGH-severity fix): a BODY section with bodyBytes==0
    // followed by another section, so reading its u32 count consumes the NEXT
    // section's tag (0xFFFFFFFF here) and advances the cursor PAST bodyEnd. The
    // CLAMPED countFits yields avail==0 => BadSectionLength before any reserve.
    // WITHOUT the clamp, (bodyEnd - c.off) wraps to ~2^64 and reserve(0xFFFFFFFF)
    // requests ~549 GB (bad_alloc -> TooLarge, or an OOM crash on overcommit).
    {
        const RawSection epok{ TAG_EPOK, 1, 0, 1u, EpokBody() };
        const RawSection frms{ TAG_FRMS, 1, 0, 1u, FrmsBody0() };
        const RawSection emptyBody{ TAG_BODY, 1, 0, 0u, {} };          // bodyBytes == 0
        const RawSection tail{ 0xFFFFFFFFu, 1, 0, 0u, { 1, 2, 3, 4 } }; // its tag = BODY's "count"
        CHECK_EQ(St(BuildRaw({ epok, frms, emptyBody, tail })), static_cast<int>(SimLoadStatus::BadSectionLength));
    }
    // A section declaring bodyBytes < 4 (so reading the count crosses bodyEnd): the
    // clamped countFits must NOT underflow into a huge reserve; clean rejection, no crash.
    {
        std::vector<uint8_t> bytes = Serialize(MakeSnapshot());
        size_t off = 32; bool patched = false;
        for (int i = 0; i < 8 && off + 20 <= bytes.size(); ++i)
        {
            uint32_t tag = uint32_t(bytes[off]) | (uint32_t(bytes[off + 1]) << 8) | (uint32_t(bytes[off + 2]) << 16) | (uint32_t(bytes[off + 3]) << 24);
            uint64_t bb = 0; for (int k = 0; k < 8; ++k) bb |= uint64_t(bytes[off + 12 + k]) << (8 * k);
            if (tag == TAG_BODY) { bytes[off + 12] = 2; for (int k = 1; k < 8; ++k) bytes[off + 12 + k] = 0; patched = true; break; }
            off += 20 + bb;
        }
        CHECK(patched);
        const uint32_t crc = ComputeSimCrc32(bytes.data(), bytes.size(), 12);
        bytes[12] = uint8_t(crc); bytes[13] = uint8_t(crc >> 8); bytes[14] = uint8_t(crc >> 16); bytes[15] = uint8_t(crc >> 24);
        const SimLoadResult r = Deserialize(bytes);
        CHECK_FALSE(r.Ok()); // clean rejection, never a crash/OOM
        CHECK_EQ(static_cast<int>(r.status), static_cast<int>(SimLoadStatus::BadSectionLength));
    }
    // A RAW out-of-band frame offset (never laundered through Serialize) hits the
    // offset-band gate => InvalidData, with no float->int64 UB in the loader.
    {
        std::vector<uint8_t> frms; PutU32(frms, 1);                 // 1 frame
        PutU32(frms, kInvalidFrame);                                // parent
        PutU64(frms, 0); PutU64(frms, 0); PutU64(frms, 0);          // sectors sx,sy,sz = 0
        PutF64(frms, 1e300); PutF64(frms, 0.0); PutF64(frms, 0.0);  // offset.x wildly out of band
        PutF64(frms, 0.0); PutF64(frms, 0.0); PutF64(frms, 0.0);    // velocity
        const RawSection epok{ TAG_EPOK, 1, 0, 1u, EpokBody() };
        const RawSection frmsSec{ TAG_FRMS, 1, 0, 1u, frms };
        CHECK_EQ(St(BuildRaw({ epok, frmsSec })), static_cast<int>(SimLoadStatus::InvalidData));
    }
}

// =============================================================================
// T11 - Remaining ValidateSnapshot guards: one witness per guard (each the SOLE
//       violation in an otherwise-valid MakeSnapshot).
// =============================================================================
TEST_CASE(SimSerialize_ValidationGuards_OneWitnessEach)
{
    auto expectInvalid = [](SimSnapshot s) {
        CHECK_EQ(static_cast<int>(Deserialize(Serialize(s)).status), static_cast<int>(SimLoadStatus::InvalidData));
    };
    { SimSnapshot s = MakeSnapshot(); s.masterFrame = 99;             expectInvalid(s); } // masterFrame OOB
    { SimSnapshot s = MakeSnapshot(); s.bodies[0].frame = 99;         expectInvalid(s); } // body.frame OOB
    { SimSnapshot s = MakeSnapshot(); s.bodies[0].velocityFrame = 99; expectInvalid(s); } // velocityFrame OOB
    { SimSnapshot s = MakeSnapshot(); s.gravity[0].isSource = 2;      expectInvalid(s); } // enum range
    { SimSnapshot s = MakeSnapshot(); s.gravity[0].owner = 5;         expectInvalid(s); } // enum range
    { SimSnapshot s = MakeSnapshot(); s.gravity[1].hasRails = 1;
      s.gravity[1].inclination = std::numeric_limits<double>::infinity(); expectInvalid(s); } // rails non-finite
    { SimSnapshot s = MakeSnapshot(); s.frames[1].velocity.y = std::numeric_limits<double>::quiet_NaN(); expectInvalid(s); } // frame vel NaN
    { SimSnapshot s = MakeSnapshot(); s.bodies[0].rotation.x = std::numeric_limits<float>::quiet_NaN(); expectInvalid(s); } // rotation NaN
    { SimSnapshot s = MakeSnapshot(); s.bodies[0].rotation = Quatf{ 2, 0, 0, 0 }; expectInvalid(s); } // grossly non-unit (qn=4)
    { SimSnapshot s = MakeSnapshot(); s.gravity[0].bodyId = s.gravity[1].bodyId; expectInvalid(s); } // dup GRAV id
    { SimSnapshot s = MakeSnapshot(); ClockRecord c2 = s.clocks[0]; s.clocks.push_back(c2); expectInvalid(s); } // dup CLKS id
    { SimSnapshot s = MakeSnapshot(); s.gravity[0].mu = -1.0;         expectInvalid(s); } // negative mu
    { SimSnapshot s = MakeSnapshot(); s.gravity[0].radius = -1.0;     expectInvalid(s); } // negative radius
    { SimSnapshot s = MakeSnapshot(); s.coordinateTimeEpoch = std::numeric_limits<double>::infinity(); expectInvalid(s); } // global non-finite
}
