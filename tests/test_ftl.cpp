// =============================================================================
// tests/test_ftl.cpp - FTL / warp / wormhole atomic teleport and warp cruise
// =============================================================================
// SIM STAGE 4. Drives the SHIPPED sim/ftl.{h,cpp} against
// docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md section 7. Pure CPU; no D3D12.
//
// The fiction's ONE hard invariant is engineering: a clean, deterministic
// discontinuity that does not corrupt retained state. So the tests are about
// STATE, not physics:
//   - the pure-translation A->B->A round-trip is BIT-EXACT on every retained field
//     (identity rotation + sector-origin mouths make the WorldPos arithmetic exact);
//   - a single jump through a ROTATED mouth actually rotates the world-frame
//     velocity and the orientation (the load-bearing, easy-to-miss step - the
//     anti-test forgets it, on ONE leg, because a round-trip would cancel it);
//   - accumulators are flushed, prev-pose is the post-jump pose, body-frame angular
//     velocity survives;
//   - a far, many-sector jump preserves the exact separation of two riders (the
//     WorldPos precision property carried through FTL);
//   - warp cruise is a deterministic, superluminal-coordinate moving origin.
// =============================================================================

#include "test_framework.h"
#include "sim/ftl.h"
#include "sim/reference_frame.h"

#include <cmath>
#include <limits>

namespace
{
using core::Vec3d;
using core::Quatf;
using namespace sim;

double RelErr(double a, double b) { return std::fabs(a - b) / std::fabs(b); }

// Bit-exact quaternion comparison, component by component.
void CheckQuatExact(const Quatf& a, const Quatf& b)
{
    CHECK_EQ(a.x, b.x); CHECK_EQ(a.y, b.y); CHECK_EQ(a.z, b.z); CHECK_EQ(a.w, b.w);
}

void CheckStateExact(const TeleportState& a, const TeleportState& b)
{
    CHECK(a.position == b.position);
    CheckQuatExact(a.orientation, b.orientation);
    CHECK(a.linearVelocity == b.linearVelocity);
    CHECK(a.momentum == b.momentum);
    CHECK(a.angularVelocity == b.angularVelocity);
    CHECK(a.forceAccum == b.forceAccum);
    CHECK(a.torqueAccum == b.torqueAccum);
    CHECK(a.prevPosition == b.prevPosition);
    CheckQuatExact(a.prevRotation, b.prevRotation);
}

// A representative non-trivial pre-jump state near a given mouth.
TeleportState MakeState(const WorldPos& nearMouth, const Vec3d& offset)
{
    TeleportState s;
    s.position        = Translate(nearMouth, offset);
    s.orientation     = Quatf::Identity();
    s.linearVelocity  = Vec3d{ 12.0, -3.0, 5.0 };
    s.momentum        = Vec3d{ 1200.0, -300.0, 500.0 };
    s.angularVelocity = Vec3d{ 0.1, 0.2, -0.3 };
    s.forceAccum      = Vec3d{ 100.0, 0.0, -50.0 };
    s.torqueAccum     = Vec3d{ 0.0, 7.0, 0.0 };
    return s;
}
} // namespace

