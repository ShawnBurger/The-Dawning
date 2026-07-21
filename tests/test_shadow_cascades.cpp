// =============================================================================
// tests/test_shadow_cascades.cpp - Cascaded shadow map geometry
// =============================================================================
// These call the SHIPPED core::ShadowCascade* functions, not a reimplementation.
// That distinction is the whole reason src/core/shadow_cascades.{h,cpp} exists as
// a GPU-free module: tests/test_math.cpp already has two cases that read as
// shadow-matrix tests but rebuild LookAt*OrthoLH inline, so they cover
// core::Mat4x4 and would keep passing if the light-matrix code were deleted.
//
// Each case names the failure mode it exists to catch. Every "NEGATIVE TEST RUN"
// note below was ACTUALLY EXECUTED against this suite - the shipped source was
// mutated, the test target rebuilt, and the failing cases recorded - not asserted
// from reading the code. The failed-check counts are the observed ones.
// =============================================================================

#include "test_framework.h"

#include "core/shadow_cascades.h"
#include "core/types.h"

#include <cmath>
#include <vector>

namespace
{

// The light basis, rederived exactly as BuildShadowCascadeMatrix and
// Mat4x4::LookAt derive it. Used only to construct probe points in LIGHT space,
// which is what makes the containment tests independent of camera orientation.
struct LightBasis
{
    core::Vec3f x;
    core::Vec3f y;
    core::Vec3f z;
};

LightBasis MakeLightBasis(const core::Vec3f& lightDir)
{
    const core::Vec3f up = (std::fabs(lightDir.y) > 0.99f)
                               ? core::Vec3f(0.0f, 0.0f, 1.0f)
                               : core::Vec3f(0.0f, 1.0f, 0.0f);
    LightBasis b;
    b.z = (core::Vec3f(0.0f, 0.0f, 0.0f) - lightDir).Normalized();
    b.x = up.Cross(b.z).Normalized();
    b.y = b.z.Cross(b.x);
    return b;
}

// Does a camera-relative point project inside this cascade's clip volume?
// Mirrors exactly what the pixel shader's guards test.
bool InsideClip(const core::Mat4x4& viewProj, const core::Vec3f& p)
{
    const core::Vec3f c = viewProj.TransformPoint(p);
    return c.x >= -1.0f && c.x <= 1.0f &&
           c.y >= -1.0f && c.y <= 1.0f &&
           c.z >= 0.0f && c.z <= 1.0f;
}

// A spread of directions over the sphere, deterministic (no RNG) so a failure
// reproduces exactly. Fibonacci-style spiral.
std::vector<core::Vec3f> SphereDirections(int count)
{
    std::vector<core::Vec3f> dirs;
    dirs.reserve(static_cast<size_t>(count));
    const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < count; ++i)
    {
        const double y = 1.0 - (2.0 * i) / static_cast<double>(count - 1);
        const double r = std::sqrt((std::max)(0.0, 1.0 - y * y));
        const double th = golden * i;
        dirs.push_back(core::Vec3f{ static_cast<float>(std::cos(th) * r),
                                    static_cast<float>(y),
                                    static_cast<float>(std::sin(th) * r) });
    }
    return dirs;
}

// The three light directions every geometric case is repeated over, including
// one steep enough to trip the |lightDir.y| > 0.99 up-vector switch.
const core::Vec3f kTestLightDirs[3] = {
    core::Vec3f(0.5f, 0.8f, 0.3f).Normalized(),    // the engine's default
    core::Vec3f(-0.3f, 0.4f, -0.9f).Normalized(),  // shallow, off-axis
    core::Vec3f(0.02f, 0.9995f, 0.02f).Normalized() // near-zenith: up-vector flip
};

} // namespace

