// =============================================================================
// ibl_probe_ps.hlsl - GPU evidence for the environment cubemap
// =============================================================================
//
// Two 64x1 passes, both read back and asserted on the CPU. These are probes in
// the sense the draw-record probe already established in this tree: they witness
// the value the SHIPPED code computes, rather than re-implementing it and
// comparing two re-implementations.
//
// SkyAgreementPS   - assertion 1.3. Evaluates the environment through the SAME
//                    seam the prefilter integrates (DawningEnvironmentRadiance)
//                    for 64 directions. The CPU compares against
//                    core::SkyRadiance. src/core/sky_radiance.h states in its own
//                    header that its tripwire "pins agreement in TIME, not in
//                    VALUE" and names this probe as the thing that closes the
//                    gap. This file is that thing.
//
// DirectionRoundTripPS - assertion 1.2. Samples the direction cubemap along 64
//                    known query directions. The CPU requires the sampled
//                    direction to agree with the query, which fails on any
//                    permutation, sign flip or v-flip in the face table.
//
// Both entry points are indexed by SV_POSITION.x, so probe slot i is pixel i and
// there is no separate index plumbing to get wrong.
// =============================================================================

#include "ibl_environment.hlsli"

// The query/probe direction set. Uploaded by C++, which then compares against
// the SAME array - so there is no second generator of directions to drift.
cbuffer IBLProbeDirections : register(b1)
{
    float4 g_probeDirections[64];
};

TextureCube<float4> g_directionCube : register(t0);
SamplerState        g_linearClamp   : register(s0);

struct VSOutput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float4 SkyAgreementPS(VSOutput input) : SV_TARGET
{
    uint slot = (uint)input.positionCS.x;
    float3 direction = g_probeDirections[slot].xyz;
    return float4(DawningEnvironmentRadiance(direction), 1.0f);
}

float4 DirectionRoundTripPS(VSOutput input) : SV_TARGET
{
    uint slot = (uint)input.positionCS.x;
    float3 query = normalize(g_probeDirections[slot].xyz);

    // The cube stores UNNORMALISED face directions, which are affine in (u, v)
    // across a face, so this bilinear fetch reconstructs the generator's
    // direction exactly rather than approximately. The CPU normalises and
    // compares against the query.
    float3 sampled = g_directionCube.SampleLevel(g_linearClamp, query, 0.0f).xyz;
    return float4(sampled, 1.0f);
}
