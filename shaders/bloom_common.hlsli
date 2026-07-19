#ifndef THE_DAWNING_BLOOM_COMMON_HLSLI
#define THE_DAWNING_BLOOM_COMMON_HLSLI

// =============================================================================
// bloom_common.hlsli — shared constants for the bloom chain
// =============================================================================
// Bloom exists because the scene is now available as linear radiance in an
// R16G16B16A16_FLOAT target. It cannot be done from an 8-bit tone-mapped buffer:
// the information it needs - how far above display white a pixel actually is -
// has already been destroyed by that point. This is the payoff of the HDR pass.
// =============================================================================

cbuffer BloomConstants : register(b0)
{
    float2 texelSize;      // 1 / source dimensions, for the blur taps
    float  threshold;      // luminance above which a pixel blooms
    float  softKnee;       // width of the soft rolloff below the threshold
    float  intensity;      // composite strength
    float3 bloomPad;
};

// Rec. 709 luma, matching DawningLuminance in brdf_common.hlsli.
float BloomLuminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Quadratic soft knee. A hard threshold makes bloom pop on and off as a
// highlight crosses it, which reads as flicker under any camera motion; the
// knee fades contribution in across a band instead.
float3 BloomPrefilter(float3 color)
{
    const float luma = BloomLuminance(color);
    const float knee = max(threshold * softKnee, 1e-4f);

    float soft = luma - threshold + knee;
    soft = clamp(soft, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 1e-6f);

    const float contribution = max(soft, luma - threshold) / max(luma, 1e-6f);
    return color * contribution;
}

#endif
