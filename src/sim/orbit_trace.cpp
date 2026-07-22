// =============================================================================
// sim/orbit_trace.cpp — see orbit_trace.h.
// =============================================================================

#include "orbit_trace.h"

#include "kepler.h"  // ElementsToState, EccentricToTrueAnomaly

#include <cmath>

namespace sim
{

namespace
{
constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;
} // namespace

std::vector<Vec3d> SampleOrbitPath(const ecs::OrbitalElements& el, double mu,
                                   uint32_t segments)
{
    std::vector<Vec3d> path;
    if (segments < 3u || !std::isfinite(mu) || mu <= 0.0)
        return path;

    const double a = el.semiMajorAxis;
    const double e = el.eccentricity;
    if (!std::isfinite(a) || !std::isfinite(e) || e < 0.0 || a == 0.0)
        return path;

    path.reserve(segments + 1u);
    ecs::OrbitalElements sample = el; // shares i / RAAN / argp; only trueAnomaly varies

    if (e < 1.0)
    {
        // Closed ellipse: sweep eccentric anomaly E over [0, 2pi]. Evaluating the
        // position through ElementsToState (which converts nu -> r,v in the same
        // perifocal->inertial frame the sim uses) guarantees the loop is the exact
        // orbit the body rides.
        for (uint32_t k = 0; k <= segments; ++k)
        {
            const double E = (kTwoPi * static_cast<double>(k)) / static_cast<double>(segments);
            sample.trueAnomaly = EccentricToTrueAnomaly(E, e);
            path.push_back(ElementsToState(sample, mu).position);
        }
    }
    else
    {
        // Open hyperbolic arc: true anomaly is bounded by the asymptote acos(-1/e).
        // Stop just inside it so r stays finite.
        const double nuMax = std::acos(-1.0 / e) - 0.02;
        for (uint32_t k = 0; k <= segments; ++k)
        {
            const double t = static_cast<double>(k) / static_cast<double>(segments);
            sample.trueAnomaly = -nuMax + 2.0 * nuMax * t;
            path.push_back(ElementsToState(sample, mu).position);
        }
    }

    return path;
}

} // namespace sim
