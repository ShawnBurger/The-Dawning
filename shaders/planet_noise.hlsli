// =============================================================================
// planet_noise.hlsli — shared procedural planet noise + elevation field
// =============================================================================
// The SINGLE definition of a planet's surface height, so the far-field shaded
// sphere (planet_ps.hlsl) and the near-field displaced terrain mesh (chunked-LOD,
// via the C++ twin core::PlanetHeight) evaluate the EXACT same function. If these
// diverged by a single hash constant the near terrain would visibly pop/shear as
// it faded in over the smooth sphere, so a value-agreement probe (GPU vs the C++
// twin) guards this at startup — NOT a hash tripwire (a re-pinned hash pins
// agreement in TIME, not VALUE; see sky_radiance.h's own header for the trap).
//
// Keep this byte-for-byte in step with core/planet_height.{h,cpp}: same hash
// constants, octave counts, kNoiseRot, lacunarity 2.02, gain 0.5, seed derivation.
// =============================================================================

#ifndef DAWNING_PLANET_NOISE_HLSLI
#define DAWNING_PLANET_NOISE_HLSLI

// -----------------------------------------------------------------------------
// Hash-based value noise (Dave Hoskins "hash without sine") — LUT-free, stable
// across GPUs, no frac(sin()) banding at multi-octave frequencies.
// -----------------------------------------------------------------------------
float Hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float ValueNoise(float3 x)
{
    float3 i = floor(x);
    float3 f = frac(x);
    float3 u = f * f * (3.0 - 2.0 * f); // smoothstep interpolant

    float n000 = Hash13(i + float3(0, 0, 0));
    float n100 = Hash13(i + float3(1, 0, 0));
    float n010 = Hash13(i + float3(0, 1, 0));
    float n110 = Hash13(i + float3(1, 1, 0));
    float n001 = Hash13(i + float3(0, 0, 1));
    float n101 = Hash13(i + float3(1, 0, 1));
    float n011 = Hash13(i + float3(0, 1, 1));
    float n111 = Hash13(i + float3(1, 1, 1));

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

float3 Hash33(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

// Sparse impact craters (Mars/Moon). One jittered crater per grid cell, its centre
// kept away from the cell edge (like the starfield) so a single-cell lookup does not
// slice it. Returns a signed elevation delta: a depressed bowl plus a raised rim.
float CraterField(float3 p, float freq, float density)
{
    float3 pp = p * freq;
    float3 id = floor(pp);
    float3 f  = frac(pp) - 0.5;

    float present = step(1.0 - density, Hash13(id * 1.7 + 4.4));
    float3 c   = (Hash33(id + 2.2) - 0.5) * 0.5;   // centred jitter (edge-safe)
    float  d   = length(f - c);
    float  rad = 0.16 + 0.16 * Hash13(id + 8.8);

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
