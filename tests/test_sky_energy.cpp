// =============================================================================
// tests/test_sky_energy.cpp - the sky carries no sun, and that is asserted
// =============================================================================
//
// WHAT THIS FILE IS FOR
//
// docs/research/IBL_DESIGN.md section 12 opens with the biggest risk it could
// identify in the image-based-lighting design, and states plainly that it had no
// good answer:
//
//     "The entire energy argument in section 9 rests on DawningSkyRadiance
//      containing no sun. It does not today. The moment someone adds one - and
//      'add a sun to the sky' is an obviously desirable, obviously small-looking
//      change - the directional light and the environment become two copies of
//      the brightest energy in the scene [...] It will not crash. It will not
//      fail any assertion in section 11 [...] It will look like 'the tone
//      mapping needs tuning'.
//      Mitigation: a comment [...] A comment is a weak mitigation. I do not have
//      a better one - an assertion would have to know what 'a sun' is. If
//      someone can think of a test with teeth here, it is worth more than
//      anything else in section 11."
//
// This file is that test. The defining property of the defect is that it FAILS
// NOTHING: every verification in that design compares the renderer against the
// sky function, so both sides move together and stay in agreement while the
// image is wrong. The only thing that can catch it is an assertion about the
// SHAPE OF THE SKY ITSELF, which is what is below.
//
// -----------------------------------------------------------------------------
// AN ASSERTION DOES NOT HAVE TO KNOW WHAT "A SUN" IS
// -----------------------------------------------------------------------------
// That was the stated obstacle, and it dissolves once the question is turned
// around. Do not ask "is there a sun?". Assert the two properties the current
// sky has that NO sun disc can have, and one energy bound that no sun disc can
// satisfy:
//
//   1. AZIMUTHAL SYMMETRY. The sky is a function of direction.y alone. A sun
//      disc anywhere except exactly at a pole makes radiance depend on azimuth.
//      Exact to 1e-6 - it catches a sun of ANY brightness, however dim.
//
//   2. AFFINE IN ELEVATION. The sky is a straight line in direction.y. A sun
//      disc AT a pole - the one case symmetry cannot see - puts a step in that
//      line. Also exact to 1e-6.
//
//   3. NO LOCALISED ENERGY. Peak luminance over the sphere stays within a small
//      factor of mean luminance. This is the energy claim itself, stated
//      directly, and it is the one that survives a legitimate redesign of the
//      sky: 1 and 2 pin the sky's exact current shape, while 3 permits any
//      smooth environment and forbids only a concentrated source.
//
// Together: 1 catches every off-pole sun, 2 catches every polar sun, 3 catches
// any localised source regardless of geometry and keeps guarding after someone
// legitimately replaces the gradient with something better.
//
// -----------------------------------------------------------------------------
// THIS FILE CONTAINS A C++ TWIN OF HLSL. SAID EXPLICITLY, AS REQUIRED.
// -----------------------------------------------------------------------------
// The tests evaluate core::SkyRadiance (src/core/sky_radiance.cpp), which is a
// hand-written mirror of DawningSkyRadiance in shaders/sky_common.hlsli. That is
// the same class of hazard src/render/rt_texture_lod.h:9-23 records: the suite
// stays green while the shader diverges.
//
// It was unavoidable. Deciding "no localised lobe" requires evaluating the sky
// densely over the sphere, and this binary is CPU-only by construction - no
// D3D12, no device (see the TheDawningTests notes in CMakeLists.txt). The HLSL
// cannot be run here.
//
// What is done about it: Sky_TwinIsInStepWithShaderSource below hashes the
// normalised text of shaders/sky_common.hlsli and fails if it changed. Editing
// the sky in HLSL without acknowledging the twin is therefore a loud failure
// rather than a silent pass - which is precisely the drift rt_texture_lod.h
// documents and does not catch.
//
// WHAT THAT STILL DOES NOT BUY, stated so nobody over-reads it: the tripwire
// pins agreement in TIME, not in VALUE. It proves the HLSL has not changed since
// a human last checked the two against each other. It does NOT prove they
// compute the same numbers. A twin that was wrong the day it was written stays
// wrong and stays green.
//
// TO CLOSE IT COMPLETELY: evaluate DawningSkyRadiance on the GPU for a fixed
// direction set, read it back, and compare against core::SkyRadiance. That is
// assertion 1.3 of IBL_DESIGN.md section 11 Stage 1, it belongs in
// tools/smoke_test.ps1 rather than in a CPU-only unit test, and IT IS NOT BUILT.
// Until it is, the mirror is watched but not verified.
// =============================================================================

