// =============================================================================
// sim/relative_motion.cpp — Clohessy-Wiltshire / Hill relative motion.
// State-transition matrix from Curtis "Orbital Mechanics for Engineering
// Students" §7.4 / Vallado. See relative_motion.h for the frame convention.
// =============================================================================

#include "relative_motion.h"

#include <cmath>

namespace sim {

using core::Vec3d;

double MeanMotion(double mu, double a)
{
    if (!std::isfinite(mu) || mu <= 0.0 || !std::isfinite(a) || a <= 0.0)
        return 0.0;
    return std::sqrt(mu / (a * a * a));
}

CWState PropagateCW(const CWState& rel, double n, double t)
{
    if (!std::isfinite(n) || n <= 0.0 || !std::isfinite(t))
        return rel;

    const double nt = n * t;
    const double s  = std::sin(nt);
    const double c  = std::cos(nt);

    const double x0 = rel.position.x, y0 = rel.position.y, z0 = rel.position.z;
    const double vx0 = rel.velocity.x, vy0 = rel.velocity.y, vz0 = rel.velocity.z;

    CWState out;
    // Position (Curtis 7.53 Φ_rr, Φ_rv). In-plane (x,y) are Coriolis-coupled;
    // cross-track (z) is decoupled simple harmonic.
    out.position.x = (4.0 - 3.0 * c) * x0 + (s / n) * vx0 +
                     (2.0 / n) * (1.0 - c) * vy0;
    out.position.y = 6.0 * (s - nt) * x0 + y0 + (2.0 / n) * (c - 1.0) * vx0 +
                     (1.0 / n) * (4.0 * s - 3.0 * nt) * vy0;
    out.position.z = c * z0 + (s / n) * vz0;

    // Velocity (Curtis 7.53 Φ_vr, Φ_vv).
    out.velocity.x = 3.0 * n * s * x0 + c * vx0 + 2.0 * s * vy0;
    out.velocity.y = 6.0 * n * (c - 1.0) * x0 - 2.0 * s * vx0 +
                     (4.0 * c - 3.0) * vy0;
    out.velocity.z = -n * s * z0 + c * vz0;
    return out;
}

CWTargeting SolveCWTargeting(const Vec3d& r0, const Vec3d& rf, double n, double t)
{
    CWTargeting out;
    if (!std::isfinite(n) || n <= 0.0 || !std::isfinite(t) ||
        !std::isfinite(r0.x) || !std::isfinite(r0.y) || !std::isfinite(r0.z) ||
        !std::isfinite(rf.x) || !std::isfinite(rf.y) || !std::isfinite(rf.z))
        return out;

    const double nt = n * t;
    const double s  = std::sin(nt);
    const double c  = std::cos(nt);

    // r(t) = M_rr·r0 + M_rv·v0. Solve v0 = M_rv⁻¹ (rf − M_rr·r0).
    // In-plane 2×2 velocity block M_rv_xy and its target vector b.
    const double A = s / n;                         // ∂x/∂vx0
    const double B = (2.0 / n) * (1.0 - c);         // ∂x/∂vy0
    const double C = (2.0 / n) * (c - 1.0);         // ∂y/∂vx0
    const double D = (1.0 / n) * (4.0 * s - 3.0 * nt); // ∂y/∂vy0
    const double det = A * D - B * C;

    // M_rr contributions (position from r0) for x and y.
    const double bx = rf.x - (4.0 - 3.0 * c) * r0.x;
    const double by = rf.y - 6.0 * (s - nt) * r0.x - r0.y;

    // Reject SINGULAR targeting geometry with SCALE-FREE conditioning tests. det
    // carries a 1/n² factor, so a fixed absolute threshold is no conditioning
    // guard at all — test the dimensionless determinant det·n² = 8(1−cos nt) −
    // 3·nt·sin nt, whose roots are the true singular times: t = k·period AND
    // interior times (the first near nt ≈ 8.84 ≈ 1.4·period), where the inverse
    // blows up as 1/det. The cross-track map z = c·z0 + (s/n)·vz0 is conditioned
    // by |s| relative to the 1/n time scale, so |s| is the dimensionless measure —
    // 1e-12 was far too small (it admitted |v0z| ~ 1e9·Δz). One threshold, both.
    constexpr double kConditioning = 1.0e-6;
    if (std::fabs(det) * n * n < kConditioning || std::fabs(s) < kConditioning)
        return out; // singular / ill-conditioned geometry near a degenerate time

    const double v0x = (D * bx - B * by) / det;
    const double v0y = (-C * bx + A * by) / det;
    const double v0z = (rf.z - c * r0.z) * n / s;

    out.v0 = Vec3d{ v0x, v0y, v0z };

    // Arrival velocity from the full STM with the solved v0.
    const CWState arrival = PropagateCW(CWState{ r0, out.v0 }, n, t);
    out.arrivalVel = arrival.velocity;
    out.feasible = true;
    return out;
}

} // namespace sim
