// Depth-only vertex shader for the directional shadow map.
//
// Shares ObjectData (t0/space2), CBDrawIndex (b3) and CBPerPass (b4) with
// basic_vs.hlsl rather than introducing shadow-specific bindings. The two
// passes now share one struct, one buffer and one root parameter set, and
// differ only in what CBPerPass holds: the light view-projection here, the
// camera view-projection there. That is a stronger version of the guarantee the
// old comment claimed for reusing b0 - it is the usual way a shadow map ends up
// subtly misaligned with the geometry it is supposed to shadow.
//
// The two passes take DISJOINT ranges of the shared object buffer: this one
// writes elements [0, N), the main pass [N, 2N). They do not share records, so
// a future divergence in the two scene walks' filters cannot silently pair a
// draw with another entity's transform.
//
// There is no pixel shader. The PSO binds no render targets and writes only
// depth, so the rasteriser fills the depth buffer directly.
//
// Keep ObjectData byte-identical with struct ObjectData in
// src/render/gpu_draw_records.h and in basic_vs.hlsl. A root SRV has no
// descriptor and therefore no StructureByteStride for anything to validate.

struct ObjectData
{
    float4 worldRow0;
    float4 worldRow1;
    float4 worldRow2;
    float4 normalRow0;
    float4 normalRow1;
    float4 normalRow2;
    uint   recordId;
    uint3  recordPad;
};
StructuredBuffer<ObjectData> objectBuffer : register(t0, space2);

// The draw-index witness, shared with basic_vs.hlsl - see the long note there
// and in gpu_draw_records.h. This pass needs it as much as the main one does,
// and rather more quietly: pinning the shadow pass to record 0 moves the
// rendered image so little that a capture-statistics gate cannot see it (it was
// measured at one colour bucket and 0.7 luminance, well inside the noise that
// gate had across checkouts). Here it is a hard, exact failure.
//
// The four cascades all execute THIS shader over the same rewound record range,
// so they all write the same slots with the same values. That means this marker
// proves the shared shadow_vs consumed the right record on every cascade, but
// it cannot attribute a write to one cascade - the cascades differ only in
// CBPerPass, never in which record they index, so there is no per-cascade
// indexing failure for it to miss.
RWStructuredBuffer<uint> drawIndexWitness : register(u0, space2);

cbuffer CBDrawIndex : register(b3)
{
    uint objectIndex;
    uint materialIndex;
};

cbuffer CBPerPass : register(b4)
{
    float4x4 viewProj;   // the LIGHT view-projection in this pass
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD0;
};

float4 main(VSInput input) : SV_POSITION
{
    ObjectData obj = objectBuffer[objectIndex];

    // +1 so 0 means "no draw wrote this slot". Same reasoning as basic_vs.hlsl.
    drawIndexWitness[objectIndex] = obj.recordId + 1u;

    float4 p = float4(input.position, 1.0);
    float3 positionWS = float3(dot(obj.worldRow0, p),
                               dot(obj.worldRow1, p),
                               dot(obj.worldRow2, p));

    return mul(viewProj, float4(positionWS, 1.0));
}
