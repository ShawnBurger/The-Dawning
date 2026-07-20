// =============================================================================
// core/sh_irradiance.cpp - L2 spherical-harmonic diffuse irradiance
// =============================================================================
// See sh_irradiance.h. The basis below has a hand-written twin in
// shaders/ibl_common.hlsli; the header states what watches that mirror and what
// the watch does not cover. Keep the expressions one-to-one so the two can be
// read side by side.
// =============================================================================

#include "sh_irradiance.h"

#include <cmath>

#include "sky_radiance.h"

namespace core
{
namespace
{

constexpr float kPi = 3.14159265358979323846f;

// Normalisation constants for the real SH basis, L0..L2.
constexpr float kY0  = 0.282095f;   // 0.5 * sqrt(1/pi)
constexpr float kY1  = 0.488603f;   // 0.5 * sqrt(3/pi)
constexpr float kY2a = 1.092548f;   // 0.5 * sqrt(15/pi)
constexpr float kY2b = 0.315392f;   // 0.25 * sqrt(5/pi)
constexpr float kY2c = 0.546274f;   // 0.25 * sqrt(15/pi)

// Lambertian transfer coefficients (Ramamoorthi & Hanrahan 2001).
//
// A_l = 2*pi * ratio of the cosine lobe's own SH expansion:
//   A0 = pi, A1 = 2*pi/3, A2 = pi/4.
// Bands 3 and above are either zero (odd l) or below 1% (l = 4), which is why
// L2 captures ~99% of ANY smooth environment's irradiance and 100% of this one.
constexpr float kA0 = kPi;
constexpr float kA1 = 2.0f * kPi / 3.0f;
constexpr float kA2 = kPi / 4.0f;

Vec3f SkyAdapter(const Vec3f& direction, void*)
{
    return SkyRadiance(direction);
}

} // namespace

void SHBasisL2(const Vec3f& d, float out[kSHCoefficientCount])
{
    const float x = d.x;
    const float y = d.y;
    const float z = d.z;

    out[0] = kY0;
    out[1] = kY1 * y;
    out[2] = kY1 * z;
    out[3] = kY1 * x;
    out[4] = kY2a * x * y;
    out[5] = kY2a * y * z;
    out[6] = kY2b * (3.0f * z * z - 1.0f);
    out[7] = kY2a * x * z;
    out[8] = kY2c * (x * x - y * y);
}

SHColor9 ProjectRadiance(RadianceFn fn, void* user, uint32_t sampleCount)
{
    SHColor9 result;
    if (!fn || sampleCount == 0)
        return result;

    // Uniform solid angle per sample. DROPPING THIS is the single most likely SH
    // bug and it produces a perfectly plausible image at the wrong brightness -
    // Sh_WhiteFurnaceIsExactlyPiTimesRadiance is the case that watches it.
    const double weight = 4.0 * static_cast<double>(kPi) / static_cast<double>(sampleCount);

    // THE ACCUMULATOR IS DOUBLE, AND THAT IS NOT DEFENSIVE PADDING - IT WAS
    // MEASURED. Summing 16384 float terms naively carries a relative error of
    // about sqrt(N)*eps ~ 8e-6, which on an irradiance of ~2.2 is 1.7e-5 - and
    // Sh_WhiteFurnaceIsExactlyPiTimesRadiance asserts 1e-5. That case FAILED
    // with a float accumulator here, and the residual was traced to naive
    // summation rather than to the maths: the analytic-irradiance case, whose
    // bound is 1e-4, passed at the same time.
    //
    // The fix is the accumulator, not the tolerance. Widening the bound to fit a
    // known-avoidable rounding artefact would have spent the assertion's teeth
    // to save nine characters in a routine that runs ONCE, at startup.
    //
    // RULE 1 is not in play. These are not world positions; `double` here is
    // accumulator width, and the result narrows back to Vec3f on the way out
    // because that is what the constant buffer holds.
    double acc[kSHCoefficientCount][3] = {};

    // Spherical Fibonacci. Deterministic, so the coefficients are bit-identical
    // from run to run and every downstream assertion is a statement about the
    // maths rather than about a seed.
    const double golden = static_cast<double>(kPi) * (3.0 - std::sqrt(5.0));

    for (uint32_t i = 0; i < sampleCount; ++i)
    {
        const double t     = (static_cast<double>(i) + 0.5) / static_cast<double>(sampleCount);
        const double y     = 1.0 - 2.0 * t;
        const double r     = std::sqrt((1.0 - y * y) > 0.0 ? (1.0 - y * y) : 0.0);
        const double theta = golden * static_cast<double>(i);

        // The DIRECTION narrows to float before the radiance call, deliberately:
        // `fn` is the CPU twin of an HLSL function that will only ever see
        // floats, so projecting through a double direction would evaluate a sky
        // the GPU never evaluates.
        const Vec3f dir{ static_cast<float>(r * std::cos(theta)),
                         static_cast<float>(y),
                         static_cast<float>(r * std::sin(theta)) };
        const Vec3f L = fn(dir, user);

        float basis[kSHCoefficientCount];
        SHBasisL2(dir, basis);

        for (uint32_t k = 0; k < kSHCoefficientCount; ++k)
        {
            const double w = static_cast<double>(basis[k]) * weight;
            acc[k][0] += static_cast<double>(L.x) * w;
            acc[k][1] += static_cast<double>(L.y) * w;
            acc[k][2] += static_cast<double>(L.z) * w;
        }
    }

    for (uint32_t k = 0; k < kSHCoefficientCount; ++k)
        result.c[k] = Vec3f{ static_cast<float>(acc[k][0]),
                             static_cast<float>(acc[k][1]),
                             static_cast<float>(acc[k][2]) };

    return result;
}

SHColor9 ProjectSkyRadiance(uint32_t sampleCount)
{
    return ProjectRadiance(&SkyAdapter, nullptr, sampleCount);
}

SHColor9 PackIrradianceCoefficients(const SHColor9& radiance)
{
    SHColor9 packed;
    packed.c[0] = radiance.c[0] * kA0;
    packed.c[1] = radiance.c[1] * kA1;
    packed.c[2] = radiance.c[2] * kA1;
    packed.c[3] = radiance.c[3] * kA1;
    for (uint32_t k = 4; k < kSHCoefficientCount; ++k)
        packed.c[k] = radiance.c[k] * kA2;
    return packed;
}

Vec3f EvaluateIrradianceRaw(const SHColor9& packed, const Vec3f& normal)
{
    float basis[kSHCoefficientCount];
    SHBasisL2(normal, basis);

    Vec3f e{ 0.0f, 0.0f, 0.0f };
    for (uint32_t k = 0; k < kSHCoefficientCount; ++k)
        e += packed.c[k] * basis[k];
    return e;
}

// The clamp is not a nicety - it is the last line of DawningIrradianceSH in
// shaders/ibl_common.hlsli, and this function's entire purpose is to be that
// function. Without it the GPU agreement probe compared two different functions
// and passed only because this sky's reconstruction never goes negative, which
// made a property of the environment look like a property of the code.
Vec3f EvaluateIrradiance(const SHColor9& packed, const Vec3f& normal)
{
    const Vec3f e = EvaluateIrradianceRaw(packed, normal);
    return Vec3f{ e.x > 0.0f ? e.x : 0.0f,
                  e.y > 0.0f ? e.y : 0.0f,
                  e.z > 0.0f ? e.z : 0.0f };
}

} // namespace core
