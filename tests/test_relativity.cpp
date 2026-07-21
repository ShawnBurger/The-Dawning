// =============================================================================
// tests/test_relativity.cpp — Sim Stage 2: relativistic dynamics + time dilation
// =============================================================================
// Drives the SHIPPED src/sim/relativity.{h,cpp} against ABUNDANT analytic ground
// truth. Verification per the prompt's (a)-(g), each with a DISCRIMINATING
// negative control kept IN the suite (an assertion nobody has watched fail is not
// evidence):
//
//   (a) momentum-space |v| < c STRUCTURAL for a huge sustained force, vs the
//       naive velocity update that exceeds c and the naive γ that goes non-finite.
//   (b) γ and dilation vs closed forms at several β; (1−β)(1+β) keeps precision
//       near c where 1−β·β loses it — shown.
//   (c) proper-time DEVIATION accumulation reads coordinate_time·√(1−β²) to full
//       precision on a long trip where naive τ += dτ loses the residual.
//   (d) GR clock matches √(1−r_s/r) away from the mass, stays FINITE at the
//       softened floor where the unsoftened form is NaN, weak-field linear form
//       agrees where r_s ≪ r and diverges where it does not.
//   (e) Newtonian limit: momentum-space v and dilation agree with Newtonian /
//       no-dilation below β ~ 1e-3 and diverge correctly above.
//   (f) FRAME-RELATIVE dilation: master-frame β is correct for a body at rest in
//       a moving frame; the naive local-frame β gives the WRONG answer.
//   (g) SR+GR composition matches the product of the two factors.
//
// Never asserts a tolerance the method cannot meet. Pure CPU; nothing here
// touches D3D12.
// =============================================================================

#include "test_framework.h"
#include "sim/relativity.h"
#include "sim/nbody.h"
#include "sim/reference_frame.h"

#include <cmath>
#include <cstdint>

namespace
{
using core::Vec3d;
using sim::kSpeedOfLight;

constexpr double c  = kSpeedOfLight;
constexpr double c2 = kSpeedOfLight * kSpeedOfLight;

double RelErr(double a, double b) { return std::fabs(a - b) / std::fabs(b); }

// Closed-form Lorentz γ from β (the textbook 1/√(1−β²)); used as GROUND TRUTH at
// moderate β where it is well-conditioned.
double GammaClosed(double beta) { return 1.0 / std::sqrt((1.0 - beta) * (1.0 + beta)); }

// Build a momentum with |v| = β·c along +x, INDEPENDENTLY of the shipped
// momentum<->velocity code (p = γ m β c), so the recovery tests have a real
// reference rather than a round-trip through the code under test.
Vec3d MomentumForBeta(double beta, double m)
{
    const double gamma = GammaClosed(beta);
    return Vec3d{ gamma * m * beta * c, 0.0, 0.0 };
}
} // namespace

// =============================================================================
// (a) MOMENTUM-SPACE STRUCTURAL BOUND — |v| < c by construction.
//     NEGATIVE CONTROL: the naive velocity update exceeds c on the SAME input
//     and the naive Lorentz γ goes non-finite; momentum-space stays finite/sub-c.
// =============================================================================
TEST_CASE(Relativity_MomentumStep_StructuralSubC_vs_NaiveExceedsC)
{
    const double m  = 1.0;
    const double dt = 1.0;
    const Vec3d  F{ 1.0e9, 0.0, 0.0 };   // enough to exceed c in Newtonian terms fast
    const int    N  = 2000;

    Vec3d p{ 0.0, 0.0, 0.0 };
    Vec3d vNaive{ 0.0, 0.0, 0.0 };       // the naive v += (F/m)·dt update
    Vec3d v{ 0.0, 0.0, 0.0 };
    double firstSpeed = -1.0;

    for (int i = 0; i < N; ++i)
    {
        p = sim::RelativisticMomentumStep(p, F, m, dt, v);
        const double speed = v.Length();

        // STRUCTURAL: strictly sub-c at EVERY step, and γ finite for every p.
        CHECK(speed < c);
        CHECK(std::isfinite(sim::GammaFromMomentum(p, m)));

        if (i == 0) firstSpeed = speed;
        vNaive += F * (dt / m);          // same F/m/dt, velocity-space
    }

    // Asymptotes to c: grew a lot, ends just under c, never at/over it.
    CHECK(v.Length() > firstSpeed);
    CHECK(v.Length() < c);
    CHECK(v.Length() > 0.999 * c);

    // NEGATIVE CONTROL — the naive velocity-space update blew past c ...
    CHECK(vNaive.Length() > c);
    // ... and the naive Lorentz γ = 1/√(1−β²) on that speed is NON-FINITE
    // (√ of a negative), the exact overflow momentum-space designs out.
    const double betaNaive  = vNaive.Length() / c;
    const double gammaNaive = 1.0 / std::sqrt(1.0 - betaNaive * betaNaive);
    CHECK(!std::isfinite(gammaNaive));
    // The momentum-space γ on the same trajectory is finite and large.
    CHECK(std::isfinite(sim::GammaFromMomentum(p, m)));
    CHECK(sim::GammaFromMomentum(p, m) > 1000.0);
}

