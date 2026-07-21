// =============================================================================
// tests/test_atmosphere.cpp — atmospheric flight: USSA76, drag stiffness, forces
// =============================================================================
// SIM STAGE 3. Drives the SHIPPED sim/atmosphere.{h,cpp} per the verification
// contract in docs/research/ATMOSPHERIC_FLIGHT.md. Pure CPU; touches no D3D12.
//
// The load-bearing test is (E): the SEMI-IMPLICIT drag update is contractive for
// a dt many times the drag time constant, where the naive EXPLICIT-EULER control
// injects energy on the SAME input — the same stiffness failure class Stage 1 hit
// with the explicit gyroscopic term. Every other test states a value the method
// must hit (terminal velocity, USSA76 boundary values, C0 seams) or a sign it must
// carry (static-stability restoring moment), never a tolerance the method cannot
// meet.
//
// The rule from the prior stages holds: no assertion that only passes because the
// feature is present. Drag has its zero-airspeed negative control; heating has its
// firewall control; the semi-implicit update has the explicit-Euler control.
// =============================================================================

#include "test_framework.h"
#include "sim/atmosphere.h"

#include <cmath>
#include <limits>

namespace
{
using core::Vec3d;
using namespace sim;

double RelErr(double a, double b) { return std::fabs(a - b) / std::fabs(b); }
} // namespace

// =============================================================================
// (A) USSA76 BOUNDARY VALUES — sea level and the 11 km tropopause against the
//     independently published US Standard Atmosphere 1976 table, not against a
//     round-trip through the model's own precompute.
// =============================================================================
TEST_CASE(Atmosphere_USSA76_BoundaryValues_MatchPublished)
{
    const AtmosphereModel earth = AtmosphereModel::EarthUSSA76();

    // Sea level (h = 0).
    const AtmosphereState s0 = SampleAtmosphere(earth, 0.0);
    CHECK_APPROX_EPS(s0.temperature, 288.15, 1e-9);
    CHECK_APPROX_EPS(s0.pressure, 101325.0, 1e-6);
    CHECK(RelErr(s0.density, 1.225) < 5e-4);      // published sea-level density
    CHECK(RelErr(s0.speedOfSound, 340.294) < 1e-4); // published sea-level a

    // Tropopause (h = 11 000 m, top of the troposphere layer).
    const AtmosphereState s11 = SampleAtmosphere(earth, 11000.0);
    CHECK_APPROX_EPS(s11.temperature, 216.65, 1e-9);
    CHECK(RelErr(s11.pressure, 22632.06) < 5e-5); // published P_b at 11 km
    CHECK(RelErr(s11.density, 0.363918) < 5e-4);  // published rho at 11 km
    CHECK(RelErr(s11.speedOfSound, 295.070) < 1e-4);

    // 20 km and 32 km base pressures are also published (5474.89, 868.019 Pa).
    CHECK(RelErr(SampleAtmosphere(earth, 20000.0).pressure, 5474.89) < 1e-4);
    CHECK(RelErr(SampleAtmosphere(earth, 32000.0).pressure, 868.019) < 1e-4);

    // MID-LAYER, not a boundary: at a layer base T/Tb == 1 so the barometric
    // EXPONENT cancels and the base pressures alone cannot witness the in-layer
    // integration. 5 km is deep inside the troposphere; the published density is
    // 0.73643 kg/m^3 and T = 255.65 K. A wrong exponent sign fails HERE.
    const AtmosphereState s5 = SampleAtmosphere(earth, 5000.0);
    CHECK_APPROX_EPS(s5.temperature, 255.65, 1e-9);
    CHECK(RelErr(s5.density, 0.73643) < 1e-3);
}