// -----------------------------------------------------------------------------
// F10 - cascade 0 must not regress from the legacy single cascade.
// This is the anchor for bisecting any visual regression: if cascade 0 is
// bit-identical to what shipped before, every near-field pixel is unchanged and
// anything that moved did so at range.
// NEGATIVE TEST RUN: kShadowCascadeExtent[0] = 25.0f -> this case fails,
// 27 failed checks.
// -----------------------------------------------------------------------------
TEST_CASE(Cascades_CascadeZeroMatchesLegacySingleCascade)
{
    CHECK_APPROX(core::kShadowCascadeExtent[0], 24.0f);
    CHECK_APPROX(core::ShadowCascadeDepthRange(0), 120.0f);
    CHECK_APPROX(core::ShadowCascadeTexelWorld(0), 48.0f / 2048.0f);

    for (const core::Vec3f& dir : kTestLightDirs)
    {
        const core::Vec3f up = (std::fabs(dir.y) > 0.99f)
                                   ? core::Vec3f(0.0f, 0.0f, 1.0f)
                                   : core::Vec3f(0.0f, 1.0f, 0.0f);
        const core::Mat4x4 legacy =
            core::Mat4x4::LookAt(dir * 60.0f, core::Vec3f(0.0f, 0.0f, 0.0f), up) *
            core::Mat4x4::OrthoLH(48.0f, 48.0f, 0.1f, 120.0f);

        // cameraPosition zero => the snap residual is zero => the matrix must be
        // elementwise identical to the legacy construction.
        const core::Mat4x4 built =
            core::BuildShadowCascadeMatrix(dir, 0, core::Vec3d{});

        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 4; ++col)
                CHECK_APPROX(built.m[r][col], legacy.m[r][col]);
    }
}

// -----------------------------------------------------------------------------
// F12 - the cascade table degenerates (all the same size, or reversed).
// Consecutive texel ratios must sit in (1, 8]: above 1 or the cascades do not
// grow, above 8 and the resolution step is visible as a hard seam.
// NEGATIVE TEST RUN: kShadowCascadeExtent = {24,24,24,24} -> this case fails
// (9 checks) and Cascades_SelectionIsNotStuckAtZero fails with it (18 checks).
// -----------------------------------------------------------------------------
TEST_CASE(Cascades_TexelSizeIsStrictlyIncreasingAndBounded)
{
    for (uint32_t c = 1; c < core::kShadowCascadeCount; ++c)
    {
        const float prev = core::ShadowCascadeTexelWorld(c - 1);
        const float cur  = core::ShadowCascadeTexelWorld(c);
        CHECK(cur > prev);
        const float ratio = cur / prev;
        CHECK(ratio > 1.0f);
        CHECK(ratio <= 8.0f);
    }

    // Split radii must be strictly increasing too, or selection is ill-defined.
    for (uint32_t c = 1; c < core::kShadowCascadeCount; ++c)
        CHECK(core::ShadowCascadeSplitRadius(c) > core::ShadowCascadeSplitRadius(c - 1));

    // Depth range tracks extent, which is what lets one shadow PSO serve all
    // four cascades with a single depth bias.
    for (uint32_t c = 0; c < core::kShadowCascadeCount; ++c)
        CHECK_APPROX(core::ShadowCascadeDepthRange(c),
                     core::kShadowCascadeExtent[c] * 5.0f);
}

// -----------------------------------------------------------------------------
// The fade table must leave a non-empty single-boundary band. If one band starts
// before the previous split, a pixel can belong to two transitions at once and
// the shader's two-sample contract no longer forms a partition of unity.
// -----------------------------------------------------------------------------
TEST_CASE(Cascades_FadeBandsAreOrderedAndDisjoint)
{
    for (uint32_t c = 0; c < core::kShadowCascadeCount; ++c)
    {
        const float fadeLo = core::ShadowCascadeFadeLo(c);
        const float split = core::ShadowCascadeSplitRadius(c);
        CHECK(fadeLo >= 0.0f);
        CHECK(fadeLo < split);
        if (c > 0)
            CHECK(fadeLo > core::ShadowCascadeSplitRadius(c - 1u));
    }
}