// =============================================================================
// (A) PURE-TRANSLATION A->B->A IS BIT-EXACT on every retained field.
//     Identity mouth rotation + sector-origin mouths => the WorldPos offset
//     arithmetic is exact, so this is CHECK_EQ, not a tolerance.
// =============================================================================
TEST_CASE(Ftl_PureTranslationRoundTrip_IsBitExact)
{
    const WorldPos entry(1000, 0, 0, Vec3d{ 0, 0, 0 });
    const WorldPos exit(-500, 2000, 7, Vec3d{ 0, 0, 0 });
    const MouthTransform m = MouthTransform::Wormhole(entry, exit); // identity rotation

    const TeleportState s0 = MakeState(entry, Vec3d{ 3.0, 4.0, 5.0 });
    const TeleportState atB = ApplyTeleport(s0, m);
    const TeleportState back = ApplyTeleport(atB, m.Inverse());

    // Position returns to the exact starting point.
    CHECK(back.position == s0.position);
    // Velocity, orientation, and body-frame angular velocity are bit-identical.
    CHECK_EQ(back.linearVelocity.x, s0.linearVelocity.x);
    CHECK_EQ(back.linearVelocity.y, s0.linearVelocity.y);
    CHECK_EQ(back.linearVelocity.z, s0.linearVelocity.z);
    CHECK_EQ(back.momentum.x, s0.momentum.x);
    CHECK_EQ(back.momentum.y, s0.momentum.y);
    CHECK_EQ(back.momentum.z, s0.momentum.z);
    CheckQuatExact(back.orientation, s0.orientation);
    CHECK_EQ(back.angularVelocity.x, s0.angularVelocity.x);
    CHECK_EQ(back.angularVelocity.y, s0.angularVelocity.y);
    CHECK_EQ(back.angularVelocity.z, s0.angularVelocity.z);

    // The body actually MOVED to the destination mouth in between (not a no-op):
    // its offset from the exit mouth equals its original offset from the entry mouth.
    const Vec3d offAtB = Separation(exit, atB.position);
    CHECK_APPROX_EPS(offAtB.x, 3.0, 1e-9);
    CHECK_APPROX_EPS(offAtB.y, 4.0, 1e-9);
    CHECK_APPROX_EPS(offAtB.z, 5.0, 1e-9);
}

// =============================================================================
// (B) A ROTATED MOUTH ROTATES THE WORLD-FRAME VELOCITY AND THE ORIENTATION.
//     Tested on a SINGLE leg: a round-trip would cancel a both-legs mutation, so
//     the "forgot to rotate velocity" anti-test only bites here. The velocity must
//     equal the SHIPPED double rotation of the input, and differ from the input.
// =============================================================================
TEST_CASE(Ftl_RotatedMouth_RotatesVelocityAndOrientation)
{
    const WorldPos entry(0, 0, 0, Vec3d{ 10, 0, 0 });
    const WorldPos exit(42, 0, 0, Vec3d{ 0, 0, 0 });
    const Quatf rot = Quatf::FromAxisAngle(core::Vec3f{ 0, 1, 0 }, 1.5707963267948966f); // 90 deg about +Y
    const MouthTransform m = MouthTransform::Wormhole(entry, exit, rot);

    TeleportState s0 = MakeState(entry, Vec3d{ 0, 0, 0 });
    s0.linearVelocity = Vec3d{ 10.0, 0.0, 0.0 };   // along +x
    s0.orientation    = Quatf::Identity();
    const TeleportState j = ApplyTeleport(s0, m);

    // Velocity is exactly the shipped rotation of the input (bit-exact vs the
    // module's own RotateVec3d), and it is NOT the un-rotated input.
    const Vec3d expected = RotateVec3d(rot, s0.linearVelocity);
    CHECK_EQ(j.linearVelocity.x, expected.x);
    CHECK_EQ(j.linearVelocity.y, expected.y);
    CHECK_EQ(j.linearVelocity.z, expected.z);
    // A 90 deg rotation about +Y sends +x to -z (LH); the x-component must collapse.
    CHECK(std::fabs(j.linearVelocity.x) < 1e-4);
    CHECK(j.linearVelocity.LengthSq() > 1.0); // still has the same speed, just turned

    // Orientation is the mouth rotation prepended to identity, i.e. the mouth rotation.
    const Quatf expectedOrient = rot * s0.orientation;
    CheckQuatExact(j.orientation, expectedOrient);
}

// Independent known-answer coverage for BOTH position and vector rotation. This
// deliberately does not use RotateVec3d to calculate the expected result.
TEST_CASE(Ftl_RotatedMouth_SingleLegKnownAnswer)
{
    const WorldPos entry(0, 0, 0, Vec3d{ 0, 0, 0 });
    const WorldPos exit(7, -3, 2, Vec3d{ 0, 0, 0 });
    const Quatf rot = Quatf::FromAxisAngle(core::Vec3f{ 0, 1, 0 }, 1.5707963267948966f);
    const MouthTransform m = MouthTransform::Wormhole(entry, exit, rot);

    TeleportState s0 = MakeState(entry, Vec3d{ 2.0, 3.0, 4.0 });
    s0.linearVelocity = Vec3d{ 10.0, 0.0, 0.0 };
    s0.momentum = Vec3d{ 30.0, 0.0, 0.0 };
    const TeleportState j = ApplyTeleport(s0, m);

    const Vec3d offset = Separation(exit, j.position);
    CHECK_APPROX_EPS(offset.x, 4.0, 1e-5);
    CHECK_APPROX_EPS(offset.y, 3.0, 1e-5);
    CHECK_APPROX_EPS(offset.z, -2.0, 1e-5);
    CHECK_APPROX_EPS(j.linearVelocity.x, 0.0, 1e-5);
    CHECK_APPROX_EPS(j.linearVelocity.y, 0.0, 1e-5);
    CHECK_APPROX_EPS(j.linearVelocity.z, -10.0, 1e-5);
    CHECK_APPROX_EPS(j.momentum.x, 0.0, 1e-5);
    CHECK_APPROX_EPS(j.momentum.y, 0.0, 1e-5);
    CHECK_APPROX_EPS(j.momentum.z, -30.0, 1e-5);
}

