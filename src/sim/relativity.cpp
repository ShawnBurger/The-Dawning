// =============================================================================
// sim/relativity.cpp — momentum-space relativistic dynamics + time dilation
// =============================================================================
// See relativity.h for the module contract, the structural |v|<c argument, the
// deviation-accumulation discipline, the shared-softening reuse, and the §2.3
// master-frame subtlety with its honest caveats.
// =============================================================================

#include "relativity.h"

#include <cmath>

namespace sim
{

namespace
{
// β = |v|/c is a speed magnitude, so it is >= 0; clamp its magnitude to kBetaMax
// as defense-in-depth (§2.6). Dynamics never reaches this because momentum
// recovery keeps β < 1 structurally; it only guards a caller-supplied β.
double ClampBeta(double beta)
{
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
    if (!(restMass > 0.0))
        return 1.0; // house-guard limit: no rest mass => no boost
    const double x = momentum.Length() / (restMass * kSpeedOfLight); // |p|/(mc)
    // sqrt(1 + x^2) via hypot: never overflows the intermediate square, so γ is
    // finite for every finite p — the property 1/sqrt(1−β²) cannot offer.
    return std::hypot(1.0, x);
}

Vec3d VelocityFromMomentum(const Vec3d& momentum, double restMass)
{
    if (!(restMass > 0.0))
        return Vec3d{ 0.0, 0.0, 0.0 };
    const double pOverC = momentum.Length() / kSpeedOfLight;
    // denom = sqrt(m^2 + (|p|/c)^2) = m·γ, strictly positive; hypot dodges the
    // overflow of (|p|/c)^2 at absurd |p|. |v| = |p|/denom < c for all finite p.
    const double denom = std::hypot(restMass, pOverC);
    return momentum * (1.0 / denom);
}

Vec3d MomentumFromVelocity(const Vec3d& velocity, double restMass)
{
    if (!(restMass > 0.0))
        return Vec3d{ 0.0, 0.0, 0.0 };
    const double beta = ClampBeta(velocity.Length() / kSpeedOfLight);
    // γ from β for the SEEDING direction only (v→p). Not structurally bounded,
    // hence the ClampBeta defense-in-depth. 1−β² as (1−β)(1+β) for near-c seeds.
    const double gamma = 1.0 / std::sqrt(OneMinusBetaSq(beta));
    return velocity * (restMass * gamma);
}

double RelativisticKineticEnergy(const Vec3d& momentum, double restMass)
{
    if (!(restMass > 0.0))
        return 0.0;
    const double gamma = GammaFromMomentum(momentum, restMass);
    // (γ−1) m c² written as |p|² / (m(γ+1)) — no (γ−1) cancellation near β=0,
    // where it reduces to the Newtonian |p|²/(2m) = ½ m v².
    return momentum.LengthSq() / (restMass * (gamma + 1.0));
}

double RapidityFromMomentum(const Vec3d& momentum, double restMass)
{
    if (!(restMass > 0.0))
        return 0.0;
    const double x = momentum.Length() / (restMass * kSpeedOfLight);
    return std::asinh(x); // p = m c sinh φ
}

double SpeedFromRapidity(double rapidity)
{
    return kSpeedOfLight * std::tanh(rapidity); // v = c tanh φ, |v| < c structurally
}

Vec3d RelativisticMomentumStep(const Vec3d& momentum, const Vec3d& force,
                               double restMass, double dt, Vec3d& velocityOut)
{
    // House guard (matches rigid_body.cpp / nbody.cpp): dt<=0 or non-finite is a
    // no-op — momentum unchanged, velocityOut the current recovered velocity.
    if (!(dt > 0.0) || !std::isfinite(dt))
    {
        velocityOut = VelocityFromMomentum(momentum, restMass);
        return momentum;
    }
    const Vec3d pNew = momentum + force * dt;      // dp = F·dt
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
    return 2.0 * mu / (kSpeedOfLight * kSpeedOfLight); // r_s = 2GM/c², μ = GM
}

double FlooredRadius(double r, double softening)
{
    // Floor r at the SHARED softening (nbody.h). (r > softening) is false for a
    // NaN/negative r, so the result is always the finite softening in that case.
    return (r > softening) ? r : softening;
}

double GRDilationFactor(double mu, double r, double softening)
{
    const double rs = SchwarzschildRadius(mu);
    const double rf = FlooredRadius(r, softening);
    const double ratio = rs / rf; // < 1 when softening comes from SofteningLength
    const double rad = 1.0 - ratio;
    // rad is strictly in (0,1] for the intended shared softening (rf ≥ softening
    // ≥ r_s + ε0 > r_s). The <0 guard keeps it real (horizon limit 0) rather than
    // NaN if a caller passes a softening below r_s — the horizon NaN designed out.
    return std::sqrt(rad < 0.0 ? 0.0 : rad);
}

double GRDilationFactorMinusOne(double mu, double r, double softening)
{
    const double rs = SchwarzschildRadius(mu);
    const double rf = FlooredRadius(r, softening);
    const double ratio = rs / rf;
    const double rad = 1.0 - ratio;
    const double root = std::sqrt(rad < 0.0 ? 0.0 : rad);
    // sqrt(1 − r_s/r) − 1 = −(r_s/r) / (1 + sqrt(1 − r_s/r)), cancellation-free.
    return -ratio / (1.0 + root);
}

double GRDilationFactorWeak(double mu, double r, double softening)
{
    const double rf = FlooredRadius(r, softening);
    // 1 − GM/(r c²) = 1 − (r_s/2)/r. Stable linear weak-field form (r_s ≪ r);
    // no sqrt, so no near-1 cancellation. GM = μ.
    return 1.0 - mu / (rf * kSpeedOfLight * kSpeedOfLight);
}

double GRDilationFactorWeakMinusOne(double mu, double r, double softening)
{
    const double rf = FlooredRadius(r, softening);
    return -mu / (rf * kSpeedOfLight * kSpeedOfLight); // −GM/(r c²)
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
    return dt * factorMinusOne; // dτ − dt = dt·(factor − 1)
}

void AdvanceClock(ecs::RelativisticClock& clock, double factorMinusOne, double dt)
{
    // House guard: a bad dt or a non-finite factor contributes deficit += 0 and
    // leaves the clock intact (§2.6), never propagating a NaN into saved state.
    if (!(dt > 0.0) || !std::isfinite(dt) || !std::isfinite(factorMinusOne))
        return;
    clock.coordinateTime      += dt;
    clock.properTimeDeviation += dt * factorMinusOne; // Σ(dτ − dt), in isolation
}

double ProperTime(const ecs::RelativisticClock& clock)
{
    // Reconstruction, NOT a running τ += dτ: the deviation kept full precision the
    // whole trip and meets the coordinate-time magnitude only here, on demand.
    return clock.coordinateTime + clock.properTimeDeviation;
}

// =============================================================================
// 3. Frame-relative velocity — the master-frame composition (§2.3)
// =============================================================================

Vec3d VelocityInMasterFrame(const FrameGraph& graph, const Body& body, FrameId masterFrame)
{
    // Translation-only frames (reference_frame.h): the body's velocity in the
    // master frame is its global velocity minus the master frame's global
    // velocity. ResolveWorldVel already composes frame.velocity + body.localVel.
    const Vec3d worldVel  = graph.ResolveWorldVel(body);
    const Vec3d masterVel = graph.GetFrame(masterFrame).velocity;
    return worldVel - masterVel;
}

double BetaInMasterFrame(const FrameGraph& graph, const Body& body, FrameId masterFrame)
{
    return VelocityInMasterFrame(graph, body, masterFrame).Length() / kSpeedOfLight;
}

double BetaLocalNaive(const Body& body)
{
    // The WRONG one: β from the body's own-frame velocity ignores that the frame
    // itself moves in the master frame. Shipped as the labeled negative control
    // for §2.3 — see the header. Never drive a clock with this.
    return body.localVel.Length() / kSpeedOfLight;
}

} // namespace sim
