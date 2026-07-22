// =============================================================================
// terrain_vs.hlsl — vertex shader for chunked-LOD planetary terrain patches
// =============================================================================
// A near-copy of planet_vs.hlsl, so terrain chunks shade through the SAME
// planet_ps.hlsl as the far sphere (procedural surface, bit-identical shading —
// the chunk's objectDir is the sphere's objectDir). Two differences from planet_vs:
//
//  1. The chunk vertices are ALREADY displaced on the CPU (chunk_mesh.cpp) and
//     stored chunk-local, so this VS is a pure transform + passthrough — it does
//     NOT re-displace.
//  2. The planet-fixed surface direction (objectDir, what planet_ps textures with)
//     is not the vertex normal here — the vertex normal is the real TERRAIN normal
//     (for lighting). objectDir is packed into the COLOR channel on the CPU as the
//     vertex's body-space unit direction, and passed straight through.
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
    float3 normal   : NORMAL;      // TERRAIN surface normal (body space)
    float4 color    : COLOR;       // xyz = planet-fixed surface direction (objectDir)
    float2 uv       : TEXCOORD0;
};

struct VSOutput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD0;
    float3 normalWS   : TEXCOORD1;
    float4 color      : COLOR;
    float2 uv         : TEXCOORD2;
    float3 objectDir  : TEXCOORD3;
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

    float3 n = input.normal;
    output.normalWS = normalize(float3(dot(obj.normalRow0.xyz, n),
                                       dot(obj.normalRow1.xyz, n),
                                       dot(obj.normalRow2.xyz, n)));

    output.color     = input.color;
    output.uv        = input.uv;
    output.objectDir = normalize(input.color.xyz); // planet-fixed dir (CPU-packed)

    return output;
}
