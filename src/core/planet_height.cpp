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

namespace core
{
namespace
{

// --- HLSL-equivalent scalar/vector helpers -----------------------------------
inline float Frac(float x) { return x - std::floor(x); }
inline float StepF(float edge, float x) { return x >= edge ? 1.0f : 0.0f; }

inline float SmoothStepF(float e0, float e1, float x)
{
    float t = Saturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

inline Vec3f Floor3(const Vec3f& v) { return { std::floor(v.x), std::floor(v.y), std::floor(v.z) }; }
inline Vec3f Frac3(const Vec3f& v)  { return { Frac(v.x), Frac(v.y), Frac(v.z) }; }
inline Vec3f AddS(const Vec3f& v, float s) { return { v.x + s, v.y + s, v.z + s }; }
// Component-wise (Hadamard) product — HLSL float3 * float3.
inline Vec3f Had(const Vec3f& a, const Vec3f& b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }

// kNoiseRot rows, exactly as the HLSL float3x3 constructor lays them out.
inline Vec3f MulNoiseRot(const Vec3f& p)
{
    const Vec3f r0{ 0.00f,  0.80f,  0.60f };
    const Vec3f r1{ -0.80f, 0.36f, -0.48f };
    const Vec3f r2{ -0.60f, -0.48f, 0.64f };
    return { r0.Dot(p), r1.Dot(p), r2.Dot(p) };
}

// --- Hash / noise (mirror of planet_noise.hlsli) -----------------------------
float Hash13(Vec3f p)
{
    p = Frac3(p * 0.1031f);
    // p.zyx = (p.z, p.y, p.x)
    p = AddS(p, p.Dot(Vec3f{ p.z, p.y, p.x } + Vec3f{ 31.32f, 31.32f, 31.32f }));
    return Frac((p.x + p.y) * p.z);
}

float ValueNoise(const Vec3f& x)
{
    Vec3f i = Floor3(x);
    Vec3f f = Frac3(x);
    Vec3f u = Had(Had(f, f), Vec3f{ 3.0f - 2.0f * f.x, 3.0f - 2.0f * f.y, 3.0f - 2.0f * f.z });

    float n000 = Hash13(i + Vec3f{ 0, 0, 0 });
    float n100 = Hash13(i + Vec3f{ 1, 0, 0 });
    float n010 = Hash13(i + Vec3f{ 0, 1, 0 });
    float n110 = Hash13(i + Vec3f{ 1, 1, 0 });
    float n001 = Hash13(i + Vec3f{ 0, 0, 1 });
    float n101 = Hash13(i + Vec3f{ 1, 0, 1 });
    float n011 = Hash13(i + Vec3f{ 0, 1, 1 });
    float n111 = Hash13(i + Vec3f{ 1, 1, 1 });

    float nx00 = Lerp(n000, n100, u.x);
    float nx10 = Lerp(n010, n110, u.x);
    float nx01 = Lerp(n001, n101, u.x);
    float nx11 = Lerp(n011, n111, u.x);
    float nxy0 = Lerp(nx00, nx10, u.y);
    float nxy1 = Lerp(nx01, nx11, u.y);
    return Lerp(nxy0, nxy1, u.z);
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

Vec3f Hash33(Vec3f p)
{
    p = Frac3(Had(p, Vec3f{ 0.1031f, 0.1030f, 0.0973f }));
    // p.yxz = (p.y, p.x, p.z)
    p = AddS(p, p.Dot(Vec3f{ p.y, p.x, p.z } + Vec3f{ 33.33f, 33.33f, 33.33f }));
    // (p.xxy + p.yxx) * p.zyx
    Vec3f a{ p.x, p.x, p.y };
    Vec3f b{ p.y, p.x, p.x };
    Vec3f c{ p.z, p.y, p.x };
    return Frac3(Had(a + b, c));
}

float CraterField(const Vec3f& p, float freq, float density)
{
    Vec3f pp = p * freq;
    Vec3f id = Floor3(pp);
    Vec3f f  = AddS(Frac3(pp), -0.5f);

    float present = StepF(1.0f - density, Hash13(id * 1.7f + Vec3f{ 4.4f, 4.4f, 4.4f }));
    Vec3f c   = (Hash33(id + Vec3f{ 2.2f, 2.2f, 2.2f }) + Vec3f{ -0.5f, -0.5f, -0.5f }) * 0.5f;
    float d   = (f - c).Length();
    float rad = 0.16f + 0.16f * Hash13(id + Vec3f{ 8.8f, 8.8f, 8.8f });

    float bowl = -SmoothStepF(rad, rad * 0.2f, d) * 0.7f;
    float rim  = std::exp(-std::pow(Saturate((d - rad) / (rad * 0.5f)), 2.0f) * 4.0f) * 0.5f;
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
