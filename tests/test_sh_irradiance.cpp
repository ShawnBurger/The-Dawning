// =============================================================================
// tests/test_sh_irradiance.cpp - the SH diffuse projection, CPU side
// =============================================================================
// IBL_DESIGN.md section 11 Stage 2. These are the device-free half of the
// evidence for spherical-harmonic diffuse IBL; the other half is the
// [SMOKE] ibl_sh_agreement GPU probe, which witnesses that the SHIPPED HLSL
// evaluator agrees with core::EvaluateIrradiance on the SHIPPED coefficients.
// Neither covers the other:
//
//   these cases      prove the maths is right, densely, over the whole sphere,
//                    on any machine including one with no GPU
//   the GPU probe    proves the HLSL basis is the same basis, which no CPU test
//                    can see, because the HLSL is not linked into this binary
//
// Every case below is a closed-form identity, not a golden value. Nothing here
// is calibrated against a measurement, so nothing here can be "re-tuned" green.
// =============================================================================

#include "test_framework.h"

#include <cmath>

#include "core/sh_irradiance.h"
#include "core/sky_radiance.h"

namespace
{

constexpr float kPi = 3.14159265358979323846f;

// A deterministic, non-degenerate normal set. Used as the query set for every
// evaluation case below; it is NOT the projection sample set, so a projector
// that only happened to be right at its own sample points would still fail.
core::Vec3f QueryNormal(uint32_t i, uint32_t count)
{
    const float golden = kPi * (3.0f - std::sqrt(5.0f));
    const float t      = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
    const float y      = 1.0f - 2.0f * t;
    const float r      = std::sqrt((1.0f - y * y) > 0.0f ? (1.0f - y * y) : 0.0f);
    const float theta  = golden * static_cast<float>(i) + 0.37f;   // offset off the Fibonacci lattice
    return core::Vec3f{ r * std::cos(theta), y, r * std::sin(theta) }.Normalized();
}

// L(w) = constant.
core::Vec3f ConstantRadiance(const core::Vec3f&, void* user)
{
    return *static_cast<const core::Vec3f*>(user);
}

// L(w) = a + b * (w . axis). The general linear environment: it exercises the
// WHOLE L1 band including all three signs, which a y-only sky cannot.
struct LinearField
{
    core::Vec3f a;
    core::Vec3f b;
    core::Vec3f axis;
};

core::Vec3f LinearRadiance(const core::Vec3f& d, void* user)
{
    const LinearField& f = *static_cast<const LinearField*>(user);
    const float t = d.Dot(f.axis);
    return f.a + f.b * t;
}

float MaxComponentDelta(const core::Vec3f& p, const core::Vec3f& q)
{
    const float dx = std::fabs(p.x - q.x);
    const float dy = std::fabs(p.y - q.y);
    const float dz = std::fabs(p.z - q.z);
    return (dx > dy) ? ((dx > dz) ? dx : dz) : ((dy > dz) ? dy : dz);
}

} // namespace

// =============================================================================
// 2.1 - the white furnace
// =============================================================================
// For constant radiance L the irradiance is EXACTLY pi*L in every direction, so
// diffuse = kD * albedo * E/pi = kD * albedo * L. This is the identity that pins
// the solid-angle weight in the projector and the A_l constants in the packer at
// the same time. Getting either wrong yields a plausible image at the wrong
// brightness - which nothing else in this feature would notice.
//
// WATCHED FAILING: delete the `weight` factor from ProjectRadiance (or drop the
// 4*pi and keep 1/N) and every one of the 32 directions misses by a factor of
// 4*pi. Watched again with kA0 changed from pi to 1.
TEST_CASE(Sh_WhiteFurnaceIsExactlyPiTimesRadiance)
{
    core::Vec3f L{ 0.7f, 1.3f, 0.25f };
    const core::SHColor9 packed = core::PackIrradianceCoefficients(
        core::ProjectRadiance(&ConstantRadiance, &L, core::kSHProjectionSamples));

    const core::Vec3f expected = L * kPi;

    float worst = 0.0f;
    for (uint32_t i = 0; i < 32; ++i)
    {
        const core::Vec3f E = core::EvaluateIrradiance(packed, QueryNormal(i, 32));
        const float d = MaxComponentDelta(E, expected);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1e-5f);
}