TEST_CASE(Cascades_BlendWeightsPartitionUnityAcrossEveryBoundary)
{
    for (uint32_t c = 0; c < core::kShadowCascadeCount; ++c)
    {
        const float fadeLo = core::ShadowCascadeFadeLo(c);
        const float split = core::ShadowCascadeSplitRadius(c);

        for (int step = 0; step <= 64; ++step)
        {
            const float t = static_cast<float>(step) / 64.0f;
            const float radius = fadeLo + (split - fadeLo) * t;
            const core::ShadowCascadeBlend blend =
                core::ComputeShadowCascadeBlend(radius);

            CHECK(blend.primaryWeight >= 0.0f);
            CHECK(blend.secondaryWeight >= 0.0f);
            CHECK(blend.litWeight >= 0.0f);
            CHECK_APPROX_EPS(blend.primaryWeight + blend.secondaryWeight +
                                 blend.litWeight,
                             1.0f, 1e-6f);

            if (step < 64)
            {
                CHECK_EQ(blend.primaryCascade, c);
                if (c + 1u < core::kShadowCascadeCount)
                {
                    CHECK_EQ(blend.secondaryCascade, c + 1u);
                    CHECK_APPROX(blend.litWeight, 0.0f);
                }
                else
                {
                    CHECK_EQ(blend.secondaryCascade, c);
                    CHECK_APPROX(blend.secondaryWeight, 0.0f);
                }
            }
        }
    }
}

TEST_CASE(Cascades_BlendEndpointsAreContinuous)
{
    for (uint32_t c = 0; c < core::kShadowCascadeCount; ++c)
    {
        const float fadeLo = core::ShadowCascadeFadeLo(c);
        const float split = core::ShadowCascadeSplitRadius(c);
        const float middle = fadeLo + (split - fadeLo) * 0.5f;

        const core::ShadowCascadeBlend atStart =
            core::ComputeShadowCascadeBlend(fadeLo);
        CHECK_EQ(atStart.primaryCascade, c);
        CHECK_APPROX(atStart.primaryWeight, 1.0f);
        CHECK_APPROX(atStart.secondaryWeight, 0.0f);
        CHECK_APPROX(atStart.litWeight, 0.0f);

        const core::ShadowCascadeBlend atMiddle =
            core::ComputeShadowCascadeBlend(middle);
        CHECK_APPROX_EPS(atMiddle.primaryWeight, 0.5f, 1e-6f);
        if (c + 1u < core::kShadowCascadeCount)
            CHECK_APPROX_EPS(atMiddle.secondaryWeight, 0.5f, 1e-6f);
        else
            CHECK_APPROX_EPS(atMiddle.litWeight, 0.5f, 1e-6f);

        const core::ShadowCascadeBlend atSplit =
            core::ComputeShadowCascadeBlend(split);
        if (c + 1u < core::kShadowCascadeCount)
        {
            CHECK_EQ(atSplit.primaryCascade, c + 1u);
            CHECK_APPROX(atSplit.primaryWeight, 1.0f);
            CHECK_APPROX(atSplit.secondaryWeight, 0.0f);
            CHECK_APPROX(atSplit.litWeight, 0.0f);
        }
        else
        {
            CHECK_APPROX(atSplit.primaryWeight, 0.0f);
            CHECK_APPROX(atSplit.secondaryWeight, 0.0f);
            CHECK_APPROX(atSplit.litWeight, 1.0f);
        }
    }

    const core::ShadowCascadeBlend negative = core::ComputeShadowCascadeBlend(-1.0f);
    CHECK_EQ(negative.primaryCascade, 0u);
    CHECK_APPROX(negative.primaryWeight, 1.0f);

    const core::ShadowCascadeBlend beyond = core::ComputeShadowCascadeBlend(1.0e9f);
    CHECK_APPROX(beyond.primaryWeight, 0.0f);
    CHECK_APPROX(beyond.secondaryWeight, 0.0f);
    CHECK_APPROX(beyond.litWeight, 1.0f);
}

