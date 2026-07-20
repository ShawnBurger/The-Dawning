// =============================================================================
// ibl_eval_probe_ps.hlsl - GPU evidence for the SHIPPED IBL evaluation
// =============================================================================
// IBL_DESIGN.md section 11, Stages 2 and 3. Four 64x1 passes, all read back and
// asserted on the CPU by render::EnvironmentIBL.
//
// WHAT MAKES THESE PROBES AND NOT TESTS: every entry point below calls the same
// functions in the same shaders/ibl_common.hlsli that basic_ps.hlsl calls, on
// the same cube through the same trilinear-clamp sampler. They witness the value
// the SHIPPED code computes rather than re-implementing it and comparing two
// re-implementations. That is the standard the draw-record probe set in this
// tree and it is the reason these live in a shader at all.
//
// Compiling this file at startup with fxc /T ps_5_1 /WX is itself evidence: it
// is what resolves IBL_DESIGN.md section 6.3's open question - whether FXC
// accepts TextureCube and SamplerState as function parameters - at build time,
// every launch, rather than by assertion in a document.
//
// WHAT THESE DO NOT COVER, SAID PLAINLY: they witness ibl_common.hlsli. They do
// NOT witness basic_ps.hlsl's CALL SITE. A change that deleted the `if
// (iblParams.z != 0)` block from basic_ps leaves every assertion here green, and
// so does binding the WRONG DESCRIPTOR as the environment cube - these passes
// reach the cube through EnvironmentIBL's own SRV heap and cannot see what the
// raster root signature has bound at t0/space6.
//
// THAT IS NOW COVERED, and it is covered somewhere else on purpose:
// shaders/ibl_consume_probe.hlsli, written from basic_ps's consumption site into
// a UAV on a raster frame - "the draw-record probe's territory", which is where
// this note previously said the fix would have to live. It also runs a NEGATIVE
// CONTROL frame, so the claim is not merely that its assertions pass with the
// feature present but that they fail with it absent.
//
// Nothing in THIS file changed as a result. The paragraph above is still exactly
// true of every entry point below, and it stays here so the next reader does not
// mistake six green markers for evidence about the raster path.
//
// Every entry point is indexed by SV_POSITION.x, so probe slot i is pixel i and
// there is no separate index plumbing to get wrong.
// =============================================================================

#include "brdf_common.hlsli"    // PI, used by ibl_common.hlsli
#include "ibl_common.hlsli"

// The probe constant buffer. The first 64 float4s are the direction set shared
// with ibl_probe_ps.hlsl (which declares only that prefix); the tail is the same
// SH and parameter data BeginFrame uploads into CBPerFrame, so these passes
// evaluate the coefficients the frame shaders actually receive.
cbuffer IBLProbeConstants : register(b1)
{
    float4 g_probeDirections[64];
    float4 g_iblSH[9];
    float4 g_iblParams;          // x: mip count, y: intensity, z: enable, w: 0
};

TextureCube<float4> g_directionCube : register(t0);
TextureCube<float4> g_envCube       : register(t1);
SamplerState        g_linearClamp   : register(s0);

struct VSOutput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

// -----------------------------------------------------------------------------
// The (NdotV, roughness) grid for the env-BRDF pass.
// -----------------------------------------------------------------------------
// THIS IS A TWO-LINE MIRROR OF C++ (EnvironmentIBL::EnvBRDFProbeTuple), and it
// is disclosed rather than hidden. It is a pure function of the slot index with
// no floating-point subtlety, and both sides are pinned by the fact that the
// CPU's expectations are physical properties of the FIT rather than a golden
// table - a desynchronised grid would move the tuples into a region where the
// grazing-Fresnel and energy bounds no longer hold, and the assertions fail.
// 8x8 over the unit square, endpoints included.
void DawningProbeGrid(uint slot, out float NdotV, out float roughness)
{
    NdotV     = (float)(slot % 8u) / 7.0f;
    roughness = (float)(slot / 8u) / 7.0f;
}

// =============================================================================
// Pass 2 - diffuse SH irradiance. Closes the C++/HLSL BASIS mirror.
// =============================================================================
// Evaluates DawningIrradianceSH, the shipped evaluator, against the shipped
// coefficients. The CPU compares against core::EvaluateIrradiance. A one-sided
// sign flip or permutation between core::SHBasisL2 and DawningSHBasisL2 - the
// only kind that does not cancel, and the only kind that can happen, since the
// two are separate files - fails here and nowhere else in this repository.
float4 SHIrradianceProbePS(VSOutput input) : SV_TARGET
{
    uint slot = (uint)input.positionCS.x;
    float3 N = normalize(g_probeDirections[slot].xyz);
    return float4(DawningIrradianceSH(g_iblSH, N), 1.0f);
}

