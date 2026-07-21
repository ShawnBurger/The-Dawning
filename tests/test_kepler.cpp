// =============================================================================
// tests/test_kepler.cpp — robust Kepler solver + elements<->state conversion
// =============================================================================
// SIM STAGE 1 (analytic half). Drives the SHIPPED sim/kepler.{h,cpp}: the Markley
// non-iterative elliptic solver, the hyperbolic solver, the universal-variable
// (Stumpff) propagator, and the osculating-elements <-> state conversion both
// directions. Verification per RELATIVISTIC_SIM_ARCHITECTURE.md §4.5 and the
// prompt's (g): Markley matches a reference solve; round-trips are identity to
// tolerance across elliptic/parabolic/hyperbolic; the (e->1,M->0) critical region
// is handled. NO exact energy-conservation fantasy — analytic Kepler is exact by
// construction, so these test the SOLVER math, not an integrator tolerance.
//
// Nothing here touches D3D12. Pure CPU header tests.
// =============================================================================

#include "test_framework.h"
#include "sim/kepler.h"

#include <cmath>

namespace
{
using core::Vec3d;
using sim::StateVector;

constexpr double kEarthMu = 3.986004418e14; // m^3/s^2
constexpr double kPi = 3.14159265358979323846;

double VecDiff(const Vec3d& a, const Vec3d& b) { return (a - b).Length(); }

// Residual of Kepler's elliptic equation for a candidate E.
double EllipticResidual(double E, double e, double M)
{
    double d = std::fmod((E - e * std::sin(E)) - M, 2.0 * kPi);
    if (d >  kPi) d -= 2.0 * kPi;
    if (d < -kPi) d += 2.0 * kPi;
    return d;
}
} // namespace

// -----------------------------------------------------------------------------
// Markley elliptic solver: self-consistency + agreement with a reference solve
// across the full eccentricity range, INCLUDING the e>0.75 cancellation regime.
// -----------------------------------------------------------------------------
TEST_CASE(Kepler_MarkleyResidualIsMachineSmall)
{
    const double es[] = { 0.0, 0.05, 0.3, 0.5, 0.75, 0.85, 0.9, 0.99, 0.999 };
    for (double e : es)
    {
        for (int k = -18; k <= 18; ++k)
        {
            const double M = k * (kPi / 18.0); // sweep [-pi, pi]
            const double E = sim::SolveKeplerElliptic(M, e);
            const double res = EllipticResidual(E, e, M);
            // One-pass Markley reaches machine precision across the range.
            CHECK(std::fabs(res) < 1e-11);
        }
    }
}

TEST_CASE(Kepler_MarkleyMatchesReferenceSolve)
{
    const double es[] = { 0.1, 0.5, 0.75, 0.9, 0.95, 0.99 };
    for (double e : es)
    {
        for (int k = -30; k <= 30; ++k)
        {
            const double M = k * (kPi / 30.0);
            const double Em = sim::SolveKeplerElliptic(M, e);
            const double Er = sim::SolveKeplerEllipticReference(M, e);
            CHECK_APPROX_EPS(Em, Er, 1e-10);
        }
    }
}

// The (e->1, M->0) critical region the research flags: Markley's lone singularity
// (e==1 && M==0) is harmless because E==0 there. Assert E is exactly 0 at M==0 and
// stays continuous/finite for tiny M at near-parabolic e.
TEST_CASE(Kepler_CriticalRegion_NearParabolicSmallM)
{
    const double e = 0.9999;
    CHECK_EQ(sim::SolveKeplerElliptic(0.0, e), 0.0); // exact short-circuit

    double prev = 0.0;
    for (int k = 1; k <= 20; ++k)
    {
        const double M = k * 1e-7;
        const double E = sim::SolveKeplerElliptic(M, e);
        CHECK(std::isfinite(E));
        CHECK(std::fabs(EllipticResidual(E, e, M)) < 1e-10);
        CHECK(E >= prev);       // monotone increasing in M — no discontinuity
        prev = E;
    }
    // Odd symmetry through the singular point.
    CHECK_APPROX_EPS(sim::SolveKeplerElliptic(-1e-6, e),
                     -sim::SolveKeplerElliptic(1e-6, e), 1e-14);
}

// -----------------------------------------------------------------------------
// Hyperbolic solver
// -----------------------------------------------------------------------------
TEST_CASE(Kepler_HyperbolicResidualIsSmall)
{
    const double es[] = { 1.05, 1.3, 2.0, 5.0 };
    for (double e : es)
    {
        for (int k = -20; k <= 20; ++k)
        {
            const double M = k * 0.5; // hyperbolic M is unbounded; sweep a wide range
            const double H = sim::SolveKeplerHyperbolic(M, e);
            const double res = e * std::sinh(H) - H - M;
            CHECK(std::fabs(res) < 1e-9 * (1.0 + std::fabs(M)));
        }
    }
}

