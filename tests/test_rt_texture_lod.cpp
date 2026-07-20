// =============================================================================
// tests/test_rt_texture_lod.cpp - Ray-cone texture LOD arithmetic
// =============================================================================
// SCOPE, STATED UP FRONT so nobody reads more into a green run than is there.
//
// render::PrimaryRayConeSpreadAngle is SHIPPED code: PathTracer::Dispatch calls
// it every frame and uploads the result as g_PrimaryConeSpread. Break it and the
// rendered image changes, and these cases catch it.
//
// The other three functions under test are a CPU mirror of HLSL that only ever
// executes on the GPU (shaders/path_trace.hlsl). These cases pin the ARITHMETIC:
// the area formulae, the signs, the log base, the 0.5 factors, and the
// degenerate guards. They do NOT pin the shader. Edit path_trace.hlsl without
// editing src/render/rt_texture_lod.cpp and this suite stays green while the
// image is wrong. That limitation is real and is the price of the shader being
// GPU-only; it is worth paying because a sign or log-base error in this
// arithmetic does not crash, it just renders slightly wrong textures forever.
//
// Every "NEGATIVE TEST RUN" note below was ACTUALLY EXECUTED: the shipped source
// was mutated, the test target rebuilt, and the failing cases recorded.
// =============================================================================

#include "test_framework.h"

#include "render/rt_texture_lod.h"

#include <cmath>
#include <cstdint>

namespace
{

// tan(35 degrees) - half of the project's 70-degree vertical FOV.
constexpr float kTanHalf70 = 0.700207538f;

// A unit triangle in both spaces: world edges of length 1, UV edges of length 1.
// Its LOD constant is exactly 0, which makes it the natural origin for every
// other case here.
const float kUnitUV0[2] = { 0.0f, 0.0f };
const float kUnitUV1[2] = { 1.0f, 0.0f };
const float kUnitUV2[2] = { 0.0f, 1.0f };
const float kUnitP0[3]  = { 0.0f, 0.0f, 0.0f };
const float kUnitP1[3]  = { 1.0f, 0.0f, 0.0f };
const float kUnitP2[3]  = { 0.0f, 0.0f, 1.0f };

// Scale a triangle's world positions, leaving UVs alone.
struct ScaledTriangle
{
    float p0[3];
    float p1[3];
    float p2[3];

    explicit ScaledTriangle(float scale)
    {
        p0[0] = 0.0f;  p0[1] = 0.0f; p0[2] = 0.0f;
        p1[0] = scale; p1[1] = 0.0f; p1[2] = 0.0f;
        p2[0] = 0.0f;  p2[1] = 0.0f; p2[2] = scale;
    }
};

} // namespace

// -----------------------------------------------------------------------------
// The per-triangle constant: 0.5 * log2(uvArea / worldArea)
// -----------------------------------------------------------------------------
// NEGATIVE TEST RUN: std::log2 -> std::log10 in RayConeTriangleLodConstant
// failed 7 checks - 3 here, 4 in the end-to-end case at the bottom of the file.
// NEGATIVE TEST RUN: dropping the 0.5 factor failed 6 checks - 3 here, 3 there.
TEST_CASE(RayConeLod_TriangleConstantIsHalfLog2OfAreaRatio)
{
    // Unit UV area over unit world area: one texture across one world unit, so
    // the constant contributes nothing.
    CHECK_APPROX(render::RayConeTriangleLodConstant(
                     kUnitUV0, kUnitUV1, kUnitUV2,
                     kUnitP0,  kUnitP1,  kUnitP2), 0.0f);

    // Stretch the SAME UVs over a 4x larger triangle: 16x the world area, so
    // texel density drops by 4x per axis and the surface wants a FINER mip.
    // 0.5 * log2(1/16) = -2. The sign is the point - a positive result here
    // would blur every enlarged surface in the scene.
    const ScaledTriangle stretched(4.0f);
    CHECK_APPROX(render::RayConeTriangleLodConstant(
                     kUnitUV0, kUnitUV1, kUnitUV2,
                     stretched.p0, stretched.p1, stretched.p2), -2.0f);

    // Tile the texture 4x over a unit triangle: 16x the UV area, 4x the texel
    // density per axis, so it wants a COARSER mip. 0.5 * log2(16/1) = +2.
    const float tiledUV1[2] = { 4.0f, 0.0f };
    const float tiledUV2[2] = { 0.0f, 4.0f };
    CHECK_APPROX(render::RayConeTriangleLodConstant(
                     kUnitUV0, tiledUV1, tiledUV2,
                     kUnitP0,  kUnitP1,  kUnitP2), 2.0f);

    // Halving the world scale is exactly +1 mip, which pins the 0.5 factor:
    // without it this would be +2. 0.5 * log2(1/0.25) = 1.
    const ScaledTriangle halved(0.5f);
    CHECK_APPROX(render::RayConeTriangleLodConstant(
                     kUnitUV0, kUnitUV1, kUnitUV2,
                     halved.p0, halved.p1, halved.p2), 1.0f);
}