// A finite force can never reach c, no matter how absurd the momentum.
TEST_CASE(Relativity_VelocityRecovery_NeverReachesC_EvenAtHugeMomentum)
{
    const double m = 2.0;

    // While the sub-ULP gap below c is representable (|p| ≪ m·c·2^26 ≈ 4e16),
    // recovery is STRICTLY sub-c.
    for (double pMag : { 1.0e3, 1.0e9, 1.0e15 })
    {
        const Vec3d v = sim::VelocityFromMomentum(Vec3d{ pMag, 0.0, 0.0 }, m);
        CHECK(v.Length() < c);
        CHECK(std::isfinite(sim::GammaFromMomentum(Vec3d{ pMag, 0.0, 0.0 }, m)));
    }

    // At ASTRONOMICAL momentum the rest-mass term underflows relative to (|p|/c)²
    // and |v| rounds to exactly c (the supremum). The structural guarantee that
    // floating point can honour is: |v| NEVER EXCEEDS c and NEVER NaNs — never a
    // >c overshoot, never the γ overflow the naive form produces. (Capped at 1e150:
    // above ~1e154 the foundation Vec3d::Length itself overflows squaring |p|, a
    // types.h limit far beyond any physical momentum, not a limit of this formula.)
    for (double pMag : { 1.0e30, 1.0e150 })
    {
        const Vec3d v = sim::VelocityFromMomentum(Vec3d{ pMag, 0.0, 0.0 }, m);
        CHECK(v.Length() <= c);
        CHECK(std::isfinite(v.Length()));
        CHECK(std::isfinite(sim::GammaFromMomentum(Vec3d{ pMag, 0.0, 0.0 }, m)));
    }
}

// =============================================================================
// (b) γ and dilation vs closed forms at several β; the (1−β)(1+β) form keeps
//     full precision near c where the naive 1−β·β loses digits.
// =============================================================================
TEST_CASE(Relativity_Gamma_And_Dilation_MatchClosedForms)
{
    const double m = 3.0;
    for (double beta : { 0.1, 0.5, 0.9, 0.99, 0.999 })
    {
        const Vec3d p = MomentumForBeta(beta, m);

        // γ from momentum matches the textbook closed form.
        CHECK(RelErr(sim::GammaFromMomentum(p, m), GammaClosed(beta)) < 1e-12);
        // recovered speed is exactly β·c.
        CHECK(RelErr(sim::VelocityFromMomentum(p, m).Length(), beta * c) < 1e-12);
        // SR dilation factor is √(1−β²) = 1/γ.
        CHECK(RelErr(sim::SRDilationFactor(beta), std::sqrt((1.0 - beta) * (1.0 + beta))) < 1e-12);
        CHECK(RelErr(sim::SRDilationFactor(beta) * GammaClosed(beta), 1.0) < 1e-12);
        // 1 + (factor−1) == factor: the two representations agree.
        CHECK(RelErr(1.0 + sim::SRDilationFactorMinusOne(beta), sim::SRDilationFactor(beta)) < 1e-12);
    }
}

