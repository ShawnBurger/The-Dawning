// =============================================================================
// planet_ps.hlsl — procedural celestial-body surface (planet from orbit)
// =============================================================================
// A dedicated surface shader for the seeded celestial bodies, so basic_ps.hlsl
// (and its byte-identical IBL consumption probe) is never touched. Opaque, writes
// reversed-Z depth, drawn in the RenderEntities slot BEFORE the atmosphere shell
// so the analytic scattering composites over it.
//
// The whole surface is analytic — hash-based value-noise fBm over the object-space
// (planet-fixed) surface direction, so there are no texture assets and the look is
// seamless with no UV pinch. Layers (INCREMENT 1): continents vs ocean from an fBm
// height field thresholded at sea level, a latitude+elevation+moisture biome colour
// ramp with polar ice, ridged mountains, and a Fresnel ocean sun-glint. Clouds,
// night-side city lights, and crater relief follow in later increments. Branches on
// params0.x: 0 Earth-like (ocean), 1 Mars-like, 2 Moon-like (sharp terminator), 3
// generic rock.
//
// FXC /WX single-exit: accumulate into `col`, one return.
// =============================================================================

// Per-body surface parameters (root CBV b5). Byte-identical to
// Renderer::PlanetConstants in renderer.h — 11 float4 rows, 176 bytes.
cbuffer CBPlanet : register(b5)
{
    float4 sunDir;       // xyz world sun direction (camera-relative math), w = cloud rotation angle (radians, pre-wrapped [0,2pi))
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

// The procedural noise + elevation field lives in a shared header so the near-field
// displaced terrain mesh (C++ twin core::PlanetHeight) evaluates the EXACT same
// height as this far-field shaded sphere — see planet_noise.hlsli.
#include "planet_noise.hlsli"

float4 main(PSInput input) : SV_TARGET
{
    const int   type   = (int)(params0.x + 0.5);
    const float seed   = params0.z;
    const float3 seedO = float3(seed, seed * 1.7, seed * 0.3);

    float3 N  = normalize(input.objectDir);   // planet-fixed surface direction
    float3 Nw = normalize(input.normalWS);     // world normal (lighting)
    float3 L  = normalize(sunDir.xyz);
    float3 V  = normalize(-input.positionWS);  // camera at origin (camera-relative)

    // Near the limb the surface foreshortens, so object-space direction N steps by a
    // large angle per screen pixel and the highest-frequency noise terms undersample
    // (sparkle/crawl under motion). There is no analytic AA here, so fade those terms
    // out toward the grazing limb: 1 over most of the disc, 0 at the edge.
    float limbFade = saturate(dot(Nw, V) * 1.7);

    const bool hasOcean = (type == 0);

    // --- Continent height / elevation (the SHARED field the terrain mesh
    // displaces by; see planet_noise.hlsli). h is the raw [0,1] height, elev the
    // final surface elevation (coast->peak+mountains for Earth, broad+craters for
    // Mars/Moon). landMask/h are reused below for the ocean and biome colours.
    float seaLevel   = params0.y;
    float coastWidth = max(shallowColor.w, 0.001);
    float h          = PlanetHeightRaw(N, seedO);
    float landMask   = PlanetLandMask(type, h, seaLevel, coastWidth);
    float elev       = PlanetElevation(N, type, seedO, h, landMask, seaLevel);

    // --- Latitude & moisture ------------------------------------------------
    float lat      = abs(N.y);                       // 0 equator, 1 pole
    float moisture = Fbm3(N * 3.3 + seedO * 2.1) * 0.5 + 0.5;

    // --- Land colour --------------------------------------------------------
    float3 landColor;
    if (type == 0)
    {
        // Earth: dry desert vs green lowland by moisture, rock by elevation.
        float3 dry  = float3(0.55, 0.47, 0.29);
        float3 low  = lerp(dry, landLow.rgb, smoothstep(0.30, 0.62, moisture));
        landColor   = lerp(low, landHigh.rgb, smoothstep(0.35, 0.85, elev));
        // Snow on high peaks and cold, moist high latitudes.
        float snow = saturate(smoothstep(0.80, 0.96, elev) +
                              smoothstep(iceColor.w, iceColor.w + 0.10, lat) *
                              smoothstep(0.35, 0.6, moisture));
        landColor = lerp(landColor, iceColor.rgb, saturate(snow));
    }
    else
    {
        // Mars / Moon / generic: base→rock by elevation, polar caps by latitude.
        landColor = lerp(landLow.rgb, landHigh.rgb, smoothstep(0.15, 0.80, elev));
        float cap = (iceColor.w < 1.5) ? smoothstep(iceColor.w, iceColor.w + 0.06, lat) : 0.0;
        landColor = lerp(landColor, iceColor.rgb, cap);
    }
    // Break up flat albedo with a touch of fine noise so it never reads as paint.
    // Fade the high-frequency break-up toward the limb where it would alias.
    landColor *= lerp(1.0, 0.9 + 0.2 * ValueNoise(N * 40.0 + seedO), limbFade);

    // --- Ocean colour (Earth) ----------------------------------------------
    float3 surfaceColor = landColor;
    if (hasOcean)
    {
        float depth = saturate((seaLevel - h) / max(deepColor.w, 0.001));
        float3 ocean = lerp(shallowColor.rgb, deepColor.rgb, depth);
        surfaceColor = lerp(ocean, landColor, landMask);
    }

    // --- Cloud layer (rotates over the fixed surface) -----------------------
    // A separate noise field on a Y-rotated copy of the surface direction, so the
    // cloud deck drifts independently of the continents. Computed here so its cast
    // shadow can darken the surface before lighting, and its own lit colour can
    // composite over the shaded result afterwards. cloud.x == 0 (Mars/Moon) skips it.
    float cloudDensity = 0.0;
    float cloudShadow  = 0.0;
    if (cloud.x > 0.001)
    {
        float ca = sunDir.w;                     // pre-wrapped rotation angle
        float cc = cos(ca), sc = sin(ca);
        float3 cDir = float3(N.x * cc - N.z * sc, N.y, N.x * sc + N.z * cc);
        float3 cSeed = seedO * 3.7 + 12.3;

        float cov = 0.62 * (Fbm5(cDir * 2.6 + cSeed) * 0.5 + 0.5) +
                    0.38 * (Fbm3(cDir * 7.3 + cSeed) * 0.5 + 0.5);
        float thresh = 1.0 - cloud.x;
        cloudDensity = smoothstep(thresh, thresh + max(cloud.y, 0.001), cov);

        // Cast shadow: the coverage a short way toward the sun, so cloud shadows
        // fall on the surface offset from the clouds themselves.
        float3 sDir = normalize(cDir + L * 0.035);
        float scov = 0.62 * (Fbm5(sDir * 2.6 + cSeed) * 0.5 + 0.5) +
                     0.38 * (Fbm3(sDir * 7.3 + cSeed) * 0.5 + 0.5);
        cloudShadow = smoothstep(thresh, thresh + max(cloud.y, 0.001), scov);
    }

    // Cloud shadow darkens the surface it does not itself cover.
    surfaceColor *= 1.0 - 0.45 * cloudShadow * (1.0 - cloudDensity);

    // --- Lighting -----------------------------------------------------------
    float ndl     = dot(Nw, L);
    // Lambert with a soft terminator; airless Moon gets a crisp one.
    float termLo  = (type == 2) ? -0.02 : -0.12;
    float termHi  = (type == 2) ?  0.02 :  0.10;
    float dayGate = smoothstep(termLo, termHi, ndl);
    float diffuse = saturate(ndl) * dayGate;

    float3 col = surfaceColor * (sunColor.rgb * sunColor.w * diffuse + ambient.rgb);

    // --- Ocean sun-glint (Earth day-side water, not under cloud) ------------
    if (hasOcean)
    {
        float oceanMask = 1.0 - landMask;
        float3 R = reflect(-L, Nw);
        float  spec = pow(saturate(dot(R, V)), max(ambient.w, 1.0));
        float  fres = 0.02 + 0.98 * pow(saturate(1.0 - dot(Nw, V)), 5.0);
        col += sunColor.rgb * sunColor.w * spec * fres * oceanMask * dayGate *
               (1.0 - cloudDensity);
    }

    // --- Composite lit clouds over the surface ------------------------------
    if (cloud.x > 0.001)
    {
        // Bright on the day side, a soft wrap so the terminator clouds catch the
        // golden light rather than going abruptly black.
        float cloudLit = saturate(ndl) * 0.9 + 0.1;
        float3 cloudCol = cloud.w * sunColor.rgb * cloudLit;
        // Night-side clouds stay faintly visible (earthshine) but do not glow.
        float cloudOpacity = cloudDensity * (0.12 + 0.88 * dayGate);
        col = lerp(col, cloudCol, saturate(cloudOpacity));
    }

    // --- Night-side city lights (emissive, additive) ------------------------
    // Warm clustered lights on the DARK side only, on land, off the ice caps, and
    // dimmed under cloud. A coarse "population" field decides where civilisation
    // clusters; a fine grain breaks it into individual lights. night.w == 0
    // (Mars/Moon/generic) skips it.
    if (night.w > 0.001)
    {
        float pop    = Fbm3(N * 9.0 + seedO * 5.0) * 0.5 + 0.5;
        float grain  = ValueNoise(N * 95.0 + seedO * 7.0);
        float cities = smoothstep(0.60, 0.80, pop) * smoothstep(0.45, 0.85, grain);
        cities *= landMask;                                         // land only
        cities *= 1.0 - smoothstep(iceColor.w - 0.08, iceColor.w, lat); // not polar
        cities *= 1.0 - dayGate;                                    // dark side only
        cities *= 1.0 - 0.7 * cloudDensity;                         // clouds occlude
        cities *= limbFade;                                         // no sub-pixel grain at the limb
        col += night.rgb * night.w * cities;
    }

    return float4(col, 1.0);
}
