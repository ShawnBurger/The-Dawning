#ifndef THE_DAWNING_IBL_COMMON_HLSLI
#define THE_DAWNING_IBL_COMMON_HLSLI

// =============================================================================
// ibl_common.hlsli - the ONE evaluation of image-based lighting
// =============================================================================
// docs/research/IBL_DESIGN.md sections 3, 4 and 8.3. This header is layer 2 of
// the anti-divergence mechanism: sky_common.hlsli makes both render paths agree
// on what the environment IS, and this file makes them agree on how a surface
// RESPONDS to it. Both are structural - one copy of one function - rather than
// something a test has to police.
//
// Anything here must compile under BOTH toolchains, exactly as
// brdf_common.hlsli must:
//   - FXC, ps_5_1 /WX  (raster: basic_ps.hlsl, ibl_eval_probe_ps.hlsl)
//   - DXC, lib_6_3     (DXR:    path_trace.hlsl, when Stage 4 lands)
// So: no SM 6.x intrinsics, no wave ops, no ray-tracing types, and single-exit
// functions - FXC reports X4000 "potentially uninitialized" for returns inside
// branches and the raster path builds with /WX.
//
// RESOURCES ARE FUNCTION PARAMETERS, and that is load-bearing rather than
// stylistic. It is what lets the raster and DXR declarations differ - different
// registers, different spaces, different samplers - while the EVALUATION stays
// one copy of one function. IBL_DESIGN.md section 6.3 left "does FXC accept
// TextureCube/SamplerState as parameters" open; it does, and the standing proof
// is that EnvironmentIBL::CreatePipelines compiles ibl_eval_probe_ps.hlsl - which
// includes this header and calls DawningSpecularIBL - with fxc /T ps_5_1 /WX on
// every launch. If that ever stops being true the engine fails at startup rather
// than silently.
//
// This header assumes brdf_common.hlsli has already been included: it uses PI.
// =============================================================================

