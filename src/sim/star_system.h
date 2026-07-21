#pragma once
// =============================================================================
// sim/star_system.h — deterministic star-system BUILDER (Stage 11). Produces the
// physical + orbital definition of a whole system (a central star, planets on
// analytic rails, moons on rails about their planet) with EXACT two-body physics:
// circular orbital speeds, Kepler's third law across the planets, and Stage-7
// sphere-of-influence radii. This is the data the scene seeder instantiates into
// ECS entities + a frame graph to bring the sim's world to life.
//
// Pure CPU math + data; no ECS registry, no rendering. The physics is scale-free
// (Kepler holds at any consistent mu/radius), so a consumer may pick real or
// compact parameters — this builder ships one concrete reference system.
// =============================================================================

#include "../core/types.h"     // core::Vec3d
#include "../ecs/components.h" // ecs::OrbitOwner, ecs::OrbitState

#include <cstdint>
#include <vector>

namespace sim {

// One body's definition. Bodies are emitted PARENT-BEFORE-CHILD so a consumer can
// build frames/entities top-down and always resolve a primary before its orbiters.
struct SystemBody
{
    uint64_t        bodyId        = 0;
    uint64_t        primaryBodyId = 0;  // 0 for the central star (has no primary)
    ecs::OrbitOwner owner         = ecs::OrbitOwner::OnRails;
    double          mu            = 0.0;  // GM (m^3/s^2)
    double          radius        = 0.0;  // physical radius (m)
    double          invMass       = 0.0;  // 1/mass = G/mu (0 => immovable central body)
    double          soiRadius     = 0.0;  // sphere of influence (m); +inf for the star
    bool            isSource      = true; // contributes gravity
    bool            hasOrbit      = false;// true => `orbit` is valid (OnRails bodies)
    ecs::OrbitState orbit;                // osculating elements about primaryBodyId
};

struct StarSystem
{
    std::vector<SystemBody> bodies; // parent-before-child order
};

// A circular OrbitState about a primary of parameter `primaryMu` at orbital radius
// `radius` (semi-major axis, e = 0), with the given inclination, ascending-node
// longitude, and phase (true anomaly at `epoch`). The circular speed sqrt(mu/r)
// and period 2π·sqrt(r^3/mu) follow from these by construction.
ecs::OrbitState CircularOrbit(double primaryMu, uint64_t primaryBodyId,
                              double radius, double inclination,
                              double longitudeAscNode, double phase, double epoch);

// The deterministic reference system: a Sun-like star (the sole N-body body, held
// at the origin by invMass = 0), two planets on analytic rails, and a moon on
// rails about the inner planet. Real gravitational parameters; circular orbits
// with a small mutual inclination; SOI radii from Stage 7.
StarSystem BuildReferenceSystem();

} // namespace sim