// =============================================================================
// (B) C0 CONTINUITY ACROSS EVERY LAYER SEAM — the profile has NO force-injecting
//     discontinuity. Base pressure is integrated layer by layer precisely so the
//     piecewise barometric formula is continuous; this measures that it is.
// =============================================================================
TEST_CASE(Atmosphere_USSA76_ContinuousAcrossLayerSeams)
{
    const AtmosphereModel earth = AtmosphereModel::EarthUSSA76();
    const double seams[] = { 11000.0, 20000.0, 32000.0, 47000.0, 51000.0, 71000.0 };
    for (double hb : seams)
    {
        const AtmosphereState below = SampleAtmosphere(earth, hb - 1e-3);
        const AtmosphereState at    = SampleAtmosphere(earth, hb);
        // Pressure and temperature are continuous by construction; density = P/(RT)
        // inherits it. A 1 mm altitude step must not move any of them measurably.
        CHECK(RelErr(below.pressure,    at.pressure)    < 1e-6);
        CHECK(RelErr(below.temperature, at.temperature) < 1e-6);
        CHECK(RelErr(below.density,     at.density)     < 1e-6);
    }
}

// =============================================================================
// (C) THE CEILING IS A HARD ZERO, AND ONLY THERE — density is EXACTLY 0.0 at and
//     above the ceiling (rho -> 0 is what makes atmosphere one vanishing force
//     term, not a separate regime), and strictly positive just below it.
// =============================================================================
TEST_CASE(Atmosphere_DensityExactlyZeroAtAndAboveCeiling)
{
    const AtmosphereModel earth = AtmosphereModel::EarthUSSA76();
    CHECK_EQ(SampleAtmosphere(earth, earth.ceiling).density, 0.0);       // exactly 0
    CHECK_EQ(SampleAtmosphere(earth, earth.ceiling + 1000.0).density, 0.0);
    CHECK(SampleAtmosphere(earth, earth.ceiling - 1.0).density > 0.0);   // positive below

    // The exponential model shares the property.
    const AtmosphereModel expo = AtmosphereModel::ExponentialBody(1.225, 8500.0, 100000.0);
    CHECK_EQ(SampleAtmosphere(expo, 100000.0).density, 0.0);
    CHECK_EQ(SampleAtmosphere(expo, 150000.0).density, 0.0);
    CHECK(SampleAtmosphere(expo, 99999.0).density > 0.0);
    // Exponential falls off as exp(-h/H): at one scale height, density is rho0/e.
    CHECK(RelErr(SampleAtmosphere(expo, 8500.0).density, 1.225 / std::exp(1.0)) < 1e-9);

    // Vacuum is always zero.
    CHECK_EQ(SampleAtmosphere(AtmosphereModel::Vacuum(), 0.0).density, 0.0);
}

// =============================================================================
// (D) DYNAMIC PRESSURE AND MACH ARE EXACT DEFINITIONS — q = 0.5*rho*|v|^2 and
//     M = |v|/a, so these are equality checks, not tolerances.
// =============================================================================
TEST_CASE(Atmosphere_DynamicPressureAndMach_AreExact)
{
    const Vec3d v{ 3.0, 4.0, 12.0 };          // |v| = 13 exactly
    const double rho = 1.225;
    CHECK_APPROX_EPS(DynamicPressure(rho, v), 0.5 * rho * 169.0, 1e-12);
    CHECK_APPROX_EPS(MachNumber(v, 340.0), 13.0 / 340.0, 1e-12);

    // Airspeed is body velocity minus co-rotating atmosphere velocity: a body
    // moving WITH the air has zero airspeed, hence zero q and zero Mach.
    const Vec3d wind{ 3.0, 4.0, 12.0 };
    const Vec3d rel = AirspeedVector(v, wind);
    CHECK_EQ(rel.LengthSq(), 0.0);
    CHECK_EQ(DynamicPressure(rho, rel), 0.0);
    CHECK_EQ(MachNumber(rel, 340.0), 0.0);

    // Speed of sound 0 (vacuum) must not divide by zero.
    CHECK_EQ(MachNumber(v, 0.0), 0.0);
}