// The near-c precision claim, made concrete. At β = 1 − 1e-11 (γ ≈ 2.2e5, a
// genuinely relativistic speed, and NOT inside the defense-in-depth β clamp) the
// shipped (1−β)(1+β) reconstructs the exact factor (1+β), while the naive 1−β·β
// systematically drops the (1−β)² term and loses digits. The discriminator: since
// (1−β) is EXACT (Sterbenz), dividing each form by (1−β) must recover (1+β) — the
// good form does to ~1 ULP, the naive form does not.
TEST_CASE(Relativity_OneMinusBetaSq_KeepsPrecisionNearC)
{
    const double beta        = 1.0 - 1.0e-11;
    const double oneMinusBeta = 1.0 - beta;   // EXACT (Sterbenz) = 1e-11
    const double onePlusBeta  = 1.0 + beta;   // ≈ 2

    const double good  = sim::OneMinusBetaSq(beta); // shipped (1−β)(1+β)
    const double naive = 1.0 - beta * beta;         // catastrophic cancellation form

    // The good form reconstructs (1+β) to full precision.
    // (This assertion FAILS if OneMinusBetaSq is mutated to 1 − β·β: the mutated
    //  value divides out to ≈ (1−β)/2 ≈ 5e-12 off, well above 1e-13.)
    CHECK(RelErr(good / oneMinusBeta, onePlusBeta) < 1e-13);

    // The naive form does NOT — it lost precision near c (measured ~5e-12 here,
    // ≈ (1−β)/2 — several significant digits gone).
    CHECK(RelErr(naive / oneMinusBeta, onePlusBeta) > 1e-13);
    CHECK(RelErr(naive, good) > 1e-13);

    // Control: at a moderate β the two forms agree to full precision, proving this
    // is specifically a near-c cancellation, not a general bug in one formula.
    const double bm = 0.5;
    CHECK(RelErr(1.0 - bm * bm, sim::OneMinusBetaSq(bm)) < 1e-15);
}

// =============================================================================
// (c) PROPER-TIME DEVIATION accumulation over a long trip. The deviation
//     accumulator reads coordinate_time·√(1−β²) to full precision; the naive
//     τ += dτ loses the residual once the running total is large.
// =============================================================================
TEST_CASE(Relativity_ProperTimeDeviation_SurvivesLongTrip_vs_NaiveTauSum)
{
    const double dt   = 1.0;
    const double beta = 1.0e-4;          // tiny β: dτ − dt is a ~5e-9 residual
    const int    N    = 10'000'000;      // long trip: coordinate time reaches 1e7

    const double factor      = std::sqrt((1.0 - beta) * (1.0 + beta)); // ground truth
    const double factorMinus = sim::SRDilationFactorMinusOne(beta);
    const double exact        = static_cast<double>(N) * dt * factor;

    // Deviation accumulator (the shipped clock).
    ecs::RelativisticClock clock;
    // Naive τ += dτ, summed into one running total (the WRONG method).
    double tauNaive = 0.0;
    const double dtau = dt * factor;

    for (int i = 0; i < N; ++i)
    {
        sim::AdvanceClock(clock, factorMinus, dt);
        tauNaive += dtau;
    }

    // The deviation accumulator reconstructs proper time to full precision.
    // (Under a τ += dτ mutation of AdvanceClock/ProperTime this degrades to the
    //  naive error ~3e-4 below and this assertion FAILS.)
    CHECK(std::fabs(sim::ProperTime(clock) - exact) < 1e-6);
    // The isolated deviation itself is exactly coordinate_time·(factor−1).
    CHECK(std::fabs(clock.properTimeDeviation - static_cast<double>(N) * dt * factorMinus) < 1e-9);

    // NEGATIVE CONTROL — the naive running τ += dτ lost the residual once the
    // running total grew: it is off from the exact proper time by ≫ 1e-4 (measured
    // ~3.5e-4), where the deviation accumulator is off by < 1e-6 — the whole
    // reason the sidecar accumulates the deviation and not τ.
    CHECK(std::fabs(tauNaive - exact) > 1e-4);
    CHECK(std::fabs(tauNaive - exact) > 100.0 * (std::fabs(sim::ProperTime(clock) - exact) + 1e-12));
}

