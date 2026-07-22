// =============================================================================
// billboard_ps.hlsl — soft circular body marker
// =============================================================================
// Turns the quad emitted by billboard_vs.hlsl into a round dot. `uv` is the quad
// corner in [-1, 1], so its length is the normalized radius: 0 at the center, 1 at
// the edge midpoints, sqrt(2) at the corners. Fragments past radius 1 are discarded
// so the marker reads as a disc, and a short smoothstep feathers the rim so it does
// not alias at astronomical distances where the whole dot is only a few pixels.

struct PSInput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
    float r = length(input.uv);
    if (r > 1.0)
        discard;

    // 1 at the center, fading to 0 across the outer quarter of the radius.
    float edge = smoothstep(1.0, 0.75, r);

    float4 c = input.color;
    c.a *= edge;
    return c;
}