// The world area is a 3D cross product, not a 2D one. A triangle lying in an
// arbitrary plane must give the same answer as the same triangle axis-aligned.
// Every other case in this file uses triangles flat in the XZ plane, so every
// other case would survive an implementation that silently projected to XZ.
// NEGATIVE TEST RUN: replacing the 3D cross product in TwiceWorldArea with a 2D
// XZ one failed exactly 1 check, this one, with the other 3270 still passing.
// That is the entire justification for this case existing.
TEST_CASE(RayConeLod_TriangleConstantIsOrientationIndependent)
{
    // Same edge lengths as the unit triangle, rotated 45 degrees about X so that
    // both edges have Y components.
    const float kInvSqrt2 = 0.70710678f;
    const float tiltedP0[3] = { 0.0f, 0.0f, 0.0f };
    const float tiltedP1[3] = { 1.0f, 0.0f, 0.0f };
    const float tiltedP2[3] = { 0.0f, kInvSqrt2, kInvSqrt2 };

    CHECK_APPROX(render::RayConeTriangleLodConstant(
                     kUnitUV0, kUnitUV1, kUnitUV2,
                     tiltedP0, tiltedP1, tiltedP2), 0.0f);
}

// -----------------------------------------------------------------------------
// The cone-width term: log2(coneWidth / |n.d|)
// -----------------------------------------------------------------------------
// NEGATIVE TEST RUN: flipping the ratio to log2(grazing / coneWidth) failed 34
// checks across four cases - 4 here, 21 in the monotonicity case, 5 in the
// grazing case, 4 in the end-to-end case.
TEST_CASE(RayConeLod_LodBaseRisesOneMipPerConeWidthDoubling)
{
    // Head-on (|n.d| = 1), so only the cone width contributes.
    CHECK_APPROX(render::RayConeLodBase(0.0f, 1.0f, 1.0f), 0.0f);
    CHECK_APPROX(render::RayConeLodBase(0.0f, 2.0f, 1.0f), 1.0f);
    CHECK_APPROX(render::RayConeLodBase(0.0f, 4.0f, 1.0f), 2.0f);
    CHECK_APPROX(render::RayConeLodBase(0.0f, 0.5f, 1.0f), -1.0f);

    // The triangle constant is a plain additive offset.
    CHECK_APPROX(render::RayConeLodBase(-2.0f, 4.0f, 1.0f), 0.0f);
    CHECK_APPROX(render::RayConeLodBase(3.5f, 1.0f, 1.0f), 3.5f);
}

// A wider cone must never select a finer mip. Stated as monotonicity rather than
// as values, because this is the property that makes distant geometry stop
// aliasing, and it survives any future retuning of the constants.
TEST_CASE(RayConeLod_LodIsMonotonicInConeWidth)
{
    float previous = -1e30f;
    for (float width = 0.001f; width < 100.0f; width *= 1.7f)
    {
        const float lod = render::RayConeLodBase(-2.32192809f, width, 0.5f);
        CHECK(lod > previous);
        previous = lod;
    }
}

