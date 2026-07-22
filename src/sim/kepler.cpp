// =============================================================================
// sim/kepler.cpp — Robust Kepler solver + elements <-> state conversion
// =============================================================================
// See kepler.h for the module contract and the literature citations.
// =============================================================================

#include "kepler.h"

#include <cmath>

namespace sim
{

namespace
{
constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

// Wrap an angle to [-pi, pi].
double WrapPi(double x)
{
    double y = std::fmod(x, kTwoPi);
    if (y >  kPi) y -= kTwoPi;
    if (y < -kPi) y += kTwoPi;
    return y;
}

// Wrap an angle to [0, 2pi).
double WrapTwoPi(double x)
{
    double y = std::fmod(x, kTwoPi);
    if (y < 0.0) y += kTwoPi;
    return y;
}
} // namespace

// -----------------------------------------------------------------------------
// Markley elliptic Kepler solver (NASA 1995), non-iterative fifth order.
// -----------------------------------------------------------------------------
double SolveKeplerElliptic(double meanAnomaly, double eccentricity)
{
    const double e = eccentricity;
    double M = WrapPi(meanAnomaly);

    // The Markley singularity is at e==1 && M==0; e<1 here so it never bites.
    // M==0 -> E==0 exactly, short-circuit (also the harmless (e->1,M->0) corner).
    if (M == 0.0)
        return 0.0;

    // Solution is odd in M: solve for |M|, restore the sign at the end. This keeps
    // the starter's cube roots on non-negative arguments and is numerically tidy.
    const double sign = (M < 0.0) ? -1.0 : 1.0;
    M = std::fabs(M);

    // --- Markley cubic starter (his Eqs. 19-22) -------------------------------
    const double pi2 = kPi * kPi;
    const double alpha = (3.0 * pi2 + 1.6 * kPi * (kPi - M) / (1.0 + e)) / (pi2 - 6.0);
    const double d = 3.0 * (1.0 - e) + alpha * e;
    const double q = 2.0 * alpha * d * (1.0 - e) - M * M;
    const double r = 3.0 * alpha * d * (d - 1.0 + e) * M + M * M * M;
    const double w = std::pow(std::fabs(r) + std::sqrt(q * q * q + r * r), 2.0 / 3.0);
    double E = (2.0 * r * w / (w * w + w * q + q * q) + M) / d;

    // --- One fifth-order correction (his Eqs. 24-27) --------------------------
    // f = E - e sinE - M and derivatives. f' uses the STABLE form
    // (1-e) + 2e sin^2(E/2) == 1 - e cosE, cancellation-free at e->1, E->0.
    const double sinE = std::sin(E);
    const double cosE = std::cos(E);
    const double sinHalf = std::sin(0.5 * E);
    const double f0   = E - e * sinE - M;         // residual
    const double f1   = (1.0 - e) + 2.0 * e * sinHalf * sinHalf; // f'  (stable)
    const double f2   = e * sinE;                 // f''
    const double f3   = e * cosE;                 // f'''
    const double f4   = -e * sinE;                // f''''

    const double d3 = -f0 / (f1 - 0.5 * f0 * f2 / f1);
    const double d4 = -f0 / (f1 + 0.5 * d3 * f2 + (d3 * d3) * f3 / 6.0);
    const double d5 = -f0 / (f1 + 0.5 * d4 * f2 + (d4 * d4) * f3 / 6.0
                                   + (d4 * d4 * d4) * f4 / 24.0);
    E += d5;

    return sign * E;
}

double SolveKeplerEllipticReference(double meanAnomaly, double eccentricity)
{
    const double e = eccentricity;
    const double M = WrapPi(meanAnomaly);

    // Newton from the Danby/Markley seed E0 = M + e sinM, with a bisection bracket
    // [M-1, M+1] (E is within 1 rad of M for any e<1 since |E-M| = |e sinE| < 1)
    // as a safety net if a Newton step leaves the bracket.
    double lo = M - 1.0;
    double hi = M + 1.0;
    double E = M + e * std::sin(M);

    for (int it = 0; it < 100; ++it)
    {
        const double f  = E - e * std::sin(E) - M;
        // Keep the bracket tight around the root.
        if (f > 0.0) hi = E; else lo = E;

        const double sinHalf = std::sin(0.5 * E);
        const double fp = (1.0 - e) + 2.0 * e * sinHalf * sinHalf; // stable f'
        double next = E - f / fp;
        if (!(next > lo && next < hi))
            next = 0.5 * (lo + hi); // bisection fallback

        if (std::fabs(next - E) <= 1e-15 * (1.0 + std::fabs(E)))
            return next;
        E = next;
    }
    return E;
}

// -----------------------------------------------------------------------------
// Hyperbolic Kepler solver: M = e sinhH - H, e > 1.
// -----------------------------------------------------------------------------
double SolveKeplerHyperbolic(double meanAnomaly, double eccentricity)
{
    const double e = eccentricity;
    const double M = meanAnomaly; // hyperbolic M is not periodic; use as-is

    if (M == 0.0)
        return 0.0;

    const double sign = (M < 0.0) ? -1.0 : 1.0;
    const double Mabs = std::fabs(M);

    // Seed. asinh(M/e) is a good global starter; for large M grow logarithmically.
    double H;
    if (Mabs > 4.0 * e)
        H = std::log(2.0 * Mabs / e + 1.8); // Newton-safe large-M starter
    else
        H = std::asinh(Mabs / e);

    // Newton with an expanding bisection bracket as a fallback.
    double lo = 0.0;
    double hi = H > 0.0 ? 2.0 * H + 1.0 : 1.0;
    // Grow hi until it brackets (f(hi) > 0). f(H) = e sinhH - H - Mabs.
    while (e * std::sinh(hi) - hi - Mabs < 0.0)
        hi *= 2.0;

    for (int it = 0; it < 100; ++it)
    {
        const double f  = e * std::sinh(H) - H - Mabs;
        if (f > 0.0) hi = H; else lo = H;

        const double fp = e * std::cosh(H) - 1.0;
        double next = H - f / fp;
        if (!(next > lo && next < hi))
            next = 0.5 * (lo + hi);

        if (std::fabs(next - H) <= 1e-14 * (1.0 + std::fabs(H)))
        {
            H = next;
            break;
        }
        H = next;
    }
    return sign * H;
}

// -----------------------------------------------------------------------------
// Stumpff functions
// -----------------------------------------------------------------------------
double StumpffC2(double psi)
{
    if (psi > 1e-8)
    {
        const double s = std::sqrt(psi);
        return (1.0 - std::cos(s)) / psi;
    }
    if (psi < -1e-8)
    {
        const double s = std::sqrt(-psi);
        return (std::cosh(s) - 1.0) / (-psi);
    }
    // Series near 0: c2 = 1/2 - psi/24 + psi^2/720 - ...
    return 0.5 - psi / 24.0 + psi * psi / 720.0;
}

double StumpffC3(double psi)
{
    if (psi > 1e-8)
    {
        const double s = std::sqrt(psi);
        return (s - std::sin(s)) / (s * s * s);
    }
    if (psi < -1e-8)
    {
        const double s = std::sqrt(-psi);
        return (std::sinh(s) - s) / (s * s * s);
    }
    // Series near 0: c3 = 1/6 - psi/120 + psi^2/5040 - ...
    return 1.0 / 6.0 - psi / 120.0 + psi * psi / 5040.0;
}

// -----------------------------------------------------------------------------
// Universal-variable propagation (Vallado Algorithm 8, KEPLER)
// -----------------------------------------------------------------------------
StateVector PropagateUniversal(const StateVector& state, double mu, double dt, bool* ok)
{
    if (ok) *ok = true;
    if (dt == 0.0)
        return state;

    const Vec3d r0v = state.position;
    const Vec3d v0v = state.velocity;
    const double r0 = r0v.Length();
    const double v0 = v0v.Length();
    const double sqrtMu = std::sqrt(mu);
    const double rdotv = r0v.Dot(v0v);

    const double alpha = -v0 * v0 / mu + 2.0 / r0; // = 1/a

    // Initial guess for the universal anomaly chi. The conic is chosen on the
    // SCALE-FREE ratio alpha*r0 = r0/a (dimensionless), not on alpha (units 1/m)
    // directly: an absolute 1/length threshold misclassifies every solar-scale
    // ellipse (a ~ 1e11 m => alpha ~ 1e-11 < 1e-9) as near-parabolic and denies it
    // the ellipse seed. Newton still converges from the generic seed for benign
    // cases, but the correct seed protects the eccentric/large-dt arcs the transfer
    // planner propagates. (Same scale-free-threshold discipline as
    // relative_motion.cpp's kConditioning.)
    double chi;
    const double alphaR0 = alpha * r0;
    if (alphaR0 > 1e-9)
    {
        // Ellipse (and circle). chi0 = sqrt(mu) dt / a = sqrt(mu) dt alpha.
        chi = sqrtMu * dt * alpha;
    }
    else if (alphaR0 < -1e-9)
    {
        // Hyperbola.
        const double a = 1.0 / alpha; // negative
        const double s = (dt < 0.0) ? -1.0 : 1.0;
        const double num = -2.0 * mu * alpha * dt;
        const double den = rdotv + s * std::sqrt(-mu * a) * (1.0 - r0 * alpha);
        chi = s * std::sqrt(-a) * std::log(num / den);
    }
    else
    {
        // Near-parabolic: a robust generic seed. The Newton iteration below
        // converges from here for |alpha| ~ 0.
        chi = sqrtMu * std::fabs(dt) / r0;
        if (dt < 0.0) chi = -chi;
    }

    // Newton iteration on chi.
    double r = r0;
    bool converged = false;
    for (int it = 0; it < 200; ++it)
    {
        const double psi = chi * chi * alpha;
        const double c2 = StumpffC2(psi);
        const double c3 = StumpffC3(psi);
        const double chi2 = chi * chi;
        r = chi2 * c2 + (rdotv / sqrtMu) * chi * (1.0 - psi * c3) + r0 * (1.0 - psi * c2);
        const double F = (rdotv / sqrtMu) * chi2 * c2
                       + r0 * chi * (1.0 - psi * c3) + chi2 * chi * c3
                       - sqrtMu * dt;
        const double dchi = F / r; // r == dF/dchi
        chi -= dchi;
        if (std::fabs(dchi) <= 1e-12 * (1.0 + std::fabs(chi)))
        {
            converged = true;
            break;
        }
    }
    if (!converged && ok) *ok = false;

    // f and g functions.
    const double psi = chi * chi * alpha;
    const double c2 = StumpffC2(psi);
    const double c3 = StumpffC3(psi);
    const double chi2 = chi * chi;

    const double f = 1.0 - chi2 / r0 * c2;
    const double g = dt - chi2 * chi / sqrtMu * c3;

    Vec3d rv = r0v * f + v0v * g;
    const double rMag = rv.Length();

    const double fdot = sqrtMu / (rMag * r0) * chi * (psi * c3 - 1.0);
    const double gdot = 1.0 - chi2 / rMag * c2;

    Vec3d vv = r0v * fdot + v0v * gdot;

    return { rv, vv };
}

// -----------------------------------------------------------------------------
// Anomaly conversions
// -----------------------------------------------------------------------------
double TrueToEccentricAnomaly(double nu, double e)
{
    // tan(E/2) = sqrt((1-e)/(1+e)) tan(nu/2). atan2 keeps the branch.
    const double E = 2.0 * std::atan2(std::sqrt(1.0 - e) * std::sin(0.5 * nu),
                                      std::sqrt(1.0 + e) * std::cos(0.5 * nu));
    return E;
}

double EccentricToMeanAnomaly(double E, double e)
{
    return E - e * std::sin(E);
}

double EccentricToTrueAnomaly(double E, double e)
{
    const double nu = 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(0.5 * E),
                                       std::sqrt(1.0 - e) * std::cos(0.5 * E));
    return nu;
}

