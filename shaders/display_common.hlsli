#ifndef THE_DAWNING_DISPLAY_COMMON_HLSLI
#define THE_DAWNING_DISPLAY_COMMON_HLSLI

float3 DawningLinearToDisplay(float3 color)
{
    color = saturate(color);
    return pow(color, 1.0f / 2.2f);
}

float3 DawningToneMapForDisplay(float3 color)
{
    color = max(color, float3(0.0f, 0.0f, 0.0f));
    color *= 1.25f;
    color = color / (color + float3(1.0f, 1.0f, 1.0f));
    return DawningLinearToDisplay(color);
}

#endif

