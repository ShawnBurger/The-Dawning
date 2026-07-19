// =============================================================================
// sky_ps.hlsl - raster sky, evaluated from the same world-space direction
//               function the DXR miss shader uses
// =============================================================================

#include "display_common.hlsli"
#include "sky_common.hlsli"

cbuffer CBPerFrame : register(b1)
{
    float3 lightDir;
    float  pad0;
    float3 lightColor;
    float  pad1;
    float3 ambientColor;
    float  pad2;
    float3 eyePos;
    float  pad3;
    float3 camRight;
    float  tanHalfFovY;
    float3 camUp;
    float  aspect;
    float3 camForward;
    float  pad4;
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    // Reconstruct the world-space view ray for this pixel, matching how the path
    // tracer builds primary rays. Previously this shader used
    // `1.0 - saturate(input.uv.y)` directly as the sky blend, which nailed the
    // gradient to the framebuffer: it did not rotate with the camera and did not
    // respond to pitch or FOV, so the horizon jumped position when toggling F1.
    float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);

    float3 direction = normalize(camForward
                              + camRight * (ndc.x * aspect * tanHalfFovY)
                              + camUp    * (ndc.y * tanHalfFovY));

    // Same entry point as the DXR miss shader and environment reflections.
    float3 radiance = DawningSkyRadiance(direction);
    return float4(DawningToneMapForDisplay(radiance), 1.0);
}