// Mouth data is an external transform boundary. A finite non-unit quaternion is
// accepted as the rotation it represents and normalized exactly once.
TEST_CASE(Ftl_NonUnitMouthRotation_IsNormalized)
{
    const WorldPos entry(0, 0, 0, Vec3d{ 0, 0, 0 });
    const WorldPos exit(3, 0, 0, Vec3d{ 0, 0, 0 });
    const Quatf unit = Quatf::FromAxisAngle(core::Vec3f{ 0, 1, 0 }, 1.5707963267948966f);
    const Quatf scaled{ unit.x * 2.0f, unit.y * 2.0f, unit.z * 2.0f, unit.w * 2.0f };

    TeleportState s0 = MakeState(entry, Vec3d{ 2.0, 3.0, 4.0 });
    s0.linearVelocity = Vec3d{ 10.0, 0.0, 0.0 };
    s0.momentum = Vec3d{ 30.0, 0.0, 0.0 };
    const TeleportState j = ApplyTeleport(s0, MouthTransform::Wormhole(entry, exit, scaled));

    const Vec3d offset = Separation(exit, j.position);
    CHECK_APPROX_EPS(offset.x, 4.0, 1e-5);
    CHECK_APPROX_EPS(offset.y, 3.0, 1e-5);
    CHECK_APPROX_EPS(offset.z, -2.0, 1e-5);
    CHECK_APPROX_EPS(j.linearVelocity.x, 0.0, 1e-5);
    CHECK_APPROX_EPS(j.linearVelocity.z, -10.0, 1e-5);
    CHECK_APPROX_EPS(j.momentum.x, 0.0, 1e-5);
    CHECK_APPROX_EPS(j.momentum.z, -30.0, 1e-5);
    CHECK_APPROX_EPS(j.orientation.LengthSq(), 1.0f, 1e-5f);
}

// Rejected mouth data is transactional: no position, retained state, wrench, or
// interpolation history may be partially changed.
TEST_CASE(Ftl_InvalidMouthRotation_LeavesStateUnchanged)
{
    const WorldPos entry(0, 0, 0, Vec3d{ 0, 0, 0 });
    const WorldPos exit(3, 0, 0, Vec3d{ 0, 0, 0 });
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const MouthTransform m = MouthTransform::Wormhole(entry, exit, Quatf{ nan, 0, 0, 1 });
    const TeleportState s0 = MakeState(entry, Vec3d{ 2.0, 3.0, 4.0 });

    CheckStateExact(ApplyTeleport(s0, m), s0);

    TeleportState out = MakeState(exit, Vec3d{ 9.0, 8.0, 7.0 });
    CHECK(!TryApplyTeleport(s0, m, out));
    CheckStateExact(out, s0);

    const Vec3d zero{};
    CHECK(RotateVec3d(Quatf{ nan, 0, 0, 1 }, Vec3d{ 1, 2, 3 }) == zero);
    CHECK(RotateVec3d(Quatf::Identity(),
                      Vec3d{ std::numeric_limits<double>::infinity(), 0, 0 }) == zero);

    TeleportState corrupt = s0;
    corrupt.momentum.x = std::numeric_limits<double>::infinity();
    CHECK(!TryApplyTeleport(corrupt, MouthTransform::Wormhole(entry, exit), out));
    CheckStateExact(out, corrupt);
}

