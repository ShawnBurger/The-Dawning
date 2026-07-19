// =============================================================================
// tonemap_ps.hlsl — composite bloom, then resolve linear HDR to the back buffer
// =============================================================================
// The single place raster tone mapping happens. It used to happen inside
// basic_ps.hlsl and sky_ps.hlsl, which meant the frame existed only as 8-bit
// non-linear data. Bloom is added here, in linear space, before tone mapping -
// adding it after would be compositing light into a display-referred signal,
// which is not what light does.
// =============================================================================

#include "display_common.hlsli"

Texture2D<float4> sceneHDR    : register(t0);
Texture2D<float4> bloom       : register(t1);
SamplerState      pointClamp  : register(s0);
SamplerState      linearClamp : register(s1);

cbuffer TonemapConstants : register(b0)
{
    float exposure;        // linear multiplier applied before the curve
    float bloomIntensity;  // 0 disables the bloom contribution entirely
    float2 tonemapPad;
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    // Point sampling for the scene: this is a 1:1 resolve, not a rescale, so
    // filtering would only soften it. Bloom is half-resolution and IS being
    // rescaled, so it takes the linear sampler.
    float3 hdr = sceneHDR.SampleLevel(pointClamp, input.uv, 0.0).rgb;

    // Clamp to the half range as a backstop. NaN is handled at the PRODUCER
    // (basic_ps.hlsl clamps before storing) rather than here: this compiles as
    // ps_5_1 without /Gis, so FXC is entitled to assume no NaNs and optimises
    // isnan() away entirely - it warns X3577 and, under the engine's /WX, fails
    // the build. A check the compiler deletes is not defence in depth.
    hdr = min(hdr, 65504.0);

    // Additive in LINEAR space, before the curve. Bloom is scattered light, so
    // it adds to radiance; compositing it after tone mapping would brighten
    // already-saturated pixels that physically cannot get brighter.
    if (bloomIntensity > 0.0)
    {
        float3 bloomColor = bloom.SampleLevel(linearClamp, input.uv, 0.0).rgb;
        hdr += min(bloomColor, 65504.0) * bloomIntensity;
    }

    return float4(DawningToneMapForDisplay(hdr, exposure), 1.0);
}
