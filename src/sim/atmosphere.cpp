#include "atmosphere.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace sim
{

namespace
{

// USSA76 base layers below 86 km (geopotential altitude, base temperature, lapse
// rate), verbatim from the standard (ATMOSPHERIC_FLIGHT.md sec 1.2).
struct Layer { double hb; double Tb; double L; };
constexpr int kLayerCount = 7;
constexpr std::array<Layer, kLayerCount> kLayers = {{
    {     0.0, 288.15, -0.0065 },
    { 11000.0, 216.65,  0.0    },
    { 20000.0, 216.65,  0.0010 },
    { 32000.0, 228.65,  0.0028 },
    { 47000.0, 270.65,  0.0    },
    { 51000.0, 270.65, -0.0028 },
    { 71000.0, 214.65, -0.0020 },
}};
constexpr double kUSSATop = 84852.0; // geopotential altitude of 86 km geometric

// Base pressure at each layer boundary, computed ONCE by integrating the same
// barometric formula layer by layer from P0. Computing rather than hardcoding
// makes the piecewise profile C0-continuous by construction: the base pressure of
// layer n+1 is exactly layer n's formula evaluated at its top.
const std::array<double, kLayerCount>& BasePressures()
{
    static const std::array<double, kLayerCount> pb = [] {
        std::array<double, kLayerCount> p{};
        p[0] = kSeaLevelPressure;
        for (int n = 0; n + 1 < kLayerCount; ++n)
        {
            const Layer& lyr = kLayers[n];
            const double top = kLayers[n + 1].hb;
            const double Ttop = lyr.Tb + lyr.L * (top - lyr.hb);
            if (lyr.L != 0.0)
            {
                const double exponent = -kEarthG0 / (kAirGasConstant * lyr.L);
                p[n + 1] = p[n] * std::pow(Ttop / lyr.Tb, exponent);
            }
            else
            {
                p[n + 1] = p[n] * std::exp(-kEarthG0 * (top - lyr.hb) /
                                           (kAirGasConstant * lyr.Tb));
            }
        }
        return p;
    }();
    return pb;
}

AtmosphereState SampleUSSA76(double h, double ceiling)
{
    AtmosphereState s;
    if (h >= ceiling || h >= kUSSATop)
        return s; // density 0 above the modeled ceiling
    if (h < 0.0)
        h = 0.0;  // clamp to sea level below the datum

    // Find the layer containing h (layers are few; a linear scan is fine).
    int n = 0;
    for (int i = 0; i + 1 < kLayerCount; ++i)
        if (h >= kLayers[i + 1].hb) n = i + 1;

    const Layer& lyr = kLayers[n];
    const double Pb = BasePressures()[n];
    const double T = lyr.Tb + lyr.L * (h - lyr.hb);
    double P;
    if (lyr.L != 0.0)
    {
        const double exponent = -kEarthG0 / (kAirGasConstant * lyr.L);
        P = Pb * std::pow(T / lyr.Tb, exponent);
    }
    else
    {
        P = Pb * std::exp(-kEarthG0 * (h - lyr.hb) / (kAirGasConstant * lyr.Tb));
    }
    s.temperature  = T;
    s.pressure     = P;
    s.density      = P / (kAirGasConstant * T);
    s.speedOfSound = std::sqrt(kAirGamma * kAirGasConstant * T);
    return s;
}

} // namespace

AtmosphereModel AtmosphereModel::EarthUSSA76()
{
    AtmosphereModel m;
    m.kind = AtmosphereKind::USSA76;
    m.ceiling = kUSSATop;
    return m;
}

AtmosphereModel AtmosphereModel::ExponentialBody(double rho0, double H, double ceiling)
{
    AtmosphereModel m;
    m.kind = AtmosphereKind::Exponential;
    m.seaLevelDensity = rho0;
    m.scaleHeight = H;
    m.ceiling = ceiling;
    return m;
}

double GeometricToGeopotential(double z)
{
    return kEarthGeopotentialR * z / (kEarthGeopotentialR + z);
}

AtmosphereState SampleAtmosphere(const AtmosphereModel& model, double h)
{
    switch (model.kind)
    {
    case AtmosphereKind::None:
        return AtmosphereState{};
    case AtmosphereKind::USSA76:
        return SampleUSSA76(h, model.ceiling);
    case AtmosphereKind::Exponential:
    {
        AtmosphereState s;
        if (h >= model.ceiling)
            return s; // exactly 0 above the ceiling
        const double hh = h < 0.0 ? 0.0 : h;
        // An isothermal exponential has no temperature profile of its own; use the
        // sea-level temperature so speed of sound is defined. Density is the model.
        s.temperature  = kSeaLevelTemperature;
        s.density      = model.seaLevelDensity * std::exp(-hh / model.scaleHeight);
        s.pressure     = s.density * model.gasConstant * s.temperature;
        s.speedOfSound = std::sqrt(model.gamma * model.gasConstant * s.temperature);
        return s;
    }
    }
    return AtmosphereState{};
}

Vec3d AirspeedVector(const Vec3d& bodyVelocity, const Vec3d& atmosphereVelocity)
{
    return bodyVelocity - atmosphereVelocity;
}

double DynamicPressure(double density, const Vec3d& airspeed)
{
    return 0.5 * density * airspeed.LengthSq();
}

double MachNumber(const Vec3d& airspeed, double speedOfSound)
{
    if (speedOfSound <= 0.0)
        return 0.0;
    return airspeed.Length() / speedOfSound;
}

double DragCoefficientAtMach(double cd0, double mach)
{
    // A smooth, monotone-free Cd(M): flat subsonic, a transonic bump peaking at
    // M=1, decaying to a hypersonic plateau. Coefficients chosen for the standard
    // qualitative shape, not a specific body.
    const double M = mach < 0.0 ? 0.0 : mach;
    // Transonic wave-drag bump: a Gaussian centred on M=1, so the peak is exactly
    // at M=1 and it is C-infinity (no force discontinuity).
    const double bump = 1.0 * std::exp(-((M - 1.0) * (M - 1.0)) / (2.0 * 0.15 * 0.15));
    // Supersonic falloff toward a hypersonic plateau at ~0.9*cd0 above the bump.
    const double plateau = (M <= 1.0) ? 0.0 : 0.4 * (1.0 - std::exp(-(M - 1.0) / 2.0));
    return cd0 * (1.0 + bump) - cd0 * plateau;
}

double AngleOfAttackDragFactor(double alpha, double crossflowK)
{
    const double s = std::sin(alpha);
    return 1.0 + crossflowK * s * s;
}

double LiftCoefficient(double clAlpha, double alpha, double stall)
{
    const double a = std::abs(alpha);
    if (a <= stall)
        return clAlpha * alpha;
    // Past stall, lift decays back toward zero over the next stall-width of AoA.
    const double excess = a - stall;
    const double decay = std::max(0.0, 1.0 - excess / stall);
    const double peak = clAlpha * stall;
    return (alpha >= 0.0 ? 1.0 : -1.0) * peak * decay;
}

Vec3d DragForce(double q, double cd, double area, const Vec3d& airspeed)
{
    const double speed = airspeed.Length();
    if (speed <= 0.0 || q <= 0.0)
        return Vec3d{ 0.0, 0.0, 0.0 };  // zero airspeed -> exactly zero drag
    const Vec3d vhat = airspeed * (1.0 / speed);
    return vhat * (-(q * cd * area));
}

Vec3d LiftForce(double q, double cl, double area, const Vec3d& liftUnitDir)
{
    return liftUnitDir * (q * cl * area);
}

Vec3d AeroTorqueAboutCoM(const Vec3d& copOffsetFromCoM, const Vec3d& aeroForce)
{
    return copOffsetFromCoM.Cross(aeroForce);
}

double DragTimeConstant(double mass, double density, double cd, double area, const Vec3d& airspeed)
{
    const double speed = airspeed.Length();
    const double denom = density * cd * area * speed;
    if (denom <= 0.0)
        return std::numeric_limits<double>::infinity(); // no drag -> infinite time constant
    return 2.0 * mass / denom;
}

double BallisticCoefficient(double mass, double cd, double area)
{
    const double denom = cd * area;
    if (denom <= 0.0)
        return std::numeric_limits<double>::infinity();
    return mass / denom;
}

Vec3d SemiImplicitDragAirspeed(const Vec3d& airspeed, double density, double cd,
                               double area, double mass, double dt)
{
    const double speed = airspeed.Length();
    if (speed <= 0.0 || density <= 0.0 || mass <= 0.0 || dt <= 0.0)
        return airspeed;
    // Backward-Euler on the quadratic law: v_{n+1} = v_n / (1 + c*|v_n|*dt).
    // c = rho*Cd*A/(2m). The denominator is > 1, so this is strictly contractive
    // for any dt - it can never overshoot zero or gain speed.
    const double c = density * cd * area / (2.0 * mass);
    const double factor = 1.0 / (1.0 + c * speed * dt);
    return airspeed * factor;
}

Vec3d ExplicitDragAirspeed(const Vec3d& airspeed, double density, double cd,
                           double area, double mass, double dt)
{
    // NEGATIVE CONTROL ONLY. v_{n+1} = v_n - (rho*Cd*A/(2m))*|v_n|*v_n*dt.
    const double speed = airspeed.Length();
    if (speed <= 0.0 || density <= 0.0 || mass <= 0.0 || dt <= 0.0)
        return airspeed;
    const double c = density * cd * area / (2.0 * mass);
    return airspeed - airspeed * (c * speed * dt);
}

double TerminalVelocity(double mass, double gravity, double density, double cd, double area)
{
    const double denom = density * cd * area;
    if (denom <= 0.0 || gravity <= 0.0 || mass <= 0.0)
        return 0.0;
    return std::sqrt(2.0 * mass * gravity / denom);
}

double SuttonGravesHeatFlux(double density, double noseRadius, const Vec3d& airspeed)
{
    if (density <= 0.0 || noseRadius <= 0.0)
        return 0.0;
    const double v = airspeed.Length();
    return kSuttonGravesConstant * std::sqrt(density / noseRadius) * v * v * v;
}

} // namespace sim