// At v=0, r=∞ (no dilation) the factor is 1.0 bit-exact and the deviation
// increment is EXACTLY 0.0 — the always-on cheap sanity from §2 / prompt.
TEST_CASE(Relativity_NoDilation_FactorIsExactlyOne_DeviationExactlyZero)
{
    CHECK(sim::SRDilationFactor(0.0) == 1.0);
    CHECK(sim::SRDilationFactorMinusOne(0.0) == 0.0);

    ecs::RelativisticClock clock;
    sim::AdvanceClock(clock, sim::SRDilationFactorMinusOne(0.0), 1.0 / 60.0);
    CHECK(clock.properTimeDeviation == 0.0);
    CHECK(sim::ProperTime(clock) == clock.coordinateTime);
}

// =============================================================================
// (d) GR clock: matches √(1−r_s/r) away from the mass; stays FINITE at the
//     softened floor where the unsoftened form NaNs; weak-field linear form
//     agrees where r_s ≪ r and diverges where it does not.
// =============================================================================
TEST_CASE(Relativity_GRClock_MatchesSchwarzschild_And_FiniteAtSoftenedFloor)
{
    // A compact mass whose Schwarzschild radius is macroscopic, so we can probe
    // INSIDE r_s where the unsoftened clock is imaginary.
    const double rs  = 1000.0;                       // metres
    const double mu  = rs * c2 / 2.0;                // r_s = 2μ/c²  =>  μ = r_s c²/2
    const double radius = 0.0;                       // point source
    const double soft = sim::SofteningLength(mu, radius); // = r_s + kSofteningBase

    CHECK(RelErr(sim::SchwarzschildRadius(mu), rs) < 1e-12);
    CHECK(soft > rs);                                 // floor sits OUTSIDE the horizon

    // --- Away from the mass: matches the closed Schwarzschild form. ---
    const double rFar = 1.0e7;                        // ≫ soft, no floor
    const double refFar = std::sqrt(1.0 - rs / rFar);
    CHECK(RelErr(sim::GRDilationFactor(mu, rFar, soft), refFar) < 1e-14);
    CHECK(sim::GRDilationFactor(mu, rFar, soft) < 1.0);   // deep-ish well slows the clock
    CHECK(sim::GRDilationFactor(mu, rFar, soft) > 0.0);

    // --- Inside the horizon: softened clock stays FINITE and real. ---
    const double rInside = 500.0;                     // < r_s: unsoftened is NaN
    const double grInside = sim::GRDilationFactor(mu, rInside, soft);
    CHECK(std::isfinite(grInside));                   // (FAILS if the floor is removed)
    CHECK(grInside > 0.0);
    CHECK(grInside <= 1.0);
    // The value equals the clock evaluated AT the softened floor, by construction.
    CHECK(RelErr(grInside, std::sqrt(1.0 - rs / soft)) < 1e-14);

    // NEGATIVE CONTROL — the UNSOFTENED form at r < r_s is √ of a negative → NaN.
    const double unsoftened = std::sqrt(1.0 - rs / rInside);
    CHECK(!std::isfinite(unsoftened));
}

