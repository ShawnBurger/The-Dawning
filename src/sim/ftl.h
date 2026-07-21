#pragma once
// =============================================================================
// sim/ftl.h - FTL / warp / wormhole: the consistent fiction, cleanest cut
// =============================================================================
// SIM STAGE 4, per docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md section 7.
// CPU-only, GPU-free. Includes ONLY reference_frame.h (Stage 0) + core/types.h,
// so it links into BOTH TheDawningV3 and TheDawningTests and the unit tests drive
// the SHIPPED arithmetic.
//
// FTL, warp, and wormhole are INTERNALLY-CONSISTENT FICTION, not real physics
// (architecture 0/7.1). Their ONE hard invariant is engineering, not physics: a
// clean, deterministic frame/position discontinuity that does NOT corrupt retained
// state. Wormhole teleport and warp enter/exit reduce to the SAME object - an
// atomic transform over the full retained-state set (7.3) - so this module
// implements that one operation and the warp-space moving origin (7.2).
//
// WHAT IS BIT-EXACT AND WHAT IS NOT (measured, not claimed):
//   - The retained-state LOGIC (velocity's frame rotated, accumulators flushed,
//     prev-pose set to the post-jump pose, body-frame angular velocity preserved)
//     is bit-exact under an identity mouth rotation.
//   - POSITION round-trips bit-exactly when the mouths sit at sector origins (the
//     WorldPos offset arithmetic is then exact); with a large intra-sector offset
//     it round-trips only to the sector-offset ULP (architecture 1.4), which is
//     correct and finite, never a blow-up. A ROTATED mouth adds float-quaternion
//     round-off, so a rotated round-trip is tolerance-bounded, NOT bit-exact. The
//     tests assert each of these at the tolerance it can actually meet.
//
// The engine stores orientation as Quatf (ecs Transform.rotation), so orientation
// is float here too; only the world-frame VELOCITY rotation is done in double.

#include "reference_frame.h"
#include "core/types.h"

namespace sim
{

using core::Quatf;

// Rotate a DOUBLE world vector by a finite quaternion, normalizing the quaternion
// at this public boundary. Invalid input returns the neutral zero vector. The
// arithmetic promotes the quaternion to double and never narrows the Vec3d.
Vec3d RotateVec3d(const Quatf& q, const Vec3d& v);

// -----------------------------------------------------------------------------
// MouthTransform - the wormhole mouth pair / warp enter-exit rigid transform
// -----------------------------------------------------------------------------
// A pose expressed relative to `entry` is reproduced relative to `exit`, with all
// world-frame directions rotated by `rotation`. A PURE-TRANSLATION wormhole still
// has rotation == identity here, and the teleport still moves the body between the
// mouths. The easy-to-miss hazard (7.3 step 3) is that even a rotation-only mouth
// must rotate the world-frame linearVelocity, because it lives in the frame the
// rotation changes - ApplyTeleport does exactly that.
struct MouthTransform
{
    WorldPos entry;                    // source mouth, global coords
    WorldPos exit;                     // destination mouth, global coords
    Quatf    rotation = Quatf::Identity(); // mouth-to-mouth world rotation (normalized on use)

    static MouthTransform Wormhole(const WorldPos& entry, const WorldPos& exit,
                                   const Quatf& rotation = Quatf::Identity())
    {
        return MouthTransform{ entry, exit, rotation };
    }

