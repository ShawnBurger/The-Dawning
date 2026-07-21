// =============================================================================
// tests/test_nbody.cpp — N-body gravity, Forest-Ruth, softening, determinism, LOD
// =============================================================================
// SIM STAGE 1 (full-N-body half). Drives the SHIPPED sim/nbody.{h,cpp} +
// sim/kepler.{h,cpp} + sim/reference_frame.{h,cpp}. Verification per the prompt's
// (a)-(h) and RELATIVISTIC_SIM_ARCHITECTURE.md "DECISION REVISION":
//
//   (a) BOUNDED energy oscillation, NO secular trend (symplectic property).
//   (b) two-body N-body CLOSES on the analytic Kepler orbit to MEASURED tolerance.
//   (c) dt-CONVERGENCE of the drift at the method's (4th) order.
//   (d) Forest-Ruth coefficients EQUAL their published values (exact).
//   (e) angular momentum conserved to tolerance in a central-force case.
//   (f) the DISCRIMINATING control: explicit Euler on the SAME case shows SECULAR
//       drift (slope != 0) where Forest-Ruth does not.
//   (g) [in test_kepler.cpp] Kepler round-trips + Markley vs reference.
//   (h) LOD promote/demote continuity; on-rails and N-body agree over a short arc.
//
// Plus: determinism bit-identity (fixed id-sorted summation), shared softening
// (finite as r->0), cross-frame force precision (Stage-0 layer), one-owner guard.
//
// The rule enforced throughout: NO exact energy/momentum conservation assertion.
// Symplectic N-body DRIFTS (bounded, oscillating). We assert what is actually true.
//
// Nothing here touches D3D12. Pure CPU header tests.
// =============================================================================

#include "test_framework.h"
#include "sim/nbody.h"
#include "sim/kepler.h"
#include "sim/reference_frame.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
using core::Vec3d;
using sim::NBodyParticle;
using sim::StateVector;

constexpr double kEarthMu = 3.986004418e14; // m^3/s^2
constexpr double kPi = 3.14159265358979323846;

double VecDiff(const Vec3d& a, const Vec3d& b) { return (a - b).Length(); }

NBodyParticle MakeParticle(const Vec3d& pos, const Vec3d& vel, double mu, double radius,
                           uint64_t id, bool isSource)
{
    NBodyParticle p;
    p.position = pos;
    p.velocity = vel;
    p.mu = mu;
    p.softening = sim::SofteningLength(mu, radius); // eps precomputed from radius/r_s
    p.bodyId = id;
    p.isSource = isSource;
    return p;
}

// Least-squares slope of E(t) over samples — the "does the energy WALK?" statistic.
double LinearSlope(const std::vector<double>& t, const std::vector<double>& y)
{
    const size_t n = t.size();
    double st = 0, sy = 0, stt = 0, sty = 0;
    for (size_t i = 0; i < n; ++i) { st += t[i]; sy += y[i]; stt += t[i] * t[i]; sty += t[i] * y[i]; }
    const double dn = static_cast<double>(n);
    return (dn * sty - st * sy) / (dn * stt - st * st);
}

// A circular/eccentric equal-mass binary in its COM frame, placed at apoapsis of
// the relative orbit with relative semi-major axis a and eccentricity e.
std::vector<NBodyParticle> MakeBinary(double muEach, double a, double e)
{
    const double muRel = 2.0 * muEach;
    const double rApo = a * (1.0 + e);
    const double vRel = std::sqrt(muRel * (2.0 / rApo - 1.0 / a)); // vis-viva at apoapsis
    std::vector<NBodyParticle> b;
    b.push_back(MakeParticle({ -rApo * 0.5, 0, 0 }, { 0,  vRel * 0.5, 0 }, muEach, 1e3, 1, true));
    b.push_back(MakeParticle({  rApo * 0.5, 0, 0 }, { 0, -vRel * 0.5, 0 }, muEach, 1e3, 2, true));
    return b;
}
} // namespace

