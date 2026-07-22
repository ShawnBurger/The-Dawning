// =============================================================================
// core/planet_height.cpp — CPU twin of shaders/planet_noise.hlsli
// =============================================================================
// Port discipline: every operation mirrors the HLSL in the SAME order, in float32,
// so the GPU-vs-CPU value-agreement probe (render::TerrainHeightProbe) agrees to
// its measured ~0.03 reproducibility floor rather than diverging outright. Straight
// IEEE +-*/ are deterministic across CPU and GPU; the residual floor is the GPU dp3
// reduction and frac() rounding differently, which a bit-exact integer hash would
// remove (a terrain follow-on that also matters for P5 landing collision). The
// HLSL intrinsics map as: frac(x)=x-floor(x); lerp(a,b,t)=a+(b-a)*t;
// saturate=clamp[0,1]; step(e,x)=x>=e?1:0; smoothstep as below; mul(M,v) with the
// HLSL float3x3(row0,row1,row2) constructor = (dot(row0,v),dot(row1,v),dot(row2,v)).
// =============================================================================

#include "planet_height.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace core
{
namespace
{

// --- HLSL-equivalent scalar/vector helpers -----------------------------------
inline float StepF(float edge, float x) { return x >= edge ? 1.0f : 0.0f; }

inline float SmoothStepF(float e0, float e1, float x)
{
    float t = Saturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

inline Vec3f AddS(const Vec3f& v, float s) { return { v.x + s, v.y + s, v.z + s }; }

// --- pcg3d integer hash — the CPU twin of planet_noise.hlsli's Pcg3d ----------
// BIT-IDENTICAL to the HLSL by construction: uint32 * + ^ >> are 32-bit modular
// operations both languages define the same way, in the same statement order.
struct U3 { uint32_t x, y, z; };
inline U3 Pcg3d(U3 v)
{
    v.x = v.x * 1664525u + 1013904223u;
    v.y = v.y * 1664525u + 1013904223u;
    v.z = v.z * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v.x ^= v.x >> 16; v.y ^= v.y >> 16; v.z ^= v.z >> 16;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

// uint -> [0,1) via the exact mantissa bit-trick (HLSL asfloat, C++ memcpy).
inline float UintToUnit(uint32_t h)
{
    const uint32_t bits = 0x3f800000u | (h >> 9);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f - 1.0f;
}

// Hash an integer lattice cell; `salt` (XORed into z) decorrelates independent uses.
inline float HashCell1(int cx, int cy, int cz, uint32_t salt)
{
    const U3 h = Pcg3d({ static_cast<uint32_t>(cx),
                         static_cast<uint32_t>(cy),
                         static_cast<uint32_t>(cz) ^ salt });
    return UintToUnit(h.x);
}
inline Vec3f HashCell3(int cx, int cy, int cz, uint32_t salt)
{
    const U3 h = Pcg3d({ static_cast<uint32_t>(cx),
                         static_cast<uint32_t>(cy),
                         static_cast<uint32_t>(cz) ^ salt });
    return { UintToUnit(h.x), UintToUnit(h.y), UintToUnit(h.z) };
}

// kNoiseRot rows, exactly as the HLSL float3x3 constructor lays them out.
inline Vec3f MulNoiseRot(const Vec3f& p)
{
    const Vec3f r0{ 0.00f,  0.80f,  0.60f };
    const Vec3f r1{ -0.80f, 0.36f, -0.48f };
    const Vec3f r2{ -0.60f, -0.48f, 0.64f };
    return { r0.Dot(p), r1.Dot(p), r2.Dot(p) };
}

// --- Value noise (mirror of planet_noise.hlsli) ------------------------------
float ValueNoise(const Vec3f& x)
{
    const float flx = std::floor(x.x), fly = std::floor(x.y), flz = std::floor(x.z);
    const int   ix = static_cast<int>(flx), iy = static_cast<int>(fly), iz = static_cast<int>(flz);
    const float fx = x.x - flx, fy = x.y - fly, fz = x.z - flz;
    // Quintic (C2) interpolant — same expression as the HLSL, component by component.
    const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    const float uz = fz * fz * fz * (fz * (fz * 6.0f - 15.0f) + 10.0f);

    const float n000 = HashCell1(ix,     iy,     iz,     0u);
    const float n100 = HashCell1(ix + 1, iy,     iz,     0u);
    const float n010 = HashCell1(ix,     iy + 1, iz,     0u);
    const float n110 = HashCell1(ix + 1, iy + 1, iz,     0u);
    const float n001 = HashCell1(ix,     iy,     iz + 1, 0u);
    const float n101 = HashCell1(ix + 1, iy,     iz + 1, 0u);
    const float n011 = HashCell1(ix,     iy + 1, iz + 1, 0u);
    const float n111 = HashCell1(ix + 1, iy + 1, iz + 1, 0u);

    const float nx00 = Lerp(n000, n100, ux);
    const float nx10 = Lerp(n010, n110, ux);
    const float nx01 = Lerp(n001, n101, ux);
    const float nx11 = Lerp(n011, n111, ux);
    const float nxy0 = Lerp(nx00, nx10, uy);
    const float nxy1 = Lerp(nx01, nx11, uy);
    return Lerp(nxy0, nxy1, uz);
}

float Fbm5(Vec3f p)
{
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < 5; ++i)
    {
        sum += amp * (ValueNoise(p) * 2.0f - 1.0f);
        p = MulNoiseRot(p) * 2.02f;
        amp *= 0.5f;
    }
    return sum;
}

float Fbm3(Vec3f p)
{
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < 3; ++i)
    {
        sum += amp * (ValueNoise(p) * 2.0f - 1.0f);
        p = MulNoiseRot(p) * 2.02f;
        amp *= 0.5f;
    }
    return sum;
}

float Ridged3(Vec3f p)
{
    float sum = 0.0f, amp = 0.5f, prev = 1.0f;
    for (int i = 0; i < 3; ++i)
    {
        float n = 1.0f - std::fabs(ValueNoise(p) * 2.0f - 1.0f);
        n = n * n;
        sum += n * amp * prev;
        prev = n;
        p = MulNoiseRot(p) * 2.02f;
        amp *= 0.5f;
    }
    return sum;
}

float CraterField(const Vec3f& p, float freq, float density)
{
    const Vec3f pp = p * freq;
    const float flx = std::floor(pp.x), fly = std::floor(pp.y), flz = std::floor(pp.z);
    const int   idx = static_cast<int>(flx), idy = static_cast<int>(fly), idz = static_cast<int>(flz);
    const Vec3f f{ (pp.x - flx) - 0.5f, (pp.y - fly) - 0.5f, (pp.z - flz) - 0.5f };

    const float present = StepF(1.0f - density, HashCell1(idx, idy, idz, 0x9E3779B9u));
    const Vec3f h3 = HashCell3(idx, idy, idz, 0x85EBCA6Bu);
    const Vec3f c{ (h3.x - 0.5f) * 0.5f, (h3.y - 0.5f) * 0.5f, (h3.z - 0.5f) * 0.5f };
    const float d   = (f - c).Length();
    const float rad = 0.16f + 0.16f * HashCell1(idx, idy, idz, 0xC2B2AE35u);

    const float bowl = -SmoothStepF(rad, rad * 0.2f, d) * 0.7f;
    const float rim  = std::exp(-std::pow(Saturate((d - rad) / (rad * 0.5f)), 2.0f) * 4.0f) * 0.5f;
    return present * (bowl + rim);
}

} // namespace

// --- Public entry points (mirror the hlsli entry points) ---------------------
Vec3f PlanetSeedOffset(float seed) { return { seed, seed * 1.7f, seed * 0.3f }; }

float PlanetHeightRaw(const Vec3f& n, const Vec3f& seedOffset)
{
    Vec3f wp   = n * 2.1f + seedOffset;
    Vec3f warp{ Fbm3(AddS(wp, 5.2f)), Fbm3(AddS(wp, 9.1f)), Fbm3(AddS(wp, 1.7f)) };
    return Fbm5(wp + warp * 0.6f) * 0.5f + 0.5f;
}

float PlanetLandMask(int type, float h, float seaLevel, float coastWidth)
{
    return (type == 0) ? SmoothStepF(seaLevel - coastWidth, seaLevel + coastWidth, h)
                       : 1.0f;
}

float PlanetElevation(const Vec3f& n, int type, const Vec3f& seedOffset,
                      float h, float landMask, float seaLevel)
{
    float landSpan = (std::max)(1.0f - seaLevel, 0.05f);
    float elev;
    if (type == 0)
    {
        elev = Saturate((h - seaLevel) / landSpan);
        elev = Saturate(elev + Ridged3(n * 5.7f + seedOffset * 1.3f) * 0.35f * landMask);
    }
    else
    {
        elev = Saturate(h);
        if (type == 1 || type == 2)
        {
            float cd = (type == 2) ? 0.55f : 0.30f;
            float cr = CraterField(n, 11.0f, cd) + 0.6f * CraterField(n, 25.0f, cd * 0.8f);
            elev = Saturate(elev + cr * 0.6f);
        }
    }
    return elev;
}

float PlanetHeight(const Vec3f& n, int type, float seed, float seaLevel, float coastWidth)
{
    Vec3f seedO = PlanetSeedOffset(seed);
    float h     = PlanetHeightRaw(n, seedO);
    float lm    = PlanetLandMask(type, h, seaLevel, coastWidth);
    return PlanetElevation(n, type, seedO, h, lm, seaLevel);
}

} // namespace core