    // The reverse jump (exit->entry, inverse rotation). Composing a transform with
    // its Inverse is the identity mouth pair, which is what makes the A->B->A
    // round-trip well-defined (7.4).
    MouthTransform Inverse() const
    {
        return MouthTransform{ exit, entry, rotation.Conjugate() };
    }
};

// -----------------------------------------------------------------------------
// TeleportState - the complete retained-state set (architecture 7.3), in
// sim-native types. A future ship-lane adapter must combine the ECS local Vec3d
// position with its FrameId to form WorldPos, and widen the ECS Vec3f angular
// velocity/torque fields. OrbitState.owner and FrameId are the caller's to flip;
// this struct carries the numeric state whose correctness across the discontinuity
// is the whole point.
// -----------------------------------------------------------------------------
struct TeleportState
{
    WorldPos position;                              // Transform.position
    Quatf    orientation   = Quatf::Identity();     // Transform.rotation
    Vec3d    linearVelocity{ 0.0, 0.0, 0.0 };       // RigidBody.linearVelocity (WORLD/frame)
    Vec3d    momentum{ 0.0, 0.0, 0.0 };             // RelativisticBody.momentum (WORLD/frame)
    Vec3d    angularVelocity{ 0.0, 0.0, 0.0 };      // RigidBody.angularVelocity (BODY frame)
    Vec3d    forceAccum{ 0.0, 0.0, 0.0 };           // RigidBody.forceAccum
    Vec3d    torqueAccum{ 0.0, 0.0, 0.0 };          // RigidBody.torqueAccum
    WorldPos prevPosition;                          // render interpolation history
    Quatf    prevRotation  = Quatf::Identity();     // render interpolation history
};

// THE ATOMIC TELEPORT (architecture 7.3 steps 2-5). A PURE FUNCTION of
// { pre-jump state, mouth transform } - no wall-clock, no RNG - so it is
// deterministic and replay/save-load safe (7.4). Steps this function owns:
//
//   2. position -> destination mouth, preserving the body's offset-from-mouth
//      (rotated), expressed via WorldPos so it stays precise at any jump distance.
//   3. linearVelocity -> ROTATED by the mouth rotation (world-frame vector).
//      orientation    -> ROTATED by the mouth rotation (prepended in world space).
//      angularVelocity-> UNCHANGED: it is body-frame, so a rotation of the body's
//      orientation leaves the co-rotating spin vector's components alone.
//   4. forceAccum / torqueAccum -> FLUSHED to zero, so a staged pre-jump wrench is
//      not consumed on the first post-jump step in the new frame.
//   5. prevPosition / prevRotation -> set to the POST-teleport pose, so render
//      interpolation's delta across the jump is zero (no source->dest smear).
//
// Steps 1 (run inside the fixed step), 6 (reset the path-trace accumulation index,
// NOT the RNG seed counter) and 7 (drain the timestep accumulator) are engine /
// call-site concerns outside this pure module; they are documented at the call
// site, not simulated here.
//
// TryApplyTeleport reports whether the complete operation was accepted. On
// rejection, out receives s unchanged. ApplyTeleport is the compatibility
// wrapper with the same transactional behavior.
bool TryApplyTeleport(const TeleportState& s, const MouthTransform& m,
                      TeleportState& out);
TeleportState ApplyTeleport(const TeleportState& s, const MouthTransform& m);

// -----------------------------------------------------------------------------
// Warp cruise - a dedicated warp-space frame that is literally a moving floating
// origin (architecture 7.2). The origin advances along the route at a (possibly
// SUPERLUMINAL) COORDINATE velocity, deterministically, at fixed dt. Inside the
// bubble local physics run normally with beta << 1 (no relativistic overflow),
// because the ship is at rest / sub-c relative to the bubble frame; real-space
// gravity and atmosphere are suspended by the caller during cruise.
// -----------------------------------------------------------------------------

// Advance a warp-space frame origin by coordVelocity*dt, re-canonicalized so the
// frame's local coordinates stay small and precise the whole way (WorldPos carries
// the growth into the integer sector). coordVelocity MAY exceed c: it is a
// coordinate speed of the bubble, not a local velocity, so nothing inside ever
// locally exceeds c. Pure and deterministic.
//
// One call may move by at most one sector per axis. Larger route advances must be
// split into fixed steps by the caller. TryAdvanceWarpOrigin reports rejection;
// its wrapper returns origin unchanged for an invalid or unrepresentable step.
bool TryAdvanceWarpOrigin(const WorldPos& origin, const Vec3d& coordVelocity,
                          double dt, WorldPos& out);
WorldPos AdvanceWarpOrigin(const WorldPos& origin, const Vec3d& coordVelocity, double dt);

// Proper time elapsed for a warp rider over coordinate dt. CONVENTION (7.2,
// flagged for owner sign-off in 11): the bubble interior is treated as flat, so
// dtau == dt - a warp rider ages normally. This does NOT contradict the section-2
// dilation model: a near-c SUBLIGHT traveler still ages less; warp is a different
// worldline. Kept as a named function so the convention lives in exactly one place.
// Invalid or non-positive intervals contribute no proper time.
double WarpProperTime(double dt);

} // namespace sim