// =============================================================================
// Pass 3 - mirror specular. Mip 0 really is an exact evaluation of the sky.
// =============================================================================
// N = V = d, roughness 0, F0 = 1: the split-sum reduces to the mirror fetch
// times the env-BRDF scalar. rgb carries the shipped DawningSpecularIBL result;
// w carries the env-BRDF scalar the SHADER itself computed, so the CPU can
// divide it out and compare the remaining radiance against core::SkyRadiance
// WITHOUT re-implementing the Lazarov fit.
//
// Catches, MEASURED: mip 0 not being an exact evaluation of the sky, and a
// reflect() that produces the wrong direction (reflect(V, N) for reflect(-V, N)
// gives a relative error of 1.03).
//
// Does NOT catch the sampler address mode, though IBL_DESIGN.md says it should.
// Switching the sampler to WRAP was watched leaving this green: cube sampling
// resolves a direction to a face in hardware and never consults the 2-D address
// modes. See kEnvMirrorTolerance in render/environment_ibl.h.
float4 MirrorSpecularProbePS(VSOutput input) : SV_TARGET
{
    uint slot = (uint)input.positionCS.x;
    float3 d = normalize(g_probeDirections[slot].xyz);

    // N = V means reflect(-V, N) = N = d, so this fetches the cube along d.
    float3 spec = DawningSpecularIBL(g_envCube, g_linearClamp, d, d,
                                     0.0f, (float3)1.0f, g_iblParams.x);
    float2 ab = DawningEnvBRDFApprox(1.0f, 0.0f);
    return float4(spec, ab.x + ab.y);
}

// =============================================================================
// Pass 4 - the environment BRDF fit itself.
// =============================================================================
// Writes (A, B) over an 8x8 grid of (NdotV, roughness). The CPU asserts PHYSICAL
// properties of the fit rather than golden numbers: normal-incidence reflectance
// equals F0, grazing reflectance approaches unity, and F0 = 1 never exceeds
// unity (no energy creation).
//
// Those bounds are what give the A/B swap teeth. IBL_DESIGN.md's negative test
// for this is "swap A and B, fails at grazing angles" - which is true ONLY for a
// dielectric: at F0 = 1 the expression F0*A + B is symmetric in A and B and the
// swap is completely invisible. That is why the CPU side evaluates the bounds at
// F0 = 0.04 as well as at F0 = 1.
float4 EnvBRDFProbePS(VSOutput input) : SV_TARGET
{
    uint slot = (uint)input.positionCS.x;
    float NdotV, roughness;
    DawningProbeGrid(slot, NdotV, roughness);
    float2 ab = DawningEnvBRDFApprox(NdotV, roughness);
    return float4(ab.x, ab.y, 0.0f, 1.0f);
}

// =============================================================================
// Pass 5 - roughness -> mip monotonicity.
// =============================================================================
// A FIXED direction with roughness swept 0..1 across the 64 slots. Writes the
// PREFILTERED RADIANCE ALONE, deliberately not multiplied by the env-BRDF: the
// fit has its own strong roughness dependence and folding it in would confound
// the claim being made, which is about the MIP the fetch lands on.
//
// The direction is +Y, chosen because it is where this sky has the largest
// spread between the mirror value and the fully-rough value: the zenith is the
// dimmest part of the gradient, so widening the lobe can only brighten the
// result, monotonically. A horizontal direction would have been nearly vacuous -
// for a sky linear in y, the cosine-weighted average about a horizontal axis
// equals the value AT that axis, so mip 0 and mip 7 would agree and a reversed
// mapping would produce an almost identical sequence.
//
// Catches roughness -> mip inverted or off by one, which is otherwise invisible:
// it reads as "reflections are slightly too blurry".
float4 MipSweepProbePS(VSOutput input) : SV_TARGET
{
    uint slot = (uint)input.positionCS.x;
    float roughness = (float)slot / 63.0f;
    float3 up = float3(0.0f, 1.0f, 0.0f);
    float3 radiance = DawningPrefilteredRadiance(g_envCube, g_linearClamp, up,
                                                 roughness, g_iblParams.x);
    return float4(radiance, 1.0f);
}
