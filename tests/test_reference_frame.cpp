// =============================================================================
// tests/test_reference_frame.cpp - galactic world position + frame graph
// =============================================================================
// SIM STAGE 0. Drives the SHIPPED sim/reference_frame.{h,cpp} - the sector-split
// world position and the hierarchical frame graph that close the interstellar
// precision hole (docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md 1.6 / 11).
//
// This file extends the pattern of test_math.cpp's
// Vec3d_CameraRelativeSurvivesPlanetaryDistance: assert a sub-metre offset
// survives real separation/narrowing at every scale, with a DISCRIMINATING
// NEGATIVE CONTROL - the naive absolute-then-subtract that the architecture is
// built to avoid - so the tests watch the exact difference the whole design
// rests on, not merely that "some number came out right".
//
// Nothing here touches D3D12. Pure CPU header tests.
// =============================================================================

#include "test_framework.h"
#include "sim/reference_frame.h"
#include "ecs/components.h"   // ecs::Transform::ToCameraRelativeMatrix

#include <cmath>
#include <limits>

namespace
{

using core::Vec3d;
using core::Vec3f;
using sim::WorldPos;
using sim::kSectorSize;

// -----------------------------------------------------------------------------
// Physical scales used across the precision tests (metres).
// -----------------------------------------------------------------------------
constexpr double kAU       = 1.495978707e11;   // 1 astronomical unit
constexpr double kLightYr  = 9.4607e15;        // 1 light-year
constexpr double kKiloLy   = 9.4607e18;        // 1000 light-years

// -----------------------------------------------------------------------------
// ulp(x): the spacing between x and the next representable double. This is the
// ground truth the "degradation matches the ULP math" assertions compare
// against - x * 2^-52 to leading order.
// -----------------------------------------------------------------------------
double ulp(double x)
{
    x = std::fabs(x);
    if (x == 0.0) return std::numeric_limits<double>::denorm_min();
    int e = 0;
    std::frexp(x, &e);                 // x in [0.5, 1) * 2^e; leading bit at 2^(e-1)
    return std::ldexp(1.0, (e - 1) - 52);
}

// -----------------------------------------------------------------------------
// THE NAIVE (WRONG) WAY - present ONLY to prove it fails. Reconstruct the
// absolute double  sector*kSectorSize + offset  and subtract. This is the
// catastrophic-cancellation mistake WorldPos exists to prevent; the shipped code
// never does this.
// -----------------------------------------------------------------------------
Vec3d NaiveAbsolute(const WorldPos& p)
{
    return {
        static_cast<double>(p.sx) * kSectorSize + p.offset.x,
        static_cast<double>(p.sy) * kSectorSize + p.offset.y,
        static_cast<double>(p.sz) * kSectorSize + p.offset.z,
    };
}

Vec3d NaiveSeparation(const WorldPos& from, const WorldPos& to)
{
    return NaiveAbsolute(to) - NaiveAbsolute(from);
}

// A WorldPos at approximately `D` metres from the galactic origin along +X, using
// the sector split so the offset stays SMALL once a sector carries the bulk. For
// sub-sector distances the whole distance lives in the offset (that is what those
// distances physically are). Second body is placed via Translate so it is exactly
// `sep` metres further along +X.
WorldPos AtScaleX(double D, double baseOffset = 100.0)
{
    const int64_t sec = static_cast<int64_t>(std::llround(D / kSectorSize));
    const double  off = (sec == 0) ? D : baseOffset;   // sector carries the bulk when far
    return sim::Canonicalize(WorldPos{ sec, 0, 0, { off, 0.0, 0.0 } });
}

} // namespace

// =============================================================================
// SECTOR_SIZE / addressable range sanity
// =============================================================================