// -----------------------------------------------------------------------------
// Anomaly conversions round-trip
// -----------------------------------------------------------------------------
TEST_CASE(Kepler_AnomalyRoundTrips)
{
    const double e = 0.6;
    for (int k = -10; k <= 10; ++k)
    {
        const double nu = k * (kPi / 11.0);
        const double E  = sim::TrueToEccentricAnomaly(nu, e);
        const double nu2 = sim::EccentricToTrueAnomaly(E, e);
        CHECK_APPROX_EPS(nu, nu2, 1e-12);
    }
    // Hyperbolic true<->anomaly round trip (bounded true-anomaly range).
    const double eh = 1.7;
    const double numax = std::acos(-1.0 / eh) - 0.05; // inside the asymptote
    for (int k = -8; k <= 8; ++k)
    {
        const double nu = k * (numax / 9.0);
        const double H  = sim::TrueToHyperbolicAnomaly(nu, eh);
        const double nu2 = sim::HyperbolicToTrueAnomaly(H, eh);
        CHECK_APPROX_EPS(nu, nu2, 1e-11);
    }
}

// -----------------------------------------------------------------------------
// Stumpff functions
// -----------------------------------------------------------------------------
TEST_CASE(Kepler_StumpffFunctions)
{
    // Limits at psi -> 0.
    CHECK_APPROX_EPS(sim::StumpffC2(0.0), 0.5, 1e-15);
    CHECK_APPROX_EPS(sim::StumpffC3(0.0), 1.0 / 6.0, 1e-15);

    // The series branch (|psi|<1e-8) returns the correct limit to HIGH accuracy —
    // which the closed form CANNOT at tiny psi, because (1-cos)/psi cancels. That
    // is precisely why the series branch exists.
    CHECK_APPROX_EPS(sim::StumpffC2(1e-10), 0.5, 1e-11);
    CHECK_APPROX_EPS(sim::StumpffC3(1e-10), 1.0 / 6.0, 1e-11);

    // Continuity ACROSS the branch boundary (psi ~ 1e-8): series (9e-9) and closed
    // form (1.1e-8) agree to the closed form's own accuracy there (cancellation
    // limits it to ~1e-7 at this scale, which is the honest bound).
    CHECK_APPROX_EPS(sim::StumpffC2(9e-9), sim::StumpffC2(1.1e-8), 1e-7);
    CHECK_APPROX_EPS(sim::StumpffC3(9e-9), sim::StumpffC3(1.1e-8), 1e-7);

    // Positive psi (elliptic) closed form.
    const double p = 1.5;
    CHECK_APPROX_EPS(sim::StumpffC2(p), (1.0 - std::cos(std::sqrt(p))) / p, 1e-13);
    // Negative psi (hyperbolic) closed form.
    const double n = -1.5;
    CHECK_APPROX_EPS(sim::StumpffC2(n), (std::cosh(std::sqrt(-n)) - 1.0) / (-n), 1e-13);
}

// -----------------------------------------------------------------------------
// Elements <-> state round-trips: elliptic, near-parabolic, hyperbolic
// -----------------------------------------------------------------------------
namespace
{
void CheckElementRoundTrip(double a, double e, double i, double Om, double w, double nu,
                           double tolPos)
{
    ecs::OrbitalElements el;
    el.semiMajorAxis = a;
    el.eccentricity = e;
    el.inclination = i;
    el.longitudeAscNode = Om;
    el.argPeriapsis = w;
    el.trueAnomaly = nu;

    const StateVector sv = sim::ElementsToState(el, kEarthMu);
    const ecs::OrbitalElements el2 = sim::StateToElements(sv, kEarthMu);
    const StateVector sv2 = sim::ElementsToState(el2, kEarthMu);

    // Elements can differ by 2pi wraps / branch choices; the invariant that must
    // hold is that the STATE reconstructed from the fitted elements matches.
    CHECK(VecDiff(sv.position, sv2.position) < tolPos);
    CHECK(VecDiff(sv.velocity, sv2.velocity) < tolPos * 1e-3 + 1e-6);

    // And the scalar shape elements come back directly.
    CHECK_APPROX_EPS(el2.semiMajorAxis, a, std::fabs(a) * 1e-9);
    CHECK_APPROX_EPS(el2.eccentricity, e, 1e-9);
    CHECK_APPROX_EPS(el2.inclination, i, 1e-9);
}
} // namespace

TEST_CASE(Kepler_ElementStateRoundTrip_Elliptic)
{
    CheckElementRoundTrip(2.0e7, 0.3, 0.5, 1.0, 2.0, 1.2, 1e-3);
}

