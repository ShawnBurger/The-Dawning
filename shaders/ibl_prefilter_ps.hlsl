// =============================================================================
// ibl_prefilter_ps.hlsl - split-sum specular prefilter of the procedural sky
// =============================================================================
//
// One fullscreen triangle per (cube face, mip). The pass takes NO SRV INPUT:
// the environment is a closed-form function of direction, so mip m is an
// independent integral of ground truth rather than of a downsampled copy of
// mip m-1. That removes the ping-pong, the source descriptor, and the whole
// class of error where filtering degrades down the chain.
// docs/research/IBL_DESIGN.md section 2 argues this at length.
//
// Roughness -> mip is LINEAR: mip m is generated at roughness m / (mips - 1),
// and the eventual consumer must look it up with exactly that mapping. A
// mismatch between generation and lookup is invisible to the eye - it reads as
// "reflections are a bit too blurry" - which is why the design gates it with an
// assertion rather than an eyeball.
//
// This file has two entry points on purpose. PrefilterPS writes the shipped
// cubemap; DirectionPS writes a direction cubemap for the round-trip probe that
// verifies the face table. They share DawningCubeFaceDirection, so the probe
// tests the code path the prefilter actually uses rather than a copy of it. If
// they were separate files, the probe could pass while the prefilter used a
// different table.
// =============================================================================

#include "ibl_environment.hlsli"

static const float DAWNING_IBL_PI = 3.14159265358979323846f;

cbuffer IBLFaceConstants : register(b0)
{
    uint  g_faceIndex;    // 0..5, D3D cube face order
    float g_roughness;    // mip / (mipCount - 1)
    uint  g_sampleCount;  // Hammersley samples for the GGX integral
    float g_invFaceSize;  // 1 / faceSize for this mip
};

struct VSOutput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

// Van der Corput radical inverse, base 2. Plain SM 5.1 integer ops - no wave
// intrinsics, no SM 6.x, so this file compiles under FXC ps_5_1 /WX.
float DawningRadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 DawningHammersley(uint i, uint count)
{
    return float2(float(i) / float(count), DawningRadicalInverseVdC(i));
}

// GGX/Trowbridge-Reitz importance sample of the half-vector about N.
float3 DawningImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi      = 2.0f * DAWNING_IBL_PI * Xi.x;
    float cosTheta = sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));

    float3 hTangent = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    // Build a tangent frame around N. The 0.999 guard picks a different "up"
    // near the pole so the cross product does not degenerate.
    float3 up      = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangentX = normalize(cross(up, N));
    float3 tangentY = cross(N, tangentX);

    return tangentX * hTangent.x + tangentY * hTangent.y + N * hTangent.z;
}

// -----------------------------------------------------------------------------
// The shipped prefilter.
// -----------------------------------------------------------------------------
float4 PrefilterPS(VSOutput input) : SV_TARGET
{
    float3 N = normalize(DawningCubeFaceDirection(g_faceIndex, input.uv));

    float3 result;
    if (g_roughness <= 0.0f)
    {
        // Mip 0 is a MIRROR. Roughness 0 is a delta lobe, so integrating it
        // would only add sampling error to a value that is available exactly.
        // Assertion 3.3 in the design depends on this being exact.
        result = DawningEnvironmentRadiance(N);
    }
    else
    {
        // Karis split-sum prefilter with the N = V = R assumption.
        float3 radianceSum = float3(0.0f, 0.0f, 0.0f);
        float  weightSum   = 0.0f;

        for (uint i = 0u; i < g_sampleCount; ++i)
        {
            float2 Xi = DawningHammersley(i, g_sampleCount);
            float3 H  = DawningImportanceSampleGGX(Xi, N, g_roughness);
            float3 L  = 2.0f * dot(N, H) * H - N;   // reflect N about H, with V = N

            float NdotL = saturate(dot(N, L));
            if (NdotL > 0.0f)
            {
                radianceSum += DawningEnvironmentRadiance(L) * NdotL;
                weightSum   += NdotL;
            }
        }

        // DIVIDING BY THE WEIGHT SUM IS NOT OPTIONAL. Dropping it leaves every
        // rough mip scaled by an arbitrary factor that varies with roughness -
        // it does not look like a bug, it looks like an art choice, and every
        // rough surface in the engine is then systematically wrong. Assertion
        // 1.4 (per-mip mean luminance) exists to catch exactly this deletion.
        result = radianceSum / max(weightSum, 1e-4f);
    }

    return float4(result, 1.0f);
}

// -----------------------------------------------------------------------------
// Verification only: write the direction this texel was generated from.
// -----------------------------------------------------------------------------
// Deliberately UNNORMALISED - see the note on DawningCubeFaceDirection. The
// direction is affine in (u, v) across a face, so bilinear filtering of the
// unnormalised value is exact and the round trip measures the face table rather
// than the filter.
float4 DirectionPS(VSOutput input) : SV_TARGET
{
    return float4(DawningCubeFaceDirection(g_faceIndex, input.uv), 1.0f);
}