// The chosen sector size must be exactly representable (so sector carries are
// exact), give millimetre-scale worst-case intra-sector precision, and hold a
// whole solar system. These are the three claims the header's ULP justification
// makes; pin them.
TEST_CASE(Sector_SizeAndUlpJustification)
{
    // Exact integer in double (1e13 < 2^53), so a few-sector carry is bit-exact.
    CHECK_EQ(kSectorSize, 1.0e13);
    CHECK_EQ(static_cast<double>(static_cast<int64_t>(kSectorSize)), kSectorSize);

    // Worst-case intra-sector ULP is at the far edge (offset ~ kSectorSize):
    // 1e13 * 2^-52 ~= 2.2 mm. Confirm against nextafter ground truth.
    const double edge    = kSectorSize - 2048.0;   // a hair inside the far edge
    const double spacing = std::nextafter(edge, 1.0e300) - edge;
    CHECK(spacing > 0.001 && spacing < 0.004);      // ~2.2 mm
    CHECK_APPROX_EPS(spacing, ulp(kSectorSize), ulp(kSectorSize));

    // A solar system fits in one sector: 1e13 m = ~66.8 AU across; Neptune orbits
    // at 30 AU, so the planetary system is comfortably inside one sector.
    CHECK(kSectorSize / kAU > 60.0);

    // Addressable range: galaxy ~1e21 m => ~1e8 sectors/axis, far inside the cap.
    CHECK(static_cast<double>(sim::kMaxSectorCoord) * kSectorSize > 1.0e21);
    // The cap keeps a sector delta from overflowing int64.
    CHECK(sim::kMaxSectorCoord <= INT64_MAX / 2);
}

// =============================================================================
// THE DISCRIMINATING NEGATIVE CONTROL
//   sector-aware Separation PRESERVES a 0.25 m offset at 1 ly; the naive
//   narrow/subtract-of-absolutes LOSES it. A test where both agree proves
//   nothing, so this asserts they DIVERGE exactly where the ULP math says.
// =============================================================================
TEST_CASE(WorldPos_SectorAwareSeparationBeatsNaive_AtEveryScale)
{
    const double kSep = 0.25;

    struct Scale { double D; const char* name; };
    const Scale scales[] = {
        { 1.0e7,   "1e7 m"    },
        { kAU,     "1 AU"     },
        { 1.0e13,  "1e13 m"   },
        { kLightYr,"1 ly"     },
        { kKiloLy, "1000 ly"  },
    };

    for (const Scale& s : scales)
    {
        const WorldPos a = AtScaleX(s.D);
        const WorldPos b = sim::Translate(a, { kSep, 0.0, 0.0 });

        const double real  = sim::Separation(a, b).x;
        const double naive = NaiveSeparation(a, b).x;

        // The offset magnitude that carries the difference in the sector-aware
        // path: the full distance when a sector cannot (sub-sector D), else the
        // small residual offset. Its ULP is the honest precision of `real`.
        const double offMag = (std::llround(s.D / kSectorSize) == 0) ? s.D : 100.25;
        const double realTol = 8.0 * ulp(offMag);

        // Sector-aware ALWAYS keeps the 0.25 m to within the offset's own ULP.
        CHECK_APPROX_EPS(real, kSep, realTol);

        // The naive absolute-subtract keeps it only while the ABSOLUTE magnitude's
        // ULP is below the separation. That crossover is the ULP math:
        const double absUlp = ulp(NaiveAbsolute(a).x);
        if (absUlp < 0.5 * kSep)
        {
            // e.g. 1e7, 1 AU, 1e13: naive still resolves it - the controls agree,
            // which is required, else the divergence below would prove nothing.
            CHECK_APPROX_EPS(naive, kSep, 8.0 * absUlp + 1e-9);
        }
        else
        {
            // e.g. 1 ly, 1000 ly: one ULP already exceeds the offset, so the naive
            // subtract collapses it to ~0 while the sector-aware path kept 0.25.
            CHECK(absUlp > kSep);                       // ULP math: unresolvable
            CHECK(std::fabs(naive - kSep) > 0.1);       // naive LOST it
            CHECK(std::fabs(real  - kSep) < realTol);   // sector-aware KEPT it
        }
    }
}