double TrueToHyperbolicAnomaly(double nu, double e)
{
    // tanh(H/2) = sqrt((e-1)/(e+1)) tan(nu/2).
    const double t = std::sqrt((e - 1.0) / (e + 1.0)) * std::tan(0.5 * nu);
    return 2.0 * std::atanh(t);
}

double HyperbolicToMeanAnomaly(double H, double e)
{
    return e * std::sinh(H) - H;
}

double HyperbolicToTrueAnomaly(double H, double e)
{
    const double nu = 2.0 * std::atan2(std::sqrt(e + 1.0) * std::sinh(0.5 * H),
                                       std::sqrt(e - 1.0) * std::cosh(0.5 * H));
    return nu;
}

// -----------------------------------------------------------------------------
// Elements <-> state
// -----------------------------------------------------------------------------
namespace
{
// Rotate a perifocal (PQW) vector into the inertial (IJK) frame using the
// 3-1-3 sequence Rz(Omega) Rx(i) Rz(argp). Standard right-handed convention;
// ElementsToState and StateToElements use it consistently so the round trip is
// exact regardless of the engine's rendering handedness.
Vec3d PerifocalToInertial(const Vec3d& pqw, double Omega, double inc, double argp)
{
    const double cO = std::cos(Omega), sO = std::sin(Omega);
    const double ci = std::cos(inc),   si = std::sin(inc);
    const double cw = std::cos(argp),  sw = std::sin(argp);

    const double m00 = cO * cw - sO * sw * ci;
    const double m01 = -cO * sw - sO * cw * ci;
    const double m02 = sO * si;
    const double m10 = sO * cw + cO * sw * ci;
    const double m11 = -sO * sw + cO * cw * ci;
    const double m12 = -cO * si;
    const double m20 = sw * si;
    const double m21 = cw * si;
    const double m22 = ci;

    return {
        m00 * pqw.x + m01 * pqw.y + m02 * pqw.z,
        m10 * pqw.x + m11 * pqw.y + m12 * pqw.z,
        m20 * pqw.x + m21 * pqw.y + m22 * pqw.z,
    };
}
} // namespace

