// =============================================================================
// ssao_ps.hlsl — screen-space ambient occlusion from the camera depth prepass
// =============================================================================
// Estimates, per pixel, how much of the hemisphere above the surface is blocked by
// nearby geometry, so the IBL ambient can be darkened in creases and contact areas
// instead of washing shadows out flat. Output is a single R8 factor: 1 = fully
// open, 0 = fully occluded.
//
// Depth is the reversed-Z camera depth from the prepass (kMainDepthClear = 0 = far,
// GREATER compare). For the reversed-Z infinite-far projection the mapping is
//   depth d = near / z_view   ->   z_view = near / d,
// so a LARGER d is CLOSER to the camera. View-space normals are reconstructed from
// screen-space derivatives of the reconstructed position (no normal buffer exists).
// The kernel is generated inline (golden-angle spiral over a cosine-weighted
// hemisphere) so no per-frame kernel upload is needed.
// =============================================================================

Texture2D<float> depthTex : register(t0);

cbuffer SSAOConstants : register(b0)
{
    float  g_tanHalfFovY; // tan(vertical fov / 2)
    float  g_aspect;      // width / height
    float  g_near;        // camera near plane (reversed-Z: d = near / z_view)
    float  g_radius;      // view-space sample radius (metres)
    float2 g_screenSize;  // render-target size in pixels
    float  g_bias;        // depth range bias (metres) to suppress self-occlusion
    float  g_intensity;   // AO darkening strength
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

// Reversed-Z infinite-far: d = near / z_view.  d == 0 is the far plane / cleared sky.
float ViewZ(float d)
{
    return (d > 1e-12) ? (g_near / d) : 1e30;
}

// View-space position from a screen UV and its depth.
float3 ViewPosFromUV(float2 uv, float d)
{
    float z = ViewZ(d);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(ndc.x * g_tanHalfFovY * g_aspect * z,
                  ndc.y * g_tanHalfFovY * z,
                  z);
}

// View-space point -> screen UV (left-handed perspective, +z into the screen).
float2 UVFromViewPos(float3 p)
{
    float2 ndc = float2(p.x / (g_tanHalfFovY * g_aspect * p.z),
                        p.y / (g_tanHalfFovY * p.z));
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float Hash(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float4 main(PSInput input) : SV_TARGET
{
    int2 pixel = int2(input.positionCS.xy);
    float d = depthTex.Load(int3(pixel, 0));
    if (d <= 1e-12)
        return 1.0; // far plane / sky — nothing to occlude

    float3 P = ViewPosFromUV(input.uv, d);

    // Surface normal from screen-space derivatives of the reconstructed position.
    // No normal buffer exists, so this is the only source. Face it toward the camera
    // (-z in view space) so the hemisphere points away from the surface.
    float3 N = normalize(cross(ddx(P), ddy(P)));
    if (N.z > 0.0)
        N = -N;

    // Per-pixel rotated tangent basis, so the fixed kernel dithers between pixels.
    float rnd = Hash(input.positionCS.xy) * 6.2831853;
    float3 randDir = float3(cos(rnd), sin(rnd), 0.0);
    float3 T = normalize(randDir - N * dot(randDir, N));
    float3 B = cross(N, T);
    float3x3 TBN = float3x3(T, B, N);

    const int kSamples = 16;
    const float kGoldenAngle = 2.3999632; // radians

    float occlusion = 0.0;
    [unroll]
    for (int i = 0; i < kSamples; ++i)
    {
        // Golden-angle spiral over the unit disc, lifted onto the hemisphere; the
        // radius is biased toward the origin so nearby geometry dominates.
        float t     = (float(i) + 0.5) / float(kSamples);
        float discR = sqrt(t);
        float ang   = float(i) * kGoldenAngle;
        float3 s    = float3(cos(ang) * discR, sin(ang) * discR, sqrt(1.0 - t));
        s *= lerp(0.1, 1.0, t * t);

        float3 samplePos = P + mul(s, TBN) * g_radius;
        float2 suv = UVFromViewPos(samplePos);
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0)
            continue;

        float sd = depthTex.Load(int3(int2(suv * g_screenSize), 0));
        if (sd <= 1e-12)
            continue; // sampled the sky
        float sceneZ = ViewZ(sd);

        // Occluded when the real surface at that pixel is CLOSER to the camera than
        // the sample point (smaller z_view), with a range check so a distant
        // background silhouette does not darken a foreground pixel.
        float rangeCheck = smoothstep(0.0, 1.0, g_radius / max(abs(P.z - sceneZ), 1e-4));
        occlusion += (sceneZ < samplePos.z - g_bias) ? rangeCheck : 0.0;
    }

    float ao = 1.0 - (occlusion / float(kSamples)) * g_intensity;
    return saturate(ao);
}
