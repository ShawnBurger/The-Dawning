// =============================================================================
// shaders/line_ps.hlsl — flat colored lines. The alpha rides the vertex color,
// so the caller controls opacity; the PSO alpha-blends into the HDR target.
// =============================================================================

struct PSInput
{
    float4 pos   : SV_Position;
    float4 color : COLOR;
};

float4 main(PSInput input) : SV_Target
{
    return input.color;
}
