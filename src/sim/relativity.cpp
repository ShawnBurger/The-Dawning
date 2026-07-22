// =============================================================================
// sim/relativity.cpp — momentum-space relativistic dynamics + time dilation
// =============================================================================
// See relativity.h for the module contract, the structural |v|<c argument, the
// deviation-accumulation discipline, the shared-softening reuse, and the §2.3
// master-frame subtlety with its honest caveats.
// =============================================================================

#include "relativity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim
{

namespace
{
bool IsFinite(const Vec3d& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

double MaxAbsComponent(const Vec3d& value)
{
    return (std::max)((std::max)(std::fabs(value.x), std::fabs(value.y)),
                      std::fabs(value.z));
}

double StableLength(const Vec3d& value)
{
    return std::hypot(value.x, value.y, value.z);
}

struct MomentumNorm
{
    double scale = 1.0;
    double scaledLength = 1.0;
};

MomentumNorm BuildMomentumNorm(const Vec3d& momentum, double restMass)
{
    const Vec3d pOverC = momentum / kSpeedOfLight;
    MomentumNorm result;
    result.scale = (std::max)(restMass, MaxAbsComponent(pOverC));
    const double m = restMass / result.scale;
    const double x = pOverC.x / result.scale;
    const double y = pOverC.y / result.scale;
    const double z = pOverC.z / result.scale;
    result.scaledLength = std::sqrt(m * m + x * x + y * y + z * z);
    return result;
}

double SaturatingProduct(double lhs, double rhs)
{
    if (lhs == 0.0 || rhs == 0.0)
        return 0.0;
    constexpr double maximum = (std::numeric_limits<double>::max)();
    if (lhs > maximum / rhs)
        return maximum;
    return lhs * rhs;
}

// β = |v|/c is a speed magnitude, so it is >= 0; clamp its magnitude to kBetaMax
// as defense-in-depth (§2.6). Dynamics never reaches this because momentum
// recovery keeps β < 1 structurally; it only guards a caller-supplied β.
double ClampBeta(double beta)
{
    if (!std::isfinite(beta))
        return 0.0;
    double b = std::fabs(beta);
    if (b > kBetaMax) b = kBetaMax;
    return b;
}
} // namespace

// =============================================================================
// 1. Momentum-space dynamics (§3)
// =============================================================================

double GammaFromMomentum(const Vec3d& momentum, double restMass)
{
    if (!(restMass > 0.0) || !std::isfinite(restMass) || !IsFinite(momentum))
        return 1.0; // house-guard limit: no rest mass => no boost
    const MomentumNorm norm = BuildMomentumNorm(momentum, restMass);
    return SaturatingProduct(norm.scale / restMass, norm.scaledLength);
}

Vec3d VelocityFromMomentum(const Vec3d& momentum, double restMass)
{
    if (!(restMass > 0.0) || !std::isfinite(restMass) || !IsFinite(momentum))
        return Vec3d{ 0.0, 0.0, 0.0 };
    const MomentumNorm norm = BuildMomentumNorm(momentum, restMass);
    Vec3d velocity{ (momentum.x / norm.scale) / norm.scaledLength,
                    (momentum.y / norm.scale) / norm.scaledLength,
                    (momentum.z / norm.scale) / norm.scaledLength };
    for (int correction = 0; correction < 2; ++correction)
    {
        const double speed = velocity.Length();
        if (speed <= kSpeedOfLight)
            break;
        const double representableLimit = std::nextafter(kSpeedOfLight, 0.0);
        velocity *= representableLimit / speed;
    }
    return velocity;
}

Vec3d MomentumFromVelocity(const Vec3d& velocity, double restMass)
{
    if (!(restMass > 0.0) || !std::isfinite(restMass) || !IsFinite(velocity))
        return Vec3d{ 0.0, 0.0, 0.0 };

    const double componentScale = MaxAbsComponent(velocity);
    if (componentScale == 0.0)
        return Vec3d{ 0.0, 0.0, 0.0 };
    const Vec3d scaled = velocity / componentScale;
    const double scaledLength = std::sqrt(scaled.LengthSq());
    const Vec3d direction = scaled / scaledLength;
    const double speed =
        (componentScale > (std::numeric_limits<double>::max)() / scaledLength)
            ? (std::numeric_limits<double>::infinity)()
            : componentScale * scaledLength;
    double beta = std::isfinite(speed)
                      ? (std::min)(speed / kSpeedOfLight, kBetaMax)
                      : kBetaMax;
    if (beta == kBetaMax)
        beta = std::nextafter(kBetaMax, 0.0);
    // γ from β for the SEEDING direction only (v→p). Not structurally bounded,
    // hence the kBetaMax defense-in-depth. 1−β² stays factored near c.
    const double gamma = 1.0 / std::sqrt(OneMinusBetaSq(beta));
    const double boundedSpeed = beta * kSpeedOfLight;
    const double magnitude = SaturatingProduct(
        SaturatingProduct(restMass, gamma), boundedSpeed);
    return direction * magnitude;
}

double RelativisticKineticEnergy(const Vec3d& momentum, double restMass)
{
    if (!(restMass > 0.0) || !std::isfinite(restMass) || !IsFinite(momentum))
        return 0.0;
    const double pMagnitude = StableLength(momentum);
    if (!std::isfinite(pMagnitude))
        return (std::numeric_limits<double>::max)();
    const MomentumNorm norm = BuildMomentumNorm(momentum, restMass);
    const double denominator = norm.scaledLength + restMass / norm.scale;
    const double velocityTerm = (pMagnitude / norm.scale) / denominator;
    return SaturatingProduct(pMagnitude, velocityTerm);
}

double RapidityFromMomentum(const Vec3d& momentum, double restMass)
{
    if (!(restMass > 0.0) || !std::isfinite(restMass) || !IsFinite(momentum))
        return 0.0;
    const double pOverC = StableLength(momentum / kSpeedOfLight);
    if (pOverC == 0.0)
        return 0.0;
    const double x = pOverC / restMass;
    if (std::isfinite(x))
        return std::asinh(x); // p = m c sinh φ
    return std::log(pOverC) - std::log(restMass) + std::log(2.0);
}

double SpeedFromRapidity(double rapidity)
{
    if (!std::isfinite(rapidity))
        return 0.0;
    return kSpeedOfLight * std::tanh(rapidity); // v = c tanh φ, |v| < c structurally
}

Vec3d RelativisticMomentumStep(const Vec3d& momentum, const Vec3d& force,
                               double restMass, double dt, Vec3d& velocityOut)
{
    // House guard (matches rigid_body.cpp / nbody.cpp): dt<=0 or non-finite is a
    // no-op — momentum unchanged, velocityOut the current recovered velocity.
    if (!IsFinite(momentum))
    {
        velocityOut = Vec3d{};
        return Vec3d{};
    }
    if (!(restMass > 0.0) || !std::isfinite(restMass) ||
        !(dt > 0.0) || !std::isfinite(dt) || !IsFinite(force))
    {
        velocityOut = VelocityFromMomentum(momentum, restMass);
        return momentum;
    }
    const Vec3d pNew = momentum + force * dt;      // dp = F·dt
    if (!IsFinite(pNew))
    {
        velocityOut = VelocityFromMomentum(momentum, restMass);
        return momentum;
    }
    velocityOut = VelocityFromMomentum(pNew, restMass); // v = p/sqrt(m²+(|p|/c)²)
    return pNew;
}

// =============================================================================
// 2. Time dilation (§2)
// =============================================================================

// ---- SR velocity dilation ----------------------------------------------------

double OneMinusBetaSq(double beta)
{
    // (1−β)(1+β), NEVER 1.0 − β*β: near β=1 the latter loses all significant
    // digits to cancellation. The factored form keeps them.
    return (1.0 - beta) * (1.0 + beta);
}

double SRDilationFactor(double beta)
{
    const double b = ClampBeta(beta);
    const double omb2 = OneMinusBetaSq(b);
    return std::sqrt(omb2 < 0.0 ? 0.0 : omb2);
}

double SRDilationFactorMinusOne(double beta)
{
    const double b = ClampBeta(beta);
    const double omb2 = OneMinusBetaSq(b);
    const double root = std::sqrt(omb2 < 0.0 ? 0.0 : omb2);
    // sqrt(1−β²) − 1 = −β² / (1 + sqrt(1−β²)). Cancellation-free: series −β²/2
    // near 0, clean −1 near c. 1 + (this) == SRDilationFactor exactly.
    return -(b * b) / (1.0 + root);
}

// ---- GR gravitational dilation ----------------------------------------------

double SchwarzschildRadius(double mu)
{
    if (!(mu >= 0.0) || !std::isfinite(mu))
        return 0.0;
    return (mu / kSpeedOfLight) * (2.0 / kSpeedOfLight); // r_s = 2GM/c²
}

double FlooredRadius(double r, double softening)
{
    const double floor = (softening > 0.0 && std::isfinite(softening))
                             ? softening
                             : kSofteningBase;
    return (r > floor) ? r : floor;
}

double GRDilationFactor(double mu, double r, double softening)
{
    if (!(mu >= 0.0) || !std::isfinite(mu) ||
        !(softening > 0.0) || !std::isfinite(softening))
        return 1.0;
    const double rs = SchwarzschildRadius(mu);
    const double rf = FlooredRadius(r, softening);
    const double ratio = (std::min)(rs / rf, 1.0);
    const double rad = 1.0 - ratio;
    // rad is strictly in (0,1] for the intended shared softening (rf ≥ softening
    // ≥ r_s + ε0 > r_s). The <0 guard keeps it real (horizon limit 0) rather than
    // NaN if a caller passes a softening below r_s — the horizon NaN designed out.
    return std::sqrt(rad < 0.0 ? 0.0 : rad);
}

double GRDilationFactorMinusOne(double mu, double r, double softening)
{
    if (!(mu >= 0.0) || !std::isfinite(mu) ||
        !(softening > 0.0) || !std::isfinite(softening))
        return 0.0;
    const double rs = SchwarzschildRadius(mu);
    const double rf = FlooredRadius(r, softening);
    const double ratio = (std::min)(rs / rf, 1.0);
    const double rad = 1.0 - ratio;
    const double root = std::sqrt(rad < 0.0 ? 0.0 : rad);
    // sqrt(1 − r_s/r) − 1 = −(r_s/r) / (1 + sqrt(1 − r_s/r)), cancellation-free.
    return -ratio / (1.0 + root);
}

double GRDilationFactorWeak(double mu, double r, double softening)
{
    if (!(mu >= 0.0) || !std::isfinite(mu) ||
        !(softening > 0.0) || !std::isfinite(softening))
        return 1.0;
    const double rf = FlooredRadius(r, softening);
    // 1 − GM/(r c²) = 1 − (r_s/2)/r. Stable linear weak-field form (r_s ≪ r);
    // no sqrt, so no near-1 cancellation. GM = μ.
    return 1.0 - 0.5 * (std::min)(SchwarzschildRadius(mu) / rf, 1.0);
}

double GRDilationFactorWeakMinusOne(double mu, double r, double softening)
{
    if (!(mu >= 0.0) || !std::isfinite(mu) ||
        !(softening > 0.0) || !std::isfinite(softening))
        return 0.0;
    const double rf = FlooredRadius(r, softening);
    return -0.5 * (std::min)(SchwarzschildRadius(mu) / rf, 1.0);
}

// ---- Composition (§2.2) ------------------------------------------------------

double CombinedDilationFactor(double beta, double mu, double r, double softening)
{
    // The weak-field first-order combination: the two rates multiply. Labeled an
    // approximation in §2.2 (exact SR·GR is not a simple product in strong field).
    return SRDilationFactor(beta) * GRDilationFactor(mu, r, softening);
}

double CombinedDilationFactorMinusOne(double beta, double mu, double r, double softening)
{
    const double a = SRDilationFactorMinusOne(beta);
    const double b = GRDilationFactorMinusOne(mu, r, softening);
    // (1+a)(1+b) − 1 = a + b + a·b — no product-then-subtract-1 cancellation, so
    // the combined residual keeps full precision even when both factors ≈ 1.
    return a + b + a * b;
}

// ---- Proper-time deviation accumulation (§2.2) ------------------------------

double ProperTimeDeviationStep(double factorMinusOne, double dt)
{
    if (!(dt > 0.0) || !std::isfinite(dt) ||
        !std::isfinite(factorMinusOne) || factorMinusOne < -1.0 ||
        factorMinusOne > 0.0)
        return 0.0;
    return dt * factorMinusOne; // dτ − dt = dt·(factor − 1)
}

void AdvanceClock(ecs::RelativisticClock& clock, double factorMinusOne, double dt)
{
    // Reject a corrupt stored clock to a finite sidecar; bad step inputs remain
    // no-ops so they cannot propagate a NaN into saved state.
    if (!std::isfinite(clock.coordinateTime) ||
        !std::isfinite(clock.properTimeDeviation))
    {
        clock = ecs::RelativisticClock{};
        return;
    }
    if (!(dt > 0.0) || !std::isfinite(dt) ||
        !std::isfinite(factorMinusOne) || factorMinusOne < -1.0 ||
        factorMinusOne > 0.0)
        return;
    const double coordinateTime = clock.coordinateTime + dt;
    const double properTimeDeviation =
        clock.properTimeDeviation + dt * factorMinusOne;
    if (!std::isfinite(coordinateTime) || !std::isfinite(properTimeDeviation))
        return;
    clock.coordinateTime = coordinateTime;
    clock.properTimeDeviation = properTimeDeviation;
}

double ProperTime(const ecs::RelativisticClock& clock)
{
    // Reconstruction, NOT a running τ += dτ: the deviation kept full precision the
    // whole trip and meets the coordinate-time magnitude only here, on demand.
    if (!std::isfinite(clock.coordinateTime) ||
        !std::isfinite(clock.properTimeDeviation))
        return 0.0;
    const double properTime = clock.coordinateTime + clock.properTimeDeviation;
    return std::isfinite(properTime) ? properTime : 0.0;
}

// =============================================================================
// 3. Frame-relative velocity — the master-frame composition (§2.3)
// =============================================================================

Vec3d VelocityInMasterFrame(const FrameGraph& graph, const Body& body, FrameId masterFrame)
{
    // Translation-only frames (reference_frame.h): the body's velocity in the
    // master frame is its global velocity minus the master frame's global
    // velocity. ResolveWorldVel already composes frame.velocity + body.localVel.
    if (body.frame >= graph.FrameCount() || masterFrame >= graph.FrameCount())
        return Vec3d{};
    const Vec3d worldVel  = graph.ResolveWorldVel(body);
    const Vec3d masterVel = graph.GetFrame(masterFrame).velocity;
    const Vec3d relative = worldVel - masterVel;
    return IsFinite(relative) ? relative : Vec3d{};
}

double BetaInMasterFrame(const FrameGraph& graph, const Body& body, FrameId masterFrame)
{
    return StableLength(VelocityInMasterFrame(graph, body, masterFrame)) /
           kSpeedOfLight;
}

double BetaLocalNaive(const Body& body)
{
    // The WRONG one: β from the body's own-frame velocity ignores that the frame
    // itself moves in the master frame. Shipped as the labeled negative control
    // for §2.3 — see the header. Never drive a clock with this.
    return IsFinite(body.localVel) ? StableLength(body.localVel) / kSpeedOfLight
                                   : 0.0;
}

} // namespace sim
