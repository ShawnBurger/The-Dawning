// =============================================================================
// sim/star_system.cpp — star-system builder. See star_system.h.
// =============================================================================

#include "star_system.h"

#include "nbody.h"  // kGravitationalConstant
#include "soi.h"    // SphereOfInfluenceRadius

#include <limits>

namespace sim {

using core::Vec3d;

ecs::OrbitState CircularOrbit(double primaryMu, uint64_t primaryBodyId,
                              double radius, double inclination,
                              double longitudeAscNode, double phase, double epoch)
{
    ecs::OrbitState orbit;
    orbit.elements.semiMajorAxis    = radius;
    orbit.elements.eccentricity     = 0.0;
    orbit.elements.inclination      = inclination;
    orbit.elements.longitudeAscNode = longitudeAscNode;
    orbit.elements.argPeriapsis     = 0.0;   // undefined for e=0; fold phase into ν
    orbit.elements.trueAnomaly      = phase;
    orbit.primaryMu                 = primaryMu;
    orbit.primaryBodyId             = primaryBodyId;
    orbit.epoch                     = epoch;
    return orbit;
}

namespace {

// A rails body about `primary`: derives invMass from mu, its SOI radius from the
// orbit, and packs the circular orbit. isSource so it can host its own orbiters.
SystemBody RailsBody(uint64_t bodyId, uint64_t primaryBodyId, double primaryMu,
                     double mu, double radius, double orbitRadius,
                     double inclination, double node, double phase)
{
    SystemBody b;
    b.bodyId        = bodyId;
    b.primaryBodyId = primaryBodyId;
    b.owner         = ecs::OrbitOwner::OnRails;
    b.mu            = mu;
    b.radius        = radius;
    b.invMass       = kGravitationalConstant / mu; // 1/mass, mass = mu/G
    b.soiRadius     = SphereOfInfluenceRadius(orbitRadius, mu, primaryMu);
    b.isSource      = true;
    b.hasOrbit      = true;
    b.orbit = CircularOrbit(primaryMu, primaryBodyId, orbitRadius, inclination,
                            node, phase, 0.0);
    return b;
}

} // namespace

StarSystem BuildReferenceSystem()
{
    // Real gravitational parameters (m^3/s^2) and radii (m).
    constexpr double kMuSun   = 1.32712440018e20;
    constexpr double kMuEarth = 3.986004418e14;
    constexpr double kMuMars  = 4.282837e13;
    constexpr double kMuMoon  = 4.9048695e12;
    constexpr double kRSun    = 6.957e8;
    constexpr double kREarth  = 6.371e6;
    constexpr double kRMars   = 3.3895e6;
    constexpr double kRMoon   = 1.7374e6;
    constexpr double kAU      = 1.495978707e11;

    StarSystem sys;

    // Central star: the sole N-body body, invMass 0 so it holds the origin. Its
    // orbit is unused (no primary). Unbounded SOI so interplanetary space is its.
    SystemBody star;
    star.bodyId    = 1;
    star.owner     = ecs::OrbitOwner::NBodyActive;
    star.mu        = kMuSun;
    star.radius    = kRSun;
    star.invMass   = 0.0;
    star.soiRadius = std::numeric_limits<double>::infinity();
    star.isSource  = true;
    star.hasOrbit  = false;
    sys.bodies.push_back(star);

    // Inner planet at 1 AU, small inclination.
    sys.bodies.push_back(RailsBody(/*id*/ 10, /*primary*/ 1, kMuSun, kMuEarth,
                                   kREarth, 1.0 * kAU, /*inc*/ 0.0,
                                   /*node*/ 0.0, /*phase*/ 0.0));

    // Outer planet at 1.52 AU, a few degrees inclined so the system is not planar.
    sys.bodies.push_back(RailsBody(/*id*/ 20, /*primary*/ 1, kMuSun, kMuMars,
                                   kRMars, 1.52 * kAU, /*inc*/ 0.0323 /*~1.85°*/,
                                   /*node*/ 0.9, /*phase*/ 2.0));

    // Moon on rails about the inner planet (parent-before-child: planet 10 above).
    sys.bodies.push_back(RailsBody(/*id*/ 11, /*primary*/ 10, kMuEarth, kMuMoon,
                                   kRMoon, 3.844e8, /*inc*/ 0.089 /*~5.1°*/,
                                   /*node*/ 0.3, /*phase*/ 1.0));

    return sys;
}

} // namespace sim