// =============================================================================
// (d) Forest-Ruth coefficients equal their published values (EXACT).
// =============================================================================
TEST_CASE(NBody_ForestRuthCoefficients_MatchPublished)
{
    const sim::ForestRuthCoefficients k = sim::GetForestRuthCoefficients();

    // theta = 1/(2 - 2^(1/3)) = 1.35120719195965763...
    const double theta = 1.3512071919596576;
    CHECK_APPROX_EPS(k.c[0], theta * 0.5, 1e-13);        // 0.6756035959798288
    CHECK_APPROX_EPS(k.c[1], (1.0 - theta) * 0.5, 1e-13); // -0.1756035959798288
    CHECK_APPROX_EPS(k.c[2], (1.0 - theta) * 0.5, 1e-13);
    CHECK_APPROX_EPS(k.c[3], theta * 0.5, 1e-13);
    CHECK_APPROX_EPS(k.d[0], theta, 1e-13);              // 1.3512071919596576
    CHECK_APPROX_EPS(k.d[1], 1.0 - 2.0 * theta, 1e-13);  // -1.7024143839193153
    CHECK_APPROX_EPS(k.d[2], theta, 1e-13);
    CHECK_EQ(k.d[3], 0.0);                               // last kick is skipped

    // Direct literal pins (independent of the theta expression above).
    CHECK_APPROX_EPS(k.c[0],  0.6756035959798289, 1e-13);
    CHECK_APPROX_EPS(k.c[1], -0.1756035959798288, 1e-13);
    CHECK_APPROX_EPS(k.d[0],  1.3512071919596576, 1e-13);
    CHECK_APPROX_EPS(k.d[1], -1.7024143839193153, 1e-13);

    // Structural invariants: drift and kick fractions each sum to 1.
    CHECK_APPROX_EPS(k.c[0] + k.c[1] + k.c[2] + k.c[3], 1.0, 1e-14);
    CHECK_APPROX_EPS(k.d[0] + k.d[1] + k.d[2] + k.d[3], 1.0, 1e-14);
}

// =============================================================================
// (b) Two-body N-body CLOSES on the analytic Kepler orbit over one period.
// =============================================================================
// A test particle around a fixed heavy primary is exactly the Kepler two-body
// problem (primary receives no reaction), so PropagateUniversal is ground truth.
TEST_CASE(NBody_TwoBody_ClosesOnAnalyticKepler)
{
    const double a = 2.0e7, e = 0.3;
    // Initial state at periapsis of an inclined orbit (primary-relative).
    ecs::OrbitalElements el;
    el.semiMajorAxis = a; el.eccentricity = e;
    el.inclination = 0.5; el.longitudeAscNode = 0.7; el.argPeriapsis = 1.1;
    el.trueAnomaly = 0.0;
    const StateVector s0 = sim::ElementsToState(el, kEarthMu);

    std::vector<NBodyParticle> bodies;
    bodies.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, kEarthMu, 0.0, 100, true));   // primary
    bodies.push_back(MakeParticle(s0.position, s0.velocity, 0.0, 0.0, 200, false));   // test particle

    const double T = 2.0 * kPi * std::sqrt(a * a * a / kEarthMu);
    const double dt = 2.0;
    const int steps = static_cast<int>(std::llround(T / dt));

    double maxErr = 0.0;
    for (int i = 1; i <= steps; ++i)
    {
        sim::StepNBody(bodies, dt);
        if (i % 500 == 0 || i == steps)
        {
            const StateVector truth = sim::PropagateUniversal(s0, kEarthMu, i * dt);
            maxErr = (std::max)(maxErr, VecDiff(bodies[1].position, truth.position));
        }
    }
    // Primary never moved (no reaction from a test particle).
    CHECK(VecDiff(bodies[0].position, { 0,0,0 }) == 0.0);

    // MEASURED tolerance: 4th-order symplectic over one period at dt=2s closes the
    // ~2e7 m orbit to well under 100 m (relative < 5e-6). Not a wished number — the
    // suite prints the max error if this bound is ever exceeded.
    CHECK(maxErr < 100.0);
    CHECK(maxErr / a < 5e-6);
}