// =============================================================================
// (E) THE STIFF-DRAG FIX — the discriminating test of this stage.
//     With dt MANY times the drag time constant tau, the SEMI-IMPLICIT update is
//     contractive (speed strictly decreases, never flips, never grows) while the
//     naive EXPLICIT-EULER control on the SAME input injects energy: it overshoots
//     zero and the speed GROWS. This is the whole reason the shipped path uses the
//     semi-implicit form.
// =============================================================================
TEST_CASE(Atmosphere_SemiImplicitDrag_ContractiveWhereExplicitExplodes)
{
    const double rho = 1.225, cd = 1.0, area = 10.0, mass = 1.0;
    const Vec3d v0{ 100.0, 0.0, 0.0 };

    const double tau = DragTimeConstant(mass, rho, cd, area, v0);
    const double dt  = 60.0 * tau; // deep into the stiff regime

    // Semi-implicit: strictly contractive.
    const Vec3d vSemi1 = SemiImplicitDragAirspeed(v0, rho, cd, area, mass, dt);
    CHECK(vSemi1.Length() < v0.Length());          // lost speed
    CHECK(vSemi1.Dot(v0) > 0.0);                    // did NOT flip direction
    const Vec3d vSemi2 = SemiImplicitDragAirspeed(vSemi1, rho, cd, area, mass, dt);
    CHECK(vSemi2.Length() < vSemi1.Length());       // still monotone decreasing

    // Explicit Euler control on the SAME input: energy injection.
    const Vec3d vExpl = ExplicitDragAirspeed(v0, rho, cd, area, mass, dt);
    CHECK(vExpl.Length() > v0.Length());            // GAINED speed — unphysical
    CHECK(vExpl.Dot(v0) < 0.0);                      // and flipped past zero

    // The two disagree by orders of magnitude — the point of the fix.
    CHECK(vExpl.Length() > 10.0 * vSemi1.Length());

    // Contractivity is UNCONDITIONAL: even at an absurd dt, semi-implicit stays
    // sub-initial and never negative-projecting.
    const Vec3d vHuge = SemiImplicitDragAirspeed(v0, rho, cd, area, mass, 1.0e6);
    CHECK(vHuge.Length() < v0.Length());
    CHECK(vHuge.Dot(v0) >= 0.0);
    // Zero airspeed is a fixed point (no NaN from dividing by |v|).
    const Vec3d z = SemiImplicitDragAirspeed(Vec3d{ 0, 0, 0 }, rho, cd, area, mass, dt);
    CHECK_EQ(z.LengthSq(), 0.0);
}

// =============================================================================
// (F) TERMINAL VELOCITY — integrate constant gravity against semi-implicit drag
//     and confirm it CONVERGES to the closed form v_t = sqrt(2 m g /(rho Cd A)).
//     This exercises the shipped update in the loop it is actually used in.
// =============================================================================
TEST_CASE(Atmosphere_FallingBody_ConvergesToTerminalVelocity)
{
    const double m = 100.0, g = kEarthG0, rho = 1.225, cd = 1.0, area = 1.0;
    const double vt = TerminalVelocity(m, g, rho, cd, area);
    CHECK(RelErr(vt, std::sqrt(2.0 * m * g / (rho * cd * area))) < 1e-12); // closed form

    // Semi-implicit leapfrog of a 1-D fall: gravity kick, then drag on the speed.
    Vec3d v{ 0.0, 0.0, 0.0 };
    const double dt = 0.001;
    for (int i = 0; i < 200000; ++i) // 200 s, ~50 drag time constants at terminal
    {
        v = v + Vec3d{ 0.0, 0.0, g * dt };                     // gravity (downward +z)
        v = SemiImplicitDragAirspeed(v, rho, cd, area, m, dt); // drag opposing motion
    }
    CHECK(RelErr(v.Length(), vt) < 1e-3); // converged to the analytic terminal speed
}

// =============================================================================
// (G) STATIC STABILITY IS A SIGN, NOT A MAGNITUDE — a center of pressure AFT of
//     the center of mass produces a RESTORING moment (rotates the nose toward the
//     velocity, reducing angle of attack); a CoP FORWARD produces a diverging one.
//     Same force, opposite offsets, opposite torque sign — a clean discriminator.
// =============================================================================
TEST_CASE(Atmosphere_CenterOfPressureAft_IsStaticallyRestoring)
{
    // Airspeed: mostly +x (forward) with a small +y perturbation => small AoA.
    const Vec3d airspeed{ 100.0, 2.0, 0.0 };
    const double q  = DynamicPressure(1.225, airspeed);
    const Vec3d F   = DragForce(q, 1.0, 1.0, airspeed); // opposes airspeed

    // CoP one metre AFT of the CoM (nose at +x, so aft is -x).
    const Vec3d tauAft = AeroTorqueAboutCoM(Vec3d{ -1.0, 0.0, 0.0 }, F);
    // CoP one metre FORWARD of the CoM.
    const Vec3d tauFwd = AeroTorqueAboutCoM(Vec3d{ +1.0, 0.0, 0.0 }, F);

    // +z torque rotates +x toward +y, i.e. the body toward the velocity vector,
    // reducing AoA: RESTORING. Aft is stable, forward is unstable.
    CHECK(tauAft.z > 0.0);
    CHECK(tauFwd.z < 0.0);
    // Reflected offsets give reflected torque: same magnitude, opposite sign.
    CHECK_APPROX_EPS(tauAft.z, -tauFwd.z, 1e-9);
}