// =============================================================================
// Diffuse - L2 spherical harmonics
// =============================================================================
// TWIN OF C++. DawningSHBasisL2 below is a hand-written mirror of
// core::SHBasisL2 in src/core/sh_irradiance.cpp, which is the projector that
// produced the coefficients this function consumes. The header of
// src/core/sh_irradiance.h states what that costs and what watches it:
// EnvironmentIBL evaluates DawningIrradianceSH HERE, on the GPU, for 64
// directions against the SHIPPED coefficients, and compares against
// core::EvaluateIrradiance on the CPU within a tolerance. The verdict is the
// [SMOKE] ibl_sh_agreement marker, and it runs on every launch in every mode.
//
// A SIGN CONVENTION IN THIS BASIS IS NOT FREE THE WAY IT IS ON THE C++ SIDE.
// Over there the same expressions serve projection and evaluation, so a flipped
// sign cancels. Here only the evaluation lives; the projection is in C++. A
// one-sided flip is therefore a real defect that produces a smooth, plausible
// image lit from the wrong direction, and the GPU probe is the only thing that
// sees it. Keep the nine lines below identical to core::SHBasisL2, in order.
// =============================================================================
void DawningSHBasisL2(float3 d, out float y[9])
{
    y[0] = 0.282095f;
    y[1] = 0.488603f * d.y;
    y[2] = 0.488603f * d.z;
    y[3] = 0.488603f * d.x;
    y[4] = 1.092548f * d.x * d.y;
    y[5] = 1.092548f * d.y * d.z;
    y[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    y[7] = 1.092548f * d.x * d.z;
    y[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

// E(N) = integral L(w) max(N.w, 0) dw.
//
// `sh` holds coefficients that ALREADY have the Lambertian transfer constants
// A0 = pi, A1 = 2pi/3, A2 = pi/4 folded in - see core::PackIrradianceCoefficients.
// Do NOT reintroduce them here. Putting those three constants on both sides of
// the C++/HLSL boundary is how they come to disagree, and a mismatch reads as
// "the ambient needs tuning" rather than as a bug.
//
// The caller divides by PI for the Lambert BRDF; this returns irradiance, not
// outgoing radiance.
float3 DawningIrradianceSH(float4 sh[9], float3 N)
{
    float basis[9];
    DawningSHBasisL2(N, basis);

    float3 e = (float3)0.0f;
    [unroll]
    for (int i = 0; i < 9; ++i)
        e += sh[i].rgb * basis[i];

    // The projection is exact only for the bands it keeps; a steep environment
    // can push a reconstruction slightly negative at grazing normals, and a
    // negative irradiance multiplied into albedo is a black rim that looks like
    // a normal-map bug. Clamping is the standard remedy and costs one max.
    return max(e, (float3)0.0f);
}

// =============================================================================
// Specular - split-sum (Karis), with the ANALYTIC env-BRDF fit
// =============================================================================

// -----------------------------------------------------------------------------
// Fresnel with a roughness-aware horizon term.
// -----------------------------------------------------------------------------
// Plain Schlick drives F to 1 at grazing regardless of roughness, which on a
// rough surface reflects a mirror-sharp rim of environment that physically is
// not there. The max(1-roughness, F0) form is the standard fix (Lagarde).
//
// This is the ENVIRONMENT Fresnel. It takes NdotV, because it stands in for an
// integral over the whole hemisphere. The DIRECT lighting Fresnel in
// brdf_common.hlsli takes VdotH, the half-vector of one actual light. They are
// different integrals of the same physics, not the same quantity computed twice
// - which is why adding both terms is not a double count (IBL_DESIGN.md 9.2).
float3 DawningFresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 horizon = max((float3)(1.0f - roughness), F0);
    return F0 + (horizon - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// -----------------------------------------------------------------------------
// Environment BRDF (Lazarov 2013, as used by Karis). Returns (A, B) for
//     specular = prefilteredRadiance * (F0 * A + B)
// -----------------------------------------------------------------------------
// A LUT would be the reference; this is a polynomial fit accurate to a few
// percent over most of the domain, worst at low roughness and grazing angles.
// The trade is deliberate and IBL_DESIGN.md section 3 states it: zero descriptor
// slots against about ten ALU, in a shader that already runs nine
// SampleCmpLevelZero, against a 128-slot heap that renderer.cpp:784-786 names as
// the scarce budget in as many words - "Spend DWORDs, not heap slots."
//
// *** DO NOT RE-EXPRESS THIS IN TERMS OF DawningGeometrySmithG1. ***
//
// They look interchangeable and they are not. brdf_common.hlsli's Smith uses the
// DIRECT-lighting remapping k = (r+1)^2/8; the split-sum BRDF integral - LUT or
// fit - is derived with the IBL remapping k = alpha/2 = r^2/2. Both are correct
// and they integrate different things. A later "cleanup" that unifies them
// silently darkens every rough surface's environment specular, with no compile
// error and no assertion in this tree that would notice except assertion 3.1.
// brdf_common.hlsli:50-54 already carries a warning of exactly this shape about
// the VNDF weight; this is the second instance of the same hazard.
//
// Single expression, no branches, so FXC's X4000 analysis has nothing to say.
float2 DawningEnvBRDFApprox(float NdotV, float roughness)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f,  0.022f);
    const float4 c1 = float4( 1.0f,  0.0425f,  1.04f,  -0.04f);
    float4 r = roughness * c0 + c1;
    float  a004 = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
}

// -----------------------------------------------------------------------------
// Prefiltered radiance fetch.
// -----------------------------------------------------------------------------
// roughness -> mip is LINEAR and must be exactly the inverse of what the
// prefilter generated: EnvironmentIBL::RecordBake renders mip m at
// roughness = m / (mips - 1), so the lookup is mip = roughness * (mips - 1).
//
// A mismatch here is INVISIBLE to inspection - it reads as "reflections are a
// bit too blurry" - which is why the [SMOKE] ibl_spec_mip_monotonic assertion
// exists rather than an eyeball. mipCount arrives as a float in iblParams.x
// precisely so there is no int/float split in the constant buffer.
//
// SampleLevel, not Sample: the mip is a continuous function of roughness, chosen
// analytically, and hardware derivative-based selection would fight it. It also
// makes this callable from a non-pixel stage, which Stage 4's DXR consumer needs.
float3 DawningPrefilteredRadiance(TextureCube<float4> envCube, SamplerState envSampler,
                                  float3 R, float roughness, float mipCount)
{
    float mip = saturate(roughness) * max(mipCount - 1.0f, 0.0f);
    return envCube.SampleLevel(envSampler, R, mip).rgb;
}

// -----------------------------------------------------------------------------
// The split-sum specular integral.
// -----------------------------------------------------------------------------
// integral L(l) f(l,v) (n.l) dl  ~  [prefiltered(R, roughness)] * [F0*A + B]
//
// R = reflect(-V, N) is computed from CAMERA-RELATIVE V and N, and that is
// correct with no conversion: the environment is at infinity, so the lookup is
// indexed by DIRECTION only, and directions are invariant under the
// camera-relative translation both paths work in (RULE 1; IBL_DESIGN.md 6.6).
// This stops holding the day parallax-corrected local probes arrive - those need
// a world-space Vec3d subtraction against the probe volume. Not in scope; named
// so nobody reads this as a precedent.
//
// Known and accepted: the N = V = R assumption drops the elongated grazing lobe
// at high roughness, and single-scattering GGX loses energy at high roughness so
// rough metal comes out darker than ground truth. Multi-scatter compensation is
// a ~5-line follow-up (IBL_DESIGN.md 9.4) and is deliberately NOT folded in here.
float3 DawningSpecularIBL(TextureCube<float4> envCube, SamplerState envSampler,
                          float3 N, float3 V, float roughness, float3 F0,
                          float mipCount)
{
    float  NdotV = saturate(dot(N, V));
    float3 R     = reflect(-V, N);

    float3 prefiltered = DawningPrefilteredRadiance(envCube, envSampler, R, roughness, mipCount);
    float2 ab          = DawningEnvBRDFApprox(NdotV, roughness);

    return prefiltered * (F0 * ab.x + ab.y);
}

#endif // THE_DAWNING_IBL_COMMON_HLSLI