TEST_CASE(Relativity_GRClock_WeakFieldLinearForm)
{
    // Earth-like: r_s ≈ 9 mm, r = Earth radius → r_s/r ~ 1.4e-9 (deep weak field).
    const double mu   = 3.986004418e14;               // m^3/s^2
    const double soft = sim::SofteningLength(mu, 6.371e6);
    const double r    = 6.371e6;

    // Weak-field linear 1 − GM/(r c²) agrees with the √ form to O((r_s/r)²).
    CHECK(std::fabs(sim::GRDilationFactorWeak(mu, r, soft)
                  - sim::GRDilationFactor(mu, r, soft)) < 1e-15);
    // The linear form's own (factor−1) is −GM/(r c²).
    CHECK(RelErr(sim::GRDilationFactorWeakMinusOne(mu, r, soft),
                 -mu / (r * c2)) < 1e-12);

    // In a STRONG field the linear form genuinely diverges from √ (proving it is
    // the linear approximation, not the same function): at r = 2 r_s, r_s/r = 0.5.
    const double rsBig = 2000.0;
    const double muBig = rsBig * c2 / 2.0;
    const double softBig = sim::SofteningLength(muBig, 0.0); // point source, soft = r_s + ε0
    const double rStrong = 2.0 * rsBig;               // > soft, no floor; r_s/r = 0.5
    CHECK(rStrong > softBig);                          // guard: not floored
    CHECK(std::fabs(sim::GRDilationFactorWeak(muBig, rStrong, softBig)
                  - sim::GRDilationFactor(muBig, rStrong, softBig)) > 1e-2);
}

// =============================================================================
// (e) NEWTONIAN LIMIT: continuous boundary. Below β ~ 1e-3 the momentum-space v
//     and the dilation agree with Newtonian / no-dilation; above, they diverge.
// =============================================================================
TEST_CASE(Relativity_NewtonianLimit_AgreesBelow_DivergesAbove)
{
    const double m = 5.0;

    // --- Low speed: p ≈ m v, v ≈ p/m, factor ≈ 1 (agreement to tolerance). ---
    {
        const double beta = 1.0e-4;                   // ~ below 1e-3
        const Vec3d  vLow{ beta * c, 0.0, 0.0 };
        const Vec3d  pRel = sim::MomentumFromVelocity(vLow, m);
        // relativistic p differs from Newtonian m v only by γ−1 ~ 5e-9.
        CHECK(RelErr(pRel.Length(), m * vLow.Length()) < 1e-6);
        // recovering v from the Newtonian momentum m v returns v to tolerance.
        const Vec3d vBack = sim::VelocityFromMomentum(Vec3d{ m * vLow.x, 0.0, 0.0 }, m);
        CHECK(RelErr(vBack.Length(), vLow.Length()) < 1e-6);
        // no meaningful dilation.
        CHECK(std::fabs(sim::SRDilationFactor(beta) - 1.0) < 1e-6);
        CHECK(std::fabs(sim::SRDilationFactorMinusOne(beta)) < 1e-6);
    }

    // --- High speed: relativistic and Newtonian DIVERGE correctly. ---
    {
        const double beta = 0.9;
        const Vec3d  vHi{ beta * c, 0.0, 0.0 };
        const Vec3d  pRel = sim::MomentumFromVelocity(vHi, m);
        // p / (m v) = γ ≈ 2.29 → differs from Newtonian by > 100%.
        CHECK(RelErr(pRel.Length(), m * vHi.Length()) > 1.0);
        // dilation is unmistakable (√(1−0.81) ≈ 0.436).
        CHECK(sim::SRDilationFactor(beta) < 0.5);
    }
}

