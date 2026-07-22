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

// -----------------------------------------------------------------------------
// Hash-based value noise (Dave Hoskins "hash without sine") — LUT-free, stable
// across GPUs, no frac(sin()) banding at multi-octave frequencies.
// -----------------------------------------------------------------------------
float Hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float ValueNoise(float3 x)
{
    float3 i = floor(x);
    float3 f = frac(x);
    float3 u = f * f * (3.0 - 2.0 * f); // smoothstep interpolant

    float n000 = Hash13(i + float3(0, 0, 0));
    float n100 = Hash13(i + float3(1, 0, 0));
    float n010 = Hash13(i + float3(0, 1, 0));
    float n110 = Hash13(i + float3(1, 1, 0));
    float n001 = Hash13(i + float3(0, 0, 1));
    float n101 = Hash13(i + float3(1, 0, 1));
    float n011 = Hash13(i + float3(0, 1, 1));
    float n111 = Hash13(i + float3(1, 1, 1));

    float nx00 = lerp(n000, n100, u.x);
    float nx10 = lerp(n010, n110, u.x);
    float nx01 = lerp(n001, n101, u.x);
    float nx11 = lerp(n011, n111, u.x);
    float nxy0 = lerp(nx00, nx10, u.y);
    float nxy1 = lerp(nx01, nx11, u.y);
    return lerp(nxy0, nxy1, u.z); // [0,1]
}

// Per-octave rotation (IQ) so the axis-aligned grid of the value noise does not
// print through the fBm as a lattice.
static const float3x3 kNoiseRot = float3x3( 0.00,  0.80,  0.60,
                                           -0.80,  0.36, -0.48,
                                           -0.60, -0.48,  0.64);

// fBm in [-1,1]-ish. Fixed octave counts (unrolled) keep FXC /WX happy.
float Fbm5(float3 p)
{
    float sum = 0.0, amp = 0.5;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        sum += amp * (ValueNoise(p) * 2.0 - 1.0);
        p = mul(kNoiseRot, p) * 2.02;
        amp *= 0.5;
    }
    return sum;
}

float Fbm3(float3 p)
{
    float sum = 0.0, amp = 0.5;
    [unroll] for (int i = 0; i < 3; ++i)
    {
        sum += amp * (ValueNoise(p) * 2.0 - 1.0);
        p = mul(kNoiseRot, p) * 2.02;
        amp *= 0.5;
    }
    return sum;
}

// Ridged multifractal — sharp crests for mountain ranges.
float Ridged3(float3 p)
{
    float sum = 0.0, amp = 0.5, prev = 1.0;
    [unroll] for (int i = 0; i < 3; ++i)
    {
        float n = 1.0 - abs(ValueNoise(p) * 2.0 - 1.0);
        n = n * n;
        sum += n * amp * prev;
        prev = n;
        p = mul(kNoiseRot, p) * 2.02;
        amp *= 0.5;
    }
    return sum;
}

float4 main(PSInput input) : SV_TARGET
{
    const int   type   = (int)(params0.x + 0.5);
    const float seed   = params0.z;
    const float3 seedO = float3(seed, seed * 1.7, seed * 0.3);

    float3 N  = normalize(input.objectDir);   // planet-fixed surface direction
    float3 Nw = normalize(input.normalWS);     // world normal (lighting)
    float3 L  = normalize(sunDir.xyz);
    float3 V  = normalize(-input.positionWS);  // camera at origin (camera-relative)

    const bool hasOcean = (type == 0);

    // --- Continent height field --------------------------------------------
    // Domain-warp the sampling point a little for more organic coastlines.
    float3 wp = N * 2.1 + seedO;
    float3 warp = float3(Fbm3(wp + 5.2), Fbm3(wp + 9.1), Fbm3(wp + 1.7));
    float h = Fbm5(wp + 0.6 * warp) * 0.5 + 0.5;   // ~[0,1]

    float seaLevel   = params0.y;
    float coastWidth = max(shallowColor.w, 0.001);
    float landMask   = hasOcean ? smoothstep(seaLevel - coastWidth, seaLevel + coastWidth, h)
                                : 1.0;

    // --- Elevation & mountains (land only) ---------------------------------
    float landSpan = max(1.0 - seaLevel, 0.05);
    float elev     = hasOcean ? saturate((h - seaLevel) / landSpan) : saturate(h);
    float mtn      = Ridged3(N * 5.7 + seedO * 1.3);
    elev = saturate(elev + mtn * 0.35 * landMask);

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
    landColor *= 0.9 + 0.2 * ValueNoise(N * 40.0 + seedO);

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
        col += night.rgb * night.w * cities;
    }

    return float4(col, 1.0);
}