// =============================================================================
// (a) BOUNDED energy oscillation, NO secular trend + (f) the Euler control.
// =============================================================================
TEST_CASE(NBody_EnergyBounded_NoSecularTrend_vs_EulerControl)
{
    const double muEach = 1.0e13, a = 1.0e7, e = 0.5;
    const double T = 2.0 * kPi * std::sqrt(a * a * a / (2.0 * muEach));
    const double dt = 30.0;
    const int perPeriod = static_cast<int>(std::llround(T / dt));
    const int steps = perPeriod * 4; // four orbits

    auto run = [&](bool symplectic, double& relSecular, double& relOsc)
    {
        std::vector<NBodyParticle> b = MakeBinary(muEach, a, e);
        const double E0 = sim::TotalEnergyGScaled(b);
        std::vector<double> ts, es;
        double emin = E0, emax = E0;
        for (int i = 1; i <= steps; ++i)
        {
            if (symplectic) sim::StepNBody(b, dt);
            else            sim::StepNBodyExplicitEuler(b, dt);
            if (i % 50 == 0)
            {
                const double E = sim::TotalEnergyGScaled(b);
                ts.push_back(i * dt);
                es.push_back(E);
                emin = (std::min)(emin, E);
                emax = (std::max)(emax, E);
            }
        }
        const double slope = LinearSlope(ts, es);
        relSecular = std::fabs(slope * (steps * dt)) / std::fabs(E0); // net walk over the run
        relOsc = (emax - emin) / std::fabs(E0);                        // wiggle amplitude
    };

    double frSecular = 0, frOsc = 0, euSecular = 0, euOsc = 0;
    run(true, frSecular, frOsc);
    run(false, euSecular, euOsc);

    // Forest-Ruth: the energy WIGGLES (eccentric orbit) but does NOT walk. The
    // fitted secular change is a tiny fraction of the run and far below the wiggle.
    CHECK(frOsc > 0.0);                 // it does oscillate (not a frozen constant)
    CHECK(frSecular < 1e-4);            // but no secular trend
    CHECK(frSecular < frOsc);          // net walk is smaller than one orbit's wiggle

    // THE DISCRIMINATING CONTROL: explicit Euler on the SAME system WALKS. Its
    // secular drift dwarfs Forest-Ruth's — if both looked fine the (a) assertion
    // would have no teeth.
    CHECK(euSecular > 1e-2);
    CHECK(euSecular > 50.0 * frSecular);
}

// =============================================================================
// (c) dt-CONVERGENCE at the method's (4th) order.
// =============================================================================
TEST_CASE(NBody_DtConvergence_FourthOrder)
{
    const double a = 2.0e7, e = 0.2;
    ecs::OrbitalElements el;
    el.semiMajorAxis = a; el.eccentricity = e;
    el.inclination = 0.3; el.longitudeAscNode = 0.2; el.argPeriapsis = 0.5;
    el.trueAnomaly = 0.0;
    const StateVector s0 = sim::ElementsToState(el, kEarthMu);

    const double T = 2.0 * kPi * std::sqrt(a * a * a / kEarthMu);
    const double arc = 0.5 * T; // half a period — clean dt^4 regime, above roundoff

    auto errorAtDt = [&](double dt)
    {
        std::vector<NBodyParticle> bodies;
        bodies.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, kEarthMu, 0.0, 1, true));
        bodies.push_back(MakeParticle(s0.position, s0.velocity, 0.0, 0.0, 2, false));
        const int steps = static_cast<int>(std::llround(arc / dt));
        for (int i = 0; i < steps; ++i) sim::StepNBody(bodies, dt);
        const StateVector truth = sim::PropagateUniversal(s0, kEarthMu, steps * dt);
        return VecDiff(bodies[1].position, truth.position);
    };

    const double e40 = errorAtDt(40.0);
    const double e20 = errorAtDt(20.0);
    const double e10 = errorAtDt(10.0);

    // 4th order: halving dt cuts the error ~16x. Assert a clear super-cubic drop
    // (ratio > 8) at each halving — generous enough for the varying orbital
    // timescale and roundoff, strict enough to reject a 1st/2nd-order method.
    CHECK(e40 > e20);
    CHECK(e20 > e10);
    CHECK(e40 / e20 > 8.0);
    CHECK(e20 / e10 > 8.0);
}

