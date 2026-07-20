// =============================================================================
// basic_vs.hlsl — The Dawning V3 Vertex Shader
// =============================================================================
// SM 5.1 — will migrate to SM 6.6 with DXC when bindless is needed.
// Left-handed coordinate system: +Z forward, CW winding = front face.
// =============================================================================

// Per-draw transform record. Keep byte-identical with struct ObjectData in
// src/render/gpu_draw_records.h, which static_asserts the size and every
// offset. NOTHING ELSE CHECKS THIS: a root SRV is a bare GPU virtual address
// with no descriptor, so there is no StructureByteStride for the runtime or the
// debug layer to compare against the struct size FXC computes here. A one-byte
// disagreement reads shifted garbage for every element after the first.
//
// EXPLICIT float4 ROWS, NOT float4x4. StructuredBuffer elements do not get the
// column-major reinterpretation that cbuffer matrices do (render/mesh.h says so,
// and says so because this tree already shipped that bug once on
// RTInstanceData::normalMatrix). The rows hold the TRANSPOSE of the engine's
// row-major Mat4x4, so component r is a plain dot with row r.
struct ObjectData
{
    float4 worldRow0;      //  0..15
    float4 worldRow1;      // 16..31
    float4 worldRow2;      // 32..47
    float4 normalRow0;     // 48..63
    float4 normalRow1;     // 64..79
    float4 normalRow2;     // 80..95
};
StructuredBuffer<ObjectData> objectBuffer : register(t0, space2);

// Which record this draw owns. A root 32-bit constant, NOT SV_InstanceID: at
// SM 5.1 SV_InstanceID does not include StartInstanceLocation, so the obvious
// trick of passing a draw index through DrawIndexedInstanced's 5th argument
// compiles, runs, raises no debug-layer message, and hands every draw 0.
cbuffer CBDrawIndex : register(b3)
{
    uint objectIndex;
    uint materialIndex;
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
