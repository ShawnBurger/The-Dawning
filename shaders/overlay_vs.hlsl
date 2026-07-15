// =============================================================================
// overlay_vs.hlsl - full-screen triangle for debug overlay composition
// =============================================================================

struct VSOutput
{
    float4 positionCS : SV_POSITION;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    VSOutput output;
    output.positionCS = float4(positions[vertexId], 0.0, 1.0);
    return output;
}