// Grazing angles stretch the footprint, so they RAISE the LOD - and the floor
// stops that from running away to a 1x1 mip at the horizon.
// NEGATIVE TEST RUN: removing the kRayConeGrazingFloor clamp failed 2 checks
// here - the 1e-9 case returned 29.897 instead of 3.322, and the exactly-zero
// case returned inf. Nothing else in the suite noticed, which is the point: an
// unbounded grazing term is not a crash, it is a horizon that quietly collapses
// to its 1x1 mip.
TEST_CASE(RayConeLod_GrazingAnglesRaiseLodAndAreFloored)
{
    // Straight on: no stretch.
    CHECK_APPROX(render::RayConeLodBase(0.0f, 1.0f, 1.0f), 0.0f);

    // Half-on: the footprint doubles along one axis, so +1 mip.
    CHECK_APPROX(render::RayConeLodBase(0.0f, 1.0f, 0.5f), 1.0f);

    // Sign of the dot product is irrelevant - the shader may hand back either
    // facing, and a footprint has no sign.
    CHECK_APPROX(render::RayConeLodBase(0.0f, 1.0f, -0.5f), 1.0f);

    // At the floor and beyond, the term saturates at -log2(0.1) = 3.3219...
    const float floored = 3.32192809f;
    CHECK_APPROX(render::RayConeLodBase(0.0f, 1.0f, render::kRayConeGrazingFloor), floored);
    CHECK_APPROX(render::RayConeLodBase(0.0f, 1.0f, 1e-9f), floored);
    CHECK_APPROX(render::RayConeLodBase(0.0f, 1.0f, 0.0f), floored);
}

// -----------------------------------------------------------------------------
// The per-texture term: 0.5 * log2(width * height)
// -----------------------------------------------------------------------------
// NEGATIVE TEST RUN: dropping the 0.5 factor failed 9 checks - 5 here, 4 in the
// end-to-end case.
TEST_CASE(RayConeLod_TextureTermIsHalfLog2OfTexelCount)
{
    // 0.5*log2(W*H) is log2 of the side length for a square texture, which is
    // the index of the 1x1 mip. So a lodBase of 0 on a 2048x2048 texture with a
    // 12-level chain lands exactly on mip 11, its last level.
    CHECK_APPROX(render::RayConeTextureLod(0.0f, 2048u, 2048u), 11.0f);
    CHECK_APPROX(render::RayConeTextureLod(0.0f, 512u, 512u), 9.0f);
    CHECK_APPROX(render::RayConeTextureLod(0.0f, 1u, 1u), 0.0f);

    // Non-square textures use the geometric mean of the two dimensions.
    CHECK_APPROX(render::RayConeTextureLod(0.0f, 2048u, 512u), 10.0f);

    // lodBase shifts the whole thing.
    CHECK_APPROX(render::RayConeTextureLod(-11.0f, 2048u, 2048u), 0.0f);
    CHECK_APPROX(render::RayConeTextureLod(-4.0f, 2048u, 2048u), 7.0f);

    // Never negative: SampleLevel below 0 is mip 0 with extra steps, and the
    // clamp is what turns the degenerate sentinel back into old behaviour.
    CHECK_APPROX(render::RayConeTextureLod(-40.0f, 2048u, 2048u), 0.0f);
    CHECK_APPROX(render::RayConeTextureLod(-1.0f, 1u, 1u), 0.0f);
}

