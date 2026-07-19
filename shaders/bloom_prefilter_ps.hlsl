// =============================================================================
// bloom_prefilter_ps.hlsl — extract the blooming part of the scene at half res
// =============================================================================

#include "bloom_common.hlsli"

Texture2D<float4> sceneHDR      : register(t0);
SamplerState      linearClamp   : register(s0);

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    // Four bilinear taps at the source's texel centres. Running at half
    // resolution, a single tap would alias badly on exactly the small bright
    // features bloom is meant to pick up, which produces crawling highlights
    // under motion.
    const float2 o = texelSize;
    float3 c  = sceneHDR.SampleLevel(linearClamp, input.uv + float2(-o.x, -o.y), 0).rgb;
    c += sceneHDR.SampleLevel(linearClamp, input.uv + float2( o.x, -o.y), 0).rgb;
    c += sceneHDR.SampleLevel(linearClamp, input.uv + float2(-o.x,  o.y), 0).rgb;
    c += sceneHDR.SampleLevel(linearClamp, input.uv + float2( o.x,  o.y), 0).rgb;
    c *= 0.25f;

    // Clamp before prefiltering. The scene target can legitimately hold values
    // in the thousands, and letting one such pixel through unbounded makes the
    // blur smear a single sample across the whole kernel.
    c = min(c, 64.0f);

    return float4(BloomPrefilter(c), 1.0);
}
