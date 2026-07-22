// =============================================================================
// tests/test_planet_height.cpp — core::PlanetHeight (CPU twin of planet_noise.hlsli)
// =============================================================================
// CPU self-consistency for the terrain height twin: determinism, [0,1] range,
// finiteness (no NaN/Inf from the pow/exp/division in CraterField), and that the
// field actually varies with direction, type, and seed. The GPU-vs-CPU value
// agreement (that this twin matches the shader bit-for-close) is a separate
// startup probe that is PLANNED, not yet implemented — see the terrain plan. These
// tests do NOT check the twin against the shader; they check it against itself.
// =============================================================================

#include "test_framework.h"
#include "core/planet_height.h"

#include <cmath>
#include <vector>

namespace
{

// A Fibonacci sphere of unit directions — even coverage, no polar bunching.
std::vector<core::Vec3f> SphereDirs(int n)
{
    std::vector<core::Vec3f> dirs;
    dirs.reserve(n);
    const float golden = 2.399963229728653f; // pi*(3-sqrt(5))
    for (int i = 0; i < n; ++i)
    {
        float y   = 1.0f - (i + 0.5f) * 2.0f / n; // [-1,1]
        float r   = std::sqrt((std::max)(0.0f, 1.0f - y * y));
        float phi = i * golden;
        dirs.push_back(core::Vec3f{ std::cos(phi) * r, y, std::sin(phi) * r });
    }
    return dirs;
}

// The per-body params the twin is fed (mirrors PlanetParamsFor seed/seaLevel).
struct Body { int type; float seed; float seaLevel; float coastWidth; };
const Body kEarth{ 0, 11.0f, 0.52f, 0.02f };
const Body kMoon { 2, 33.0f, 1.00f, 0.00f };
const Body kMars { 1, 22.0f, 1.00f, 0.00f };

float Elev(const core::Vec3f& d, const Body& b)
{
    return core::PlanetHeight(d, b.type, b.seed, b.seaLevel, b.coastWidth);
}

} // namespace

TEST_CASE(PlanetHeight_IsDeterministic)
{
    // Same inputs → bit-identical output (a procedural field must be reproducible).
    for (const auto& d : SphereDirs(64))
    {
        float a = Elev(d, kEarth);
        float b = Elev(d, kEarth);
        CHECK_EQ(a, b);
    }
}

TEST_CASE(PlanetHeight_ElevationInUnitRangeAndFinite)
{
    // Elevation is saturated in the shader/twin, so it must stay in [0,1] and never
    // be NaN/Inf (the CraterField pow/exp/division is the hazard).
    const Body bodies[] = { kEarth, kMoon, kMars, { 3, 7.0f, 1.0f, 0.0f } };
    for (const auto& body : bodies)
        for (const auto& d : SphereDirs(400))
        {
            float e = Elev(d, body);
            CHECK(std::isfinite(e));
            CHECK(e >= 0.0f);
            CHECK(e <= 1.0f);
        }
}

TEST_CASE(PlanetHeight_RawHeightInUnitRangeAndFinite)
{
    // h = Fbm5*0.5+0.5; Fbm5 is bounded to +/-0.96875 (0.5+0.25+...+0.03125), so
    // h is strictly inside (0,1). This pins that the fBm amplitude series matches.
    const core::Vec3f seedO = core::PlanetSeedOffset(11.0f);
    for (const auto& d : SphereDirs(400))
    {
        float h = core::PlanetHeightRaw(d, seedO);
        CHECK(std::isfinite(h));
        CHECK(h >= 0.0f);
        CHECK(h <= 1.0f);
    }
}

TEST_CASE(PlanetHeight_TypesProduceDistinctFields)
{
    // Earth/Mars/Moon run different elevation logic (ocean+mountains vs craters),
    // so at the same directions their fields must differ on average.
    auto dirs = SphereDirs(300);
    float dEM = 0.0f, dEMo = 0.0f, dMMo = 0.0f;
    for (const auto& d : dirs)
    {
        float e = Elev(d, kEarth), m = Elev(d, kMars), mo = Elev(d, kMoon);
        dEM  += std::fabs(e - m);
        dEMo += std::fabs(e - mo);
        dMMo += std::fabs(m - mo);
    }
    float inv = 1.0f / dirs.size();
    CHECK(dEM  * inv > 0.02f);
    CHECK(dEMo * inv > 0.02f);
    CHECK(dMMo * inv > 0.02f);
}

TEST_CASE(PlanetHeight_FieldVariesAcrossSurface)
{
    // A constant field would render a featureless ball. Assert real spatial
    // variation (nonzero std) for each body type.
    const Body bodies[] = { kEarth, kMoon, kMars };
    for (const auto& body : bodies)
    {
        auto dirs = SphereDirs(500);
        double mean = 0.0;
        for (const auto& d : dirs) mean += Elev(d, body);
        mean /= dirs.size();
        double var = 0.0;
        for (const auto& d : dirs) { double e = Elev(d, body) - mean; var += e * e; }
        var /= dirs.size();
        CHECK(std::sqrt(var) > 0.01);
    }
}

TEST_CASE(PlanetHeight_SeedChangesTheField)
{
    // Different seeds must produce a different planet (the seed feeds the noise
    // domain offset), else every body would be identical.
    auto dirs = SphereDirs(300);
    float diff = 0.0f;
    for (const auto& d : dirs)
        diff += std::fabs(core::PlanetHeight(d, 0, 11.0f, 0.52f, 0.02f) -
                          core::PlanetHeight(d, 0, 99.0f, 0.52f, 0.02f));
    CHECK(diff / dirs.size() > 0.01f);
}

TEST_CASE(PlanetHeight_SeedOffsetDerivation)
{
    // The exact seed derivation the shader uses (seed, seed*1.7, seed*0.3) — the
    // twin and hlsli must agree on this or every downstream sample diverges.
    core::Vec3f s = core::PlanetSeedOffset(10.0f);
    CHECK_APPROX(s.x, 10.0f);
    CHECK_APPROX(s.y, 17.0f);
    CHECK_APPROX(s.z, 3.0f);
}