// =============================================================================
// (H) HEATING IS FIREWALLED — Sutton-Graves flux is COSMETIC. Calling it every
//     step must leave the trajectory BIT-IDENTICAL to the run that never calls it,
//     even though the flux it returns is large and nonzero. A cosmetic term that
//     silently fed the integrator would change the path; this proves it cannot.
// =============================================================================
TEST_CASE(Atmosphere_SuttonGravesHeating_DoesNotPerturbTrajectory)
{
    const double rho = 1.225, cd = 1.0, area = 2.0, mass = 500.0, noseR = 0.5;
    const double dt = 0.01;
    const Vec3d v0{ 2000.0, 0.0, 0.0 }; // reentry-ish speed

    auto run = [&](bool callHeating) {
        Vec3d v = v0;
        double lastFlux = 0.0;
        for (int i = 0; i < 5000; ++i)
        {
            v = SemiImplicitDragAirspeed(v, rho, cd, area, mass, dt);
            if (callHeating)
                lastFlux = SuttonGravesHeatFlux(rho, noseR, v); // discarded on purpose
        }
        return std::pair<Vec3d, double>{ v, lastFlux };
    };

    const auto without = run(false);
    const auto with    = run(true);

    // Bit-identical trajectory whether or not heating was computed.
    CHECK_EQ(with.first.x, without.first.x);
    CHECK_EQ(with.first.y, without.first.y);
    CHECK_EQ(with.first.z, without.first.z);
    // And the heating call really did compute a large, nonzero flux (so the
    // firewall is meaningful, not vacuous because the number was ~0).
    CHECK(with.second > 0.0);
    // Zero density / zero nose radius are guarded (no division blow-up).
    CHECK_EQ(SuttonGravesHeatFlux(0.0, noseR, v0), 0.0);
    CHECK_EQ(SuttonGravesHeatFlux(rho, 0.0, v0), 0.0);
}

// =============================================================================
// (I) DRAG DIRECTION AND ZERO-AIRSPEED CONTROL — drag opposes airspeed exactly,
//     and is EXACTLY zero for zero airspeed (the seam that lets the force sum to
//     nothing above the atmosphere without a special case).
// =============================================================================
TEST_CASE(Atmosphere_DragForce_OpposesAirspeed_ZeroWhenStill)
{
    const Vec3d airspeed{ 10.0, 0.0, 0.0 };
    const double q = DynamicPressure(1.225, airspeed);
    const Vec3d F  = DragForce(q, 1.0, 1.0, airspeed);
    CHECK(F.x < 0.0);                       // opposes +x motion
    CHECK_APPROX_EPS(F.y, 0.0, 1e-12);
    CHECK_APPROX_EPS(F.z, 0.0, 1e-12);
    // Anti-parallel to airspeed: F . v = -|F||v|.
    CHECK_APPROX(F.Dot(airspeed), -F.Length() * airspeed.Length());

    // Zero airspeed -> exactly zero drag, no NaN from normalising a zero vector.
    const Vec3d z = DragForce(0.0, 1.0, 1.0, Vec3d{ 0, 0, 0 });
    CHECK_EQ(z.LengthSq(), 0.0);
}

