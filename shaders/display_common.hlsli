#ifndef THE_DAWNING_DISPLAY_COMMON_HLSLI
#define THE_DAWNING_DISPLAY_COMMON_HLSLI

float3 DawningLinearToDisplay(float3 color)
{
    color = saturate(color);
    return pow(color, 1.0f / 2.2f);
}

// Exposure is a parameter rather than a constant baked into the curve. It was
// hardcoded at 1.25 here, which meant every consumer silently shared one
// magic number and auto-exposure had nowhere to attach.
float3 DawningToneMapForDisplay(float3 color, float exposure)
{
    color = max(color, float3(0.0f, 0.0f, 0.0f));
    color *= exposure;
    color = color / (color + float3(1.0f, 1.0f, 1.0f));
    return DawningLinearToDisplay(color);
}

// Default exposure, for the DXR path which tone maps inside path_trace.hlsl and
// has no constant buffer slot for it yet. Same value the constant used to be, so
// this is not a visual change.
static const float kDawningDefaultExposure = 1.25f;

float3 DawningToneMapForDisplay(float3 color)
{
    return DawningToneMapForDisplay(color, kDawningDefaultExposure);
}

#endif