#include "test_framework.h"

#include "core/sky_radiance.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using core::Vec3f;

namespace
{

// -----------------------------------------------------------------------------
// Sampling the sphere
// -----------------------------------------------------------------------------
// A Fibonacci lattice, because its worst-case gap between neighbouring samples
// is near-optimal for a given count - which is the property the whole file rests
// on. A sun disc is small, and a direction set that can MISS one turns every
// assertion below into a test that passes vacuously. That is not assumed here:
// Sky_DirectionSetCannotMissASunDisc measures the worst-case gap directly.
//
// kSphereSamples is sized so the gap comes out under kSunAngularRadiusDeg. The
// real Sun subtends 0.266 degrees of angular radius from Earth, and a sun disc
// drawn in a game sky is normally made LARGER than that so it covers more than a
// couple of pixels, so guaranteeing 0.25 degrees covers the realistic range with
// margin.
//
// The count is MEASURED, not estimated from a covering-radius formula. The first
// attempt used 600000 and Sky_DirectionSetCannotMissASunDisc reported a 0.2894
// degree gap - WIDER than a sun radius, so a disc could have slipped between
// samples and every assertion here would have passed vacuously. 1200000 measures
// 0.1702 degrees. If you reduce this number, that test is what tells you that
// you broke the guarantee.
constexpr int   kSphereSamples       = 1200000;
constexpr float kSunAngularRadiusDeg = 0.25f;
constexpr float kPi                  = 3.14159265358979323846f;

std::vector<Vec3f> BuildFibonacciSphere(int n)
{
    std::vector<Vec3f> dirs;
    dirs.reserve(static_cast<size_t>(n));

    // Golden-angle increment. y is spaced uniformly in [-1, 1], which makes the
    // samples uniform by SOLID ANGLE - the measure the energy argument is
    // written in - rather than uniform in latitude.
    const float golden = kPi * (3.0f - std::sqrt(5.0f));

    for (int i = 0; i < n; ++i)
    {
        const float y = 1.0f - (static_cast<float>(i) + 0.5f) * (2.0f / static_cast<float>(n));
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float theta = golden * static_cast<float>(i);
        dirs.push_back(Vec3f(std::cos(theta) * r, y, std::sin(theta) * r));
    }
    return dirs;
}

const std::vector<Vec3f>& SphereDirections()
{
    static const std::vector<Vec3f> s_dirs = BuildFibonacciSphere(kSphereSamples);
    return s_dirs;
}

// -----------------------------------------------------------------------------
// The sky under test, and a sky with a sun bolted on
// -----------------------------------------------------------------------------
// Every detector takes a SkyFn rather than calling core::SkyRadiance directly.
// That is what lets Sky_DetectorsCatchAnAddedSunDisc run the SAME detectors over
// a sky that does have a sun and require them to fire - so "this test has teeth"
// is itself a tested property rather than a claim in a comment.
using SkyFn = Vec3f (*)(const Vec3f&);

Vec3f ShippedSky(const Vec3f& d)
{
    return core::SkyRadiance(d);
}

// -----------------------------------------------------------------------------
// FIXTURES for the positive control - deliberately NOT the shipped sky
// -----------------------------------------------------------------------------
// The control below proves the DETECTORS have teeth. That is a property of the
// detectors, so it must not be entangled with whatever the shipped sky happens
// to be: if the control ran over core::SkyRadiance and someone later changed the
// sky, the control would go red for a reason having nothing to do with whether
// the detectors still work, and its failure message would point at the wrong
// thing. (Watched: it did exactly that on the first run.)
//
// So the control gets its own fixture sky. This is a TEST FIXTURE and is not a
// mirror of any shader - it is not required to match anything, and nothing
// should ever try to keep it in step with sky_common.hlsli. It only has to be a
// smooth, sunless gradient, which is the class of sky the detectors must accept.
Vec3f FixtureGradientSky(const Vec3f& d)
{
    const float t = 0.5f * (d.y + 1.0f);
    const Vec3f lo = { 0.7f, 0.75f, 0.95f };
    const Vec3f hi = { 0.2f, 0.4f,  0.8f  };
    return lo + (hi - lo) * std::clamp(t, 0.0f, 1.0f);
}

// A HARD-EDGED disc: zero gradient outside, so nothing can be found by hill
// climbing and the detector has to actually land a sample inside it. Real sun
// discs are written with a smooth falloff and are therefore strictly EASIER to
// detect than this. Testing against the hard case is the conservative choice.
struct SyntheticSun
{
    Vec3f sunDir      = Vec3f(0.0f, 1.0f, 0.0f);
    float cosRadius   = 0.0f;
    float intensity   = 0.0f;   // added radiance, per channel, inside the disc
};

SyntheticSun g_sun;   // read by FixtureSunlitSky below

Vec3f FixtureSunlitSky(const Vec3f& d)
{
    Vec3f base = FixtureGradientSky(d);
    if (d.Dot(g_sun.sunDir) >= g_sun.cosRadius)
    {
        base += Vec3f(g_sun.intensity, g_sun.intensity, g_sun.intensity);
    }
    return base;
}

// -----------------------------------------------------------------------------
// Detector 3 - localised energy
// -----------------------------------------------------------------------------
double PeakToMeanLuminance(SkyFn sky)
{
    const std::vector<Vec3f>& dirs = SphereDirections();

    double sum  = 0.0;
    double peak = 0.0;
    for (const Vec3f& d : dirs)
    {
        const double lum = static_cast<double>(core::Luminance(sky(d)));
        sum += lum;
        if (lum > peak) peak = lum;
    }

    const double mean = sum / static_cast<double>(dirs.size());
    if (mean <= 0.0) return 0.0;
    return peak / mean;
}

// -----------------------------------------------------------------------------
// Detector 1 - azimuthal symmetry
// -----------------------------------------------------------------------------
// For each sampled direction, rotate it about +Y onto the canonical azimuth
// (z == 0, x >= 0) and require identical radiance. Rotating about Y preserves
// direction.y exactly, so for a sky that depends on y alone this is identically
// zero; for anything with azimuthal structure it is not.
//
// Note this is doubly sensitive: a sample lands on the deviation if EITHER it or
// its canonical partner falls inside a sun disc.
double MaxAzimuthalDeviation(SkyFn sky)
{
    const std::vector<Vec3f>& dirs = SphereDirections();

    double worst = 0.0;
    for (const Vec3f& d : dirs)
    {
        const float r = std::sqrt(std::max(0.0f, d.x * d.x + d.z * d.z));
        const Vec3f canonical(r, d.y, 0.0f);

        const Vec3f a = sky(d);
        const Vec3f b = sky(canonical);

        const double dev = std::max({ std::fabs(static_cast<double>(a.x - b.x)),
                                      std::fabs(static_cast<double>(a.y - b.y)),
                                      std::fabs(static_cast<double>(a.z - b.z)) });
        if (dev > worst) worst = dev;
    }
    return worst;
}

// -----------------------------------------------------------------------------
// Detector 2 - affine in elevation
// -----------------------------------------------------------------------------
// Walk y from -1 to 1 along the canonical azimuth and take second differences.
// A straight line has second difference zero; a step of any size does not.
//
// The step count is set by the same reasoning as kSphereSamples, in one
// dimension: a disc of angular radius 0.25 degrees centred on a pole spans
// 1 - cos(0.25 deg) = 9.5e-6 in y, so the spacing 2/kElevationSteps must be
// below that. 400000 steps gives 5.0e-6, comfortably inside it.
constexpr int kElevationSteps = 400000;

double MaxElevationSecondDifference(SkyFn sky)
{
    auto lumAt = [sky](double y) -> double {
        const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
        return static_cast<double>(
            core::Luminance(sky(Vec3f(static_cast<float>(r), static_cast<float>(y), 0.0f))));
    };

    const double step = 2.0 / static_cast<double>(kElevationSteps);

    double worst = 0.0;
    double prev2 = lumAt(-1.0);
    double prev1 = lumAt(-1.0 + step);
    for (int i = 2; i <= kElevationSteps; ++i)
    {
        const double y   = -1.0 + step * static_cast<double>(i);
        const double cur = lumAt(y);

        const double secondDiff = std::fabs(cur - 2.0 * prev1 + prev2);
        if (secondDiff > worst) worst = secondDiff;

        prev2 = prev1;
        prev1 = cur;
    }
    return worst;
}

// -----------------------------------------------------------------------------
// Source tripwire
// -----------------------------------------------------------------------------
// Normalise away everything that cannot change behaviour - // comments, /* */
// comments, and all whitespace - so reformatting and comment edits do not cause
// spurious failures, while any change to an actual token does.
std::string NormaliseHlsl(const std::string& src)
{
    std::string out;
    out.reserve(src.size());

    for (size_t i = 0; i < src.size(); )
    {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/')
        {
            while (i < src.size() && src[i] != '\n') ++i;
        }
        else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i = (i + 1 < src.size()) ? i + 2 : src.size();
        }
        else
        {
            const unsigned char c = static_cast<unsigned char>(src[i]);
            if (!std::isspace(c)) out.push_back(src[i]);
            ++i;
        }
    }
    return out;
}

