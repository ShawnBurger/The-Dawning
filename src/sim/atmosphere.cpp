#include "atmosphere.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace sim
{

namespace
{

constexpr double kMaxFinite = std::numeric_limits<double>::max();

bool IsFiniteVector(const Vec3d& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double SaturatingProduct(std::initializer_list<double> factors)
{
    bool negative = false;
    for (double factor : factors)
    {
        if (!std::isfinite(factor))
            return 0.0;
        if (factor == 0.0)
            return 0.0;
        negative ^= factor < 0.0;
    }

    double result = 1.0;
    for (double factor : factors)
    {
        const double magnitude = std::abs(factor);
        if (result > kMaxFinite / magnitude)
            return negative ? -kMaxFinite : kMaxFinite;
        result *= magnitude;
    }
    return negative ? -result : result;
}

double SaturatingAdd(double a, double b)
{
    if (!std::isfinite(a) || !std::isfinite(b))
        return 0.0;
    if (b > 0.0 && a > kMaxFinite - b)
        return kMaxFinite;
    if (b < 0.0 && a < -kMaxFinite - b)
        return -kMaxFinite;
    return a + b;
}

struct MeasuredVector
{
    double length = 0.0;
    Vec3d unit{};
    bool finite = false;
};

MeasuredVector MeasureVector(const Vec3d& v)
{
    MeasuredVector measured;
    if (!IsFiniteVector(v))
        return measured;

    const double scale = std::max({ std::abs(v.x), std::abs(v.y), std::abs(v.z) });
    measured.finite = true;
    if (scale == 0.0)
        return measured;

    const Vec3d scaled{ v.x / scale, v.y / scale, v.z / scale };
    const double scaledLength = std::sqrt(scaled.x * scaled.x +
                                          scaled.y * scaled.y +
                                          scaled.z * scaled.z);
    measured.length = SaturatingProduct({ scale, scaledLength });
    measured.unit = scaled * (1.0 / scaledLength);
    return measured;
}

Vec3d ScaleFinite(const Vec3d& v, double scale)
{
    if (!IsFiniteVector(v) || !std::isfinite(scale))
        return Vec3d{};
    return Vec3d{
        SaturatingProduct({ v.x, scale }),
        SaturatingProduct({ v.y, scale }),
        SaturatingProduct({ v.z, scale }),
    };
}

Vec3d CrossFinite(const Vec3d& a, const Vec3d& b)
{
    if (!IsFiniteVector(a) || !IsFiniteVector(b))
        return Vec3d{};

    const double scaleA = std::max({ std::abs(a.x), std::abs(a.y), std::abs(a.z) });
    const double scaleB = std::max({ std::abs(b.x), std::abs(b.y), std::abs(b.z) });
    if (scaleA == 0.0 || scaleB == 0.0)
        return Vec3d{};

    const Vec3d an{ a.x / scaleA, a.y / scaleA, a.z / scaleA };
    const Vec3d bn{ b.x / scaleB, b.y / scaleB, b.z / scaleB };
    const Vec3d cross = an.Cross(bn);
    return Vec3d{
        SaturatingProduct({ scaleA, scaleB, cross.x }),
        SaturatingProduct({ scaleA, scaleB, cross.y }),
        SaturatingProduct({ scaleA, scaleB, cross.z }),
    };
}

double PositiveValueFromLog(double logarithm)
{
    if (!std::isfinite(logarithm))
        return logarithm > 0.0 ? kMaxFinite : 0.0;
    const double maxLog = std::log(kMaxFinite);
    const double minLog = std::log(std::numeric_limits<double>::denorm_min());
    if (logarithm >= maxLog)
        return kMaxFinite;
    if (logarithm <= minLog)
        return 0.0;
    return std::exp(logarithm);
}

double SmoothCeilingTaper(double h, double ceiling, double fadeWidth)
{
    if (!std::isfinite(h) || !std::isfinite(ceiling) ||
        !std::isfinite(fadeWidth) || ceiling <= 0.0 || fadeWidth <= 0.0 ||
        h >= ceiling)
    {
        return 0.0;
    }

    const double width = std::min(fadeWidth, ceiling);
    const double fadeStart = ceiling - width;
    if (h <= fadeStart)
        return 1.0;

    const double x = std::clamp((ceiling - h) / width, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

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

AtmosphereState SampleUSSA76(double h, double ceiling, double fadeWidth)
{
    AtmosphereState s;
    if (!std::isfinite(h) || !std::isfinite(ceiling) ||
        !std::isfinite(fadeWidth) || ceiling <= 0.0 || fadeWidth <= 0.0)
    {
        return s;
    }

    const double effectiveCeiling = std::min(ceiling, kUSSATop);
    if (h >= effectiveCeiling)
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
    const double taper = SmoothCeilingTaper(h, effectiveCeiling, fadeWidth);
    s.pressure *= taper;
    s.density *= taper;
    if (s.density <= 0.0 || !std::isfinite(s.density) || !std::isfinite(s.pressure))
        return AtmosphereState{};
    return s;
}

} // namespace

AtmosphereModel AtmosphereModel::EarthUSSA76()
{
    AtmosphereModel m;
    m.kind = AtmosphereKind::USSA76;
    m.ceiling = kUSSATop;
    m.ceilingFadeWidth = 5000.0;
    return m;
}

AtmosphereModel AtmosphereModel::ExponentialBody(double rho0, double H, double ceiling)
{
    if (!std::isfinite(rho0) || !std::isfinite(H) || !std::isfinite(ceiling) ||
        rho0 <= 0.0 || H <= 0.0 || ceiling <= 0.0)
    {
        return Vacuum();
    }

    AtmosphereModel m;
    m.kind = AtmosphereKind::Exponential;
    m.seaLevelDensity = rho0;
    m.scaleHeight = H;
    m.ceiling = ceiling;
    m.ceilingFadeWidth = std::min(H, ceiling);
    return m;
}

double GeometricToGeopotential(double z)
{
    if (!std::isfinite(z) || z <= -kEarthGeopotentialR)
        return 0.0;
    const double ratio = z / (kEarthGeopotentialR + z);
    if (!std::isfinite(ratio))
        return z < 0.0 ? -kMaxFinite : kEarthGeopotentialR;
    return SaturatingProduct({ kEarthGeopotentialR, ratio });
}

AtmosphereState SampleAtmosphere(const AtmosphereModel& model, double h)
{
    switch (model.kind)
    {
    case AtmosphereKind::None:
        return AtmosphereState{};
    case AtmosphereKind::USSA76:
        return SampleUSSA76(h, model.ceiling, model.ceilingFadeWidth);
    case AtmosphereKind::Exponential:
    {
        AtmosphereState s;
        if (!std::isfinite(h) || !std::isfinite(model.seaLevelDensity) ||
            !std::isfinite(model.scaleHeight) || !std::isfinite(model.gasConstant) ||
            !std::isfinite(model.gamma) || !std::isfinite(model.ceiling) ||
            !std::isfinite(model.ceilingFadeWidth) || model.seaLevelDensity <= 0.0 ||
            model.scaleHeight <= 0.0 || model.gasConstant <= 0.0 || model.gamma <= 0.0 ||
            model.ceiling <= 0.0 || model.ceilingFadeWidth <= 0.0 || h >= model.ceiling)
        {
            return s; // exactly 0 above the ceiling
        }
        const double hh = h < 0.0 ? 0.0 : h;
        // An isothermal exponential has no temperature profile of its own; use the
        // sea-level temperature so speed of sound is defined. Density is the model.
        s.temperature  = kSeaLevelTemperature;
        const double taper = SmoothCeilingTaper(hh, model.ceiling, model.ceilingFadeWidth);
        s.density = SaturatingProduct({ model.seaLevelDensity,
                                        std::exp(-hh / model.scaleHeight), taper });
        if (s.density <= 0.0)
            return AtmosphereState{};
        s.pressure = SaturatingProduct({ s.density, model.gasConstant, s.temperature });
        const double soundSquared = SaturatingProduct({ model.gamma, model.gasConstant,
                                                        s.temperature });
        s.speedOfSound = std::sqrt(soundSquared);
        return s;
    }
    }
    return AtmosphereState{};
}

Vec3d AirspeedVector(const Vec3d& bodyVelocity, const Vec3d& atmosphereVelocity)
{
    if (!IsFiniteVector(bodyVelocity) || !IsFiniteVector(atmosphereVelocity))
        return Vec3d{};
    return Vec3d{
        SaturatingAdd(bodyVelocity.x, -atmosphereVelocity.x),
        SaturatingAdd(bodyVelocity.y, -atmosphereVelocity.y),
        SaturatingAdd(bodyVelocity.z, -atmosphereVelocity.z),
    };
}

double DynamicPressure(double density, const Vec3d& airspeed)
{
    const MeasuredVector measured = MeasureVector(airspeed);
    if (!measured.finite || !std::isfinite(density) || density <= 0.0 ||
        measured.length <= 0.0)
    {
        return 0.0;
    }
    return SaturatingProduct({ 0.5, density, measured.length, measured.length });
}

double MachNumber(const Vec3d& airspeed, double speedOfSound)
{
    const MeasuredVector measured = MeasureVector(airspeed);
    if (!measured.finite || !std::isfinite(speedOfSound) || speedOfSound <= 0.0)
        return 0.0;
    if (measured.length > kMaxFinite * speedOfSound)
        return kMaxFinite;
    return measured.length / speedOfSound;
}

double DragCoefficientAtMach(double cd0, double mach)
{
    if (!std::isfinite(cd0) || !std::isfinite(mach) || cd0 <= 0.0)
        return 0.0;
    // A smooth, monotone-free Cd(M): flat subsonic, a transonic bump peaking at
    // M=1, decaying to a hypersonic plateau. Coefficients chosen for the standard
    // qualitative shape, not a specific body.
    const double M = mach < 0.0 ? 0.0 : mach;
    // Transonic wave-drag bump: a Gaussian centred on M=1, so the peak is exactly
    // at M=1 and it is C-infinity (no force discontinuity).
    const double bump = 1.0 * std::exp(-((M - 1.0) * (M - 1.0)) / (2.0 * 0.15 * 0.15));
    // Supersonic falloff: `plateau` rises to 0.4*cd0 as M->inf, so Cd asymptotes to
    // cd0*(1 - 0.4) = ~0.6*cd0 (the hypersonic plateau); it is ~0.9*cd0 just past
    // the transonic bump near M=1.5.
    const double plateau = (M <= 1.0) ? 0.0 : 0.4 * (1.0 - std::exp(-(M - 1.0) / 2.0));
    return cd0 * (1.0 + bump) - cd0 * plateau;
}

double AngleOfAttackDragFactor(double alpha, double crossflowK)
{
    if (!std::isfinite(alpha) || !std::isfinite(crossflowK) || crossflowK < 0.0)
        return 1.0;
    const double s = std::sin(alpha);
    return SaturatingAdd(1.0, SaturatingProduct({ crossflowK, s, s }));
}

double LiftCoefficient(double clAlpha, double alpha, double stall)
{
    if (!std::isfinite(clAlpha) || !std::isfinite(alpha) || !std::isfinite(stall) ||
        clAlpha <= 0.0 || stall <= 0.0)
    {
        return 0.0;
    }
    const double a = std::abs(alpha);
    if (a <= stall)
        return SaturatingProduct({ clAlpha, alpha });
    // Past stall, lift decays back toward zero over the next stall-width of AoA.
    const double excess = a - stall;
    const double decay = std::max(0.0, 1.0 - excess / stall);
    const double peak = SaturatingProduct({ clAlpha, stall });
    return SaturatingProduct({ alpha >= 0.0 ? 1.0 : -1.0, peak, decay });
}

Vec3d DragForce(double q, double cd, double area, const Vec3d& airspeed)
{
    const MeasuredVector measured = MeasureVector(airspeed);
    if (!measured.finite || measured.length <= 0.0 || !std::isfinite(q) ||
        !std::isfinite(cd) || !std::isfinite(area) || q <= 0.0 || cd <= 0.0 ||
        area <= 0.0)
    {
        return Vec3d{ 0.0, 0.0, 0.0 };  // zero airspeed -> exactly zero drag
    }
    const double magnitude = SaturatingProduct({ q, cd, area });
    return ScaleFinite(measured.unit, -magnitude);
}

Vec3d LiftForce(double q, double cl, double area, const Vec3d& liftUnitDir)
{
    const MeasuredVector measured = MeasureVector(liftUnitDir);
    if (!measured.finite || measured.length <= 0.0 || !std::isfinite(q) ||
        !std::isfinite(cl) || !std::isfinite(area) || q <= 0.0 || area <= 0.0)
    {
        return Vec3d{};
    }
    const double magnitude = SaturatingProduct({ q, cl, area });
    return ScaleFinite(measured.unit, magnitude);
}

Vec3d AeroTorqueAboutCoM(const Vec3d& copOffsetFromCoM, const Vec3d& aeroForce)
{
    return CrossFinite(copOffsetFromCoM, aeroForce);
}

double DragTimeConstant(double mass, double density, double cd, double area, const Vec3d& airspeed)
{
    const MeasuredVector measured = MeasureVector(airspeed);
    if (!measured.finite || !std::isfinite(mass) || !std::isfinite(density) ||
        !std::isfinite(cd) || !std::isfinite(area) || mass <= 0.0 || density <= 0.0 ||
        cd <= 0.0 || area <= 0.0 || measured.length <= 0.0)
    {
        return std::numeric_limits<double>::infinity(); // no drag -> infinite time constant
    }
    const double logTau = std::log(2.0) + std::log(mass) - std::log(density) -
                          std::log(cd) - std::log(area) - std::log(measured.length);
    return PositiveValueFromLog(logTau);
}

double BallisticCoefficient(double mass, double cd, double area)
{
    if (!std::isfinite(mass) || !std::isfinite(cd) || !std::isfinite(area) ||
        mass <= 0.0 || cd <= 0.0 || area <= 0.0)
    {
        return std::numeric_limits<double>::infinity();
    }
    return PositiveValueFromLog(std::log(mass) - std::log(cd) - std::log(area));
}

Vec3d SemiImplicitDragAirspeed(const Vec3d& airspeed, double density, double cd,
                               double area, double mass, double dt)
{
    const MeasuredVector measured = MeasureVector(airspeed);
    if (!measured.finite)
        return Vec3d{};
    if (measured.length <= 0.0 || !std::isfinite(density) || !std::isfinite(cd) ||
        !std::isfinite(area) || !std::isfinite(mass) || !std::isfinite(dt) ||
        density <= 0.0 || cd <= 0.0 || area <= 0.0 || mass <= 0.0 || dt <= 0.0)
    {
        return airspeed;
    }
    // Freeze the quadratic coefficient at |v_n|, then solve that linearised drag
    // term implicitly. The denominator is >= 1, so the update is contractive.
    const double logResponse = std::log(0.5) + std::log(density) + std::log(cd) +
                               std::log(area) + std::log(measured.length) +
                               std::log(dt) - std::log(mass);
    const double response = PositiveValueFromLog(logResponse);
    const double factor = 1.0 / (1.0 + response);
    return ScaleFinite(airspeed, factor);
}

Vec3d ExplicitDragAirspeed(const Vec3d& airspeed, double density, double cd,
                           double area, double mass, double dt)
{
    // NEGATIVE CONTROL ONLY. v_{n+1} = v_n - (rho*Cd*A/(2m))*|v_n|*v_n*dt.
    const MeasuredVector measured = MeasureVector(airspeed);
    if (!measured.finite)
        return Vec3d{};
    if (measured.length <= 0.0 || !std::isfinite(density) || !std::isfinite(cd) ||
        !std::isfinite(area) || !std::isfinite(mass) || !std::isfinite(dt) ||
        density <= 0.0 || cd <= 0.0 || area <= 0.0 || mass <= 0.0 || dt <= 0.0)
    {
        return airspeed;
    }
    const double logResponse = std::log(0.5) + std::log(density) + std::log(cd) +
                               std::log(area) + std::log(measured.length) +
                               std::log(dt) - std::log(mass);
    const double response = PositiveValueFromLog(logResponse);
    return ScaleFinite(airspeed, 1.0 - response);
}

double TerminalVelocity(double mass, double gravity, double density, double cd, double area)
{
    if (!std::isfinite(mass) || !std::isfinite(gravity) || !std::isfinite(density) ||
        !std::isfinite(cd) || !std::isfinite(area) || mass <= 0.0 || gravity <= 0.0 ||
        density <= 0.0 || cd <= 0.0 || area <= 0.0)
    {
        return 0.0;
    }
    const double logVelocity = 0.5 * (std::log(2.0) + std::log(mass) +
                                      std::log(gravity) - std::log(density) -
                                      std::log(cd) - std::log(area));
    return PositiveValueFromLog(logVelocity);
}

double SuttonGravesHeatFlux(double density, double noseRadius, const Vec3d& airspeed)
{
    const MeasuredVector measured = MeasureVector(airspeed);
    if (!measured.finite || !std::isfinite(density) || !std::isfinite(noseRadius) ||
        density <= 0.0 || noseRadius <= 0.0 || measured.length <= 0.0)
    {
        return 0.0;
    }
    const double logFlux = std::log(kSuttonGravesConstant) +
                           0.5 * (std::log(density) - std::log(noseRadius)) +
                           3.0 * std::log(measured.length);
    return PositiveValueFromLog(logFlux);
}

} // namespace sim
