// =============================================================================
// bloom_blur_ps.hlsl — one axis of a separable Gaussian
// =============================================================================
// Run twice, horizontally then vertically, ping-ponging between two half-res
// targets. Separable because a 9-tap 2D kernel would be 81 samples per pixel
// against 18 for two 1D passes, for the same result.
// =============================================================================

#include "bloom_common.hlsli"

Texture2D<float4> source      : register(t0);
SamplerState      linearClamp : register(s0);

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    // texelSize carries the blur direction: the caller zeroes the other axis, so
    // one shader serves both passes without a permutation or a branch.
    const float2 step = texelSize;

    // Linear-sampled Gaussian: 5 taps at offsets chosen so bilinear filtering
    // pairs up neighbouring texels, giving a 9-tap kernel for 5 fetches.
    const float weights[5] = { 0.2270270270f, 0.3162162162f, 0.3162162162f,
                               0.0702702703f, 0.0702702703f };
    const float offsets[5] = { 0.0f, 1.3846153846f, -1.3846153846f,
                               3.2307692308f, -3.2307692308f };

    float3 result = float3(0, 0, 0);
    [unroll]
    for (int i = 0; i < 5; ++i)
        result += source.SampleLevel(linearClamp, input.uv + step * offsets[i], 0).rgb
                * weights[i];

    return float4(result, 1.0);
}