// =============================================================================
// 2.2 - a sky linear in direction.y occupies exactly two coefficients
// =============================================================================
// DawningSkyRadiance is c0 + c1*direction.y. In the basis of core::SHBasisL2 -
// whose polar axis is the THIRD component - that is the DC term Y00 and the
// y-linear term Y1-1, and nothing else. Every other coefficient must vanish.
//
// This is the direct signature of a basis mismatch between projection and
// evaluation (IBL_DESIGN.md section 4): energy leaking into a band that must be
// zero cannot happen if one basis is used consistently.
//
// It is also a statement about the SKY, so it fails loudly if someone gives the
// sky azimuthal structure - the sun-disc hazard tests/test_sky_energy.cpp
// exists for - without revisiting this feature's energy argument.
//
// WATCHED FAILING: swap out[1] and out[2] in core::SHBasisL2 - the projection
// then puts the sky's gradient into the z-linear coefficient, index 1 goes to
// zero and index 2 does not, and both halves of this case fail. Unlike a sign
// flip (see the note on the case below) a PERMUTATION does not cancel between
// projection and evaluation as far as this case is concerned, because this case
// asserts on coefficient INDICES - which is exactly the property the C++ and
// HLSL sides must agree on, since they exchange an indexed array.
//
// WATCHED FAILING: delete the solid-angle weight in ProjectRadiance - the
// unweighted band-2 residuals rise well above 1e-6 and this fails too.
TEST_CASE(Sh_LinearSkyOccupiesOnlyTwoBands)
{
    const core::SHColor9 radiance = core::ProjectSkyRadiance(core::kSHProjectionSamples);

    // The two that must carry everything.
    CHECK(std::fabs(radiance.c[0].y) > 0.1f);
    CHECK(std::fabs(radiance.c[1].y) > 1e-3f);

    float worstZero = 0.0f;
    for (uint32_t k = 2; k < core::kSHCoefficientCount; ++k)
    {
        const core::Vec3f& c = radiance.c[k];
        const float m = MaxComponentDelta(c, core::Vec3f{ 0.0f, 0.0f, 0.0f });
        if (m > worstZero) worstZero = m;
    }
    CHECK(worstZero < 1e-6f);
}

// =============================================================================
// 2.3 - a general linear environment reconstructs its analytic irradiance
// =============================================================================
// This REPLACES the rotation-invariance case IBL_DESIGN.md sketches, and it is
// strictly stronger. That case rotated a y-only sky about Y, which is a symmetry
// the sky already has - it can be satisfied by a projector that discards the
// entire L1 band. The identity below cannot.
//
// For L(w) = a + b*(w . u) the irradiance is available in closed form:
//
//     E(N) = pi*a + (2*pi/3) * b * (N . u)
//
// so the DC term and all three L1 components are checked against an analytic
// answer, with u deliberately off every axis so a permutation of out[1..3] or a
// sign flip in any one of them moves E in a direction the closed form does not.
//
// WATCHED FAILING, and the first attempt taught something worth recording.
//
// Negating out[1] in core::SHBasisL2 does NOT fail this case, and that is
// correct rather than a gap. SHBasisL2 is used by the projector AND the
// evaluator, so a sign flip appears twice and cancels exactly: L_1 goes to -L_1
// and Y_1(N) goes to -Y_1(N), and the product is unchanged. THIS IS THE PROPERTY
// IBL_DESIGN.md section 4 is relying on when it says "if projection and
// evaluation share a basis, the basis cannot be wrong" - measured here, not
// assumed. A shared-basis sign convention is unobservable, so it cannot be a
// defect.
//
// The mutation with teeth is therefore a ONE-SIDED flip, because that is the
// only kind that can actually happen in this tree: the HLSL evaluator in
// shaders/ibl_common.hlsli is a separate copy of these expressions.
//
//   WATCHED: negate basis[1] inside core::EvaluateIrradiance only (leaving the
//   projector alone) -> this case fails, and so does
//   Sh_SkyIrradianceIsPositiveEverywhere.
//   WATCHED: kA0 changed from pi to 1 -> this case fails.
//
// The C++/HLSL one-sided flip is out of reach of any test in this binary and is
// watched by the [SMOKE] ibl_sh_agreement GPU probe instead.
TEST_CASE(Sh_LinearEnvironmentMatchesAnalyticIrradiance)
{
    const core::Vec3f axes[3] = {
        core::Vec3f{  0.6f,  0.8f,  0.0f }.Normalized(),
        core::Vec3f{ -0.5f,  0.3f,  0.8f }.Normalized(),
        core::Vec3f{  0.3f, -0.7f, -0.6f }.Normalized(),
    };

    float worst = 0.0f;
    for (const core::Vec3f& axis : axes)
    {
        LinearField f{ core::Vec3f{ 0.4f, 0.5f, 0.6f },
                       core::Vec3f{ 0.2f, 0.3f, 0.1f },
                       axis };

        const core::SHColor9 packed = core::PackIrradianceCoefficients(
            core::ProjectRadiance(&LinearRadiance, &f, core::kSHProjectionSamples));

        for (uint32_t i = 0; i < 32; ++i)
        {
            const core::Vec3f N = QueryNormal(i, 32);
            // RAW, deliberately. This is a closed-form identity about the
            // reconstruction, and EvaluateIrradiance clamps to match the shader -
            // a rendering decision that would quietly convert "the maths is
            // wrong and went negative" into "the maths agrees, at zero".
            const core::Vec3f E = core::EvaluateIrradianceRaw(packed, N);

            const float t = N.Dot(axis);
            const core::Vec3f expected = f.a * kPi + f.b * ((2.0f * kPi / 3.0f) * t);

            const float d = MaxComponentDelta(E, expected);
            if (d > worst) worst = d;
        }
    }
    CHECK(worst < 1e-4f);
}