// =============================================================================
// FRAME LAYER: a same-frame separation is EXACT at every galactic scale, because
// the (arbitrarily distant) frame origin never enters the subtraction. This is
// the architecture's key insight made testable.
// =============================================================================
TEST_CASE(FrameGraph_SameFrameSeparationExactAtEveryScale)
{
    const double kSep = 0.25;
    const double scales[] = { 1.0e7, kAU, 1.0e13, kLightYr, kKiloLy };

    for (double D : scales)
    {
        sim::FrameGraph g;
        const sim::FrameId root  = g.CreateFrame(sim::kInvalidFrame, WorldPos{});
        // A frame whose ORIGIN sits at scale D (sector carries the bulk).
        const sim::FrameId frame = g.CreateFrame(root, AtScaleX(D));

        sim::Body a{ frame, { 0.0,  0.0, 0.0 }, {} };
        sim::Body b{ frame, { kSep, 0.0, 0.0 }, {} };

        const Vec3d sep = g.SeparationBetween(a, b);

        // EXACT at 1e7, 1 AU, 1e13, 1 ly AND 1000 ly - the frame origin, however
        // distant, is not part of the local subtraction.
        CHECK_APPROX_EPS(sep.x, kSep, 1e-9);
        CHECK_APPROX_EPS(sep.y, 0.0, 1e-9);
        CHECK_APPROX_EPS(sep.z, 0.0, 1e-9);
    }
}

TEST_CASE(FrameGraph_CreateRejectsMalformedTopologyAndCoordinates)
{
    sim::FrameGraph graph;
    const sim::FrameId root =
        graph.CreateFrame(sim::kInvalidFrame, sim::WorldPos{});
    CHECK(root != sim::kInvalidFrame);

    CHECK_EQ(graph.CreateFrame(99u, sim::WorldPos{}), sim::kInvalidFrame);
    CHECK_EQ(graph.CreateFrame(
        root, sim::WorldPos::FromOffset({
            (std::numeric_limits<double>::quiet_NaN)(), 0.0, 0.0 })),
        sim::kInvalidFrame);
    CHECK_EQ(graph.CreateFrame(
        root, sim::WorldPos{}, {
            (std::numeric_limits<double>::infinity)(), 0.0, 0.0 }),
        sim::kInvalidFrame);
    CHECK_EQ(graph.FrameCount(), 1u);

    const sim::WorldPos minSector{
        (std::numeric_limits<int64_t>::min)(), 0, 0, {} };
    CHECK_FALSE(sim::ValidSector(minSector));
    CHECK(std::isnan(sim::Separation(minSector, sim::WorldPos{}).x));
}

// The precision limit is a THEOREM about where the design degrades, stated in
// ULP terms so the boundary is known rather than discovered.
TEST_CASE(Precision_DegradationMatchesUlpMath)
{
    // (a) Intra-sector far edge: worst-case offset ULP ~= kSectorSize*2^-52 ~2.2mm.
    CHECK(ulp(kSectorSize) > 0.0015 && ulp(kSectorSize) < 0.0030);

    // (b) At 1e13 m an absolute double still resolves 0.25 m (ULP ~2.2 mm < 0.25).
    CHECK(ulp(1.0e13) < 0.25);

    // (c) At 1 ly the absolute-double ULP (~2 m) EXCEEDS 0.25 m - this is exactly
    //     why the naive path dies there and the sector split is required.
    CHECK(ulp(kLightYr) > 0.25);
    CHECK(ulp(kLightYr) > 1.0 && ulp(kLightYr) < 4.0);   // ~2 m

    // (d) A GENUINELY distant pair (~1 ly apart) legitimately loses the 0.25 m in
    //     Separation - and does so GRACEFULLY: finite, equal to the large value,
    //     with the loss fully explained by the separation magnitude's ULP.
    const WorldPos a{ 0,   0, 0, { 100.0,    0.0, 0.0 } };
    const WorldPos b{ 946, 0, 0, { 100.25,   0.0, 0.0 } };   // ~1 ly along +X
    const double sep = sim::Separation(a, b).x;
    CHECK(std::isfinite(sep));
    CHECK(ulp(sep) > 0.25);                              // 0.25 below one ULP here
    CHECK_APPROX_EPS(sep, 946.0 * kSectorSize, 2.0 * ulp(sep));  // = the large value
}