// =============================================================================
// (J) TRANSONIC DRAG-RISE SHAPE — Cd(M) is subsonic-flat, peaks AT M = 1, and is
//     smooth (a Gaussian bump, no lookup-table step to inject a force jump).
// =============================================================================
TEST_CASE(Atmosphere_DragCoefficient_PeaksAtMachOne)
{
    const double cd0 = 0.3;
    const double subsonic = DragCoefficientAtMach(cd0, 0.3);
    const double peak     = DragCoefficientAtMach(cd0, 1.0);
    const double super    = DragCoefficientAtMach(cd0, 2.5);
    CHECK(peak > subsonic);                 // transonic rise
    CHECK(peak > super);                    // supersonic falloff past the peak
    CHECK(RelErr(subsonic, cd0) < 0.05);    // subsonic ~ cd0 (bump ~0 far from M=1)
    CHECK(super > 0.0);                     // hypersonic plateau stays positive

    // Symmetric about M=1: the bump alone is the same a little below and above.
    // (Below M=1 there is no supersonic falloff term, so compare bump-only points.)
    CHECK_APPROX(DragCoefficientAtMach(cd0, 0.9),
                 cd0 * (1.0 + std::exp(-((0.9 - 1.0) * (0.9 - 1.0)) / (2.0 * 0.15 * 0.15))));
}

// =============================================================================
// (K) THE CEILING LIMIT IS CONTINUOUS - exact zero at the ceiling is not enough.
//     Density and therefore dynamic pressure must approach zero from below rather
//     than retain a finite value until a hard branch cuts it off.
// =============================================================================
TEST_CASE(Atmosphere_CeilingApproach_IsContinuousAndMonotone)
{
    const AtmosphereModel models[] = {
        AtmosphereModel::EarthUSSA76(),
        AtmosphereModel::ExponentialBody(1.225, 8500.0, 100000.0),
    };

    for (const AtmosphereModel& model : models)
    {
        const double c = model.ceiling;
        const AtmosphereState far  = SampleAtmosphere(model, c - 1000.0);
        const AtmosphereState near = SampleAtmosphere(model, c - 1.0e-3);
        const AtmosphereState at   = SampleAtmosphere(model, c);

        CHECK(far.density > 0.0);
        CHECK(near.density >= 0.0);
        CHECK(near.density < far.density * 1.0e-6);
        CHECK_EQ(at.density, 0.0);
        CHECK(near.pressure < far.pressure * 1.0e-6);

        const Vec3d v{ 100.0, 0.0, 0.0 };
        CHECK(DynamicPressure(near.density, v) < DynamicPressure(far.density, v) * 1.0e-6);
    }
}