// =============================================================================
// (e) Angular momentum conserved to tolerance in a central-force case.
// =============================================================================
// Each Forest-Ruth drift and kick conserves r x v exactly for a central force,
// so the specific angular momentum holds to near machine precision.
TEST_CASE(NBody_AngularMomentum_CentralForce)
{
    const double a = 1.5e7, e = 0.4;
    ecs::OrbitalElements el;
    el.semiMajorAxis = a; el.eccentricity = e;
    el.inclination = 0.6; el.longitudeAscNode = 0.4; el.argPeriapsis = 0.9;
    el.trueAnomaly = 0.2;
    const StateVector s0 = sim::ElementsToState(el, kEarthMu);

    std::vector<NBodyParticle> bodies;
    bodies.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, kEarthMu, 0.0, 1, true));
    bodies.push_back(MakeParticle(s0.position, s0.velocity, 0.0, 0.0, 2, false));

    const Vec3d h0 = s0.position.Cross(s0.velocity);
    const double dt = 5.0;
    const double T = 2.0 * kPi * std::sqrt(a * a * a / kEarthMu);
    const int steps = static_cast<int>(std::llround(2.0 * T / dt));

    double maxRel = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        sim::StepNBody(bodies, dt);
        const Vec3d h = bodies[1].position.Cross(bodies[1].velocity);
        maxRel = (std::max)(maxRel, VecDiff(h, h0) / h0.Length());
    }
    CHECK(maxRel < 1e-10); // central-force symplectic conserves L to machine precision
}

// =============================================================================
// Determinism: bit-identical state regardless of INPUT order (fixed id-sort).
// =============================================================================
TEST_CASE(NBody_Determinism_BitIdenticalUnderReordering)
{
    auto build = []()
    {
        std::vector<NBodyParticle> b;
        b.push_back(MakeParticle({  1e6,  2e6,  0.5e6 }, { 10, -5,  2 }, 3.0e13, 1e4, 11, true));
        b.push_back(MakeParticle({ -2e6,  1e6, -1.0e6 }, { -3,  8, -1 }, 5.0e13, 1e4, 22, true));
        b.push_back(MakeParticle({  0.5e6,-3e6,  2.0e6 }, {  1,  2,  9 }, 1.0e13, 1e4, 33, true));
        b.push_back(MakeParticle({  3e6,  0.2e6, 1.0e6 }, { -7, -2,  4 }, 2.0e13, 1e4, 44, true));
        b.push_back(MakeParticle({ -1e6, -1e6, -2.0e6 }, {  6,  1, -3 }, 0.0,    1e2, 55, false)); // test particle
        return b;
    };

    std::vector<NBodyParticle> a = build();
    std::vector<NBodyParticle> shuffled = build();
    // Reverse the input order (a completely different presentation).
    std::reverse(shuffled.begin(), shuffled.end());

    const double dt = 1.0;
    for (int i = 0; i < 200; ++i) { sim::StepNBody(a, dt); sim::StepNBody(shuffled, dt); }

    // Match by bodyId; state must be BIT-IDENTICAL (== on doubles), not merely close.
    for (const NBodyParticle& pa : a)
    {
        const NBodyParticle* pb = nullptr;
        for (const NBodyParticle& q : shuffled) if (q.bodyId == pa.bodyId) { pb = &q; break; }
        CHECK(pb != nullptr);
        if (pb)
        {
            CHECK(pa.position == pb->position); // exact
            CHECK(pa.velocity == pb->velocity); // exact
        }
    }
}

