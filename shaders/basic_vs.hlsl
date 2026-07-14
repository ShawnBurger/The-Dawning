// =============================================================================
// basic_vs.hlsl — The Dawning V3 Vertex Shader
// =============================================================================
// SM 5.1 — will migrate to SM 6.6 with DXC when bindless is needed.
// Left-handed coordinate system: +Z forward, CW winding = front face.
// =============================================================================

cbuffer CBPerObject : register(b0)
{
    float4x4 worldViewProj;
    float4x4 world;
    float4x4 worldInvTranspose;  // For correct normal transformation
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

    output.positionCS = mul(worldViewProj, float4(input.position, 1.0));
    output.positionWS = mul(world, float4(input.position, 1.0)).xyz;
    output.normalWS   = normalize(mul((float3x3)worldInvTranspose, input.normal));
    output.color      = input.color;
    output.uv         = input.uv;

    return output;
}
