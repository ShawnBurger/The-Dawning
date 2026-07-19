#ifndef THE_DAWNING_BRDF_COMMON_HLSLI
#define THE_DAWNING_BRDF_COMMON_HLSLI

// =============================================================================
// brdf_common.hlsli — microfacet BRDF shared by the raster and DXR paths
// =============================================================================
// Both render paths previously carried their own copy of these functions. That
// duplication is what let them drift: the tangent-frame handedness, the normal
// transform and the sky evaluation all diverged between the two, while the ONE
// thing they reliably kept in common was a bug — an identical additive epsilon
// in the GGX denominator that flattened every glossy highlight in both. Shared
// code stays consistent; duplicated code diverges.
//
// Anything here must compile under BOTH toolchains:
//   - FXC, ps_5_1  (raster: basic_ps.hlsl)
//   - DXC, lib_6_3 (DXR:    path_trace.hlsl)
// So: no SM 6.x intrinsics, no wave ops, no ray-tracing types.
//
// Convention: `roughness` is perceptual/artist-facing. alpha = roughness^2 is
// applied inside these functions — do not pre-square at the call site.
// =============================================================================

static const float PI = 3.14159265358979323846f;

// -----------------------------------------------------------------------------
// Fresnel — Schlick approximation
// -----------------------------------------------------------------------------
float3 DawningFresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// -----------------------------------------------------------------------------
// Normal distribution — GGX / Trowbridge-Reitz
// -----------------------------------------------------------------------------
// The denominator uses a MULTIPLICATIVE floor, never an additive epsilon. At the
// lobe peak d == a2, so the true denominator is PI*a2*a2 — for roughness below
// about 0.35 that is smaller than 1e-4, and an added epsilon would dominate it
// and delete the narrow high-energy peak that is the entire point of GGX. At
// roughness 0.1 the additive form returned 0.03% of the correct value.
float DawningDistributionGGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-7f);
}

// -----------------------------------------------------------------------------
// Geometry / masking — Smith, separable, Schlick-GGX with the k = (r+1)^2/8
// remapping. The separability matters: it is what lets VNDF sampling reduce to
// F * G1(NdotL) with no explicit PDF division. If this is ever changed to a
// height-correlated form, DawningSampleGGXVNDF's documented weight changes too.
// -----------------------------------------------------------------------------
float DawningGeometrySmithG1(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / (NdotV * (1.0f - k) + k);
}

float DawningGeometrySmith(float NdotV, float NdotL, float roughness)
{
    return DawningGeometrySmithG1(NdotV, roughness) *
           DawningGeometrySmithG1(NdotL, roughness);
}

// -----------------------------------------------------------------------------
// Cook-Torrance specular term: D * G * F / (4 * NdotV * NdotL)
// -----------------------------------------------------------------------------
// Multiplicative floor again, for the same reason as the NDF. Both paths
// previously wrote `/ (4 * NdotV * NdotL + 0.0001)`; at grazing angles
// 4*NdotV*NdotL is legitimately of that order, so the epsilon was a meaningful
// fraction of the denominator rather than a negligible guard.
//
// The floor cannot blow up: Smith G1 goes to zero as NdotV goes to zero, so the
// numerator vanishes faster than the clamped denominator.
float3 DawningCookTorranceSpecular(float NdotV, float NdotL, float NdotH,
                                   float VdotH, float roughness, float3 F0)
{
    float3 F = DawningFresnelSchlick(VdotH, F0);
    float  D = DawningDistributionGGX(NdotH, roughness);
    float  G = DawningGeometrySmith(NdotV, NdotL, roughness);
    return (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f);
}

// Energy left for diffuse after specular reflection. Metals have no diffuse lobe.
float3 DawningDiffuseWeight(float3 F, float metallic)
{
    return (1.0f - F) * (1.0f - metallic);
}

// Standard dielectric base reflectance, lerped toward albedo for metals.
float3 DawningF0(float3 albedo, float metallic)
{
    return lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
}

// -----------------------------------------------------------------------------
// Sampling helpers
// -----------------------------------------------------------------------------
// Branchless orthonormal basis around N (Duff et al. 2017).
void DawningBuildOrthonormalBasis(float3 N, out float3 T, out float3 B)
{
    float s = N.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (s + N.z);
    float b = N.x * N.y * a;
    T = float3(1.0f + s * N.x * N.x * a, s * b, -s * N.x);
    B = float3(b, s + N.y * N.y * a, -N.y);
}

// Sample the GGX distribution of VISIBLE normals (Heitz 2018). `Ve` is the view
// direction in tangent space with the shading normal along +Z; the returned
// half-vector is in that same space. `alpha` is roughness^2, already squared.
//
// Importance-sampling visible normals rather than perturbing a mirror direction
// is what makes the Monte Carlo weight tractable: with the separable Smith G
// above, f*cos/pdf collapses to exactly F * G1(NdotL) — no PDF division, and no
// D or G evaluation needed at the sample point.
float3 DawningSampleGGXVNDF(float3 Ve, float alpha, float2 u)
{
    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0f ? float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lensq)
                             : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(Vh, T1);

    float r   = sqrt(u.x);
    float phi = 2.0f * PI * u.y;
    float t1  = r * cos(phi);
    float t2  = r * sin(phi);
    float s   = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * sqrt(saturate(1.0f - t1 * t1)) + s * t2;

    float3 Nh = t1 * T1 + t2 * T2 + sqrt(saturate(1.0f - t1 * t1 - t2 * t2)) * Vh;

    return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0f, Nh.z)));
}

// -----------------------------------------------------------------------------
// Misc
// -----------------------------------------------------------------------------
float DawningLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

#endif // THE_DAWNING_BRDF_COMMON_HLSLI