// -----------------------------------------------------------------------------
// F3 - cascade selection stuck at 0.
// The probe is built in LIGHT space, so |lightSpaceX| exceeds cascade 0's extent
// BY CONSTRUCTION, independent of camera or light orientation.
// The CHECK_FALSE clause is the load-bearing one: inclusion alone is satisfied
// by a selector hardwired to 0.
// NEGATIVE TEST RUN: hardwire SelectShadowCascade to return 0 -> this case fails
// (9 checks) along with Cascades_PartitionHasNoGapOrOverlap (2 checks), while
// EVERY GPU smoke marker stays green. That gap is the point: four green cascade
// markers are consistent with a selector that never leaves cascade 0.
// -----------------------------------------------------------------------------
TEST_CASE(Cascades_SelectionIsNotStuckAtZero)
{
    for (const core::Vec3f& dir : kTestLightDirs)
    {
        const LightBasis basis = MakeLightBasis(dir);

        for (uint32_t c = 1; c < core::kShadowCascadeCount; ++c)
        {
            // Just inside cascade c's split radius, pushed along the light-space
            // X axis so it is provably outside cascade c-1's XY footprint.
            const float r = core::ShadowCascadeSplitRadius(c) * 0.99f;
            const core::Vec3f p = basis.x * r;

            const core::Mat4x4 mThis =
                core::BuildShadowCascadeMatrix(dir, c, core::Vec3d{});
            const core::Mat4x4 mPrev =
                core::BuildShadowCascadeMatrix(dir, c - 1, core::Vec3d{});

            CHECK(InsideClip(mThis, p));
            CHECK_FALSE(InsideClip(mPrev, p));
            CHECK_EQ(core::SelectShadowCascade(p.Length()), c);
        }
    }
}

// -----------------------------------------------------------------------------
// F4 - split distances off by one, or `<` vs `<=` drift.
// That drift renders each cascade correctly and leaves a one-sample-wide
// unshadowed ring at a fixed distance - orders of magnitude below every
// threshold the smoke harness applies.
// NEGATIVE TEST RUN: compare against split[3] instead of split[2] -> this case
// fails (1 check) along with Cascades_SelectionIsNotStuckAtZero (3 checks).
// -----------------------------------------------------------------------------
TEST_CASE(Cascades_PartitionHasNoGapOrOverlap)
{
    const uint32_t last = core::kShadowCascadeCount - 1u;
    const float maxR = core::ShadowCascadeSplitRadius(last) * 1.1f;
    const int steps = 4096;

    uint32_t previous = 0;
    CHECK_EQ(core::SelectShadowCascade(0.0f), 0u);

    bool nonDecreasing = true;
    bool boundsHold = true;
    for (int i = 0; i <= steps; ++i)
    {
        const float r = maxR * (static_cast<float>(i) / static_cast<float>(steps));
        const uint32_t c = core::SelectShadowCascade(r);

        if (c < previous) nonDecreasing = false;
        previous = c;

        // The returned cascade must be the tightest one containing r.
        if (c < last && !(r < core::ShadowCascadeSplitRadius(c))) boundsHold = false;
        if (c > 0 && !(r >= core::ShadowCascadeSplitRadius(c - 1))) boundsHold = false;
    }
    CHECK(nonDecreasing);
    CHECK(boundsHold);
    CHECK_EQ(core::SelectShadowCascade(maxR), last);
}

