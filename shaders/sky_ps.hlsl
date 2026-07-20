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

    // NOTE: this is a deliberate PREFIX of CBPerFrame, not the whole thing. This
    // declaration ends at byte 112 and the C++ struct continues well past it -
    // four cascade light view-projections, three cascade tables, nine SH
    // irradiance coefficients and the IBL parameter vector, none of which this
    // shader has any use for. A cbuffer may declare fewer members than the buffer
    // holds; reads are by offset.
    //
    // DO NOT WRITE THE STRUCT'S TOTAL SIZE HERE. This comment said "static_assert'd
    // at 416 bytes" and stayed at 416 while CBPerFrame grew to 576, which is the
    // failure mode the append-only rule exists to prevent, committed into the one
    // file the rule exists to protect. The number that matters to THIS shader is
    // 112 - the end of the prefix it declares - and that number is pinned by
    // `static_assert(offsetof(CBPerFrame, lightViewProj) == 112)` in renderer.h,
    // which is a compile error rather than a comment. Read the size off the
    // static_asserts in renderer.h; do not mirror it into prose that cannot fail.
    //
    // Neither cascades nor IBL touched this declaration, by construction: every
    // new field was APPENDED at or after byte 112, and cascade 0's matrix occupies
    // exactly the bytes the old single lightViewProj did.
    //
    // The consequence: fields may only be APPENDED to CBPerFrame. Inserting one
    // anywhere above this point shifts every offset below it and this shader
    // starts reading the wrong bytes, silently, with no compile error on either
    // side. basic_ps.hlsl declares the full struct and would still agree with
    // C++, so the two raster shaders would disagree with each other.
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
    // Linear HDR out; see basic_ps.hlsl. The resolve tone-maps.
    float3 radiance = DawningSkyRadiance(direction);
    return float4(radiance, 1.0);
}
