// =============================================================================
// atmosphere_ps.hlsl — analytic single-scattering planetary atmosphere
// =============================================================================
// Nishita/O'Neil single-scattering: for the view ray through this fragment of the
// atmosphere shell, integrate Rayleigh + Mie in-scattering between the atmosphere
// entry and either the atmosphere exit or the planet surface, attenuated by the
// optical depth to the camera AND to the star. Output is the in-scattered radiance
// premultiplied, with an alpha equal to the view-ray extinction, so the pass
// composites as  result = inScatter + background * transmittance  (ONE / INV_SRC_ALPHA):
// the atmosphere both GLOWS (limb, sky) and DIMS the surface/space behind it.
//
// All lengths are metres: the pass runs only in true-scale views (K = 1), so the
// camera-relative space is the physical world scaled by 1. The eye is the origin.

cbuffer AtmosphereConstants : register(b0)
{
    float4x4 g_world;
    float4x4 g_viewProj;
    float4   g_planetCenterRadius;  // xyz = centre, w = planet radius Rp
    float4   g_sunDirAtmoRadius;    // xyz = dir TO star, w = atmosphere radius Ra
    float4   g_betaRayleighG;       // xyz = betaR (1/m), w = Mie g
    float4   g_betaMieHeights;      // x = betaM (1/m), y = Hr, z = Hm, w = sun intensity
};

struct PSInput
{
    float4 clip     : SV_POSITION;
    float3 worldPos : TEXCOORD0;
};

static const float PI = 3.14159265358979;

// Ray/sphere: returns (tNear, tFar). No hit -> (1, -1) (an empty interval). Single
// exit — FXC's flow analysis reports X4000 (potentially uninitialised return) on the
// early-return form, the same trap SampleShadowCascade in basic_ps.hlsl documents.
float2 RaySphere(float3 ro, float3 rd, float3 centre, float radius)
{
    float3 oc = ro - centre;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - radius * radius;
    float disc = b * b - c;
    float2 result = float2(1.0, -1.0);
    if (disc >= 0.0)
    {
        float s = sqrt(disc);
        result = float2(-b - s, -b + s);
    }
    return result;
}

float4 main(PSInput input) : SV_TARGET
{
    const float3 planet = g_planetCenterRadius.xyz;
    const float  Rp     = g_planetCenterRadius.w;
    const float  Ra     = g_sunDirAtmoRadius.w;
    const float3 sunDir = g_sunDirAtmoRadius.xyz;
    const float3 betaR  = g_betaRayleighG.xyz;
    const float  gMie   = g_betaRayleighG.w;
    const float  betaM  = g_betaMieHeights.x;
    const float  Hr     = g_betaMieHeights.y;
    const float  Hm     = g_betaMieHeights.z;
    const float  sunI   = g_betaMieHeights.w;

    const float3 eye = float3(0.0, 0.0, 0.0);
    const float3 rd  = normalize(input.worldPos);

    // The view-ray segment inside the atmosphere, clipped to the front of the planet.
    float2 atmo = RaySphere(eye, rd, planet, Ra);
    float tStart = max(atmo.x, 0.0);
    float tEnd   = atmo.y;
    if (tEnd <= tStart)
        discard; // ray misses the atmosphere (shell rasterised a grazing edge)

    float2 pl = RaySphere(eye, rd, planet, Rp);
    if (pl.x > 0.0 && pl.x < pl.y)
        tEnd = min(tEnd, pl.x); // the planet surface occludes the far atmosphere

    const int   kViewSamples = 16;
    const int   kSunSamples  = 8;
    const float betaMExt     = betaM * 1.1; // Mie extinction ~ 1.1x its scattering

    float segLen = (tEnd - tStart) / kViewSamples;
    float opticalR = 0.0; // view-side optical depth accumulators (Rayleigh, Mie)
    float opticalM = 0.0;
    float3 sumR = float3(0.0, 0.0, 0.0);
    float3 sumM = float3(0.0, 0.0, 0.0);

    float t = tStart + segLen * 0.5;
    [loop]
    for (int i = 0; i < kViewSamples; ++i)
    {
        float3 P = eye + rd * t;
        float  h = max(length(P - planet) - Rp, 0.0);
        float  dR = exp(-h / Hr) * segLen;
        float  dM = exp(-h / Hm) * segLen;
        opticalR += dR;
        opticalM += dM;

        // In shadow if the star is below the local horizon (the sun ray hits the
        // planet before leaving): no direct sunlight to in-scatter here.
        float2 sunPl = RaySphere(P, sunDir, planet, Rp);
        bool lit = !(sunPl.x < sunPl.y && sunPl.y > 0.0);
        if (lit)
        {
            float2 sunAtmo = RaySphere(P, sunDir, planet, Ra);
            float  sunSeg  = sunAtmo.y / kSunSamples; // P is inside, so sunAtmo.y > 0
            float  sunOptR = 0.0;
            float  sunOptM = 0.0;
            float  tj = sunSeg * 0.5;
            [loop]
            for (int j = 0; j < kSunSamples; ++j)
            {
                float3 Pj = P + sunDir * tj;
                float  hj = max(length(Pj - planet) - Rp, 0.0);
                sunOptR += exp(-hj / Hr) * sunSeg;
                sunOptM += exp(-hj / Hm) * sunSeg;
                tj += sunSeg;
            }

            float3 tau  = betaR * (opticalR + sunOptR) + betaMExt * (opticalM + sunOptM);
            float3 attn = exp(-tau);
            sumR += dR * attn;
            sumM += dM * attn;
        }

        t += segLen;
    }

    float mu = dot(rd, sunDir);
    float phaseR = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
    float g2 = gMie * gMie;
    float phaseM = 3.0 / (8.0 * PI) *
                   ((1.0 - g2) * (1.0 + mu * mu)) /
                   ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * gMie * mu, 1e-4), 1.5));

    float3 inScatter = sunI * (sumR * betaR * phaseR + sumM * betaM * phaseM);

    // View-ray extinction -> alpha, so the background is dimmed by exactly what the
    // atmosphere absorbs/out-scatters along the same ray.
    float3 viewTrans = exp(-(betaR * opticalR + betaMExt * opticalM));
    float  opacity   = saturate(1.0 - dot(viewTrans, float3(1.0, 1.0, 1.0) / 3.0));

    return float4(max(inScatter, 0.0), opacity);
}
