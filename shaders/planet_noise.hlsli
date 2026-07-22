// =============================================================================
// planet_noise.hlsli — shared procedural planet noise + elevation field
// =============================================================================
// The SINGLE definition of a planet's surface height, so the far-field shaded
// sphere (planet_ps.hlsl) and the near-field displaced terrain mesh (chunked-LOD,
// via the C++ twin core::PlanetHeight) evaluate the EXACT same function. If these
// diverged by a single hash constant the near terrain would visibly pop/shear as
// it faded in over the smooth sphere. A value-agreement probe (GPU vs the C++
// twin) — NOT a hash tripwire (a re-pinned hash pins agreement in TIME, not VALUE;
// see sky_radiance.h's own header for the trap) — guards this numerically at
// startup in every mode: render::TerrainHeightProbe (src/render/terrain_height_probe.
// {h,cpp}), asserted by the [SMOKE] terrain_height_agreement marker. It is a
// gross-drift guard (this fBm's CPU/GPU floor is ~0.03), so a one-sided edit here
// that forgets core/planet_height.{h,cpp} — or vice versa — trips it.
//
// Keep this byte-for-byte in step with core/planet_height.{h,cpp}: same hash
// constants, octave counts, kNoiseRot, lacunarity 2.02, gain 0.5, seed derivation.
// =============================================================================

#ifndef DAWNING_PLANET_NOISE_HLSLI
#define DAWNING_PLANET_NOISE_HLSLI

// -----------------------------------------------------------------------------
// Integer-hash value noise. The lattice corners are hashed with pcg3d (Jarzynski &
// Olano, "Hash Functions for GPU Rendering", JCGT 2020) — pure uint32 arithmetic,
// so the corner values are BIT-IDENTICAL on the CPU (core::planet_height, the
// terrain collision/mesh field) and the GPU (this shader, the shaded sphere) BY
// CONSTRUCTION: uint * + ^ >> are 32-bit modular ops both languages define
// identically, and FXC cannot reassociate or FMA-fuse integer math. This is what
// makes the near mesh and far sphere agree. It replaces the sine-free FLOAT hash
// (Hoskins), whose final frac() of a ~4400-magnitude product rounded differently
// on the GPU's dp3 dot hardware, giving a ~3% CPU/GPU divergence (measured).
// The residual is now only the float interpolation weights (~1e-7).
// -----------------------------------------------------------------------------
uint3 Pcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

// uint -> [0,1) via the exact mantissa bit-trick: build a float in [1,2) and
// subtract 1. No uint->float conversion rounding to disagree on.
float UintToUnit(uint h) { return asfloat(0x3f800000u | (h >> 9)) - 1.0; }

// Hash an integer lattice cell to [0,1) / [0,1)^3. `salt` (XORed into a component)
// decorrelates independent uses at the same cell (value noise vs crater fields).
float HashCell1(int3 c, uint salt)
{
    uint3 h = Pcg3d(uint3(asuint(c.x), asuint(c.y), asuint(c.z) ^ salt));
    return UintToUnit(h.x);
}
float3 HashCell3(int3 c, uint salt)
{
    uint3 h = Pcg3d(uint3(asuint(c.x), asuint(c.y), asuint(c.z) ^ salt));
    return float3(UintToUnit(h.x), UintToUnit(h.y), UintToUnit(h.z));
}

float ValueNoise(float3 x)
{
    float3 fl = floor(x);
    int3   i  = int3(fl);       // floor THEN cast: exact integer cell, negatives ok
    float3 f  = x - fl;         // == frac(x), reusing the floor
    // Quintic (C2) interpolant: field, slope AND curvature are continuous across a
    // cell boundary, so if the CPU and GPU ever pick adjacent cells (a coordinate
    // within a ULP of an integer) the disagreement is bounded to the local slope
    // times that ULP, not a hash jump.
    float3 u  = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float n000 = HashCell1(i + int3(0, 0, 0), 0u);
    float n100 = HashCell1(i + int3(1, 0, 0), 0u);
    float n010 = HashCell1(i + int3(0, 1, 0), 0u);
    float n110 = HashCell1(i + int3(1, 1, 0), 0u);
    float n001 = HashCell1(i + int3(0, 0, 1), 0u);
    float n101 = HashCell1(i + int3(1, 0, 1), 0u);
    float n011 = HashCell1(i + int3(0, 1, 1), 0u);
    float n111 = HashCell1(i + int3(1, 1, 1), 0u);

    float nx00 = lerp(n000, n100, u.x);
    float nx10 = lerp(n010, n110, u.x);
    float nx01 = lerp(n001, n101, u.x);
    float nx11 = lerp(n011, n111, u.x);
    float nxy0 = lerp(nx00, nx10, u.y);
    float nxy1 = lerp(nx01, nx11, u.y);
    return lerp(nxy0, nxy1, u.z); // [0,1]
}

// Per-octave rotation (IQ) so the axis-aligned grid of the value noise does not
// print through the fBm as a lattice.
static const float3x3 kNoiseRot = float3x3( 0.00,  0.80,  0.60,
                                           -0.80,  0.36, -0.48,
                                           -0.60, -0.48,  0.64);

// fBm in [-1,1]-ish. Fixed octave counts (unrolled) keep FXC /WX happy.
float Fbm5(float3 p)
{
    float sum = 0.0, amp = 0.5;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        sum += amp * (ValueNoise(p) * 2.0 - 1.0);
        p = mul(kNoiseRot, p) * 2.02;
        amp *= 0.5;
    }
    return sum;
}

float Fbm3(float3 p)
{
    float sum = 0.0, amp = 0.5;
    [unroll] for (int i = 0; i < 3; ++i)
    {
        sum += amp * (ValueNoise(p) * 2.0 - 1.0);
        p = mul(kNoiseRot, p) * 2.02;
        amp *= 0.5;
    }
    return sum;
}