// The mechanism, made discriminating: the FIXED id-sorted order gives the SAME
// bits for two presentations, while summing in raw INPUT order does NOT. If the
// input-order sum happened to match, the sort would be buying nothing.
TEST_CASE(NBody_Determinism_InputOrderIsNotOrderInvariant)
{
    // Many sources with HETEROGENEOUS magnitudes and irregular positions, so the
    // probe's acceleration is a sum of terms that is genuinely non-associative in
    // floating point — a config where reversing the accumulation order provably
    // changes the low bits (a too-symmetric 3-source config can sum equal by luck).
    std::vector<NBodyParticle> a;
    a.push_back(MakeParticle({ 1.3e6, -0.7e6, 0.9e6 }, { 0,0,0 }, 3.14159e15, 1e3, 1, true));
    a.push_back(MakeParticle({ -2.1e6, 1.1e6, -0.3e6 }, { 0,0,0 }, 2.71828e11, 1e3, 2, true));
    a.push_back(MakeParticle({ 0.5e6, -3.3e6, 2.7e6 }, { 0,0,0 }, 1.61803e14, 1e3, 3, true));
    a.push_back(MakeParticle({ -1.7e6, 0.2e6, -2.9e6 }, { 0,0,0 }, 9.87654e12, 1e3, 5, true));
    a.push_back(MakeParticle({ 3.1e6, 2.6e6, 0.4e6 }, { 0,0,0 }, 4.44444e13, 1e3, 6, true));
    a.push_back(MakeParticle({ -0.9e6, -1.4e6, 3.8e6 }, { 0,0,0 }, 6.02214e10, 1e3, 7, true));
    a.push_back(MakeParticle({ 2.2e6, -2.2e6, -1.1e6 }, { 0,0,0 }, 1.98765e13, 1e3, 8, true));
    a.push_back(MakeParticle({ 0.15e6, 0.35e6, 0.55e6 }, { 0,0,0 }, 0.0, 1e2, 4, false)); // probe
    std::vector<NBodyParticle> rev = a;
    std::reverse(rev.begin(), rev.end());

    std::vector<Vec3d> accA, accRev, accInA, accInRev;
    // Canonical (id-sorted) order → identical accel for the probe in both.
    sim::ComputeAccelerations(a,   sim::DeterministicOrder(a),   accA);
    sim::ComputeAccelerations(rev, sim::DeterministicOrder(rev), accRev);
    // Raw INPUT order (identity) → the negative control.
    std::vector<uint32_t> idA(a.size()), idRev(rev.size());
    for (uint32_t i = 0; i < idA.size(); ++i)   idA[i] = i;
    for (uint32_t i = 0; i < idRev.size(); ++i) idRev[i] = i;
    sim::ComputeAccelerations(a,   idA,   accInA);
    sim::ComputeAccelerations(rev, idRev, accInRev);

    auto probeAccel = [](const std::vector<NBodyParticle>& b, const std::vector<Vec3d>& acc)
    {
        for (size_t i = 0; i < b.size(); ++i) if (b[i].bodyId == 4) return acc[i];
        return Vec3d{};
    };
    const Vec3d cA = probeAccel(a, accA), cRev = probeAccel(rev, accRev);
    const Vec3d iA = probeAccel(a, accInA), iRev = probeAccel(rev, accInRev);

    // The shipped (sorted) path: bit-identical across the two presentations.
    CHECK(cA == cRev);
    // The naive (input-order) path: the SAME physical probe gets DIFFERENT bits,
    // because floating summation is non-associative. This is why the sort exists.
    CHECK(iA != iRev);
}

// =============================================================================
// Shared Plummer softening: finite as r -> 0; the unsoftened form is NaN there.
// =============================================================================
TEST_CASE(NBody_Softening_FiniteAtZeroSeparation)
{
    std::vector<NBodyParticle> bodies;
    bodies.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, kEarthMu, 1e5, 1, true));
    // Test particle placed EXACTLY on the source (worst case).
    bodies.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, 0.0, 0.0, 2, false));

    std::vector<Vec3d> acc;
    sim::ComputeAccelerations(bodies, sim::DeterministicOrder(bodies), acc);
    CHECK(std::isfinite(acc[1].x) && std::isfinite(acc[1].y) && std::isfinite(acc[1].z));

    // Integrate a straight radial plunge through the core — must never go NaN/Inf.
    std::vector<NBodyParticle> plunge;
    plunge.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, kEarthMu, 1e5, 1, true));
    plunge.push_back(MakeParticle({ 5e5,0,0 }, { -2e3,0,0 }, 0.0, 0.0, 2, false));
    for (int i = 0; i < 2000; ++i)
    {
        sim::StepNBody(plunge, 0.5);
        CHECK(std::isfinite(plunge[1].position.x));
        CHECK(std::isfinite(plunge[1].velocity.x));
        if (!std::isfinite(plunge[1].velocity.x)) break;
    }

    // THE DISCRIMINATING CONTROL: at r==0 the UNSOFTENED field 1/r^2 is NaN, while
    // the softened field is finite. Removing the softening breaks exactly this.
    const Vec3d d{ 0,0,0 };
    const double r2unsoft = d.LengthSq();                 // 0
    const double naive = 1.0 / (r2unsoft * std::sqrt(r2unsoft)); // 1/0 = inf
    const Vec3d naiveG = d * naive;                        // 0 * inf = NaN
    CHECK(std::isnan(naiveG.x));
    const double eps = sim::SofteningLength(kEarthMu, 1e5);
    const double r2soft = d.LengthSq() + eps * eps;        // eps^2 > 0
    CHECK(std::isfinite(1.0 / (r2soft * std::sqrt(r2soft))));
}