// Distant separations must never NaN or blow up - graceful degradation, not a
// catastrophe, is the requirement past the precision limit.
TEST_CASE(Separation_ExtremeDistanceStaysFinite)
{
    const WorldPos a{ -1'000'000'000LL, 0, 0, { 0.0, 0.0, 0.0 } };
    const WorldPos b{  1'000'000'000LL, 0, 0, { 0.0, 0.0, 0.0 } };  // ~2e22 m apart
    const Vec3d d = sim::Separation(a, b);
    CHECK(std::isfinite(d.x));
    CHECK_APPROX_EPS(d.x, 2.0e9 * kSectorSize, ulp(2.0e9 * kSectorSize));

    // Near the addressable cap the sector delta still cannot overflow.
    const WorldPos lo{ -sim::kMaxSectorCoord, 0, 0, {} };
    const WorldPos hi{  sim::kMaxSectorCoord, 0, 0, {} };
    CHECK(sim::ValidSector(lo) && sim::ValidSector(hi));
    CHECK(std::isfinite(sim::Separation(lo, hi).x));
}

// =============================================================================
// CANONICALIZATION - the sector carry is EXACT (physical point preserved)
// =============================================================================

// A huge offset canonicalizes to the SAME physical point. Separation between the
// original and its canonical form is bit-exactly zero. This is the assertion that
// catches a dropped sector carry (the round-trip below alone would not, because a
// consistently dropped carry cancels).
TEST_CASE(Canonicalize_PreservesPhysicalPointExactly)
{
    // Offset 2.5 sectors + 0.25 m past the sector origin - deliberately far out of
    // the canonical [0, kSectorSize) range.
    const WorldPos raw{ 5, -3, 7, { 2.5 * kSectorSize + 0.25,
                                    -1.5 * kSectorSize - 4096.0,
                                    9.25 * kSectorSize + 1024.0 } };
    const WorldPos canon = sim::Canonicalize(raw);

    CHECK(sim::IsCanonical(canon));
    CHECK_FALSE(sim::IsCanonical(raw));

    // The point did not move: separation is bit-exactly zero on every axis.
    const Vec3d d = sim::Separation(raw, canon);
    CHECK_EQ(d.x, 0.0);
    CHECK_EQ(d.y, 0.0);
    CHECK_EQ(d.z, 0.0);

    // Idempotent.
    CHECK(sim::Canonicalize(canon) == canon);
}

// Add then subtract a displacement that CROSSES a sector boundary returns the
// exact original (bit-exact for representable values).
TEST_CASE(Canonicalize_AddSubtractRoundTripsExactly)
{
    const WorldPos p0{ 3, -2, 8, { kSectorSize - 4096.0, 4096.0, 2048.0 } };
    const Vec3d    d { 8192.0, -8192.0, 0.5 * kSectorSize };

    const WorldPos p1 = sim::Translate(p0, d);
    const WorldPos p2 = sim::Translate(p1, { -d.x, -d.y, -d.z });

    // p1 actually crossed boundaries (so the carry was exercised, not a no-op).
    CHECK(p1.sx != p0.sx);   // +8192 pushed offset.x past the far edge
    CHECK(p1.sy != p0.sy);   // -8192 pushed offset.y below zero

    // Round trip is bit-exact.
    CHECK(p2 == p0);
}

// =============================================================================
// CAMERA-RELATIVE HANDOFF - the RULE 1 narrowing site subtracts FIRST
// =============================================================================
TEST_CASE(ToCameraRelative_SubtractsFirstThenNarrows)
{
    // 1e7 m from origin (float spacing ~1 m there), replicating test_math.cpp's
    // Vec3d_CameraRelativeSurvivesPlanetaryDistance but through the real handoff.
    {
        const WorldPos camera = AtScaleX(1.0e7);
        const WorldPos body   = sim::Translate(camera, { 0.25, 0.0, 0.0 });

        // Narrowing the ABSOLUTE positions loses the separation entirely.
        CHECK_EQ(NaiveAbsolute(camera).ToFloat().x, NaiveAbsolute(body).ToFloat().x);

        // Subtract-first-then-narrow preserves it.
        const Vec3f rel = sim::ToCameraRelative(body, camera);
        CHECK_APPROX_EPS(rel.x, 0.25f, 1e-6f);
    }

    // At 1 AU the double residual is exact to ~30 microns (offset ULP at 1.5e11),
    // and once narrowed the 0.25 m residual is tiny for float - it survives.
    {
        const WorldPos camera = AtScaleX(kAU);
        const WorldPos body   = sim::Translate(camera, { 0.25, 0.0, 0.0 });
        const Vec3f rel = sim::ToCameraRelative(body, camera);
        CHECK_APPROX_EPS(rel.x, 0.25f, 1e-4f);
    }
}

// The per-mode render scale K in ecs::Transform::ToCameraRelativeMatrix must be
// applied AFTER the double camera subtract. A body 100 m from the camera, both
// ~1 AU from the world origin, must render at exactly 100*K even at K = 1e-9 —
// which is only possible because the 1 AU magnitude is differenced away in double
// first. Scaling the absolute positions and narrowing before differencing would
// leave float noise ~1e-5 instead of the 1e-7 offset (the orrery-jitter failure).
TEST_CASE(ToCameraRelativeMatrix_RenderScaleAppliedAfterSubtract)
{
    const core::Vec3d camera = { kAU, 0.0, 0.0 };
    ecs::Transform body;
    body.position = { kAU + 100.0, 0.0, 0.0 };
    body.rotation = core::Quatf::Identity();
    body.scale    = { 2.0f, 2.0f, 2.0f };

    const double K = 1.0e-9;
    const core::Mat4x4 m = body.ToCameraRelativeMatrix(camera, K);

    // Body-local origin maps to (position - camera) * K = 100 m * 1e-9 = 1e-7.
    const core::Vec3f o = m.TransformPoint({ 0.0f, 0.0f, 0.0f });
    CHECK_APPROX_EPS(o.x, 1.0e-7f, 1.0e-10f);
    CHECK_APPROX(o.y, 0.0f);
    CHECK_APPROX(o.z, 0.0f);

    // K scales SIZE too: a body-local +x unit spans scale*K in render space.
    const core::Vec3f px = m.TransformPoint({ 1.0f, 0.0f, 0.0f });
    CHECK_APPROX_EPS(px.x - o.x, static_cast<float>(2.0 * K), 1.0e-10f);

    // K = 1 recovers the true 100 m residual (the existing true-scale behaviour).
    const core::Mat4x4 m1 = body.ToCameraRelativeMatrix(camera, 1.0);
    CHECK_APPROX_EPS(m1.TransformPoint({ 0.0f, 0.0f, 0.0f }).x, 100.0f, 1.0e-2f);
}

// =============================================================================
// FRAME GRAPH: resolve, cross-frame separation, relative velocity
// =============================================================================
TEST_CASE(FrameGraph_ResolveAndCrossFrameSeparation)
{
    sim::FrameGraph g;
    const sim::FrameId root   = g.CreateFrame(sim::kInvalidFrame, WorldPos{});
    // Sector frame ~1e15 m from origin.
    const sim::FrameId sector = g.CreateFrame(root, WorldPos{ 100, 0, 0, { 0.0, 0.0, 0.0 } });
    // Star system a little way inside the sector.
    const sim::FrameId star   = g.CreateFrame(sector, sim::Translate(g.GetFrame(sector).origin,
                                                                      { 1.0e11, 0.0, 0.0 }));
    // Planet inside the star system.
    const sim::FrameId planet = g.CreateFrame(star, sim::Translate(g.GetFrame(star).origin,
                                                                    { 1.0e9, 0.0, 0.0 }));

    // A body in the planet frame resolves to the composed world position.
    sim::Body probe{ planet, { 500.0, 0.0, 0.0 }, {} };
    const WorldPos w = g.ResolveWorldPos(probe);
    // world.x ~= 100*1e13 + 1e11 + 1e9 + 500
    const double expectedX = 100.0 * kSectorSize + 1.0e11 + 1.0e9 + 500.0;
    CHECK_APPROX_EPS(sim::Separation(WorldPos{}, w).x, expectedX, 1.0);

    // Two bodies in different frames, 0.25 m apart in the common (planet) frame.
    sim::Body a{ planet, { 0.0,  0.0, 0.0 }, {} };
    sim::Body b{ star,   {} , {} };
    // Put b at the same world point as a plus 0.25 m, but expressed in the STAR
    // frame, by reparenting a displaced copy.
    sim::Body bWorld = a;
    bWorld.localPos = a.localPos + Vec3d{ 0.25, 0.0, 0.0 };
    b = g.Reparent(bWorld, star);

    const Vec3d sep = g.SeparationBetween(a, b);
    CHECK_APPROX_EPS(sep.x, 0.25, 1e-3);   // cross-frame, near common ancestor
}

TEST_CASE(FrameGraph_RelativeVelocityIsFrameInvariant)
{
    sim::FrameGraph g;
    const sim::FrameId root = g.CreateFrame(sim::kInvalidFrame, WorldPos{});
    // A moving sector frame.
    const sim::FrameId sector = g.CreateFrame(root, WorldPos{ 10, 0, 0, {} },
                                              Vec3d{ 30000.0, 0.0, 0.0 });

    sim::Body a{ sector, { 0.0, 0.0, 0.0 }, { 100.0, 0.0,   0.0 } };
    sim::Body b{ sector, { 0.0, 0.0, 0.0 }, { 100.0, 250.0, 0.0 } };

    // Relative velocity is the difference of local velocities (the frame's own
    // 30 km/s cancels) - and does not depend on the frame's motion.
    const Vec3d rel = g.RelativeVelocity(a, b);
    CHECK_APPROX(rel.x, 0.0);
    CHECK_APPROX(rel.y, 250.0);
    CHECK_APPROX(rel.z, 0.0);
}

TEST_CASE(FrameGraph_SeparationBetweenDisconnectedRootsUsesWorldSpace)
{
    sim::FrameGraph g;
    const sim::FrameId rootA =
        g.CreateFrame(sim::kInvalidFrame, WorldPos{ 4, 0, 0, { 10.0, 0.0, 0.0 } });
    const sim::FrameId rootB =
        g.CreateFrame(sim::kInvalidFrame, WorldPos{ 4, 0, 0, { 25.0, 0.0, 0.0 } });

    const sim::Body a{ rootA, { 2.0, 0.0, 0.0 }, {} };
    const sim::Body b{ rootB, { 5.0, 0.0, 0.0 }, {} };

    CHECK_EQ(g.NearestCommonAncestor(rootA, rootB), sim::kInvalidFrame);
    const Vec3d separation = g.SeparationBetween(a, b);
    CHECK_APPROX(separation.x, 18.0);
    CHECK_APPROX(separation.y, 0.0);
    CHECK_APPROX(separation.z, 0.0);
}

// =============================================================================
// REBASE - world position preserved exactly
// =============================================================================

// Recentering a frame's origin and applying the returned correction to a body in
// that frame leaves the body's WORLD position bit-exactly unchanged.
TEST_CASE(RebaseFrame_PreservesWorldPositionExactly)
{
    sim::FrameGraph g;
    const sim::FrameId root  = g.CreateFrame(sim::kInvalidFrame, WorldPos{});
    const sim::FrameId frame = g.CreateFrame(root, WorldPos{ 10, 0, 0, { 2048.0, 0.0, 0.0 } });

    sim::Body body{ frame, { 512.0, -256.0, 128.0 }, {} };
    const WorldPos before = g.ResolveWorldPos(body);

    // Recenter the frame nearer the "player" (same sector, representable offset).
    const Vec3d correction = g.RebaseFrame(frame, WorldPos{ 10, 0, 0, { 1024.0, 0.0, 0.0 } });
    sim::FrameGraph::ApplyRebaseToBody(body, correction);

    const WorldPos after = g.ResolveWorldPos(body);
    CHECK(after == before);        // bit-exact world invariance
}

// A new origin that CreateFrame would reject (out-of-range sector, or a non-finite
// offset) must NOT corrupt the frame. RebaseFrame leaves the origin unchanged and
// returns a zero correction, matching CreateFrame's rejection. Regression for the
// review finding that RebaseFrame stored an UNVALIDATED origin while CreateFrame
// (commit 08fa46e) rejects the identical input — Canonicalize does not clamp the
// sector or discard a non-finite offset, so an unvalidated store would leave
// ValidSector(frame.origin) false forever and propagate NaN through every later
// Separation/ResolveWorldPos/Reparent for in-frame bodies.
TEST_CASE(RebaseFrame_RejectsInvalidOriginAndPreservesFrame)
{
    sim::FrameGraph g;
    const WorldPos good{ 10, 0, 0, { 2048.0, 0.0, 0.0 } };
    const sim::FrameId frame = g.CreateFrame(sim::kInvalidFrame, good);
    CHECK(frame != sim::kInvalidFrame);
    CHECK(g.GetFrame(frame).origin == good);

    // (a) Out-of-range sector (> kMaxSectorCoord): Canonicalize carries the offset
    // but never clamps the sector, so this survives to the store unless rejected.
    const WorldPos badSector{ sim::kMaxSectorCoord + 1, 0, 0, { 0.0, 0.0, 0.0 } };
    const Vec3d corrA = g.RebaseFrame(frame, badSector);
    CHECK(corrA.x == 0.0 && corrA.y == 0.0 && corrA.z == 0.0);
    CHECK(g.GetFrame(frame).origin == good);            // frame unchanged
    CHECK(sim::ValidSector(g.GetFrame(frame).origin));  // still valid

    // (b) Non-finite offset: IsCanonical rejects it (AxisCanonical checks isfinite).
    const WorldPos badOffset{
        10, 0, 0,
        { std::numeric_limits<double>::infinity(), 0.0, 0.0 } };
    const Vec3d corrB = g.RebaseFrame(frame, badOffset);
    CHECK(corrB.x == 0.0 && corrB.y == 0.0 && corrB.z == 0.0);
    CHECK(g.GetFrame(frame).origin == good);            // frame unchanged

    // The guard does NOT over-reject: a valid rebase still applies.
    const WorldPos okNew{ 10, 0, 0, { 1024.0, 0.0, 0.0 } };
    g.RebaseFrame(frame, okNew);
    CHECK(g.GetFrame(frame).origin == okNew);
}

// Moving a body to a new parent frame preserves its world position AND world
// velocity exactly.
TEST_CASE(Reparent_PreservesWorldStateExactly)
{
    sim::FrameGraph g;
    const sim::FrameId root = g.CreateFrame(sim::kInvalidFrame, WorldPos{});
    const sim::FrameId f1 = g.CreateFrame(root, WorldPos{ 20, 0, 0, { 100.0, 0.0, 0.0 } },
                                          Vec3d{ 10.0, 0.0, 0.0 });
    const sim::FrameId f2 = g.CreateFrame(root, WorldPos{ 20, 0, 0, { 300.0, 0.0, 0.0 } },
                                          Vec3d{ 25.0, 0.0, 0.0 });

    sim::Body body{ f1, { 50.0, 0.0, 0.0 }, { 5.0, 0.0, 0.0 } };
    const WorldPos worldBefore = g.ResolveWorldPos(body);
    const Vec3d    velBefore   = g.ResolveWorldVel(body);

    const sim::Body moved = g.Reparent(body, f2);

    CHECK_EQ(moved.frame, f2);
    CHECK(g.ResolveWorldPos(moved) == worldBefore);   // world position invariant
    const Vec3d velAfter = g.ResolveWorldVel(moved);
    CHECK_APPROX(velAfter.x, velBefore.x);            // world velocity invariant
    CHECK_APPROX(velAfter.y, velBefore.y);
    CHECK_APPROX(velAfter.z, velBefore.z);
}

// =============================================================================
// DETERMINISM - same operations, same results (bit-exact)
// =============================================================================
TEST_CASE(ReferenceFrame_IsDeterministic)
{
    sim::FrameGraph g;
    const sim::FrameId root = g.CreateFrame(sim::kInvalidFrame, WorldPos{});
    const sim::FrameId f    = g.CreateFrame(root, AtScaleX(kLightYr));

    sim::Body a{ f, { 3.0,  -7.0, 11.0 }, { 1.0, 2.0, 3.0 } };
    sim::Body b{ f, { 3.25, -7.0, 11.0 }, { 4.0, 5.0, 6.0 } };

    const Vec3d sep1 = g.SeparationBetween(a, b);
    const Vec3d sep2 = g.SeparationBetween(a, b);
    CHECK(sep1 == sep2);

    const WorldPos w1 = g.ResolveWorldPos(a);
    const WorldPos w2 = g.ResolveWorldPos(a);
    CHECK(w1 == w2);

    const Vec3f r1 = sim::ToCameraRelative(g.ResolveWorldPos(b), g.ResolveWorldPos(a));
    const Vec3f r2 = sim::ToCameraRelative(g.ResolveWorldPos(b), g.ResolveWorldPos(a));
    CHECK_EQ(r1.x, r2.x);
    CHECK_EQ(r1.y, r2.y);
    CHECK_EQ(r1.z, r2.z);
}