// =============================================================================
// (f) THE FRAME-RELATIVE-VELOCITY SUBTLETY (§2.3). A body at rest in a MOVING
//     frame has β=0 locally but β≠0 in the master frame; the master-frame β is
//     the correct one, the naive local β is the discriminating WRONG answer.
// =============================================================================
TEST_CASE(Relativity_FrameRelativeBeta_MasterFrameCorrect_LocalNaiveWrong)
{
    sim::FrameGraph graph;

    // Master (star-system) frame: at rest, at the origin.
    const sim::FrameId master =
        graph.CreateFrame(sim::kInvalidFrame, sim::WorldPos{}, Vec3d{ 0.0, 0.0, 0.0 });

    // Planet frame: moving at 0.1c in the master frame (a fast "planet" so the
    // dilation it induces is large and unambiguous).
    const double vp = 0.1 * c;
    const sim::FrameId planet =
        graph.CreateFrame(master, sim::WorldPos::FromOffset(Vec3d{ 1.0e9, 0.0, 0.0 }),
                          Vec3d{ vp, 0.0, 0.0 });

    // A ship AT REST in the planet frame (localVel = 0).
    sim::Body ship;
    ship.frame    = planet;
    ship.localPos = Vec3d{ 100.0, 0.0, 0.0 };
    ship.localVel = Vec3d{ 0.0, 0.0, 0.0 };

    // Master-frame β is 0.1 (it inherits the planet's motion). CORRECT.
    // (This assertion FAILS if VelocityInMasterFrame drops the master subtraction
    //  and returns the local velocity instead.)
    CHECK(RelErr(sim::BetaInMasterFrame(graph, ship, master), 0.1) < 1e-12);
    // Naive local β is 0 — the WRONG answer for a body at rest in a moving frame.
    CHECK(sim::BetaLocalNaive(ship) == 0.0);

    // The dilation they imply differs by a physically large amount.
    const double correctFactor = sim::SRDilationFactor(sim::BetaInMasterFrame(graph, ship, master));
    const double naiveFactor   = sim::SRDilationFactor(sim::BetaLocalNaive(ship));
    CHECK(RelErr(correctFactor, std::sqrt(1.0 - 0.1 * 0.1)) < 1e-12); // ≈ 0.99499
    CHECK(naiveFactor == 1.0);                                        // "no dilation" — wrong
    CHECK(std::fabs(correctFactor - naiveFactor) > 1e-3);

    // Velocities COMPOSE: a ship also moving within the planet frame adds to the
    // frame drift in the master frame.
    sim::Body mover = ship;
    mover.localVel = Vec3d{ 0.02 * c, 0.0, 0.0 };
    CHECK(RelErr(sim::BetaInMasterFrame(graph, mover, master), 0.12) < 1e-12);

    // CONTROL that the master method REDUCES correctly: a body genuinely at rest
    // in the master frame has master-β == local-β == 0 (they agree when the
    // containing frame is not moving), so the method is not just "always nonzero".
    sim::Body atRestInMaster;
    atRestInMaster.frame    = master;
    atRestInMaster.localVel = Vec3d{ 0.0, 0.0, 0.0 };
    CHECK(sim::BetaInMasterFrame(graph, atRestInMaster, master) == 0.0);
    CHECK(sim::BetaLocalNaive(atRestInMaster) == 0.0);
}

// =============================================================================
// (g) SR + GR COMPOSITION for a body both moving AND deep in a well: the product
//     of the two factors (§2.2 weak-field combination).
// =============================================================================
TEST_CASE(Relativity_CombinedSRGR_IsProductOfFactors)
{
    const double rs   = 3000.0;
    const double mu   = rs * c2 / 2.0;
    const double soft = sim::SofteningLength(mu, 0.0);   // = r_s + kSofteningBase
    const double r    = 1.0e6;                           // ≫ soft, no floor
    const double beta = 0.1;

    const double srF = sim::SRDilationFactor(beta);
    const double grF = sim::GRDilationFactor(mu, r, soft);
    const double comb = sim::CombinedDilationFactor(beta, mu, r, soft);

    // Combined == product of the two, and both factors are < 1 (each slows time).
    CHECK(srF < 1.0);
    CHECK(grF < 1.0);
    CHECK(RelErr(comb, srF * grF) < 1e-15);

    // Independent reference: √(1−β²)·√(1−r_s/r).
    const double ref = std::sqrt((1.0 - beta) * (1.0 + beta)) * std::sqrt(1.0 - rs / r);
    CHECK(RelErr(comb, ref) < 1e-12);

    // The (factor−1) representation agrees with the product form.
    const double combMinus = sim::CombinedDilationFactorMinusOne(beta, mu, r, soft);
    CHECK(RelErr(1.0 + combMinus, comb) < 1e-12);

    // And it accumulates into a clock as the product's deviation.
    ecs::RelativisticClock clock;
    const double dt = 0.5;
    const int    N  = 1000;
    for (int i = 0; i < N; ++i)
        sim::AdvanceClock(clock, combMinus, dt);
    CHECK(RelErr(clock.properTimeDeviation, static_cast<double>(N) * dt * (comb - 1.0)) < 1e-9);
    CHECK(std::fabs(sim::ProperTime(clock) - static_cast<double>(N) * dt * comb) < 1e-6);
}

