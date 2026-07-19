// =============================================================================
// tonemap_ps.hlsl — resolve the linear HDR scene target to the 8-bit back buffer
// =============================================================================
// The single place raster tone mapping happens. It used to happen inside
// basic_ps.hlsl and sky_ps.hlsl, which meant the frame existed only as 8-bit
// non-linear data and bloom, exposure and TAA had no linear buffer to work
// from. Those passes now have somewhere to insert themselves: between the scene
// pass and this resolve.
// =============================================================================

#include "display_common.hlsli"

Texture2D<float4> sceneHDR : register(t0);
SamplerState      pointSampler : register(s0);

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    // Point sampling and a 1:1 mapping - this is a resolve, not a rescale, so
    // any filtering here would only soften the image.
    float3 hdr = sceneHDR.SampleLevel(pointSampler, input.uv, 0.0).rgb;

    // Clamp to the half range as a backstop. NaN is handled at the PRODUCER
    // (basic_ps.hlsl clamps before storing) rather than here, deliberately:
    // this compiles as ps_5_1 without /Gis, so FXC is entitled to assume no
    // NaNs and optimises isnan() away entirely — it warns X3577 and, under the
    // engine's /WX, fails the build. A check the compiler deletes is not
    // defence in depth, it is decoration. Preventing the Inf at the source is
    // the real fix.
    hdr = min(hdr, 65504.0);

    return float4(DawningToneMapForDisplay(hdr), 1.0);
}
