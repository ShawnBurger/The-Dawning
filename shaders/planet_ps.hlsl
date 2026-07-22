// =============================================================================
// planet_ps.hlsl — procedural celestial-body surface (planet from orbit)
// =============================================================================
// A dedicated surface shader for the seeded celestial bodies, so basic_ps.hlsl
// (and its byte-identical IBL consumption probe) is never touched. Opaque, writes
// reversed-Z depth, drawn in the RenderEntities slot BEFORE the atmosphere shell
// so the analytic scattering composites over it.
//
// INCREMENT 0 (this revision): trivial Lambert shading from CBPlanet, proving the
// dedicated-PSO + root-CBV(b5) + object-space-normal plumbing renders a lit body
// through the new path with no regression. The procedural surface (continents,
// ocean, clouds, night lights, Mars/Moon variants) layers on in later increments.
//
// FXC /WX single-exit: accumulate into `col`, one return (no early returns → no
// X4000 "potentially uninitialized").
// =============================================================================

// Per-body surface parameters (root CBV b5). Byte-identical to
// Renderer::PlanetConstants in renderer.h — 11 float4 rows, 176 bytes.
cbuffer CBPlanet : register(b5)
{
    float4 sunDir;       // xyz world sun direction (camera-relative math), w = time seconds
    float4 sunColor;     // rgb light color,                                w = sun intensity
    float4 params0;      // x type(0=Earth,1=Mars,2=Moon,3=generic) y seaLevel z seed w renderScale
    float4 deepColor;    // rgb deep ocean,      w = depthScale
    float4 shallowColor; // rgb shallow ocean,   w = coastWidth
    float4 landLow;      // rgb lowland / base,  w = oceanRoughness
    float4 landHigh;     // rgb highland / rock, w = landRoughness
    float4 iceColor;     // rgb ice / cap,       w = iceLatitude
    float4 cloud;        // x coverage y softness z rotSpeed w brightness
    float4 night;        // rgb city light,      w = night intensity
    float4 ambient;      // rgb ambient/earthshine floor, w = glint shininess
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD0;
    float3 normalWS   : TEXCOORD1;
    float4 color      : COLOR;
    float2 uv         : TEXCOORD2;
    float3 objectDir  : TEXCOORD3;
};

float4 main(PSInput input) : SV_TARGET
{
    float3 N = normalize(input.normalWS);
    float3 L = normalize(sunDir.xyz);

    // Softened terminator (no hard cutoff → no stair-step ring at the day/night edge).
    float ndl       = dot(N, L);
    float dayFactor = smoothstep(-0.10, 0.10, ndl);

    float3 baseColor = landLow.rgb;

    // Direct sunlight + a small ambient floor so the night side is not pure black
    // (matches the scene's neutral IBL fill; replaced by SH ambient in increment 1).
    float3 col = baseColor * (sunColor.rgb * sunColor.w * dayFactor + ambient.rgb);

    return float4(col, 1.0);
}
