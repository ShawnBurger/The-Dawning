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
// THE ONE IMPLEMENTATION. The two extra `out` parameters report the exact
// prefiltered radiance this evaluation LOADED and the exact direction it loaded
// it along - not a second fetch, not a recomputed reflection vector.
//
// They exist for shaders/ibl_consume_probe.hlsli, which needs the shading load
// itself as evidence. A probe that re-derived R and re-sampled the cube would
// witness its own read, which is the failure mode the draw-record probe was
// rebuilt to remove; handing the values out of the shipped evaluation is what
// makes the witness be about the shipped evaluation.
//
// FXC dead-code-eliminates both outputs in the permutation that ignores them, so
// the ordinary PSO pays nothing and - the property that matters more - both
// permutations run bit-identical shading maths.
float3 DawningSpecularIBLWitnessed(TextureCube<float4> envCube, SamplerState envSampler,
                                   float3 N, float3 V, float roughness, float3 F0,
                                   float mipCount,
                                   out float3 outPrefiltered, out float3 outR)
{
    float  NdotV = saturate(dot(N, V));
    float3 R     = reflect(-V, N);

    float3 prefiltered = DawningPrefilteredRadiance(envCube, envSampler, R, roughness, mipCount);
    float2 ab          = DawningEnvBRDFApprox(NdotV, roughness);

    outPrefiltered = prefiltered;
    outR           = R;
    return prefiltered * (F0 * ab.x + ab.y);
}

// The plain signature, for callers with nothing to witness. A forwarder, NOT a
// copy: there is still exactly one evaluation of the split-sum in this tree.
float3 DawningSpecularIBL(TextureCube<float4> envCube, SamplerState envSampler,
                          float3 N, float3 V, float roughness, float3 F0,
                          float mipCount)
{
    float3 unusedRadiance;
    float3 unusedR;
    return DawningSpecularIBLWitnessed(envCube, envSampler, N, V, roughness, F0,
                                       mipCount, unusedRadiance, unusedR);
}

// =============================================================================
// Specular occlusion - IBL_DESIGN.md 9.3's deferred item
// =============================================================================
// WHAT WAS WRONG. The split-sum has no visibility term, so `envSpecular` was the
// environment a surface would see if nothing blocked it. The only thing standing
// in for visibility was `envSpecular *= ambientOcclusion` - the DIFFUSE AO map
// applied unchanged to a specular lobe. That is wrong in a specific, directional
// way, and it is wrong in BOTH directions:
//
//   too dark   a smooth surface's specular lobe is narrow. It samples a tiny
//              solid angle around R, and a hemispherical average of blockers has
//              almost no claim on whether THAT cone is blocked. Multiplying by a
//              hemispherical AO of 0.5 halves a reflection that is very likely
//              unoccluded.
//   too bright at roughness 1 the specular lobe IS hemispherical, so diffuse AO
//              is very nearly the right answer there - and the old code was as
//              wrong at roughness 0 as it was right at roughness 1, with no
//              dependence on either roughness or view direction to tell them
//              apart.
//
// THE REMAP (Lagarde & de Rousiers, "Moving Frostbite to PBR", listing 26). It
// is a function of the SAME AO value plus the two quantities that decide how much
// of the hemisphere the lobe actually covers:
//
//     specOcc = saturate(pow(NdotV + AO, exp2(-16*roughness - 1)) - 1 + AO)
//
// Every corner of it is checkable by hand, which is why this form was chosen over
// a tuned curve:
//
//   AO = 1            -> pow(NdotV + 1, e) with e in (0, 0.5] and base >= 1, so
//                        the result is >= 1 and saturate pins it to EXACTLY 1.
//                        A surface with no occlusion data is untouched. THIS IS
//                        THE MOST IMPORTANT CORNER IN THIS FILE - see below.
//   AO = 0            -> pow(NdotV, e) - 1, which is <= 0 for NdotV <= 1, so
//                        saturate pins it to 0. Fully occluded stays fully
//                        occluded.
//   roughness -> 1    -> e = exp2(-17) ~ 7.6e-6, pow(.,e) -> 1, so specOcc -> AO.
//                        It DEGENERATES to the old behaviour exactly where the old
//                        behaviour was right.
//   roughness -> 0    -> e = 0.5, so at AO 0.5 and NdotV 1 specOcc = 0.7247
//                        against a raw AO of 0.5. The narrow lobe keeps its
//                        reflection.
//
// So this is NOT "AO applied to specular"; it reduces to that only at roughness 1
// and departs from it monotonically as the lobe narrows. The probe asserts the
// departure directly (DAWNING_IBL_PROBE_SPEC_OCC_ABOVE_AO) rather than trusting
// this comment - a later "simplification" back to `* ambientOcclusion` sends that
// word to zero.
//
// *** WHAT THIS DOES FOR AN ASSET THAT SHIPS NO OCCLUSION MAP: NOTHING. ***
//
// Stated here rather than in a report, because it is the case that motivated the
// work. The Meshy corridor ships base_color / metallic / roughness / normal and
// no AO, so ambientOcclusion is 1.0 on every one of its texels, so specOcc is
// EXACTLY 1.0 by the first corner above and this function is an identity there.
// The corridor's overshoot against the DXR-full reference is MACRO-scale
// geometric visibility - a concave tube whose own walls block most of the sky -
// and no remap of an absent AO map can supply that. Closing it needs actual
// visibility: a traced ray, a screen-space trace, or a baked AO map for the
// asset. This function is the correct fix for the term it replaces; it is not,
// and cannot be, a fix for missing occlusion data.
//
// NEVER APPLY THIS TO DIRECT LIGHT. Occlusion multiplies environment terms only.
// Direct light is already gated by the shadow map in raster and by the NEE shadow
// ray in DXR; multiplying it here would double-count that visibility. That rule
// predates IBL (IBL_DESIGN.md 9.3) and this change does not touch it.
//
// Single expression, no branches, so FXC's X4000 analysis has nothing to say. The
// max() on the base is not slack: pow(0, x) is exp2(x * log2(0)) and evaluating
// log2(0) is a way to get a NaN into the frame buffer on some drivers, and
// NdotV + AO is genuinely 0 at a grazing pixel of a fully occluded texel.
float DawningSpecularOcclusion(float NdotV, float roughness, float ao)
{
    float e = exp2(-16.0f * saturate(roughness) - 1.0f);
    return saturate(pow(max(saturate(NdotV) + saturate(ao), 1e-4f), e) - 1.0f + saturate(ao));
}