TEST_CASE(Kepler_ElementStateRoundTrip_NearParabolic)
{
    // e just below 1 (bound) — the ill-conditioned near-parabolic elliptic case.
    CheckElementRoundTrip(5.0e8, 0.99, 0.7, 2.0, 0.5, 0.8, 1e-1);
}

TEST_CASE(Kepler_ElementStateRoundTrip_Hyperbolic)
{
    // a<0, e>1. True anomaly well inside the asymptote.
    CheckElementRoundTrip(-3.0e7, 1.4, 0.6, 1.5, 1.0, 0.5, 1e-2);
}

// -----------------------------------------------------------------------------
// Universal-variable propagation cross-checks
// -----------------------------------------------------------------------------

// Elliptic: universal propagation must agree with element-based (Markley)
// propagation over a fraction of a period. Two independent methods, one answer.
TEST_CASE(Kepler_UniversalMatchesElementPropagation_Elliptic)
{
    ecs::OrbitalElements el;
    el.semiMajorAxis = 2.0e7;
    el.eccentricity = 0.4;
    el.inclination = 0.5;
    el.longitudeAscNode = 1.0;
    el.argPeriapsis = 2.0;
    el.trueAnomaly = 0.3;

    const double period = 2.0 * kPi * std::sqrt(std::pow(el.semiMajorAxis, 3) / kEarthMu);
    const StateVector s0 = sim::ElementsToState(el, kEarthMu);

    for (double frac : { 0.1, 0.25, 0.5, 0.9 })
    {
        const double dt = frac * period;
        bool ok = false;
        const StateVector su = sim::PropagateUniversal(s0, kEarthMu, dt, &ok);
        CHECK(ok);
        const ecs::OrbitalElements el2 = sim::PropagateElements(el, kEarthMu, dt);
        const StateVector se = sim::ElementsToState(el2, kEarthMu);
        CHECK(VecDiff(su.position, se.position) < 1.0);       // < 1 m over ~2e7 m orbit
        CHECK(VecDiff(su.velocity, se.velocity) < 1e-3);
    }
}

// A full elliptic period returns to the start (analytic closure — exact, not an
// integrator tolerance).
TEST_CASE(Kepler_EllipticClosesOverOnePeriod)
{
    ecs::OrbitalElements el;
    el.semiMajorAxis = 1.5e7;
    el.eccentricity = 0.55;
    el.inclination = 0.9;
    el.longitudeAscNode = 0.3;
    el.argPeriapsis = 1.1;
    el.trueAnomaly = -0.7;

    const double period = 2.0 * kPi * std::sqrt(std::pow(el.semiMajorAxis, 3) / kEarthMu);
    const StateVector s0 = sim::ElementsToState(el, kEarthMu);
    const StateVector s1 = sim::PropagateUniversal(s0, kEarthMu, period);
    CHECK(VecDiff(s0.position, s1.position) < 1e-2);
    CHECK(VecDiff(s0.velocity, s1.velocity) < 1e-5);
}

// Near-parabolic / hyperbolic propagation through periapsis stays finite and
// conserves specific energy and specific angular momentum (the universal-variable
// robustness the e->1 region demands). This is the (e->1, M->0) critical region at
// the PROPAGATION level.
TEST_CASE(Kepler_UniversalHyperbolicThroughPeriapsis)
{
    // Build a hyperbolic state near periapsis from elements.
    ecs::OrbitalElements el;
    el.semiMajorAxis = -2.0e7; // a<0
    el.eccentricity = 1.001;   // barely hyperbolic — near-parabolic and stiff
    el.inclination = 0.4;
    el.longitudeAscNode = 0.6;
    el.argPeriapsis = 1.2;
    el.trueAnomaly = -0.3;     // just before periapsis

    const StateVector s0 = sim::ElementsToState(el, kEarthMu);
    const double energy0 = s0.velocity.LengthSq() * 0.5 - kEarthMu / s0.position.Length();
    const Vec3d h0 = s0.position.Cross(s0.velocity);

    // Step across periapsis in several sub-steps.
    StateVector s = s0;
    for (int k = 0; k < 20; ++k)
    {
        bool ok = false;
        s = sim::PropagateUniversal(s, kEarthMu, 30.0, &ok);
        CHECK(ok);
        CHECK(std::isfinite(s.position.x));
        CHECK(std::isfinite(s.velocity.x));
        const double energy = s.velocity.LengthSq() * 0.5 - kEarthMu / s.position.Length();
        CHECK_APPROX_EPS(energy, energy0, std::fabs(energy0) * 1e-7);
        const Vec3d h = s.position.Cross(s.velocity);
        CHECK(VecDiff(h, h0) < h0.Length() * 1e-7);
    }
}
