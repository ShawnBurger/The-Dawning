// =============================================================================
// hud_ps.hlsl — flat colored HUD primitives (see hud_vs.hlsl)
// =============================================================================
struct PSInput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

float4 main(PSInput input) : SV_TARGET
{
    return input.color;
}