// =============================================================================
// (L) PUBLIC NUMERICAL FIREWALL - invalid inputs and finite values whose direct
//     products overflow must never emit NaN/Inf into a simulation accumulator.
// =============================================================================
TEST_CASE(Atmosphere_PublicFunctions_ContainInvalidAndExtremeInputs)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const Vec3d huge{ 1.0e200, -1.0e200, 1.0e200 };
    const Vec3d corrupt{ inf, 0.0, 0.0 };

    const AtmosphereState invalidAltitude =
        SampleAtmosphere(AtmosphereModel::EarthUSSA76(), nan);
    CHECK(std::isfinite(invalidAltitude.temperature));
    CHECK(std::isfinite(invalidAltitude.pressure));
    CHECK(std::isfinite(invalidAltitude.density));
    CHECK(std::isfinite(invalidAltitude.speedOfSound));

    const AtmosphereModel invalidModel =
        AtmosphereModel::ExponentialBody(-1.0, 0.0, nan);
    const AtmosphereState invalidSample = SampleAtmosphere(invalidModel, 0.0);
    CHECK_EQ(invalidSample.density, 0.0);
    CHECK_EQ(invalidSample.pressure, 0.0);
    CHECK(std::isfinite(GeometricToGeopotential(-kEarthGeopotentialR)));

    const double qHuge = DynamicPressure(1.225, huge);
    const double machHuge = MachNumber(huge, 340.0);
    CHECK(std::isfinite(qHuge));
    CHECK(qHuge >= 0.0);
    CHECK(std::isfinite(machHuge));
    CHECK_EQ(DynamicPressure(-1.0, huge), 0.0);
    CHECK_EQ(MachNumber(huge, nan), 0.0);
    CHECK_EQ(DragCoefficientAtMach(-0.3, 1.0), 0.0);
    CHECK_EQ(AngleOfAttackDragFactor(1.0, -10.0), 1.0);
    CHECK_EQ(LiftCoefficient(1.0, nan, 0.25), 0.0);
    CHECK(std::isfinite(AngleOfAttackDragFactor(1.0,
                                                std::numeric_limits<double>::max())));
    CHECK(std::isfinite(DragCoefficientAtMach(0.3,
                                               std::numeric_limits<double>::max())));

    const Vec3d relativeHuge = AirspeedVector(
        Vec3d{ std::numeric_limits<double>::max(), 0.0, 0.0 },
        Vec3d{ -std::numeric_limits<double>::max(), 0.0, 0.0 });
    CHECK(std::isfinite(relativeHuge.x));
    CHECK(relativeHuge.x > 0.0);
    CHECK(std::isfinite(MachNumber(huge, std::numeric_limits<double>::denorm_min())));

    const Vec3d dragHuge = DragForce(qHuge, 2.0, 2.0, huge);
    const Vec3d liftHuge = LiftForce(qHuge, 2.0, 2.0, Vec3d{ 0.0, 3.0, 0.0 });
    const Vec3d torqueHuge = AeroTorqueAboutCoM(huge, dragHuge);
    CHECK(std::isfinite(dragHuge.x) && std::isfinite(dragHuge.y) && std::isfinite(dragHuge.z));
    CHECK(std::isfinite(liftHuge.x) && std::isfinite(liftHuge.y) && std::isfinite(liftHuge.z));
    CHECK(std::isfinite(torqueHuge.x) && std::isfinite(torqueHuge.y) && std::isfinite(torqueHuge.z));
    const Vec3d signedTorque = AeroTorqueAboutCoM(
        Vec3d{ std::numeric_limits<double>::max(), 0.0, 0.0 },
        Vec3d{ 0.0, -std::numeric_limits<double>::max(), 0.0 });
    CHECK(std::isfinite(signedTorque.z));
    CHECK(signedTorque.z < 0.0);

    const Vec3d contained = SemiImplicitDragAirspeed(corrupt, 1.225, 1.0, 1.0, 1.0, 1.0);
    const Vec3d extremeDrag = SemiImplicitDragAirspeed(
        huge, std::numeric_limits<double>::max(), 1.0, 1.0, 1.0,
        std::numeric_limits<double>::max());
    CHECK_EQ(contained.LengthSq(), 0.0);
    CHECK(std::isfinite(extremeDrag.x) && std::isfinite(extremeDrag.y) &&
          std::isfinite(extremeDrag.z));
    CHECK(std::isfinite(DragTimeConstant(std::numeric_limits<double>::max(),
                                         1.0, 1.0, 1.0, huge)));
    CHECK(std::isfinite(BallisticCoefficient(std::numeric_limits<double>::max(),
                                              1.0, 1.0)));
    CHECK(std::isfinite(TerminalVelocity(nan, 9.81, 1.225, 1.0, 1.0)));
    CHECK(std::isfinite(SuttonGravesHeatFlux(1.225, 0.5, huge)));
    CHECK_EQ(SuttonGravesHeatFlux(1.225, 0.5, corrupt), 0.0);
}

// =============================================================================
// (M) INVALID DRAG PARAMETERS ARE NEUTRAL - a negative coefficient or area must
//     not turn a dissipative update into acceleration or reverse the velocity.
// =============================================================================
TEST_CASE(Atmosphere_SemiImplicitDrag_InvalidParametersCannotAddEnergy)
{
    const Vec3d v{ 100.0, -20.0, 5.0 };
    const Vec3d badCd = SemiImplicitDragAirspeed(v, 1.225, -1.0, 2.0, 10.0, 1.0);
    const Vec3d badArea = SemiImplicitDragAirspeed(v, 1.225, 1.0, -2.0, 10.0, 1.0);
    const Vec3d badDt = SemiImplicitDragAirspeed(v, 1.225, 1.0, 2.0, 10.0,
                                                 std::numeric_limits<double>::quiet_NaN());
    CHECK_EQ(badCd.x, v.x);
    CHECK_EQ(badCd.y, v.y);
    CHECK_EQ(badCd.z, v.z);
    CHECK_EQ(badArea.x, v.x);
    CHECK_EQ(badArea.y, v.y);
    CHECK_EQ(badArea.z, v.z);
    CHECK_EQ(badDt.x, v.x);
    CHECK_EQ(badDt.y, v.y);
    CHECK_EQ(badDt.z, v.z);
}