uint64_t Fnv1a64(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (char c : s)
    {
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ull;
    }
    return h;
}

bool ReadFileText(const char* path, std::string& out)
{
    // Binary rather than text mode; NormaliseHlsl strips all whitespace
    // afterwards, so CRLF and LF checkouts hash identically either way.
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;

    out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

// =============================================================================
// The invariants
// =============================================================================

TEST_CASE(Sky_HasNoLocalisedEnergyLobe)
{
    // The energy claim of IBL_DESIGN.md section 9.2, stated directly: no
    // direction in the sky carries a concentration of energy, so the environment
    // and the analytic directional light are disjoint and can be summed.
    //
    // The bound permits a smooth gradient of any orientation - the current sky
    // sits at about 1.27 - and forbids any direction brighter than twice the
    // sphere's own mean. A sun disc cannot satisfy that: a sun exists precisely
    // to be much brighter than its surroundings, and any disc worth adding lands
    // in the tens or thousands, not below two.
    const double ratio = PeakToMeanLuminance(&ShippedSky);

    std::printf("      sky peak/mean luminance = %.6f  (bound 2.0)\n", ratio);

    CHECK(ratio <= 2.0);

    // Vacuity guard. A sky that is identically zero, or one whose luminance the
    // detector failed to evaluate at all, would satisfy "peak <= 2 * mean"
    // trivially. Require a real, non-degenerate distribution.
    CHECK(ratio >= 1.0);
    CHECK(ratio > 1.05);
}

TEST_CASE(Sky_IsAzimuthallySymmetric)
{
    // Catches a sun disc at ANY direction other than exactly a pole, at ANY
    // brightness - this is exact, not a bound, so a dim sun fails it just as
    // hard as a bright one.
    const double dev = MaxAzimuthalDeviation(&ShippedSky);

    std::printf("      max azimuthal deviation = %.9g  (tolerance 1e-6)\n", dev);

    CHECK(dev <= 1e-6);
}

TEST_CASE(Sky_IsAffineInElevation)
{
    // The remaining case: a sun at +Y or -Y is azimuthally symmetric and
    // invisible to the test above. It is not affine in elevation.
    const double d2 = MaxElevationSecondDifference(&ShippedSky);

    std::printf("      max elevation second difference = %.9g  (tolerance 1e-6)\n", d2);

    CHECK(d2 <= 1e-6);
}

// =============================================================================
// The teeth, made into an assertion
// =============================================================================

TEST_CASE(Sky_DirectionSetCannotMissASunDisc)
{
    // THE VACUITY GUARD FOR THIS ENTIRE FILE.
    //
    // Every assertion above evaluates the sky at a finite set of directions. If
    // that set is coarse enough to slip between a sun disc and its neighbours,
    // all three pass while a sun sits in the sky - a green test that proves
    // nothing, which is the exact failure mode this file exists to prevent.
    //
    // So measure the worst-case gap rather than trusting the lattice's
    // reputation: for a spread of probe directions, find the angle to the
    // nearest sample and require it to be inside a sun radius.
    const std::vector<Vec3f>& dirs = SphereDirections();

    // Probes chosen to include the awkward places - both poles, the horizon,
    // and directions with no relationship to the lattice's golden angle.
    std::vector<Vec3f> probes;
    probes.push_back(Vec3f(0.0f,  1.0f, 0.0f));
    probes.push_back(Vec3f(0.0f, -1.0f, 0.0f));
    probes.push_back(Vec3f(1.0f,  0.0f, 0.0f));
    probes.push_back(Vec3f(0.0f,  0.0f, 1.0f));
    for (int i = 0; i < 60; ++i)
    {
        // Deterministic, irrational-ish, and deliberately unrelated to the
        // golden angle that generated the lattice.
        const float u = std::fmod(static_cast<float>(i) * 0.7548776662f, 1.0f) * 2.0f - 1.0f;
        const float v = std::fmod(static_cast<float>(i) * 0.5698402909f, 1.0f) * 2.0f * kPi;
        const float r = std::sqrt(std::max(0.0f, 1.0f - u * u));
        probes.push_back(Vec3f(std::cos(v) * r, u, std::sin(v) * r));
    }

    double worstGapDeg = 0.0;
    for (const Vec3f& p : probes)
    {
        float best = -1.0f;
        for (const Vec3f& d : dirs)
        {
            const float c = d.Dot(p);
            if (c > best) best = c;
        }
        const double gapDeg =
            std::acos(std::min(1.0, std::max(-1.0, static_cast<double>(best)))) * 180.0 / kPi;
        if (gapDeg > worstGapDeg) worstGapDeg = gapDeg;
    }

    std::printf("      worst sampling gap = %.4f deg  (must be under %.4f deg)\n",
                worstGapDeg, static_cast<double>(kSunAngularRadiusDeg));

    // A disc of angular radius kSunAngularRadiusDeg centred anywhere is
    // therefore guaranteed to contain at least one sample.
    CHECK(worstGapDeg < static_cast<double>(kSunAngularRadiusDeg));
}

TEST_CASE(Sky_DetectorsCatchAnAddedSunDisc)
{
    // "An assertion nobody has watched fail is not evidence." Rather than
    // watching it once by hand and writing the result in a commit message, the
    // failure is encoded: add a sun to a fixture sky and require the detectors
    // above to reject it.
    //
    // If someone later weakens a tolerance or thins the direction set to make a
    // failure go away, THIS case goes red and says why.
    //
    // Runs over the FIXTURE sky, not the shipped one - see the note by
    // FixtureGradientSky. This case is about the detectors, and it must keep
    // meaning that even after the sky legitimately changes.
    const float radiusRad = kSunAngularRadiusDeg * kPi / 180.0f;
    const float cosRadius = std::cos(radiusRad);

    // --- Case 0: with NO sun, the detectors must ACCEPT the fixture. ---
    // Without this the case could "pass" by rejecting everything, which would
    // make the three invariants above unfalsifiable rather than merely strict.
    g_sun.intensity = 0.0f;
    g_sun.cosRadius = cosRadius;
    g_sun.sunDir    = Vec3f(0.0f, 1.0f, 0.0f);

    CHECK(PeakToMeanLuminance(&FixtureSunlitSky)        <= 2.0);
    CHECK(MaxAzimuthalDeviation(&FixtureSunlitSky)      <= 1e-6);
    CHECK(MaxElevationSecondDifference(&FixtureSunlitSky) <= 1e-6);

    // Deliberately DIM. The mean sky luminance is about 0.33, so this is roughly
    // three times the whole sphere's average - far below any physically-argued
    // sun, and far below what someone would pick to make a sun look like a sun.
    // Catching this one means catching the realistic cases with enormous margin.
    const float dimSun = 1.0f;

    // --- Case 1: an off-pole sun. Symmetry and energy must both reject it. ---
    g_sun.sunDir    = Vec3f(0.35f, 0.62f, 0.70f).Normalized();
    g_sun.cosRadius = cosRadius;
    g_sun.intensity = dimSun;

    const double offPoleAzimuth = MaxAzimuthalDeviation(&FixtureSunlitSky);
    const double offPoleRatio   = PeakToMeanLuminance(&FixtureSunlitSky);

    std::printf("      off-pole sun: azimuthal deviation = %.6f, peak/mean = %.4f\n",
                offPoleAzimuth, offPoleRatio);

    CHECK(offPoleAzimuth > 1e-6);    // detector 1 fires
    CHECK(offPoleRatio   > 2.0);     // detector 3 fires

    // --- Case 2: a sun at +Y. Symmetry CANNOT see it; affineness must. ---
    g_sun.sunDir = Vec3f(0.0f, 1.0f, 0.0f);

    const double polarAzimuth = MaxAzimuthalDeviation(&FixtureSunlitSky);
    const double polarSecond  = MaxElevationSecondDifference(&FixtureSunlitSky);
    const double polarRatio   = PeakToMeanLuminance(&FixtureSunlitSky);

    std::printf("      polar sun: azimuthal deviation = %.9g, second diff = %.6f, peak/mean = %.4f\n",
                polarAzimuth, polarSecond, polarRatio);

    // Stated as an assertion rather than left implicit: a polar sun really is
    // invisible to the azimuthal test. That is WHY the elevation test exists,
    // and if this ever stops being true the reasoning above needs revisiting.
    CHECK(polarAzimuth <= 1e-6);

    CHECK(polarSecond > 1e-6);       // detector 2 fires
    CHECK(polarRatio  > 2.0);        // detector 3 fires

    // --- Case 3: a sun so dim that only the exact detectors can see it. ---
    // Below the energy bound entirely. This is the case that shows why the file
    // does not rely on peak/mean alone.
    g_sun.sunDir    = Vec3f(-0.5f, 0.1f, 0.86f).Normalized();
    g_sun.intensity = 0.05f;

    const double faintAzimuth = MaxAzimuthalDeviation(&FixtureSunlitSky);
    const double faintRatio   = PeakToMeanLuminance(&FixtureSunlitSky);

    std::printf("      faint sun: azimuthal deviation = %.6f, peak/mean = %.4f\n",
                faintAzimuth, faintRatio);

    CHECK(faintAzimuth > 1e-6);      // detector 1 still fires
    CHECK(faintRatio  <= 2.0);       // detector 3 legitimately does not

    g_sun.intensity = 0.0f;
}

// =============================================================================
// The mirror tripwire
// =============================================================================

TEST_CASE(Sky_TwinIsInStepWithShaderSource)
{
    // core::SkyRadiance is a hand-written copy of DawningSkyRadiance. See this
    // file's header and src/core/sky_radiance.h for exactly what this buys and
    // what it does not.
    const char* path = TD_SHADER_SKY_COMMON_PATH;

    std::string src;
    if (!CHECK(ReadFileText(path, src)))
    {
        std::printf("          could not open %s\n", path);
        return;
    }

    const std::string normalised = NormaliseHlsl(src);
    const uint64_t    hash       = Fnv1a64(normalised);

    // Pinned against the reviewed content of shaders/sky_common.hlsli.
    constexpr uint64_t kPinnedHash = 0x4aa891ac02a9a7b2ull;

    if (hash != kPinnedHash)
    {
        std::printf(
            "\n"
            "      ---------------------------------------------------------------\n"
            "      shaders/sky_common.hlsli HAS CHANGED.\n"
            "      ---------------------------------------------------------------\n"
            "      computed hash 0x%016llx, pinned 0x%016llx\n"
            "\n"
            "      This is not a formatting complaint - comments and whitespace are\n"
            "      stripped before hashing, so a real token changed.\n"
            "\n"
            "      src/core/sky_radiance.cpp is a hand-written C++ TWIN of that\n"
            "      shader, and the sky energy tests in this file run against the\n"
            "      twin. If the two drift apart, those tests keep passing while the\n"
            "      renderer does something else - which is the whole failure this\n"
            "      tripwire exists to prevent.\n"
            "\n"
            "      DO THIS:\n"
            "        1. If you added a SUN DISC, stop. Read the banner at the top of\n"
            "           shaders/sky_common.hlsli and section 9.2 of\n"
            "           docs/research/IBL_DESIGN.md. The analytic directional light\n"
            "           already represents the sun; a second copy in the sky is a\n"
            "           double count that will read as a tone-mapping problem.\n"
            "        2. Otherwise, mirror your change into\n"
            "           src/core/sky_radiance.cpp, confirm the other cases in this\n"
            "           file still hold for the new sky, and re-pin kPinnedHash to\n"
            "           the computed value above.\n"
            "      ---------------------------------------------------------------\n",
            static_cast<unsigned long long>(hash),
            static_cast<unsigned long long>(kPinnedHash));
    }

    CHECK(hash == kPinnedHash);
}

// =============================================================================
// The twin itself - IBL_DESIGN.md section 11 Stage 1's CPU unit tests
// =============================================================================

TEST_CASE(Sky_TwinMatchesHandComputedValues)
{
    // Straight down: t = 0, so the horizon colour at half intensity.
    const Vec3f down = core::SkyRadiance(Vec3f(0.0f, -1.0f, 0.0f));
    CHECK_APPROX(down.x, 0.40f);
    CHECK_APPROX(down.y, 0.425f);
    CHECK_APPROX(down.z, 0.45f);

    // Horizon: t = 0.5, the midpoint of the two colours at half intensity.
    const Vec3f horizon = core::SkyRadiance(Vec3f(1.0f, 0.0f, 0.0f));
    CHECK_APPROX(horizon.x, 0.275f);
    CHECK_APPROX(horizon.y, 0.3375f);
    CHECK_APPROX(horizon.z, 0.45f);

    // Straight up: t = 1, the zenith colour at half intensity.
    const Vec3f up = core::SkyRadiance(Vec3f(0.0f, 1.0f, 0.0f));
    CHECK_APPROX(up.x, 0.15f);
    CHECK_APPROX(up.y, 0.25f);
    CHECK_APPROX(up.z, 0.45f);

    // saturate() on the blend: directions outside the unit sphere must clamp
    // rather than extrapolate to negative or over-bright radiance.
    const Vec3f farBelow = core::SkyRadiance(Vec3f(0.0f, -8.0f, 0.0f));
    CHECK_APPROX(farBelow.x, down.x);
    const Vec3f farAbove = core::SkyRadiance(Vec3f(0.0f, 8.0f, 0.0f));
    CHECK_APPROX(farAbove.x, up.x);
}

TEST_CASE(Sky_TwinIsPositiveAndMonotoneInElevation)
{
    float previous = 1e30f;
    for (int i = 0; i <= 256; ++i)
    {
        const float y = -1.0f + 2.0f * (static_cast<float>(i) / 256.0f);
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const Vec3f c = core::SkyRadiance(Vec3f(r, y, 0.0f));

        // Strictly positive everywhere - nothing in the lighting model should
        // ever receive negative environment radiance.
        if (!(c.x > 0.0f && c.y > 0.0f && c.z > 0.0f))
        {
            CHECK(c.x > 0.0f);
            CHECK(c.y > 0.0f);
            CHECK(c.z > 0.0f);
            return;
        }

        // Luminance falls monotonically from nadir to zenith.
        const float lum = core::Luminance(c);
        if (lum > previous + 1e-6f)
        {
            CHECK(lum <= previous + 1e-6f);
            return;
        }
        previous = lum;
    }
    CHECK(true);
}