// =============================================================================
// Toksvig normal-variance -> roughness (IBL_DESIGN.md section 10)
// =============================================================================
// WHAT WAS WRONG. Filtering a normal map averages tangent-space normals toward
// flat and DISCARDS the sub-pixel normal variance that was acting as roughness.
// The shading normal comes out more coherent than the surface warrants and the
// roughness comes out unchanged and therefore too low. Both errors point the same
// way: too sharp a reflection of too coherent a direction. MEASURED when ray-cone
// texture LOD landed: the capture's distant band brightened 130.8 -> 143.0 out of
// 255 and DXR's mean luminance moved AWAY from raster. The raster path has the
// identical defect and takes a smaller hit only because anisotropic filtering
// flattens less than an isotropic cone does.
//
// IT MATTERS MORE UNDER IBL THAN IT DID BEFORE. Roughness used to reach the
// environment through a bounded `lerp(0.2, 0.8, 1 - roughness)` gloss ramp; it now
// selects a MIP of the prefiltered cube, which is a strong, high-dynamic-range
// lookup. A surface whose roughness is understated samples too sharp a mip.
//
// THE SIGNAL IS ALREADY IN THE FETCH AND COSTS NOTHING TO READ. A filtered normal
// map returns an average of unit vectors, and the average of unit vectors that
// disagree is SHORTER than unit. |n| < 1 is exactly the variance that was
// discarded, and it arrives in the same fetch the shading normal comes from - no
// extra tap, no extra channel, no cooked variance map.
//
// TOKSVIG (2005), via the standard Blinn-power <-> GGX-alpha bridge
// (Karis, "Real Shading in UE4": alpha = sqrt(2 / (power + 2))):
//
//     power  = 2 / alpha^2 - 2          with GGX alpha = roughness^2
//     ft     = |n| / (|n| + power * (1 - |n|))
//     power' = ft * power
//     alpha' = sqrt(2 / (power' + 2))
//     rough' = sqrt(alpha')
//
// At |n| = 1 this is the EXACT identity rough' == rough, algebraically and not
// merely to a tolerance, which is the property that makes it safe to run on every
// surface unconditionally. It is monotonic and can only increase roughness.
//
// THE UNORM8 FLOOR IS DERIVED, NOT TUNED. Every normal map in this engine is
// RGBA8, so a stored unit normal decodes with a per-component error of at most
// half a step: 1/255 in [-1, 1] units. The length error is therefore bounded by
// sqrt(3)/255 = 0.0068, and a |n| within that of 1.0 carries NO information - it
// is indistinguishable from encoding noise on a perfectly flat texel. Feeding it
// to the formula above would be catastrophic rather than merely inaccurate,
// because at low roughness `power` is enormous (781246 at roughness 0.04) and ft
// is correspondingly tiny: a length of 0.9932 that is pure quantisation would
// widen roughness 0.1 to 0.42 across every flat surface in the scene. So the
// deficit is relieved by exactly that bound and no more. This is the one constant
// here, and it comes from the texture format rather than from an eyeball.
#define DAWNING_UNORM8_NORMAL_LENGTH_EPSILON 0.0068f

float DawningToksvigRoughness(float roughness, float normalLength)
{
    // Relieve the quantisation floor, then clamp. The lower bound keeps ft
    // finite; a normal map texel whose decoded length is under 1/1000 is not a
    // normal, it is a hole, and widening to roughness 1 is the right answer there.
    float len = clamp(normalLength + DAWNING_UNORM8_NORMAL_LENGTH_EPSILON, 1e-3f, 1.0f);

    float alpha  = max(roughness * roughness, 1e-4f);
    float power  = max(2.0f / (alpha * alpha) - 2.0f, 0.0f);
    float ft     = len / max(len + power * (1.0f - len), 1e-8f);
    float power2 = ft * power;
    float alpha2 = sqrt(2.0f / (power2 + 2.0f));

    // max() rather than assignment: the algebra already guarantees alpha2 >= alpha
    // at len <= 1, and this makes a future edit that breaks that property fail
    // safe rather than SHARPEN a surface. Toksvig may only ever add roughness.
    return saturate(max(sqrt(alpha2), roughness));
}

#endif // THE_DAWNING_IBL_COMMON_HLSLI
