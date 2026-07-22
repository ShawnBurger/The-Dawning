// =============================================================================
// core/planet_height.h — CPU twin of shaders/planet_noise.hlsli
// =============================================================================
// The chunked-LOD terrain mesh is generated on the CPU and must displace by the
// EXACT same surface height the GPU planet_ps shader tints, or the near terrain
// visibly pops/shears against the far shaded sphere as it fades in. This is that
// CPU twin: a byte-for-byte port of the elevation field in planet_noise.hlsli
// (same hash constants, octave counts, kNoiseRot, lacunarity 2.02, gain 0.5, and
// seed derivation), evaluated in float32 to match the shader's precision.
//
// A numeric GPU-vs-CPU VALUE-agreement probe guards the match at startup, in every
// mode, gated on nothing: render::TerrainHeightProbe evaluates the SHIPPED GPU
// PlanetHeight (planet_noise.hlsli) for 64 (direction, body-type) queries and
// compares against this twin fed identical inputs, emitting the
// [SMOKE] terrain_height_agreement marker the smoke harness asserts. It is a
// GROSS-DRIFT guard, not a bit-exactness one: this fBm's CPU/GPU reproducibility
// floor is ~0.03 (the GPU dp3 reduction and frac() round differently from the CPU;
// see terrain_height_probe.cpp), so the probe catches a one-sided edit to any hash
// constant / octave / kNoiseRot / lacunarity / seed derivation (which move the field
// by O(0.1..0.5)) but not a sub-3% drift. Anything changed in planet_noise.hlsli
// MUST still be mirrored here (and vice versa) by hand. See [[verification-discipline]].
// =============================================================================

#ifndef DAWNING_CORE_PLANET_HEIGHT_H
#define DAWNING_CORE_PLANET_HEIGHT_H

#include "types.h"

namespace core
{

// Body type keys, matching PlanetConstants.params0.x / PlanetParamsFor:
//   0 Earth-like (ocean), 1 Mars-like, 2 Moon-like, 3 generic rock.

// Raw continent height field h in ~[0,1] (domain-warped fBm). `seedOffset` is
// PlanetSeedOffset(seed).
float PlanetHeightRaw(const Vec3f& n, const Vec3f& seedOffset);

// Soft coastline land mask — Earth (type 0) only; 1 elsewhere.
float PlanetLandMask(int type, float h, float seaLevel, float coastWidth);

// Surface elevation in [0,1]: coast->peak + ridged mountains (Earth), or broad
// terrain + impact craters (Mars/Moon). This is what the mesh displaces by and
// what planet_ps tints.
float PlanetElevation(const Vec3f& n, int type, const Vec3f& seedOffset,
                      float h, float landMask, float seaLevel);

// The seed offset used by both raw height and elevation.
Vec3f PlanetSeedOffset(float seed);

// One-call elevation for the terrain generator: the same [0,1] scalar the shader
// computes. `n` is the planet-fixed UNIT surface direction.
// heightMeters = amplitude * PlanetHeight(...).
float PlanetHeight(const Vec3f& n, int type, float seed, float seaLevel, float coastWidth);

} // namespace core

#endif // DAWNING_CORE_PLANET_HEIGHT_H