// Ridged multifractal — sharp crests for mountain ranges.
float Ridged3(float3 p)
{
    float sum = 0.0, amp = 0.5, prev = 1.0;
    [unroll] for (int i = 0; i < 3; ++i)
    {
        float n = 1.0 - abs(ValueNoise(p) * 2.0 - 1.0);
        n = n * n;
        sum += n * amp * prev;
        prev = n;
        p = mul(kNoiseRot, p) * 2.02;
        amp *= 0.5;
    }
    return sum;
}

// Sparse impact craters (Mars/Moon). One jittered crater per grid cell, its centre
// kept away from the cell edge (like the starfield) so a single-cell lookup does not
// slice it. Returns a signed elevation delta: a depressed bowl plus a raised rim.
// The three per-cell hashes use distinct salts of the SAME integer cell id, so they
// are bit-exact CPU/GPU (the exp/pow in the rim are transcendentals and add ~1e-6,
// which rides the elevation channel only, never the raw field).
float CraterField(float3 p, float freq, float density)
{
    float3 pp = p * freq;
    float3 fl = floor(pp);
    int3   id = int3(fl);
    float3 f  = (pp - fl) - 0.5;

    float  present = step(1.0 - density, HashCell1(id, 0x9E3779B9u));
    float3 c   = (HashCell3(id, 0x85EBCA6Bu) - 0.5) * 0.5;   // centred jitter (edge-safe)
    float  d   = length(f - c);
    float  rad = 0.16 + 0.16 * HashCell1(id, 0xC2B2AE35u);

    float bowl = -smoothstep(rad, rad * 0.2, d) * 0.7;               // sunken floor
    float rim  = exp(-pow(saturate((d - rad) / (rad * 0.5)), 2.0) * 4.0) * 0.5; // bright rim
    return present * (bowl + rim);
}

// -----------------------------------------------------------------------------
// Elevation field — the SHARED definition displaced by the terrain mesh and
// tinted by planet_ps. Split into the three steps planet_ps needs (raw height,
// land mask, elevation) so the shader can reuse the intermediates for ocean/biome,
// plus a one-call PlanetHeight entry the terrain generator and the C++ twin use.
// N is the planet-fixed unit surface direction; seedO = float3(seed, seed*1.7,
// seed*0.3). type: 0 Earth, 1 Mars, 2 Moon, 3 generic.
// -----------------------------------------------------------------------------
float3 PlanetSeedOffset(float seed) { return float3(seed, seed * 1.7, seed * 0.3); }

float PlanetHeightRaw(float3 N, float3 seedO)
{
    float3 wp   = N * 2.1 + seedO;
    float3 warp = float3(Fbm3(wp + 5.2), Fbm3(wp + 9.1), Fbm3(wp + 1.7));
    return Fbm5(wp + 0.6 * warp) * 0.5 + 0.5;   // ~[0,1]
}

float PlanetLandMask(int type, float h, float seaLevel, float coastWidth)
{
    return (type == 0) ? smoothstep(seaLevel - coastWidth, seaLevel + coastWidth, h)
                       : 1.0;
}

float PlanetElevation(float3 N, int type, float3 seedO, float h, float landMask, float seaLevel)
{
    float landSpan = max(1.0 - seaLevel, 0.05);
    float elev;
    if (type == 0)
    {
        elev = saturate((h - seaLevel) / landSpan);
        elev = saturate(elev + Ridged3(N * 5.7 + seedO * 1.3) * 0.35 * landMask);
    }
    else
    {
        elev = saturate(h);
        if (type == 1 || type == 2)
        {
            float cd = (type == 2) ? 0.55 : 0.30;   // Moon dense, Mars sparse
            float cr = CraterField(N, 11.0, cd) + 0.6 * CraterField(N, 25.0, cd * 0.8);
            elev = saturate(elev + cr * 0.6);
        }
    }

    // Near-surface multi-scale detail. The continental fBm is a red-spectrum
    // (gain 0.5) self-similar field, so up close it reads rounded/smooth. Two
    // higher-frequency RIDGED bands add crunchier, non-stationary relief, amplitude-
    // weighted by the elevation already computed (Musgrave multifractal — ridges and
    // peaks get rocky, basins/ocean stay smooth). It is in the SHARED field, so the
    // collision mesh gets it too (the ground you land on matches the ground you see),
    // and it is sub-pixel from orbit, so the far silhouette is unchanged. Earth's
    // ocean is gated out by landMask.
    float rough = 0.35 + 0.65 * elev;                       // rocky on highs, softer in basins
    float dHi   = Ridged3(N * 200.0 + seedO * 2.7) - 0.33;  // regional ridges/hills
    float dLo   = Ridged3(N * 800.0 + seedO * 4.1) - 0.33;  // local rock
    float detailGate = (type == 0) ? landMask : 1.0;        // Earth ocean stays flat
    elev = saturate(elev + (dHi * 0.12 + dLo * 0.07) * rough * detailGate);
    return elev;
}

// One-call elevation for the terrain mesh / C++ twin: the same [0,1] scalar the
// mesh displaces by (heightMeters = amplitude * PlanetHeight) and the shader tints.
float PlanetHeight(float3 N, int type, float seed, float seaLevel, float coastWidth)
{
    float3 seedO = PlanetSeedOffset(seed);
    float  h     = PlanetHeightRaw(N, seedO);
    float  lm    = PlanetLandMask(type, h, seaLevel, coastWidth);
    return PlanetElevation(N, type, seedO, h, lm, seaLevel);
}

#endif // DAWNING_PLANET_NOISE_HLSLI