// -----------------------------------------------------------------------------
// F5 - the containment theorem breaks (margin too small, or the snap
// displacement is unaccounted for).
// Run at cameraPos = 0 AND at a camera position chosen to produce a worst-case
// snap residual, so the assertion covers the SNAPPED matrix and not just the
// unsnapped one.
// NEGATIVE TESTS RUN: kShadowCascadeMargin = 0.95f -> this case fails (24
// checks); multiplying the snap residual by 100 -> this case fails (7 checks),
// proving the assertion sees the SNAPPED matrix and not merely the unsnapped one.
// -----------------------------------------------------------------------------
TEST_CASE(Cascades_SplitFitsInsideItsOwnFootprint)
{
    const std::vector<core::Vec3f> dirs = SphereDirections(200);

    // An awkward camera position: irrational-ish coordinates at a scale where the
    // snap residual is a substantial fraction of a texel for every cascade.
    const core::Vec3d cameraPositions[2] = {
        core::Vec3d{},
        core::Vec3d{ 1234.56789, -987.654321, 4321.98765 }
    };

    for (const core::Vec3f& lightDir : kTestLightDirs)
    {
        for (const core::Vec3d& camPos : cameraPositions)
        {
            for (uint32_t c = 0; c < core::kShadowCascadeCount; ++c)
            {
                const core::Mat4x4 vp =
                    core::BuildShadowCascadeMatrix(lightDir, c, camPos);
                const float r = core::ShadowCascadeSplitRadius(c) * 0.999f;

                bool allInside = true;
                for (const core::Vec3f& d : dirs)
                    if (!InsideClip(vp, d * r)) allInside = false;

                CHECK(allInside);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// F6 - adjacent cascades do not overlap, so a seam becomes a gap.
// Non-overlapping frusta render fine individually and produce a ring of missing
// shadow. This is the only assertion that catches it.
// NEGATIVE TEST RUN: extent[1] = split[0] * 0.9 = 20.5714f -> this case fails
// (3 checks), with TexelSize (4) and SelectionIsNotStuckAtZero (6) alongside.
// -----------------------------------------------------------------------------
TEST_CASE(Cascades_BoundaryPointIsInsideBothNeighbours)
{
    const std::vector<core::Vec3f> dirs = SphereDirections(32);

    for (const core::Vec3f& lightDir : kTestLightDirs)
    {
        for (uint32_t c = 0; c + 1 < core::kShadowCascadeCount; ++c)
        {
            const float r = core::ShadowCascadeSplitRadius(c);
            const core::Mat4x4 vpLo =
                core::BuildShadowCascadeMatrix(lightDir, c, core::Vec3d{});
            const core::Mat4x4 vpHi =
                core::BuildShadowCascadeMatrix(lightDir, c + 1, core::Vec3d{});

            bool bothContain = true;
            for (const core::Vec3f& d : dirs)
            {
                if (!InsideClip(vpLo, d * r)) bothContain = false;
                if (!InsideClip(vpHi, d * r)) bothContain = false;
            }
            CHECK(bothContain);
        }
    }
}

// -----------------------------------------------------------------------------
// F7 - RULE 1 regression. The single highest-value case here, because the most
// probable future edit is someone threading the absolute Vec3d camera position
// in to "centre the cascades on the camera": correct at the origin, catastrophic
// at planetary distance, and invisible in a demo that sits near the origin.
//
// NEGATIVE TEST RUN: replace (Xd*dx + Yd*dy).ToFloat() with a form that
// reconstructs the absolute snapped position and narrows THAT -
// (Xd*floor(lx/t)*t + Yd*floor(ly/t)*t).ToFloat(). This case fails with 80
// failed checks, taking SplitFitsInsideItsOwnFootprint (12) and
// SnapIsStableUnderSubTexelCameraMotion (1) with it - while the cameraPos = 0
// half still passes, which is exactly why the far case must be here.
// Follows tests/test_math.cpp:613's template.
// -----------------------------------------------------------------------------
TEST_CASE(Cascades_MatricesAreCameraPositionInvariant)
{
    const core::Vec3d nearOrigin{};
    const core::Vec3d farAway{ 1e7, 1e7, 1e7 };

    // A world shift of t along xAxis moves clip x by t/extent, and t is at most
    // one texel = 2*extent/mapSize, so the translation row may move by at most
    // 2/mapSize in the XY columns.
    const double translationTolerance =
        2.0 / static_cast<double>(core::kShadowMapSize) + 1e-5;

    for (const core::Vec3f& lightDir : kTestLightDirs)
    {
        for (uint32_t c = 0; c < core::kShadowCascadeCount; ++c)
        {
            const core::Mat4x4 a =
                core::BuildShadowCascadeMatrix(lightDir, c, nearOrigin);
            const core::Mat4x4 b =
                core::BuildShadowCascadeMatrix(lightDir, c, farAway);

            // Rows 0..2 are the rotation/scale block. These cannot depend on the
            // camera position AT ALL.
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 4; ++col)
                    CHECK_APPROX_EPS(a.m[r][col], b.m[r][col], 1e-6);

            // Row 3 is the translation. It may move by at most one snapped texel
            // in the light-space XY columns, and not at all in Z/W.
            CHECK_APPROX_EPS(a.m[3][0], b.m[3][0], translationTolerance);
            CHECK_APPROX_EPS(a.m[3][1], b.m[3][1], translationTolerance);
            CHECK_APPROX_EPS(a.m[3][2], b.m[3][2], 1e-6);
            CHECK_APPROX_EPS(a.m[3][3], b.m[3][3], 1e-6);

            // Nothing may be NaN or Inf at planetary distance.
            for (int r = 0; r < 4; ++r)
                for (int col = 0; col < 4; ++col)
                    CHECK(std::isfinite(b.m[r][col]));
        }
    }
}

// -----------------------------------------------------------------------------
// F8 - the snap does not actually snap (shimmer).
// Nothing else in this project can see this, and getting it wrong does not
// crash: it produces edge crawl that reads as "the shadow map needs more
// resolution".
//
// Fix an ABSOLUTE world point, sweep the camera in sub-texel steps, and project
// that point through cascade 0 each time. If the lattice is anchored to the
// world origin the projected coordinate takes only a handful of distinct values
// across the sweep; if it is not, it takes a new value at every step.
//
// NEGATIVE TEST 1 RUN: drop the two std::floor() calls -> this case fails.
// NEGATIVE TEST 2 RUN (the important one): snap the camera-RELATIVE centre
// instead of the absolute one, i.e. quantise a value that is identically zero.
// That is the plausible-looking wrong implementation which produces no compile
// error, no crash, and full shimmer -> this case fails, and it is the ONLY case
// in the suite that does. Nothing else in this project can see that mistake.
// -----------------------------------------------------------------------------
TEST_CASE(Cascades_SnapIsStableUnderSubTexelCameraMotion)
{
    const core::Vec3f lightDir = kTestLightDirs[0];
    const LightBasis basis = MakeLightBasis(lightDir);

    const core::Vec3d W{ 3.0, 1.0, 5.0 };   // a fixed ABSOLUTE world point
    const double texel = static_cast<double>(core::ShadowCascadeTexelWorld(0));
    const double step  = texel / 64.0;      // sub-texel
    const int    steps = 200;               // ~3 texels of total travel

    std::vector<float> projected;
    projected.reserve(static_cast<size_t>(steps) + 1);

    for (int i = 0; i <= steps; ++i)
    {
        const core::Vec3d camPos =
            core::Vec3d::FromFloat(basis.x) * (step * i);
        const core::Mat4x4 vp =
            core::BuildShadowCascadeMatrix(lightDir, 0, camPos);

        // The camera-relative position of the fixed world point.
        const core::Vec3f rel = (W - camPos).ToFloat();
        projected.push_back(vp.TransformPoint(rel).x);
    }

    // Count distinct values at 1e-6 tolerance.
    int distinct = 0;
    for (size_t i = 0; i < projected.size(); ++i)
    {
        bool seen = false;
        for (size_t j = 0; j < i; ++j)
            if (std::fabs(projected[i] - projected[j]) <= 1e-6f) { seen = true; break; }
        if (!seen) ++distinct;
    }

    // A perfectly anchored lattice gives one value per texel crossed (~3), plus
    // slack for float rounding in the projection itself. 200 means no snap.
    CHECK(distinct <= 8);
}