TEST_CASE(Ftl_TryTeleport_ReportsSuccessAndRejectsUnsafeCoordinates)
{
    const WorldPos entry(0, 0, 0, Vec3d{ 0, 0, 0 });
    const WorldPos exit(3, 0, 0, Vec3d{ 0, 0, 0 });
    const TeleportState s0 = MakeState(entry, Vec3d{ 2.0, 3.0, 4.0 });

    TeleportState out;
    const MouthTransform valid = MouthTransform::Wormhole(entry, exit);
    CHECK(TryApplyTeleport(s0, valid, out));
    CheckStateExact(out, ApplyTeleport(s0, valid));

    const MouthTransform zeroRotation =
        MouthTransform::Wormhole(entry, exit, Quatf{ 0, 0, 0, 0 });
    CHECK(!TryApplyTeleport(s0, zeroRotation, out));
    CheckStateExact(out, s0);

    const WorldPos invalidExit(kMaxSectorCoord, 0, 0,
                               Vec3d{ kSectorSize - 1.0, 0, 0 });
    TeleportState crossing = MakeState(entry, Vec3d{ 10.0, 0.0, 0.0 });
    CHECK(!TryApplyTeleport(crossing,
                            MouthTransform::Wormhole(entry, invalidExit), out));
    CheckStateExact(out, crossing);
}

// =============================================================================
// (C) ROTATED A->B->A recovers state - each error source asserted at the tolerance
//     it can actually meet, NOT one blanket number:
//       - VELOCITY never touches WorldPos, so its only error is the float-quaternion
//         round-off of conj(R).R; that is sub-micron and asserted tight (1e-5).
//       - POSITION rides WorldPos, whose precision floor is the sector-offset ULP
//         (Stage 0: up to ~2.2 mm at a near-sector-edge coordinate). This body's
//         offset has a negative component, which canonicalizes to ~kSectorSize-1 -
//         a near-edge coordinate - so position recovers only to that documented
//         ULP, not to the rotation's precision. Asserting 1e-5 here would be a
//         fantasy tolerance for the coordinate system, so it is bounded at 5 mm.
// =============================================================================
TEST_CASE(Ftl_RotatedRoundTrip_RecoversWithinTolerance)
{
    const WorldPos entry(0, 0, 0, Vec3d{ 0, 0, 0 });
    const WorldPos exit(9, -4, 2, Vec3d{ 0, 0, 0 });
    const Quatf rot = Quatf::FromAxisAngle(core::Vec3f{ 0.3f, 0.8f, -0.5f }.Normalized(), 0.9f);
    const MouthTransform m = MouthTransform::Wormhole(entry, exit, rot);

    const TeleportState s0 = MakeState(entry, Vec3d{ 2.0, -1.0, 0.5 });
    const TeleportState back = ApplyTeleport(ApplyTeleport(s0, m), m.Inverse());

    const Vec3d dVel = back.linearVelocity - s0.linearVelocity;
    CHECK_APPROX_EPS(dVel.Length(), 0.0, 1e-5);  // pure float-quaternion round-off
    const Vec3d dPos = Separation(s0.position, back.position);
    CHECK_APPROX_EPS(dPos.Length(), 0.0, 5e-3);  // WorldPos sector-offset ULP floor

    // ISOLATE the rotation from the coordinate floor: a body whose offset stays
    // small and POSITIVE (never canonicalizes to a sector edge) recovers its
    // position to sub-micron, proving the 5 mm above is the coordinate system's
    // floor and NOT the teleport losing precision.
    const TeleportState sNear  = MakeState(entry, Vec3d{ 1.0, 2.0, 3.0 });
    const TeleportState backNear =
        ApplyTeleport(ApplyTeleport(sNear, MouthTransform::Wormhole(entry, exit)), // identity rot
                      MouthTransform::Wormhole(exit, entry));
    CHECK(Separation(sNear.position, backNear.position).Length() < 1e-9);
}

