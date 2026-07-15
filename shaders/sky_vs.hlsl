// =============================================================================
// sky_vs.hlsl - full-screen triangle for raster sky
// =============================================================================

struct VSOutput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    float2 p = positions[vertexID];

    VSOutput output;
    output.positionCS = float4(p, 0.0, 1.0);
    output.uv = float2(p.x * 0.5 + 0.5, 1.0 - (p.y * 0.5 + 0.5));
    return output;
}

