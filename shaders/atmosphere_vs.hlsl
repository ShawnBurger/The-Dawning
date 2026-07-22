// =============================================================================
// atmosphere_vs.hlsl — atmosphere shell vertex shader
// =============================================================================
// Transforms the shared unit sphere (radius 0.5) into an atmosphere SHELL around a
// planet: g_world scales it to the atmosphere radius and translates it to the
// planet's camera-relative centre, so its screen coverage is exactly the atmosphere
// disc. The pixel shader does the scattering, and it needs the fragment's
// camera-relative position to reconstruct the view ray (the eye is the origin in
// this K-scaled camera-relative space, RULE 1), so that is passed through.

cbuffer AtmosphereConstants : register(b0)
{
    float4x4 g_world;               // camera-relative shell world (scale+translate)
    float4x4 g_viewProj;            // camera-relative view-projection (no K)
    float4   g_planetCenterRadius;  // xyz = planet centre (camera-relative), w = planet radius
    float4   g_sunDirAtmoRadius;    // xyz = unit direction TO the star, w = atmosphere radius
    float4   g_betaRayleighG;       // xyz = Rayleigh scattering (1/m), w = Mie asymmetry g
    float4   g_betaMieHeights;      // x = Mie (1/m), y = Rayleigh scale height, z = Mie scale height, w = sun intensity
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
    float4 clip     : SV_POSITION;
    float3 worldPos : TEXCOORD0;    // camera-relative fragment position
};

VSOutput main(VSInput input)
{
    float4 wp = mul(g_world, float4(input.position, 1.0));
    VSOutput output;
    output.worldPos = wp.xyz;
    output.clip     = mul(g_viewProj, wp);
    return output;
}
