#pragma once
// =============================================================================
// sim/atmosphere.h - atmospheric flight: density models and aerodynamic forces
// =============================================================================
// Sim Stage 3, per docs/research/ATMOSPHERIC_FLIGHT.md. CPU-only, GPU-free.
//
// Atmosphere is NOT a separate simulation regime. It is ONE external force term,
// summed into the rigid body's force accumulator by the (ship-lane) call site,
// that vanishes to EXACTLY zero above the atmosphere because rho -> 0. The only
// transition is the force-integration seam every perturbed body already crosses;
// a separate regime would reintroduce the hard-boundary force discontinuity the
// gravity-softening rule forbids (ATMOSPHERIC_FLIGHT.md sec 7).
//
// The one robustness crux is drag STIFFNESS (sec 6.1): quadratic drag done with
// an explicit Euler update injects energy once dt exceeds the drag time constant
// tau = 2m/(rho*Cd*A*|v|) - the same failure class as the explicit gyroscopic
// term Stage 1 hit. This module provides a frozen-speed semi-implicit update that
// is contractive for any dt, and the shipped path uses it. The naive explicit form
// is only kept as the negative control in the tests.

#include "../core/types.h"

#include <cstdint>

namespace sim
{

using core::Vec3d;

// --- Physical constants (SI) -------------------------------------------------
inline constexpr double kAirGamma            = 1.4;          // ratio of specific heats, diatomic air
inline constexpr double kAirGasConstant      = 287.05287;    // specific gas constant of air, J/(kg K)
inline constexpr double kEarthG0             = 9.80665;      // standard gravity, m/s^2
inline constexpr double kEarthGeopotentialR  = 6356766.0;    // Earth geopotential radius, m (USSA76)
inline constexpr double kSeaLevelDensity     = 1.225;        // USSA76 sea-level density, kg/m^3
inline constexpr double kSeaLevelPressure    = 101325.0;     // USSA76 sea-level pressure, Pa
inline constexpr double kSeaLevelTemperature = 288.15;       // USSA76 sea-level temperature, K
// Sutton-Graves stagnation convective-heating coefficient (SI). The velocity-cubed
// form q = k*sqrt(rho/Rn)*v^3 is the DERIVED engineering correlation, NOT a verbatim
// NASA TR R-376 quantity (that primary gives a heat-transfer coefficient K, not this
// number). See ATMOSPHERIC_FLIGHT.md sec 4.1. Heating is COSMETIC - it never feeds
// the integrator.
inline constexpr double kSuttonGravesConstant = 1.7415e-4;

// --- Atmosphere state at an altitude ----------------------------------------
struct AtmosphereState
{
    double temperature  = 0.0;  // K
    double pressure     = 0.0;  // Pa
    double density      = 0.0;  // kg/m^3   (exactly 0 above the ceiling)
    double speedOfSound = 0.0;  // m/s      (0 where density is 0)
};

enum class AtmosphereKind : uint32_t
{
    None = 0,      // vacuum - always returns zero density
    Exponential,   // rho0 * exp(-h/H): the honest model for a body without a profile
    USSA76,        // the layered US Standard Atmosphere 1976 (Earth-like)
};

// A body's atmosphere. USSA76 uses the fixed Earth layer table; Exponential is
// parameterised by (seaLevelDensity, scaleHeight). Both share the ceiling above
// which density is exactly 0. A smooth terminal fade makes density and pressure
// approach that exact zero continuously instead of applying a hard force cutoff.
struct AtmosphereModel
{
    AtmosphereKind kind = AtmosphereKind::None;
    double seaLevelDensity = kSeaLevelDensity; // Exponential only
    double scaleHeight     = 8500.0;           // Exponential only, m
    double gasConstant     = kAirGasConstant;  // for speed of sound
    double gamma           = kAirGamma;
    double ceiling         = 86000.0;          // geopotential m; density 0 above
    double ceilingFadeWidth = 5000.0;          // final smoothstep interval, m