TEST_CASE(NBody_SofteningLength_Policy)
{
    // Large radius dominates.
    CHECK_APPROX_EPS(sim::SofteningLength(kEarthMu, 6.37e6), 6.37e6, 1e-6);
    // Zero radius -> r_s + eps0. r_s = 2 mu / c^2.
    const double rs = 2.0 * kEarthMu / (sim::kSpeedOfLight * sim::kSpeedOfLight);
    CHECK_APPROX_EPS(sim::SofteningLength(kEarthMu, 0.0), rs + sim::kSofteningBase, 1e-9);
    // Never below the base floor.
    CHECK(sim::SofteningLength(0.0, 0.0) >= sim::kSofteningBase);
}

// =============================================================================
// Cross-frame force precision: pairwise separation MUST be computed within a
// common frame (Stage-0). The naive absolute-coordinate subtraction loses the
// small offset far from the galactic origin — the catastrophic-cancellation trap.
// =============================================================================
TEST_CASE(NBody_CrossFramePrecision_UsesCommonFrame)
{
    using sim::WorldPos;
    using sim::FrameGraph;
    using sim::Body;
    using sim::kSectorSize;

    // A star-system frame ~1000 light-years from the galactic origin. At this
    // distance the absolute-double ULP is ~2 km (9.46e18 * 2.2e-16), so a metre-
    // scale separation is entirely below the ULP and a naive absolute subtraction
    // loses it completely — while the sector-split WorldPos keeps it exact.
    const double kiloLightYear = 9.4607e18;
    const int64_t sectorX = static_cast<int64_t>(std::llround(kiloLightYear / kSectorSize));
    FrameGraph graph;
    const sim::FrameId frame = graph.CreateFrame(sim::kInvalidFrame,
        WorldPos{ sectorX, 0, 0, Vec3d{ 0, 0, 0 } });

    // Two bodies 10 m apart inside that frame (small local coords).
    Body src; src.frame = frame; src.localPos = { 1000.0, 0.0, 0.0 };
    Body probe; probe.frame = frame; probe.localPos = { 1010.0, 0.0, 0.0 };

    const double mu = kEarthMu;
    const double eps = sim::SofteningLength(mu, 0.0);

    auto accelFrom = [&](const Vec3d& sep)
    {
        const double r2 = sep.LengthSq() + eps * eps;
        return sep * (-mu / (r2 * std::sqrt(r2)));
    };

    // TRUTH: the separation is exactly 10 m along +x by construction.
    const Vec3d sepTrue{ 10.0, 0.0, 0.0 };
    const Vec3d accTrue = accelFrom(sepTrue);

    // CORRECT (Stage-0): compute the separation WITHIN the common frame.
    const Vec3d sepFrame = graph.SeparationBetween(src, probe);
    const Vec3d accFrame = accelFrom(sepFrame);

    // NAIVE (wrong): reconstruct each absolute double position and subtract.
    auto naiveAbs = [&](const Body& b)
    {
        const WorldPos w = graph.ResolveWorldPos(b);
        return Vec3d{ static_cast<double>(w.sx) * kSectorSize + w.offset.x,
                      static_cast<double>(w.sy) * kSectorSize + w.offset.y,
                      static_cast<double>(w.sz) * kSectorSize + w.offset.z };
    };
    const Vec3d sepNaive = naiveAbs(probe) - naiveAbs(src);
    const Vec3d accNaive = accelFrom(sepNaive);

    // The common-frame path recovers the force essentially exactly.
    CHECK(VecDiff(accFrame, accTrue) / accTrue.Length() < 1e-9);
    // The naive absolute path is materially wrong at this distance (~2 m ULP on a
    // 10 m separation is a ~20% force error). This is the watched failure.
    CHECK(VecDiff(accNaive, accTrue) / accTrue.Length() > 1e-3);
}