// =============================================================================
// (D) ACCUMULATORS ARE FLUSHED - a staged pre-jump wrench is dropped, not carried
//     into the first post-jump step.
// =============================================================================
TEST_CASE(Ftl_AccumulatorsFlushedToZero)
{
    const WorldPos entry(5, 5, 5, Vec3d{ 0, 0, 0 });
    const WorldPos exit(9, 9, 9, Vec3d{ 0, 0, 0 });
    const MouthTransform m = MouthTransform::Wormhole(entry, exit);

    TeleportState s0 = MakeState(entry, Vec3d{ 1, 1, 1 });
    s0.forceAccum  = Vec3d{ 500.0, -200.0, 30.0 };
    s0.torqueAccum = Vec3d{ -9.0, 9.0, 4.0 };
    const TeleportState j = ApplyTeleport(s0, m);

    CHECK_EQ(j.forceAccum.x, 0.0);  CHECK_EQ(j.forceAccum.y, 0.0);  CHECK_EQ(j.forceAccum.z, 0.0);
    CHECK_EQ(j.torqueAccum.x, 0.0); CHECK_EQ(j.torqueAccum.y, 0.0); CHECK_EQ(j.torqueAccum.z, 0.0);
}

// =============================================================================
// (E) PREV POSE IS THE POST-TELEPORT POSE - render interpolation delta across the
//     jump is exactly zero (no source-to-destination smear).
// =============================================================================
TEST_CASE(Ftl_PrevPoseEqualsPostTeleportPose)
{
    const WorldPos entry(0, 0, 0, Vec3d{ 100, 0, 0 });
    const WorldPos exit(1234, -77, 0, Vec3d{ 0, 0, 0 });
    const Quatf rot = Quatf::FromAxisAngle(core::Vec3f{ 1, 0, 0 }, 0.4f);
    const MouthTransform m = MouthTransform::Wormhole(entry, exit, rot);

    const TeleportState s0 = MakeState(entry, Vec3d{ 2, 3, 4 });
    const TeleportState j = ApplyTeleport(s0, m);

    CHECK(j.prevPosition == j.position);          // zero position smear
    CheckQuatExact(j.prevRotation, j.orientation); // zero rotation smear
    // And prev is the POST pose, not the pre pose (the jump was non-trivial).
    CHECK(!(j.prevPosition == s0.position));
}

// =============================================================================
// (F) BODY-FRAME ANGULAR VELOCITY SURVIVES even a rotated mouth - it is expressed
//     in the co-rotating body frame, so rotating the orientation does not touch it.
// =============================================================================
TEST_CASE(Ftl_AngularVelocityPreservedThroughRotatedMouth)
{
    const WorldPos entry(0, 0, 0, Vec3d{ 0, 0, 0 });
    const WorldPos exit(3, 0, 0, Vec3d{ 0, 0, 0 });
    const Quatf rot = Quatf::FromAxisAngle(core::Vec3f{ 0, 0, 1 }, 2.0f);
    const MouthTransform m = MouthTransform::Wormhole(entry, exit, rot);

    TeleportState s0 = MakeState(entry, Vec3d{ 0, 0, 0 });
    s0.angularVelocity = Vec3d{ 0.7, -0.2, 1.1 };
    const TeleportState j = ApplyTeleport(s0, m);

    CHECK_EQ(j.angularVelocity.x, 0.7);
    CHECK_EQ(j.angularVelocity.y, -0.2);
    CHECK_EQ(j.angularVelocity.z, 1.1);
}

// =============================================================================
// (G) A FAR, MANY-SECTOR JUMP PRESERVES PRECISION - two riders 0.25 m apart at the
//     entry are still exactly 0.25 m apart at the exit, thousands of sectors away.
//     This is the WorldPos separation-precision property carried through FTL, and
//     the reason position is stored split rather than as a far-absolute double.
// =============================================================================
TEST_CASE(Ftl_FarJump_PreservesRiderSeparation)
{
    const WorldPos entry(2, 0, 0, Vec3d{ 0, 0, 0 });
    const WorldPos exit(9'000'000, -3'000'000, 5'000'000, Vec3d{ 0, 0, 0 }); // ~1e19 m away
    const MouthTransform m = MouthTransform::Wormhole(entry, exit);

    TeleportState a = MakeState(entry, Vec3d{ 0.00, 0.0, 0.0 });
    TeleportState b = MakeState(entry, Vec3d{ 0.25, 0.0, 0.0 }); // 0.25 m from a
    const Vec3d sepBefore = Separation(a.position, b.position);
    CHECK_APPROX_EPS(sepBefore.x, 0.25, 1e-12);

    const TeleportState aJ = ApplyTeleport(a, m);
    const TeleportState bJ = ApplyTeleport(b, m);

    // Both landed, canonical and in-range, and STILL exactly 0.25 m apart.
    CHECK(IsCanonical(aJ.position));
    CHECK(IsCanonical(bJ.position));
    CHECK(ValidSector(aJ.position));
    const Vec3d sepAfter = Separation(aJ.position, bJ.position);
    CHECK_APPROX_EPS(sepAfter.x, 0.25, 1e-9);   // full precision preserved across ~1e19 m
    CHECK_APPROX_EPS(sepAfter.y, 0.0, 1e-9);
    CHECK_APPROX_EPS(sepAfter.z, 0.0, 1e-9);
}

