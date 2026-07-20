// =============================================================================
// basic_vs.hlsl — The Dawning V3 Vertex Shader
// =============================================================================
// SM 5.1 — will migrate to SM 6.6 with DXC when bindless is needed.
// Left-handed coordinate system: +Z forward, CW winding = front face.
// =============================================================================

#include "gpu_draw_records.hlsli"

StructuredBuffer<ObjectData> objectBuffer : register(t0, space2);
StructuredBuffer<MaterialData> materialBuffer : register(t0, space3);
RWByteAddressBuffer drawRecordProbe : register(u0, space4);

// Which record this draw owns. A root 32-bit constant, NOT SV_InstanceID: at
// SM 5.1 SV_InstanceID does not include StartInstanceLocation, so the obvious
// trick of passing a draw index through DrawIndexedInstanced's 5th argument
// compiles, runs, raises no debug-layer message, and hands every draw 0.
cbuffer CBDrawIndex : register(b3)
{
    uint objectIndex;
    uint materialIndex;
    uint drawProbeEnabled;
};

// Per-PASS view-projection: the camera matrix here, the light matrix in
// shadow_vs.hlsl. Still a cbuffer float4x4 with mul(M, v), which preserves the
// row-major-upload / column-major-packing cancellation core/types.h documents.
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
    float4 positionCS : SV_POSITION;   // Clip space
    float3 positionWS : TEXCOORD0;     // World space (for lighting)
    float3 normalWS   : TEXCOORD1;     // World space normal
    float4 color      : COLOR;
    float2 uv         : TEXCOORD2;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    ObjectData obj = objectBuffer[objectIndex];

    // Smoke-only GPU evidence. Every vertex computes the same two hashes for a
    // draw, and InterlockedOr is idempotent for identical values, so the result
    // is deterministic without relying on one particular index-buffer value to
    // identify a "first" vertex. The probe proves the records the shader
    // actually consumed, including their field layout.
    if (drawProbeEnabled != 0)
    {
        const uint probeOffset = objectIndex * 16u;
        uint ignored;
        drawRecordProbe.InterlockedOr(
            probeOffset + 0u, DawningHashObjectData(obj), ignored);
        drawRecordProbe.InterlockedOr(probeOffset + 4u, objectIndex + 1u, ignored);
        drawRecordProbe.InterlockedOr(
            probeOffset + 8u,
            DawningHashMaterialData(materialBuffer[materialIndex]), ignored);
        drawRecordProbe.InterlockedOr(probeOffset + 12u, materialIndex + 1u, ignored);
    }

    float4 p = float4(input.position, 1.0);
    float3 positionWS = float3(dot(obj.worldRow0, p),
                               dot(obj.worldRow1, p),
                               dot(obj.worldRow2, p));

    output.positionWS = positionWS;
    output.positionCS = mul(viewProj, float4(positionWS, 1.0));

    // Normals are covectors, so this is the inverse-transpose, not the world
    // matrix. w is zero in those rows; only xyz participates.
    float3 n = input.normal;
    output.normalWS = normalize(float3(dot(obj.normalRow0.xyz, n),
                                       dot(obj.normalRow1.xyz, n),
                                       dot(obj.normalRow2.xyz, n)));

    output.color      = input.color;
    output.uv         = input.uv;

    return output;
}