StateVector ElementsToState(const ecs::OrbitalElements& el, double mu)
{
    const double a  = el.semiMajorAxis;
    const double e  = el.eccentricity;
    const double nu = el.trueAnomaly;

    // Semi-latus rectum p = a(1-e^2), finite and positive for both ellipse (a>0)
    // and hyperbola (a<0, e>1 => 1-e^2 < 0 => p>0).
    const double p = a * (1.0 - e * e);
    const double r = p / (1.0 + e * std::cos(nu));

    // Perifocal position and velocity.
    const Vec3d rPqw{ r * std::cos(nu), r * std::sin(nu), 0.0 };
    const double sqrtMuP = std::sqrt(mu / p);
    const Vec3d vPqw{ -sqrtMuP * std::sin(nu), sqrtMuP * (e + std::cos(nu)), 0.0 };

    return {
        PerifocalToInertial(rPqw, el.longitudeAscNode, el.inclination, el.argPeriapsis),
        PerifocalToInertial(vPqw, el.longitudeAscNode, el.inclination, el.argPeriapsis),
    };
}

ecs::OrbitalElements StateToElements(const StateVector& state, double mu)
{
    const Vec3d rv = state.position;
    const Vec3d vv = state.velocity;
    const double r = rv.Length();
    const double v = vv.Length();

    const Vec3d h = rv.Cross(vv);          // specific angular momentum
    const double hMag = h.Length();
    const Vec3d nodeV{ -h.y, h.x, 0.0 };   // n = zhat x h
    const double nMag = nodeV.Length();

    // Eccentricity vector.
    const Vec3d eVec = (rv * (v * v - mu / r) - vv * rv.Dot(vv)) * (1.0 / mu);
    const double e = eVec.Length();

    // Specific energy -> semi-major axis. a = -mu/(2 xi). Parabolic (xi~0) is not
    // representable as a; callers use PropagateUniversal there.
    const double xi = v * v * 0.5 - mu / r;
    const double a = -mu / (2.0 * xi);

    ecs::OrbitalElements el;
    el.semiMajorAxis = a;
    el.eccentricity  = e;

    // Inclination (acos on double, clamped to guard round-off outside [-1,1]).
    {
        double ci = h.z / hMag;
        ci = ci > 1.0 ? 1.0 : (ci < -1.0 ? -1.0 : ci);
        el.inclination = std::acos(ci);
    }

    const double kTiny = 1e-11;

    // Longitude of ascending node.
    if (nMag > kTiny)
    {
        double cO = nodeV.x / nMag;
        cO = cO > 1.0 ? 1.0 : (cO < -1.0 ? -1.0 : cO);
        double Omega = std::acos(cO);
        if (nodeV.y < 0.0) Omega = kTwoPi - Omega;
        el.longitudeAscNode = Omega;
    }
    else
    {
        el.longitudeAscNode = 0.0; // equatorial: node undefined, reference to +x
    }

    // Argument of periapsis.
    if (nMag > kTiny && e > kTiny)
    {
        double cw = nodeV.Dot(eVec) / (nMag * e);
        cw = cw > 1.0 ? 1.0 : (cw < -1.0 ? -1.0 : cw);
        double argp = std::acos(cw);
        if (eVec.z < 0.0) argp = kTwoPi - argp;
        el.argPeriapsis = argp;
    }
    else if (e > kTiny)
    {
        // Equatorial, eccentric: measure argp from +x to the eccentricity vector.
        double cw = eVec.x / e;
        cw = cw > 1.0 ? 1.0 : (cw < -1.0 ? -1.0 : cw);
        double argp = std::acos(cw);
        if (eVec.y < 0.0) argp = kTwoPi - argp;
        el.argPeriapsis = argp;
    }
    else
    {
        el.argPeriapsis = 0.0; // circular: periapsis undefined
    }

    // True anomaly.
    if (e > kTiny)
    {
        double cnu = eVec.Dot(rv) / (e * r);
        cnu = cnu > 1.0 ? 1.0 : (cnu < -1.0 ? -1.0 : cnu);
        double nu = std::acos(cnu);
        if (rv.Dot(vv) < 0.0) nu = kTwoPi - nu;
        el.trueAnomaly = WrapPi(nu);
    }
    else
    {
        // Circular: use argument of latitude from the node (or +x if equatorial).
        const Vec3d ref = (nMag > kTiny) ? nodeV : Vec3d{ 1.0, 0.0, 0.0 };
        const double refMag = (nMag > kTiny) ? nMag : 1.0;
        double cnu = ref.Dot(rv) / (refMag * r);
        cnu = cnu > 1.0 ? 1.0 : (cnu < -1.0 ? -1.0 : cnu);
        double nu = std::acos(cnu);
        if (rv.z < 0.0) nu = kTwoPi - nu;
        el.trueAnomaly = WrapPi(nu);
    }

    return el;
}

ecs::OrbitalElements PropagateElements(const ecs::OrbitalElements& el, double mu, double dt)
{
    // Elliptic only (a>0, e<1). Advance mean anomaly, resolve back to true anomaly.
    const double a = el.semiMajorAxis;
    const double e = el.eccentricity;

    const double E0 = TrueToEccentricAnomaly(el.trueAnomaly, e);
    const double M0 = EccentricToMeanAnomaly(E0, e);
    const double n  = std::sqrt(mu / (a * a * a)); // mean motion
    const double M1 = M0 + n * dt;
    const double E1 = SolveKeplerElliptic(M1, e);
    const double nu1 = EccentricToTrueAnomaly(E1, e);

    ecs::OrbitalElements out = el;
    out.trueAnomaly = WrapPi(nu1);
    return out;
}

} // namespace sim