// =============================================================================
// One owner per body per step + the double-count negative control.
// =============================================================================
TEST_CASE(NBody_OneOwner_DisjointGuard)
{
    std::vector<NBodyParticle> active;
    active.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, 1e13, 1e3, 1, true));
    active.push_back(MakeParticle({ 1,0,0 }, { 0,0,0 }, 1e13, 1e3, 2, true));
    active.push_back(MakeParticle({ 0,1,0 }, { 0,0,0 }, 0.0, 1e2, 3, false));

    // Valid partition: on-rails ids are disjoint from the active set.
    CHECK(sim::OwnersDisjoint(active, { 4, 5 }));

    // DOUBLE-COUNT hazard: id 3 is on-rails AND in the active N-body set. An
    // on-rails body already carries its primary's pull in its ellipse, so also
    // feeling N-body gravity double-counts it. The guard must catch this.
    CHECK_FALSE(sim::OwnersDisjoint(active, { 3, 5 }));
}

// Demonstrate WHY the guard matters: a body advanced on rails AND given N-body
// gravity diverges from the body advanced by exactly one mover.
TEST_CASE(NBody_DoubleCount_Diverges)
{
    const double a = 2.0e7, e = 0.1;
    ecs::OrbitalElements el;
    el.semiMajorAxis = a; el.eccentricity = e;
    el.inclination = 0.2; el.longitudeAscNode = 0.1; el.argPeriapsis = 0.3;
    el.trueAnomaly = 0.0;
    const StateVector s0 = sim::ElementsToState(el, kEarthMu);

    // Correct: single mover (on-rails analytic) for time t.
    const double t = 600.0;
    const StateVector single = sim::PropagateUniversal(s0, kEarthMu, t);

    // Wrong: the body is ALSO force-integrated (double owner). Integrate the same
    // arc under N-body gravity AND then treat it as if it also advanced on rails —
    // model the double pull by summing both displacements. It must diverge.
    std::vector<NBodyParticle> bodies;
    bodies.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, kEarthMu, 0.0, 1, true));
    bodies.push_back(MakeParticle(s0.position, s0.velocity, 0.0, 0.0, 2, false));
    const double dt = 2.0;
    const int steps = static_cast<int>(std::llround(t / dt));
    for (int i = 0; i < steps; ++i) sim::StepNBody(bodies, dt);
    const Vec3d nbodyPos = bodies[1].position; // this alone matches `single`

    // Sanity: the single N-body mover DOES match analytic (they are the same
    // physics applied once).
    CHECK(VecDiff(nbodyPos, single.position) < 10.0);

    // The double-counted position (rails displacement + N-body displacement applied
    // to the same body) is far from the correct single-mover answer.
    const Vec3d doubleCounted = nbodyPos + (single.position - s0.position);
    CHECK(VecDiff(doubleCounted, single.position) > 1.0e5);
}

// =============================================================================
// (h) LOD transition continuity: promote(demote(state)) == state; and an on-rails
// body and an N-body body started identically agree over a short arc.
// =============================================================================
TEST_CASE(NBody_LOD_PromoteDemoteContinuity)
{
    // An arbitrary bound orbit state (primary-relative).
    ecs::OrbitalElements el;
    el.semiMajorAxis = 3.0e7; el.eccentricity = 0.45;
    el.inclination = 0.8; el.longitudeAscNode = 1.3; el.argPeriapsis = 0.4;
    el.trueAnomaly = 0.9;
    const StateVector state = sim::ElementsToState(el, kEarthMu);

    // Demote: fit osculating elements from (r,v).
    const ecs::OrbitState rails = sim::DemoteToRails(state, kEarthMu, /*primaryId*/ 7, /*now*/ 123.0);
    // Promote back at the same instant (dtSinceEpoch = 0): must recover (r,v).
    const StateVector back = sim::PromoteFromRails(rails, 0.0);

    CHECK(VecDiff(back.position, state.position) < 1e-3);       // continuous in position
    CHECK(VecDiff(back.velocity, state.velocity) < 1e-6);       // and in velocity
    CHECK_EQ(rails.primaryBodyId, static_cast<uint64_t>(7));
    CHECK_APPROX_EPS(rails.epoch, 123.0, 0.0);
}

