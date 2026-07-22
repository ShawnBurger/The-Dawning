// =============================================================================
// hud_vs.hlsl — screen-space 2D HUD primitives (reticle, brackets, bars, rings)
// =============================================================================
// The gameplay HUD (targeting, flight, docking, ship status) is composed of 2D
// primitives positioned in SCREEN PIXELS (origin top-left, +x right, +y down) and
// converted to clip space here. World-anchored elements (target bracket, velocity
// vector, lead pip) are projected to pixels on the CPU (render::WorldToScreen) and
// submitted as screen-space vertices, so this shader never sees a world transform.
// Drawn after the scene into the HDR target, depth off, alpha-blended, so the HUD
// always sits on top; colours are authored bright to survive tone mapping.
// =============================================================================

cbuffer HudConstants : register(b0)
{
    float2 g_viewport;   // render-target size in pixels
    float2 g_pad;        // 16-byte cadence
};

struct VSInput
{
    float2 pos   : POSITION;  // screen pixels, origin top-left
    float4 color : COLOR;     // rgba; alpha drives the blend
};

struct VSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    // Pixel -> NDC: x in [0,w]->[-1,1], y in [0,h]->[-1,1] then flip (screen y is
    // down, NDC y is up).
    float2 ndc = input.pos / g_viewport * 2.0 - 1.0;
    ndc.y = -ndc.y;
    output.pos   = float4(ndc, 0.0, 1.0);
    output.color = input.color;
    return output;
}