// =============================================================================
// Irradiance from this sky is strictly positive and brighter facing down
// =============================================================================
// A sanity claim on the SHIPPED coefficients rather than on a test function, and
// the one that would notice if the sky's own sign convention inverted. The sky
// is brighter at the horizon and below (blend t = 0 is the pale horizon colour,
// t = 1 the darker zenith blue), so a surface whose normal points DOWN gathers
// more irradiance than one pointing up.
//
// It also guards the thing that makes the whole feature visible: E(N) must be
// well away from zero, or the corridor stays black with IBL enabled.
TEST_CASE(Sh_SkyIrradianceIsPositiveEverywhere)
{
    const core::SHColor9 packed =
        core::PackIrradianceCoefficients(core::ProjectSkyRadiance(core::kSHProjectionSamples));

    for (uint32_t i = 0; i < 64; ++i)
    {
        const core::Vec3f E = core::EvaluateIrradiance(packed, QueryNormal(i, 64));
        CHECK(E.x > 0.1f);
        CHECK(E.y > 0.1f);
        CHECK(E.z > 0.1f);
    }

    const core::Vec3f up   = core::EvaluateIrradiance(packed, core::Vec3f{ 0.0f,  1.0f, 0.0f });
    const core::Vec3f down = core::EvaluateIrradiance(packed, core::Vec3f{ 0.0f, -1.0f, 0.0f });
    CHECK(core::Luminance(down) > core::Luminance(up));
}

// =============================================================================
// The projection converges, so the sample count is a choice and not a tuning
// =============================================================================
// The startup projection uses kSHProjectionSamples. If the answer still moved
// materially at that count, every tolerance above would be a statement about the
// sample count rather than about the maths. Halving it must change nothing that
// matters.
TEST_CASE(Sh_ProjectionHasConvergedAtTheShippedSampleCount)
{
    const core::SHColor9 a = core::ProjectSkyRadiance(core::kSHProjectionSamples);
    const core::SHColor9 b = core::ProjectSkyRadiance(core::kSHProjectionSamples / 2);

    float worst = 0.0f;
    for (uint32_t k = 0; k < core::kSHCoefficientCount; ++k)
    {
        const float d = MaxComponentDelta(a.c[k], b.c[k]);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1e-4f);
}

// =============================================================================
// The C++ evaluator really is the HLSL evaluator, clamp included
// =============================================================================
// These two were NOT the same function. shaders/ibl_common.hlsli ends
// DawningIrradianceSH with max(e, 0); core::EvaluateIrradiance did not - and the
// [SMOKE] ibl_sh_agreement probe compared them to 1e-4 anyway. It passed for a
// reason that was not about either function: THIS sky's reconstruction never
// goes negative, so the clamp never fired and the difference never showed. A
// steeper environment would have failed the probe with nothing wrong.
//
// So the clamp lives on both sides now, and this case is what stops it being
// removed from one of them again. Coefficients that reconstruct negative are
// hand-built rather than projected, because a physically valid environment
// cannot produce them at L1 - which is precisely why the divergence survived
// review in the first place.
//
// WATCHED FAILING: delete the max() from core::EvaluateIrradiance.
TEST_CASE(Sh_EvaluatorClampsNegativeIrradianceExactlyAsTheShaderDoes)
{
    // DC of zero and a strong band-1 term: E(N) is a signed function of N.y that
    // must go negative somewhere on the sphere.
    core::SHColor9 packed;
    for (uint32_t k = 0; k < core::kSHCoefficientCount; ++k)
        packed.c[k] = core::Vec3f{ 0.0f, 0.0f, 0.0f };
    packed.c[1] = core::Vec3f{ 1.0f, 1.0f, 1.0f };

    const core::Vec3f down{ 0.0f, -1.0f, 0.0f };

    // The raw reconstruction goes negative, so the case is not vacuous...
    const core::Vec3f raw = core::EvaluateIrradianceRaw(packed, down);
    CHECK(raw.x < 0.0f);
    CHECK(raw.y < 0.0f);
    CHECK(raw.z < 0.0f);

    // ...and the shipped evaluator clamps it, which is what the shader does.
    const core::Vec3f clamped = core::EvaluateIrradiance(packed, down);
    CHECK(clamped.x == 0.0f);
    CHECK(clamped.y == 0.0f);
    CHECK(clamped.z == 0.0f);

    // Where the reconstruction is positive the two must be identical, or the
    // clamp has become a transform.
    const core::Vec3f up{ 0.0f, 1.0f, 0.0f };
    CHECK(MaxComponentDelta(core::EvaluateIrradiance(packed, up),
                            core::EvaluateIrradianceRaw(packed, up)) == 0.0f);
}
