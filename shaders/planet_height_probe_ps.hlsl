// =============================================================================
// planet_height_probe_ps.hlsl — GPU value-agreement probe for the terrain height
// field
// =============================================================================
// The near-field chunked-LOD terrain mesh is displaced on the CPU by
// core::PlanetHeight (src/core/planet_height.cpp), and the far-field shaded sphere
// tints itself by the GPU PlanetHeight in shaders/planet_noise.hlsli. They MUST
// evaluate the same [0,1] elevation for the same surface direction, or the near
// terrain pops/shears against the far sphere as it fades in. Those two functions
// are twins written twice, in two languages — exactly the drift hazard the sky/SH
// agreement probes already close for their own twins.
//
// This is that probe for the height twin. It evaluates the SHIPPED GPU PlanetHeight
// for a set of (direction, body-type) queries uploaded by C++, and the CPU reads
// the results back and compares them against core::PlanetHeight fed the IDENTICAL
// inputs. It is NOT a hash tripwire: a re-pinned hash pins agreement in TIME, not
// VALUE (see src/core/sky_radiance.h's header for the trap). It witnesses the value
// the shipped code computes, not a re-implementation of it.
//
// Indexed by SV_POSITION.x, so probe slot i is pixel i — no separate index
// plumbing to get wrong. Output.w is the "I ran here" witness (+1) against the
// target's -1 poison clear, so the CPU can count how many slots actually executed.
// =============================================================================

#include "planet_noise.hlsli"

// The query set. Uploaded by C++, which then compares core::PlanetHeight against
// the SAME arrays — so there is no second generator of directions or params to
// drift. Slot i: evaluate PlanetHeight(g_dir[i].xyz, g_param[i]).
cbuffer TerrainHeightProbeConstants : register(b0)
{
    float4 g_dir[64];    // xyz = unit surface direction (planet-fixed), w unused
    float4 g_param[64];  // x = body type, y = seed, z = seaLevel, w = coastWidth
};

struct VSOutput
{
    float4 positionCS : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float4 main(VSOutput input) : SV_TARGET
{
    uint  slot       = (uint)input.positionCS.x;
    float3 N         = g_dir[slot].xyz;
    // Match planet_ps.hlsl's unpack: type is carried as a float and rounded.
    int   type       = (int)(g_param[slot].x + 0.5);
    float seed       = g_param[slot].y;
    float seaLevel   = g_param[slot].z;
    float coastWidth = g_param[slot].w;

    // Two channels, because the final elevation is hypersensitive at coastlines
    // (the type-0 coast smoothstep amplifies raw-field noise ~1/coastWidth), while
    // the RAW continent field carries the whole enumerated drift surface — hash
    // constants, octave counts, kNoiseRot, lacunarity, gain, seed derivation. The
    // CPU compares the raw field tightly and the final elevation loosely.
    //   r = PlanetHeight   (final [0,1] elevation the mesh displaces by)
    //   g = PlanetHeightRaw(the shared domain-warped fBm continent field)
    //   a = ran-witness (+1) against the target's -1 poison clear.
    float3 seedO = PlanetSeedOffset(seed);
    float  raw   = PlanetHeightRaw(N, seedO);
    float  h     = PlanetHeight(N, type, seed, seaLevel, coastWidth);
    return float4(h, raw, 0.0, 1.0);
}