    static AtmosphereModel Vacuum() { return AtmosphereModel{}; }
    static AtmosphereModel EarthUSSA76();
    static AtmosphereModel ExponentialBody(double rho0, double H, double ceiling);
};

// Geometric (true) altitude z -> geopotential altitude h (USSA76 convention,
// which folds the 1/r^2 weakening of gravity into a constant-g0 formulation).
double GeometricToGeopotential(double geometricAltitude);

// Sample {T, P, rho, a} at a GEOPOTENTIAL altitude. Density is EXACTLY 0.0 at and
// above model.ceiling (and below the deepest defined point clamps to the base).
AtmosphereState SampleAtmosphere(const AtmosphereModel& model, double geopotentialAltitude);

// --- Aerodynamics ------------------------------------------------------------

// Airspeed vector: body velocity minus the co-rotating atmosphere velocity.
Vec3d AirspeedVector(const Vec3d& bodyVelocity, const Vec3d& atmosphereVelocity);

// Dynamic pressure q = 0.5 * rho * |v_rel|^2.
double DynamicPressure(double density, const Vec3d& airspeed);

// Mach number M = |v_rel| / a.
double MachNumber(const Vec3d& airspeed, double speedOfSound);

// Drag coefficient vs Mach: subsonic-flat, transonic rise peaking at M=1,
// supersonic falloff, hypersonic continuum plateau. cd0 is the subsonic value on
// the body's reference area; the returned Cd multiplies q*A. A smooth model, not a
// lookup table, so it has no discontinuities to inject force steps.
double DragCoefficientAtMach(double cd0, double mach);

// Angle-of-attack drag multiplier: Cd(alpha) ~ Cd0 + k*sin^2(alpha) (crossflow).
double AngleOfAttackDragFactor(double alphaRadians, double crossflowK);

// Thin-airfoil lift: Cl = clAlpha * alpha, linear to the stall angle, then decays.
double LiftCoefficient(double clAlpha, double alphaRadians, double stallRadians);

// Drag force = -q * Cd * A * v_hat_rel (opposes airspeed). Zero for zero airspeed.
Vec3d DragForce(double dynamicPressure, double cd, double area, const Vec3d& airspeed);

// Lift force = q * Cl * A * liftDir (perpendicular to airspeed; caller supplies a
// unit liftDir in the plane of airspeed and the body lift axis).
Vec3d LiftForce(double dynamicPressure, double cl, double area, const Vec3d& liftUnitDir);

// Torque about the center of mass from an aerodynamic force applied at the center
// of pressure: tau = (r_cop - r_com) x F. A CoP AFT of the CoM (along -v_rel)
// yields a RESTORING moment, i.e. static stability (sec 2.4).
Vec3d AeroTorqueAboutCoM(const Vec3d& copOffsetFromCoM, const Vec3d& aeroForce);

// --- The stiff-drag fix: unconditionally stable velocity update --------------
// tau_drag = 2m / (rho*Cd*A*|v|). Explicit Euler is stable only for dt < tau.
double DragTimeConstant(double mass, double density, double cd, double area, const Vec3d& airspeed);

// Ballistic coefficient beta = m / (Cd*A).
double BallisticCoefficient(double mass, double cd, double area);

// FROZEN-SPEED SEMI-IMPLICIT quadratic-drag velocity update:
//   v_{n+1} = v_n / (1 + c*|v_rel_n|*dt),  c = rho*Cd*A/(2m).
// Contractive for any positive dt (0 < 1/(1+positive) < 1), so it can NEVER
// overshoot or gain energy. This freezes the quadratic coefficient at |v_n|; it
// is not the exact nonlinear backward-Euler root. Operates in the wind frame:
// pass the airspeed component; the co-rotating atmosphere velocity is added back
// by the caller. Returns the new airspeed vector.
Vec3d SemiImplicitDragAirspeed(const Vec3d& airspeed, double density, double cd,
                               double area, double mass, double dt);

// EXPLICIT-EULER quadratic-drag update - the NEGATIVE CONTROL only. Gains energy
// once dt > ~tau_drag. Do NOT ship this path.
Vec3d ExplicitDragAirspeed(const Vec3d& airspeed, double density, double cd,
                           double area, double mass, double dt);

// Terminal velocity under constant gravity g balanced by drag:
//   v_t = sqrt(2 m g / (rho * Cd * A)).
double TerminalVelocity(double mass, double gravity, double density, double cd, double area);

// --- Reentry heating (COSMETIC - firewalled from the integrator) -------------
// Sutton-Graves stagnation convective heat flux, W/m^2. For display/gameplay only;
// it MUST NOT be summed into any force or energy. A test asserts the trajectory is
// identical with and without calling this.
double SuttonGravesHeatFlux(double density, double noseRadius, const Vec3d& airspeed);

} // namespace sim
