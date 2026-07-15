// =============================================================================
// sky_ps.hlsl - raster sky approximation shared with path tracing
// =============================================================================

#include "display_common.hlsli"
#include "sky_common.hlsli"

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float skyBlend = 1.0 - saturate(input.uv.y);
    float3 radiance = DawningSkyRadianceFromBlend(skyBlend);
    return float4(DawningToneMapForDisplay(radiance), 1.0);
}

