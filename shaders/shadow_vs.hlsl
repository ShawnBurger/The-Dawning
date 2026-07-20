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
#include "gpu_draw_records.hlsli"

StructuredBuffer<ObjectData> objectBuffer : register(t0, space2);
RWByteAddressBuffer drawRecordProbe : register(u0, space4);

cbuffer CBDrawIndex : register(b3)
{
    uint objectIndex;
    uint materialIndex;
    uint drawProbeEnabled;
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

    // The shadow pass has no pixel shader, so it writes only the object words.
    // Its records live in [0, N), disjoint from the main pass's [N, 2N), so the
    // material words of its slots stay zero and the reader requires that.
    if (drawProbeEnabled != 0)
    {
        DawningWriteObjectProbe(drawRecordProbe, objectIndex, obj);
    }

    float4 p = float4(input.position, 1.0);
    float3 positionWS = float3(dot(obj.worldRow0, p),
                               dot(obj.worldRow1, p),
                               dot(obj.worldRow2, p));

    return mul(viewProj, float4(positionWS, 1.0));
}