// =============================================================================
// Momentum <-> velocity round trips (both directions), rapidity, kinetic energy.
// =============================================================================
TEST_CASE(Relativity_MomentumVelocity_RoundTrip_BothDirections)
{
    const double m = 7.0;
    // A fixed unit direction so the constructed speed is EXACTLY β·c (< c).
    const Vec3d dir = Vec3d{ 1.0, 0.1, -0.2 }.Normalized();
    for (double beta : { 0.0, 1.0e-3, 0.3, 0.7, 0.95, 0.999 })
    {
        const Vec3d v0 = dir * (beta * c);
        // v -> p -> v
        const Vec3d p  = sim::MomentumFromVelocity(v0, m);
        const Vec3d v1 = sim::VelocityFromMomentum(p, m);
        CHECK(RelErr(v1.x, v0.x) < 1e-10 || std::fabs(v0.x) < 1e-9);
        CHECK(std::fabs(v1.Length() - v0.Length()) < 1e-6 * (v0.Length() + 1.0));

        // p -> v -> p
        const Vec3d p2 = sim::MomentumFromVelocity(v1, m);
        CHECK(std::fabs(p2.Length() - p.Length()) < 1e-6 * (p.Length() + 1.0));
    }
}

TEST_CASE(Relativity_Rapidity_ConsistentWithSpeed)
{
    const double m = 1.5;
    for (double beta : { 0.1, 0.5, 0.9, 0.99 })
    {
        const Vec3d p = MomentumForBeta(beta, m);
        const double phi = sim::RapidityFromMomentum(p, m);
        // v = c tanh φ recovers β·c.
        CHECK(RelErr(sim::SpeedFromRapidity(phi), beta * c) < 1e-10);
        // φ = asinh(γβ): rapidity adds where velocity would not.
        CHECK(RelErr(phi, std::asinh(GammaClosed(beta) * beta)) < 1e-12);
    }
}

TEST_CASE(Relativity_KineticEnergy_MatchesClosedForm_And_NewtonianLimit)
{
    const double m = 4.0;

    // Relativistic: KE = (γ−1) m c².
    for (double beta : { 0.2, 0.6, 0.9, 0.99 })
    {
        const Vec3d p = MomentumForBeta(beta, m);
        const double keClosed = (GammaClosed(beta) - 1.0) * m * c2;
        CHECK(RelErr(sim::RelativisticKineticEnergy(p, m), keClosed) < 1e-10);
    }

    // Newtonian limit: KE → ½ m v² as β → 0.
    const double beta = 1.0e-4;
    const Vec3d  p    = MomentumForBeta(beta, m);
    const double v    = beta * c;
    CHECK(RelErr(sim::RelativisticKineticEnergy(p, m), 0.5 * m * v * v) < 1e-6);
}

// House guard: dt <= 0 or non-finite is a no-op (momentum unchanged), matching
// the shipped rigid_body / nbody guards.
TEST_CASE(Relativity_MomentumStep_HouseGuard)
{
    const double m = 2.0;
    const Vec3d p0{ 10.0, 0.0, 0.0 };
    const Vec3d F{ 1.0e6, 0.0, 0.0 };
    Vec3d v;

    const Vec3d pZero = sim::RelativisticMomentumStep(p0, F, m, 0.0, v);
    CHECK(pZero.x == p0.x);
    const Vec3d pNeg  = sim::RelativisticMomentumStep(p0, F, m, -1.0, v);
    CHECK(pNeg.x == p0.x);
    const double nan = std::nan("");
    const Vec3d pNan  = sim::RelativisticMomentumStep(p0, F, m, nan, v);
    CHECK(pNan.x == p0.x);
    // velocityOut still holds the recovered velocity of the unchanged momentum.
    CHECK(RelErr(v.Length(), sim::VelocityFromMomentum(p0, m).Length()) < 1e-12);
}
