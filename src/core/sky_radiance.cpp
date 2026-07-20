// =============================================================================
// core/sky_radiance.cpp - CPU evaluation of the procedural sky
// =============================================================================
// See sky_radiance.h. This is a MIRROR of shaders/sky_common.hlsli, not shipped
// code, and the header states exactly what the mirror tripwire does and does not
// guarantee. Every line below has a named counterpart in the HLSL; keep the
// correspondence one-to-one so the two can be read side by side.
// =============================================================================

#include "sky_radiance.h"

#include <algorithm>

namespace core
{

// HLSL:
//     float3 DawningSkyRadianceFromBlend(float t)
//     {
//         t = saturate(t);
//         return lerp(float3(0.8f, 0.85f, 0.9f), float3(0.3f, 0.5f, 0.9f), t) * 0.5f;
//     }
Vec3f SkyRadianceFromBlend(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);           // saturate(t)

    const Vec3f horizon = { 0.8f, 0.85f, 0.9f };
    const Vec3f zenith  = { 0.3f, 0.5f,  0.9f };

    // lerp(a, b, t) == a + (b - a) * t, which is what the HLSL lerp expands to.
    return (horizon + (zenith - horizon) * t) * 0.5f;
}

// HLSL:
//     float3 DawningSkyRadiance(float3 direction)
//     {
//         return DawningSkyRadianceFromBlend(0.5f * (direction.y + 1.0f));
//     }
//
// NOTE, and it is the whole point of tests/test_sky_energy.cpp: the blend
// parameter is a function of direction.y ALONE. The sky has no azimuthal
// structure and no localised lobe of any kind - in particular NO SUN DISC. The
// analytic directional light is the only representation of the sun in this
// renderer, and IBL_DESIGN.md section 9.2 rests the entire energy argument on
// that being true. See the comment in shaders/sky_common.hlsli.
Vec3f SkyRadiance(const Vec3f& direction)
{
    return SkyRadianceFromBlend(0.5f * (direction.y + 1.0f));
}

float Luminance(const Vec3f& linearRGB)
{
    // Rec. 709 / sRGB primaries, linear light.
    return 0.2126f * linearRGB.x + 0.7152f * linearRGB.y + 0.0722f * linearRGB.z;
}

} // namespace core