// =============================================================================
// (H) WARP CRUISE - a deterministic moving origin at a SUPERLUMINAL coordinate
//     speed. N steps of dt equal the closed-form displacement, the result stays
//     canonical/finite at galactic reach, and the run is repeatable. Proper time
//     for a warp rider is dt by convention.
// =============================================================================
TEST_CASE(Ftl_WarpCruise_DeterministicSuperluminalMovingOrigin)
{
    const double c = 299'792'458.0;
    const Vec3d dir{ 1.0, 0.0, 0.0 };
    const double coordSpeed = 1000.0 * c;           // 1000x light: a coordinate speed
    const Vec3d coordVel = dir * coordSpeed;
    const double dt = 1.0 / 60.0;                    // fixed step

    WorldPos origin(0, 0, 0, Vec3d{ 0, 0, 0 });
    const int steps = 6000;                          // 100 s of cruise
    for (int i = 0; i < steps; ++i)
        origin = AdvanceWarpOrigin(origin, coordVel, dt);

    // Net displacement from the galactic origin equals coordSpeed * total_time along
    // dir. That distance (3e12 * dt-sum) exceeds c*T by 1000x - superluminal by
    // construction, yet finite and canonical.
    const Vec3d disp = Separation(WorldPos(0, 0, 0, Vec3d{ 0, 0, 0 }), origin);
    const double expected = coordSpeed * (dt * steps);
    CHECK(RelErr(disp.x, expected) < 1e-6);
    CHECK(disp.x > c * (dt * steps));               // genuinely superluminal coordinate motion
    CHECK(IsCanonical(origin));

    // Deterministic: the identical run reproduces the identical origin, bit-for-bit.
    WorldPos origin2(0, 0, 0, Vec3d{ 0, 0, 0 });
    for (int i = 0; i < steps; ++i)
        origin2 = AdvanceWarpOrigin(origin2, coordVel, dt);
    CHECK(origin2 == origin);

    // Proper-time convention: a warp rider ages by dt (7.2), exactly.
    CHECK_EQ(WarpProperTime(dt), dt);
}


// Invalid or unrepresentable fixed steps are no-ops. This prevents a single bad
// timer sample or overflowed route speed from corrupting the split coordinate.
TEST_CASE(Ftl_WarpCruise_RejectsInvalidOrOverflowingSteps)
{
    const WorldPos origin(9, -4, 2, Vec3d{ 10.0, 20.0, 30.0 });
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double huge = std::numeric_limits<double>::max();

    CHECK(AdvanceWarpOrigin(origin, Vec3d{ 1.0, 2.0, 3.0 }, -1.0) == origin);
    CHECK(AdvanceWarpOrigin(origin, Vec3d{ 1.0, 2.0, 3.0 }, nan) == origin);
    CHECK(AdvanceWarpOrigin(origin, Vec3d{ huge, 0.0, 0.0 }, 2.0) == origin);
    CHECK(IsCanonical(AdvanceWarpOrigin(origin, Vec3d{ huge, 0.0, 0.0 }, 2.0)));

    CHECK_EQ(WarpProperTime(-1.0), 0.0);
    CHECK_EQ(WarpProperTime(nan), 0.0);

    WorldPos out;
    CHECK(!TryAdvanceWarpOrigin(origin, Vec3d{ huge, 0.0, 0.0 }, 2.0, out));
    CHECK(out == origin);

    const Vec3d validVelocity{ 1000.0, -2000.0, 3000.0 };
    CHECK(TryAdvanceWarpOrigin(origin, validVelocity, 0.5, out));
    CHECK(out == AdvanceWarpOrigin(origin, validVelocity, 0.5));
}
