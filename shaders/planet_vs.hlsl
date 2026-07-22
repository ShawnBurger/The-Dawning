// =============================================================================
// planet_vs.hlsl — vertex shader for procedurally-shaded celestial bodies
// =============================================================================
// A near-exact copy of basic_vs.hlsl (same object buffer, same camera-relative
// transform, same reversed-Z clip output) with ONE addition: it passes the raw
// OBJECT-SPACE vertex normal straight through as `objectDir`. The unit-sphere
// mesh's object-space normal IS the surface lat/long direction, and because it
// is object space it is PLANET-FIXED — the body's rotation lives in obj.worldRow,
// so spinning the body scrolls the procedural continents for free while the
// world-space normal still drives the lighting/terminator.
//
// It deliberately does NOT declare the draw-record probe UAV: celestial bodies
// are not part of the smoke probe frame, so witnessing them would witness a read
// nothing verifies. basic_vs.hlsl stays the probed vertex stage, untouched.
// =============================================================================

#include "gpu_draw_records.hlsli"

StructuredBuffer<ObjectData> objectBuffer : register(t0, space2);

cbuffer CBDrawIndex : register(b3)
{
    uint objectIndex;
    uint materialIndex;
    uint drawProbeEnabled;
};

cbuffer CBPerPass : register(b4)
{
    float4x4 viewProj;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD0;
};

struct VSOutput
{
    float4 positionCS : SV_POSITION;   // Clip space (reversed-Z)
    float3 positionWS : TEXCOORD0;     // World space (camera-relative)
    float3 normalWS   : TEXCOORD1;     // World-space normal (lighting/terminator)
    float4 color      : COLOR;
    float2 uv         : TEXCOORD2;
    float3 objectDir  : TEXCOORD3;     // Object-space surface dir (planet-fixed texturing)
};

VSOutput main(VSInput input)
{
    VSOutput output;

    ObjectData obj = objectBuffer[objectIndex];

    float4 p = float4(input.position, 1.0);
    float3 positionWS = float3(dot(obj.worldRow0, p),
                               dot(obj.worldRow1, p),
                               dot(obj.worldRow2, p));

    output.positionWS = positionWS;
    output.positionCS = mul(viewProj, float4(positionWS, 1.0));

    // Inverse-transpose for the world-space normal (same as basic_vs).
    float3 n = input.normal;
    output.normalWS = normalize(float3(dot(obj.normalRow0.xyz, n),
                                       dot(obj.normalRow1.xyz, n),
                                       dot(obj.normalRow2.xyz, n)));

    output.color     = input.color;
    output.uv        = input.uv;
    output.objectDir = normalize(input.normal); // planet-fixed unit-sphere direction

    return output;
}
