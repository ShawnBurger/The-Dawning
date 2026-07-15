#ifndef THE_DAWNING_SKY_COMMON_HLSLI
#define THE_DAWNING_SKY_COMMON_HLSLI

float3 DawningSkyRadianceFromBlend(float t)
{
    t = saturate(t);
    return lerp(float3(0.8f, 0.85f, 0.9f), float3(0.3f, 0.5f, 0.9f), t) * 0.5f;
}

float3 DawningSkyRadiance(float3 direction)
{
    return DawningSkyRadianceFromBlend(0.5f * (direction.y + 1.0f));
}

#endif