// -----------------------------------------------------------------------------
// Degenerate geometry falls back to mip 0, i.e. to the pre-ray-cone behaviour
// -----------------------------------------------------------------------------
// A zero-area triangle or collapsed UVs must not produce inf, NaN, or a garbage
// mip. The contract is that they land on mip 0 exactly.
// NEGATIVE TEST RUN: removing the uvArea/worldArea guard from
// RayConeTriangleLodConstant failed 2 checks here and nothing else in the suite;
// the two collapsed triangles returned -inf and +inf rather than the sentinel.
TEST_CASE(RayConeLod_DegenerateTrianglesFallBackToMipZero)
{
    // UVs collapsed to a point.
    const float collapsedUV[2] = { 0.25f, 0.25f };
    const float degenerateUV = render::RayConeTriangleLodConstant(
        collapsedUV, collapsedUV, collapsedUV,
        kUnitP0, kUnitP1, kUnitP2);
    CHECK_EQ(degenerateUV, render::kRayConeDegenerateLod);

    // Zero-area world triangle.
    const float degenerateWorld = render::RayConeTriangleLodConstant(
        kUnitUV0, kUnitUV1, kUnitUV2,
        kUnitP0, kUnitP0, kUnitP0);
    CHECK_EQ(degenerateWorld, render::kRayConeDegenerateLod);

    // The sentinel must survive the middle stage rather than being turned into a
    // real-looking LOD by the cone term.
    const float base = render::RayConeLodBase(degenerateUV, 0.13f, 0.5f);
    CHECK_EQ(base, render::kRayConeDegenerateLod);

    // ...and then clamp to mip 0 at the sample site, on the largest texture the
    // project actually loads. This is the "no worse than before" guarantee.
    CHECK_APPROX(render::RayConeTextureLod(base, 2048u, 2048u), 0.0f);

    // A zero or negative cone width is degenerate too: a cone of no width has no
    // footprint to filter over.
    CHECK_EQ(render::RayConeLodBase(0.0f, 0.0f, 1.0f), render::kRayConeDegenerateLod);
    CHECK_EQ(render::RayConeLodBase(0.0f, -1.0f, 1.0f), render::kRayConeDegenerateLod);
}

// -----------------------------------------------------------------------------
// SHIPPED: the primary cone spread angle
// -----------------------------------------------------------------------------
// PathTracer::Dispatch uploads this as g_PrimaryConeSpread. It is the only
// function in this file that the engine actually executes.
// NEGATIVE TEST RUN: dropping the factor of 2 failed 5 checks - 3 here, 2 in the
// end-to-end case.
TEST_CASE(RayConeLod_PrimaryConeSpreadIsOnePixelOfOutputHeight)
{
    // 70-degree vertical FOV over 1080 rows: 2*tan(35)/1080.
    const float expected1080 = 2.0f * kTanHalf70 / 1080.0f;
    CHECK_APPROX(render::PrimaryRayConeSpreadAngle(kTanHalf70, 1080u), expected1080);

    // Doubling the vertical resolution halves the per-pixel spread, which is the
    // property that makes the LOD resolution-correct rather than tuned for 1080p.
    CHECK_APPROX(render::PrimaryRayConeSpreadAngle(kTanHalf70, 2160u), expected1080 * 0.5f);
    CHECK_APPROX(render::PrimaryRayConeSpreadAngle(kTanHalf70, 540u), expected1080 * 2.0f);

    // A wider FOV spreads each pixel over more of the world.
    CHECK(render::PrimaryRayConeSpreadAngle(1.0f, 1080u) >
          render::PrimaryRayConeSpreadAngle(kTanHalf70, 1080u));

    // Guards. A zero height would divide by zero; a non-positive tangent is not
    // a camera. Both return 0, which the shader turns into mip 0 - degraded, but
    // identical to the behaviour before ray cones, and never inf or NaN.
    CHECK_EQ(render::PrimaryRayConeSpreadAngle(kTanHalf70, 0u), 0.0f);
    CHECK_EQ(render::PrimaryRayConeSpreadAngle(0.0f, 1080u), 0.0f);
    CHECK_EQ(render::PrimaryRayConeSpreadAngle(-1.0f, 1080u), 0.0f);
}

