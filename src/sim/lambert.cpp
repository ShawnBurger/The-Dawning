// =============================================================================
// sim/lambert.cpp — universal-variable Lambert solver. See lambert.h.
// Curtis, "Orbital Mechanics for Engineering Students", Algorithm 5.2.
// =============================================================================

#include "lambert.h"

#include "kepler.h"  // StumpffC2, StumpffC3

#include <cmath>

namespace sim {

using core::Vec3d;

namespace {
constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

bool Finite(const Vec3d& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
} // namespace

LambertSolution SolveLambert(const Vec3d& r1v, const Vec3d& r2v, double tof,
                             double mu, bool prograde)
{
    LambertSolution out;

    if (!std::isfinite(tof) || tof <= 0.0 || !std::isfinite(mu) || mu <= 0.0 ||
        !Finite(r1v) || !Finite(r2v))
        return out;

    const double r1 = r1v.Length();
    const double r2 = r2v.Length();
    if (!(r1 > 0.0) || !(r2 > 0.0))
        return out;

    // Transfer angle Δν from the geometry and the chosen sense. The z-component
    // of r1×r2 disambiguates the short (< 180°) vs long (> 180°) way.
    const Vec3d cross = r1v.Cross(r2v);
    double cosDnu = r1v.Dot(r2v) / (r1 * r2);
    cosDnu = (std::max)(-1.0, (std::min)(1.0, cosDnu)); // guard acos domain
    double dnu = std::acos(cosDnu);
    if (prograde) { if (cross.z < 0.0)  dnu = kTwoPi - dnu; }
    else          { if (cross.z >= 0.0) dnu = kTwoPi - dnu; }

    // A = sin Δν · sqrt(r1 r2 / (1 − cos Δν)). Colinear r1,r2 (Δν = 0 or π) leaves
    // the transfer plane undefined and A → 0/0 — reject rather than divide by ~0.
    const double denom = 1.0 - cosDnu;
    const double sinDnu = std::sin(dnu);
    if (denom <= 0.0 || std::fabs(sinDnu) < 1e-12)
        return out;
    const double A = sinDnu * std::sqrt(r1 * r2 / denom);
    if (std::fabs(A) < 1e-300)
        return out;

    const double sqrtMu = std::sqrt(mu);
    const double tolT   = 1e-8 * tof;       // converge on the time-of-flight error
    const double kZMax  = 4.0 * kPi * kPi;  // upper bound of the zero-rev branch

    // y(z) is feasible only where it is >= 0. For A > 0 that excludes too-SMALL z
    // (raise z to enter); for A < 0 too-LARGE z (lower z to enter). Evaluate C, S,
    // y at z; returns false on a non-finite / non-positive sqrt(C).
    auto evalY = [&](double zz, double& C, double& S, double& y) -> bool {
        C = StumpffC2(zz);
        S = StumpffC3(zz);
        const double sqrtC = std::sqrt(C);
        if (!std::isfinite(sqrtC) || sqrtC <= 0.0)
            return false;
        y = r1 + r2 + A * (zz * S - 1.0) / sqrtC;
        return std::isfinite(y);
    };

    double z = 0.0, C = 0.0, S = 0.0, y = 0.0;
    if (!evalY(z, C, S, y))
        return out;
    // Walk into the feasible region if z = 0 starts infeasible (only for A > 0).
    for (int g = 0; y < 0.0 && g < 4000; ++g)
        if (!evalY(z += 0.1, C, S, y))
            return out;
    if (!(y >= 0.0))
        return out;

    const int kMaxIter = 100;
    int iter = 0;
    bool converged = false;

    for (; iter < kMaxIter; ++iter)
    {
        const double sqrtY = std::sqrt(y);
        const double chi   = std::sqrt(y / C);
        const double t     = (chi * chi * chi * S + A * sqrtY) / sqrtMu;
        if (std::fabs(t - tof) < tolT)
        {
            converged = true;
            break;
        }

        // dt/dz (Curtis Alg. 5.2). At z = 0 the general form is 0/0, so use the
        // limit. The 1/√mu that t carries is applied to the derivative too.
        double dtdz;
        if (std::fabs(z) < 1e-8)
        {
            dtdz = (std::sqrt(2.0) / 40.0) * std::pow(y, 1.5) +
                   (A / 8.0) * (sqrtY + A * std::sqrt(1.0 / (2.0 * y)));
        }
        else
        {
            dtdz = std::pow(y / C, 1.5) *
                       ((1.0 / (2.0 * z)) * (C - 1.5 * S / C) +
                        0.75 * (S * S) / C) +
                   (A / 8.0) * (3.0 * (S / C) * sqrtY + A * std::sqrt(C / y));
        }
        dtdz /= sqrtMu;
        if (!std::isfinite(dtdz) || dtdz == 0.0)
            return out;

        const double dz = (t - tof) / dtdz;
        double zn = z - dz;
        // Safeguard 1: keep z on the ZERO-REVOLUTION branch, z < 4π². t(z) is
        // monotonic there but oscillatory beyond, so a full Newton step that
        // overshoots 4π² (common for the long-way A<0 branch at large tof) lands
        // in a different revolution and never returns. Bisect toward the boundary
        // instead. Since t → ∞ as z → 4π²⁻ and t(z) < tof here, the root is above.
        if (zn >= kZMax)
            zn = 0.5 * (z + kZMax);
        // Safeguard 2: keep the step feasible (y >= 0) by bisecting toward z.
        double Cn = 0.0, Sn = 0.0, yn = 0.0;
        int bt = 0;
        while ((!evalY(zn, Cn, Sn, yn) || yn < 0.0) && bt < 60)
        {
            zn = 0.5 * (z + zn);
            ++bt;
        }
        if (bt >= 60)
            return out; // no feasible step found
        z = zn; C = Cn; S = Sn; y = yn;
    }

    if (!converged || !(y >= 0.0))
        return out;

    // Lagrange coefficients from the converged y (Curtis 5.46). g is a time, so
    // g == 0 only for a degenerate transfer already screened above.
    const double f    = 1.0 - y / r1;
    const double g    = A * std::sqrt(y / mu);
    const double gdot = 1.0 - y / r2;
    if (!std::isfinite(g) || g == 0.0)
        return out;

    const Vec3d v1 = (r2v - r1v * f) / g;
    const Vec3d v2 = (r2v * gdot - r1v) / g;
    if (!Finite(v1) || !Finite(v2))
        return out;

    out.v1 = v1;
    out.v2 = v2;
    out.converged = true;
    out.iterations = static_cast<uint32_t>(iter);
    return out;
}

} // namespace sim
