// =============================================================================
// shaders/line_vs.hlsl — colored world-space line segments (orbit traces, etc.)
// =============================================================================
// Vertices arrive already in CAMERA-RELATIVE render space (the CPU has subtracted
// the camera position in double and applied the per-mode render scale K), so this
// only applies the camera-relative view * reversed-Z projection. mul(M, v) matches
// basic_vs.hlsl: the CPU uploads row-major bytes that HLSL reinterprets as the
// transpose, so mul(g_viewProj, v) evaluates the CPU's row-vector v * M.
// =============================================================================

cbuffer LineConstants : register(b0)
{
    float4x4 g_viewProj;
};

struct VSInput
{
    float3 pos   : POSITION;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 pos   : SV_Position;
    float4 color : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.pos   = mul(g_viewProj, float4(input.pos, 1.0));
    output.color = input.color;
    return output;
}
