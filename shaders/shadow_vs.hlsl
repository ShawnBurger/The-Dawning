// Depth-only vertex shader for the directional shadow map.
//
// Reuses CBPerObject (b0) unchanged rather than introducing a shadow-specific
// constant buffer: the shadow pass simply uploads worldViewProj = world x
// lightViewProj instead of world x cameraViewProj. Same struct, same root
// parameter, different matrix. That keeps the two passes from drifting apart in
// how they interpret per-object constants, which is the usual way a shadow map
// ends up subtly misaligned with the geometry it is supposed to shadow.
//
// There is no pixel shader. The PSO binds no render targets and writes only
// depth, so the rasteriser fills the depth buffer directly.

cbuffer CBPerObject : register(b0)
{
    float4x4 worldViewProj;
    float4x4 world;
    float4x4 worldInvTranspose;
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
    return mul(worldViewProj, float4(input.position, 1.0));
}