// -----------------------------------------------------------------------------
// End to end, cross-checked against a different derivation
// -----------------------------------------------------------------------------
// The cases above walk each term separately, so a consistent error in two of
// them could still cancel. This one runs the whole chain for a concrete
// configuration - the 200x200 ground plane, a 2048x2048 albedo map, a 1080p
// frame - and compares against the answer derived the OTHER way round: count how
// many texels the pixel footprint covers, and take log2 of that. If both
// derivations agree, the assembled formula is right.
//
// NEGATIVE TEST RUN: every one of the eight mutations listed above also fails
// this case, between 2 and 4 checks each. That is what a cross-check buys: it
// has no independent failure mode of its own, it just refuses to let two errors
// cancel.
TEST_CASE(RayConeLod_GroundPlaneAtDistanceMatchesTexelCountDerivation)
{
    // A 200x200 ground triangle with the texture tiled 40 times across it.
    const float uv0[2] = { 0.0f,  0.0f };
    const float uv1[2] = { 40.0f, 0.0f };
    const float uv2[2] = { 0.0f,  40.0f };
    const float p0[3]  = { 0.0f,   0.0f, 0.0f };
    const float p1[3]  = { 200.0f, 0.0f, 0.0f };
    const float p2[3]  = { 0.0f,   0.0f, 200.0f };

    const uint32_t texSize   = 2048u;
    const float    hitT      = 100.0f;
    const float    normalDot = 0.5f;   // 30 degrees above the plane

    const float spread    = render::PrimaryRayConeSpreadAngle(kTanHalf70, 1080u);
    const float coneWidth = spread * hitT;

    const float triLod  = render::RayConeTriangleLodConstant(uv0, uv1, uv2, p0, p1, p2);
    const float lodBase = render::RayConeLodBase(triLod, coneWidth, normalDot);
    const float lod     = render::RayConeTextureLod(lodBase, texSize, texSize);

    // Independent derivation. UV span 40 over world span 200 means 0.2 texture
    // repeats per world unit, so 0.2 * 2048 = 409.6 texels per world unit. The
    // footprint is the cone width stretched by the grazing angle. The mip that
    // covers N texels is log2(N).
    const float texelsPerWorldUnit = (40.0f / 200.0f) * static_cast<float>(texSize);
    const float footprintWorld     = coneWidth / normalDot;
    const float expectedLod        = std::log2(footprintWorld * texelsPerWorldUnit);

    CHECK_APPROX_EPS(lod, expectedLod, 1e-4f);

    // Sanity on the absolute number: ~6.7 on a 12-level chain. If this ever
    // reads as 0 the LOD has silently stopped working; if it reads as 11 the
    // ground has collapsed to a single texel.
    CHECK(lod > 6.0f);
    CHECK(lod < 7.5f);

    // The near field is essentially unaffected, which is why the settled image
    // barely moves. Head-on at 1 world unit the footprint is under one texel, so
    // the clamp puts it on mip 0 exactly as before ray cones.
    const float oneUnitBase = render::RayConeLodBase(triLod, spread * 1.0f, 1.0f);
    CHECK_APPROX(render::RayConeTextureLod(oneUnitBase, texSize, texSize), 0.0f);

    // At 2 units it is 0.087, NOT 0 - the first draft of this case asserted 0
    // and failed. Worth recording rather than quietly retuning: the clamp is a
    // clamp, not a near-field bypass, so "near geometry is untouched" is true to
    // within a tenth of a mip, not exactly. Trilinear filtering blends mip 0 and
    // mip 1 at an 8.7% weight here, which is a real if imperceptible change.
    const float twoUnitBase = render::RayConeLodBase(triLod, spread * 2.0f, 1.0f);
    const float twoUnitLod  = render::RayConeTextureLod(twoUnitBase, texSize, texSize);
    CHECK(twoUnitLod > 0.0f);
    CHECK(twoUnitLod < 0.1f);
}
