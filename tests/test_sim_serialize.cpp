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

    GravRecord g0; g0.bodyId = 10; g0.mu = 3.986e14; g0.radius = 6.371e6; g0.isSource = 1; g0.owner = 0;
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
