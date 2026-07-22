// =============================================================================
// space_ps.hlsl — deep-space background: near-black + a procedural starfield
// =============================================================================
// The star-system views are in SPACE, so the grey elevation gradient of
// sky_ps.hlsl is wrong there. This draws a near-black sky with procedural point
// stars fixed to the celestial sphere (the world-space view direction), so they
// rotate correctly with the camera.
//
// IMPORTANT: this ONLY changes the VISIBLE background. The IBL environment is baked
// from DawningSkyRadiance and is deliberately left untouched, so every IBL GPU probe
// stays byte-identical and surfaces keep their neutral ambient fill — a failed/absent
// star-field cannot darken the scene lighting. sky_ps.hlsl still drives the demo
// scene and the IBL bake.

#include "display_common.hlsli"

cbuffer CBPerFrame : register(b1)
{
    float3 lightDir;
    float  pad0;
    float3 lightColor;
    float  pad1;
    float3 ambientColor;
    float  pad2;
    float3 eyePos;
    float  pad3;
    float3 camRight;
    float  tanHalfFovY;
    float3 camUp;
    float  aspect;
    float3 camForward;
    float  pad4;
    // Deliberate PREFIX of CBPerFrame (bytes 0..111), identical to sky_ps.hlsl.
    // Fields may only be APPENDED to CBPerFrame; see sky_ps.hlsl for the full
    // rationale and the static_assert(offsetof(lightViewProj)==112) that pins it.
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float Hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float3 Hash33(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

// One layer of sparse point stars on a 3-D direction grid. `density` scales the grid
// (more cells = more, smaller stars); `threshold` in [0,1) is the fraction of empty
// cells (higher = sparser); `size` is the point radius in cell space.
float3 StarLayer(float3 dir, float density, float threshold, float size)
{
    float3 p  = dir * density;
    float3 id = floor(p);
    float3 fp = frac(p) - 0.5;

    float  present = step(threshold, Hash13(id));       // does this cell hold a star?
    // Sub-cell position (breaks the lattice), but kept far enough from the cell
    // edge that a disc of radius `size` never crosses the ±0.5 boundary — a star
    // is owned by exactly one cell, so it is never sliced into a hard-edged
    // crescent by the single-cell lookup. The centre may stray at most (0.5-size).
    float3 sp      = (Hash33(id + 1.7) - 0.5) * (1.0 - 2.0 * size);
    float  d       = length(fp - sp);

    // Screen-space antialiasing. These stars are sub-pixel: whether a pixel's ray
    // lands inside one flips frame to frame as the camera turns, so without this a
    // faint star scintillates and crawls. Floor the disc radius at the pixel
    // footprint (measured in cell space from the smooth p, NOT the discontinuous
    // frac, so there is no seam at cell edges) and dim by the area ratio so the
    // spread-out disc carries the same total energy — a stable dot, not a twinkle.
    // When the star is larger than a pixel (zoomed in) radius==size and the factor
    // is 1, so it renders as an exact disc with no change.
    float  fw      = max(length(ddx(p)), length(ddy(p)));
    float  radius  = max(size, fw);
    float  bright  = smoothstep(radius, 0.0, d) * present * (size * size) / (radius * radius);

    float  mag     = 0.25 + 0.75 * Hash13(id + 9.1);    // apparent-magnitude variation
    float3 tint    = lerp(float3(0.65, 0.75, 1.0),      // blue-white ...
                          float3(1.00, 0.90, 0.72),     // ... to warm
                          Hash13(id + 3.3));
    return bright * mag * tint;
}

float4 main(PSInput input) : SV_TARGET
{
    // Same world-space view ray sky_ps.hlsl / the path tracer build.
    float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
    float3 dir = normalize(camForward
                         + camRight * (ndc.x * aspect * tanHalfFovY)
                         + camUp    * (ndc.y * tanHalfFovY));

    // A near-black deep-space floor (a hint of blue so it is not a dead flat zero).
    float3 col = float3(0.004, 0.006, 0.012);

    // Two layers: a brighter, sparser foreground and a fainter, denser background dust.
    col += StarLayer(dir, 350.0, 0.965, 0.18) * 2.2;
    col += StarLayer(dir, 700.0, 0.985, 0.10) * 1.0;

    return float4(col, 1.0);
}
