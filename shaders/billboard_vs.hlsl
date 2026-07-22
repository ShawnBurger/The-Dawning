// =============================================================================
// billboard_vs.hlsl — constant-pixel body markers (impostors)
// =============================================================================
// Expands each marker vertex into a camera-facing quad whose on-screen size is a
// FIXED number of pixels regardless of how far the body is. The body's true-scale
// sphere is sub-pixel at astronomical range, so the near-body / ship views would
// otherwise show nothing where a distant planet or the star sits; this draws a
// small legible dot at the body's real depth instead.
//
// Constant-pixel math: NDC x/y span the viewport's width/height in pixels over the
// range [-1, 1], so one pixel is 2/viewport in NDC. `corner` is the quad corner in
// [-1, 1] (the half-extent direction) and `size` is the marker DIAMETER in pixels,
// so the half-extent offset in NDC is corner * (size / viewport). Applied in CLIP
// space (before the perspective divide) it must be pre-multiplied by clip.w.
//
// The offset is added AFTER the projection, so precision is unaffected: the caller
// already subtracted the camera in double and applied the render scale K (RULE 1),
// exactly as the line and mesh paths do.

cbuffer BillboardConstants : register(b0)
{
    float4x4 g_viewProj;   // camera-relative view-projection
    float2   g_viewport;   // render-target size in pixels
    float2   g_pad;        // keep the 16-byte cadence; written zero
};

struct VSInput
{
    float3 center : POSITION;  // camera-relative, render-scaled body center
    float4 color  : COLOR;     // rgba; alpha drives the blend
    float  size   : PSIZE;     // marker diameter in pixels
    float2 corner : TEXCOORD;  // quad corner in [-1, 1]
};

struct VSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;    // = corner, for the pixel-shader circular falloff
};

VSOutput main(VSInput input)
{
    VSOutput output;

    float4 clip = mul(g_viewProj, float4(input.center, 1.0));

    // Bodies at or behind the camera plane have clip.w <= 0; projecting them would
    // wrap the quad to the wrong side. Kill the vertex (w < 0 fails the clip test)
    // so the whole marker is discarded rather than smeared across the screen.
    if (clip.w <= 1e-4)
    {
        output.pos   = float4(0.0, 0.0, 0.0, -1.0);
        output.color = float4(0.0, 0.0, 0.0, 0.0);
        output.uv    = float2(0.0, 0.0);
        return output;
    }

    float2 ndcOffset = input.corner * (input.size / g_viewport);
    clip.xy += ndcOffset * clip.w;

    output.pos   = clip;
    output.color = input.color;
    output.uv    = input.corner;
    return output;
}