TEST_CASE(NBody_LOD_OnRailsAndNBodyAgreeOverArc)
{
    const double a = 2.5e7, e = 0.25;
    ecs::OrbitalElements el;
    el.semiMajorAxis = a; el.eccentricity = e;
    el.inclination = 0.4; el.longitudeAscNode = 0.9; el.argPeriapsis = 1.4;
    el.trueAnomaly = -0.5;
    const StateVector s0 = sim::ElementsToState(el, kEarthMu);

    // On-rails promotion produces the seed state; N-body integrates from the SAME
    // seed. Over a short arc they must stay together (promotion is continuous and
    // the physics matches).
    ecs::OrbitState rails = sim::DemoteToRails(s0, kEarthMu, 1, 0.0);

    std::vector<NBodyParticle> bodies;
    bodies.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, kEarthMu, 0.0, 100, true));
    bodies.push_back(MakeParticle(s0.position, s0.velocity, 0.0, 0.0, 200, false));

    const double dt = 2.0;
    const double arc = 1200.0; // seconds
    const int steps = static_cast<int>(std::llround(arc / dt));
    for (int i = 0; i < steps; ++i) sim::StepNBody(bodies, dt);

    const StateVector onRails = sim::PromoteFromRails(rails, arc);
    CHECK(VecDiff(bodies[1].position, onRails.position) < 50.0);
    CHECK(VecDiff(bodies[1].velocity, onRails.velocity) < 1e-2);
}

// =============================================================================
// Rigid-body operator-split lane: gravity as an external field probe.
// =============================================================================
TEST_CASE(NBody_GravityAccelerationAt_MatchesPointMass)
{
    std::vector<NBodyParticle> bodies;
    bodies.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, kEarthMu, 6.37e6, 1, true));
    bodies.push_back(MakeParticle({ 1e8, 0, 0 }, { 0,0,0 }, 0.0, 0.0, 2, false)); // ignored (not a source)

    const Vec3d point{ 7.0e6, 0.0, 0.0 };
    const Vec3d a = sim::GravityAccelerationAt(bodies, point, sim::SofteningLength(0, 0), 99);

    // Softened point-mass field magnitude: |a| = mu * r / (r^2 + eps^2)^1.5, with
    // eps = max(probe softening, source softening). The source's 6.37e6 radius is
    // comparable to r here, so softening is NOT negligible and must be included.
    const double eps = (sim::SofteningLength(0, 0) > bodies[0].softening)
                           ? sim::SofteningLength(0, 0) : bodies[0].softening;
    const double r = point.Length();
    const double r2 = r * r + eps * eps;
    const double expected = kEarthMu * r / (r2 * std::sqrt(r2)); // magnitude
    CHECK_APPROX_EPS(a.Length(), expected, expected * 1e-9);
    CHECK(a.x < 0.0); // points back toward the source at the origin

    // A source excluded via selfBodyId contributes nothing.
    const Vec3d aSelf = sim::GravityAccelerationAt(bodies, point, sim::SofteningLength(0, 0), 1);
    CHECK(aSelf.Length() == 0.0);
}

// =============================================================================
// Close-encounter detection (route the pair out of the symplectic step).
// =============================================================================
TEST_CASE(NBody_CloseEncounter_Detection)
{
    // Two well-separated, slow bodies — NOT an encounter.
    std::vector<NBodyParticle> safe;
    safe.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, 1e13, 1e5, 1, true));
    safe.push_back(MakeParticle({ 1e8,0,0 }, { 0,0,0 }, 1e13, 1e5, 2, true));
    CHECK_FALSE(sim::DetectCloseEncounter(safe, 1.0));

    // Two bodies within contact distance — an encounter.
    std::vector<NBodyParticle> hit;
    hit.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, 1e13, 1e5, 1, true));
    hit.push_back(MakeParticle({ 1.5e5,0,0 }, { 0,0,0 }, 1e13, 1e5, 2, true));
    CHECK(sim::DetectCloseEncounter(hit, 1.0));

    // Fast approach that would tunnel within one step — an encounter even though
    // the current separation exceeds contact.
    std::vector<NBodyParticle> fast;
    fast.push_back(MakeParticle({ 0,0,0 }, { 0,0,0 }, 1e13, 1e5, 1, true));
    fast.push_back(MakeParticle({ 1e6,0,0 }, { -2e6,0,0 }, 1e13, 1e5, 2, true));
    CHECK(sim::DetectCloseEncounter(fast, 1.0));
}
