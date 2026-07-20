#ifndef THE_DAWNING_IBL_ENVIRONMENT_HLSLI
#define THE_DAWNING_IBL_ENVIRONMENT_HLSLI

#include "sky_common.hlsli"

// =============================================================================
// THE SEAM.
// =============================================================================
// DawningEnvironmentRadiance is the ONLY place in the image-based-lighting code
// where the environment is evaluated. docs/research/IBL_DESIGN.md section 2
// requires this:
//
//     "Design the prefilter so DawningSkyRadiance(dir) appears in exactly one
//      line of one shader."
//
// Everything downstream - the prefilter integral, the mirror mip, and the
// sky-agreement probe - goes through this function. Swapping the procedural sky
// for a loaded HDR later is a change to the ONE line below and to nothing else:
// no root signature change, no cbuffer change, no change to any consumer.
//
// It is a separate header rather than a function inside ibl_prefilter_ps.hlsl
// because the sky-agreement probe (IBL_DESIGN.md section 11, assertion 1.3) must
// witness the value the PREFILTER integrates, not a second evaluation that could
// drift from it. Both shaders include this file; grep DawningSkyRadiance across
// shaders/ and the only IBL hit is the line below.
//
// The prefilter integral takes NO SRV input. That is a consequence of the source
// being closed-form: every mip is an independent integral of ground truth rather
// than of a downsampled copy of the mip above it (IBL_DESIGN.md section 2). Do
// not "optimise" this into a ping-pong over the cube's own mips - that would
// reintroduce the entire class of error this shape was chosen to avoid.
// =============================================================================
float3 DawningEnvironmentRadiance(float3 direction)
{
    return DawningSkyRadiance(direction);
}

// =============================================================================
// Cube face basis - direction for a texel, in the engine's world frame.
// =============================================================================
// RULE 7 (left-handed, +Z forward) DOES NOT mean "flip Z here for handedness".
// D3D12's cube face ordering and its direction -> (face, u, v) mapping are fixed
// by the API and are identical whatever handedness the engine's world uses. The
// direction fed to SampleLevel comes from the engine's LH +Z-forward basis, so
// the round trip is consistent precisely as long as this table generates each
// texel's direction in that same basis. A "handedness fix" applied here is the
// most likely way to break it, and IBL_DESIGN.md section 6.5 says so in as many
// words.
//
// THIS TABLE IS NOT TRUSTED. IBL_DESIGN.md section 6.5 records that it was
// written from memory of the D3D spec and never verified against a run, so the
// verification is a ROUND TRIP rather than a comparison against the table:
// EnvironmentIBL renders a direction cubemap through THIS function, samples it
// along 64 known query directions, and requires the sampled direction to agree
// with the query. That is correct even if this table is a permutation of the
// truth, and it fails if the implementation is internally inconsistent - which
// is the only property the renderer actually needs.
//
// `uv` is the fullscreen-triangle UV from bloom_vs.hlsl: origin top-left, v
// increasing DOWNWARD. `v` below is flipped so it points up in the face image.
//
// The returned direction is deliberately NOT normalised. On a cube face the
// direction is an affine function of (u, v), so the direction cubemap's bilinear
// filtering is EXACT on the unnormalised value and only approximate on the
// normalised one. The round-trip probe depends on that; normalise at the point
// of use instead.
// =============================================================================
float3 DawningCubeFaceDirection(uint faceIndex, float2 uv)
{
    float u = 2.0f * uv.x - 1.0f;
    float v = 1.0f - 2.0f * uv.y;

    // Single assignment, single exit. FXC's X4000 "potentially uninitialized"
    // analysis is satisfied by the exhaustive trailing else, and basic_ps.hlsl's
    // ComputeShadow keeps the same shape for the same reason.
    float3 direction;
    if (faceIndex == 0)      direction = float3( 1.0f,  v,    -u);    // +X
    else if (faceIndex == 1) direction = float3(-1.0f,  v,     u);    // -X
    else if (faceIndex == 2) direction = float3( u,     1.0f, -v);    // +Y
    else if (faceIndex == 3) direction = float3( u,    -1.0f,  v);    // -Y
    else if (faceIndex == 4) direction = float3( u,     v,     1.0f); // +Z
    else                     direction = float3(-u,     v,    -1.0f); // -Z
    return direction;
}

#endif
