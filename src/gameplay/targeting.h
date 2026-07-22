#pragma once
// =============================================================================
// gameplay/targeting.h — target selection, target info, and firing-solution math
// =============================================================================
// Pure CPU (GPU-free) so the tests drive the shipped code, mirroring the
// flight_control / playable_ship pattern. The app gathers TargetCandidates from
// the scene each frame (celestial bodies now; other ships later), keeps the
// selected target id, and asks this module for the derived TargetInfo (range,
// closing speed, relative speed) and the LEAD point (where to aim to hit a moving
// target). Rendering (bracket, lead pip, info readout) lives in the HUD layer.
// =============================================================================

#include "core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gameplay
{

// How the target relates to the player — drives HUD colour coding. Bodies are
// Neutral; the enum is here so ship targets slot in without a data change.
enum class TargetRelation : uint8_t { Neutral = 0, Friendly, Hostile };

// A selectable thing in the world. Positions/velocities are absolute world-frame
// doubles (RULE 1); the app narrows to camera-relative only for rendering.
struct TargetCandidate
{
    uint64_t       id = 0;                 // stable id (e.g. seeded bodyId)
    std::string    name;
    core::Vec3d    worldPos{ 0, 0, 0 };
    core::Vec3d    worldVel{ 0, 0, 0 };
    double         radius = 0.0;           // metres; sizes the bracket / gates lead
    TargetRelation relation = TargetRelation::Neutral;
};

// Derived state for the currently selected target.
struct TargetInfo
{
    bool           valid = false;
    uint64_t       id = 0;
    std::string    name;
    TargetRelation relation = TargetRelation::Neutral;
    double         rangeMeters   = 0.0;    // |target - ship|
    double         closingSpeed  = 0.0;    // > 0 range decreasing, < 0 opening
    double         relativeSpeed = 0.0;    // |targetVel - shipVel|
};

// A computed firing solution (lead point) for gunnery against a moving target.
struct FiringSolution
{
    bool        valid = false;  // false when the target outruns the projectile
    core::Vec3d worldPos{ 0, 0, 0 }; // aim here to hit; == target when V_rel == 0
    double      timeToImpact = 0.0;
};

// Range / closing / relative speed for a candidate given the ship's world state.
// closingSpeed = -(V_rel . losHat): positive means the range is shrinking.
TargetInfo ComputeTargetInfo(const core::Vec3d& shipPos, const core::Vec3d& shipVel,
                             const TargetCandidate& c);

// The lead / intercept point: where a projectile of muzzle speed `projectileSpeed`
// (fired from the ship, inheriting the ship's velocity — the usual convention)
// meets the target. Solves |P + V*t| = w*t for the smallest positive t, where
// P = targetPos - shipPos and V = targetVel - shipVel. Invalid when no positive
// real root exists (the target can outrun the round) or the speed is non-positive.
FiringSolution ComputeFiringSolution(const core::Vec3d& shipPos, const core::Vec3d& shipVel,
                                     const core::Vec3d& targetPos, const core::Vec3d& targetVel,
                                     double projectileSpeed);

// Index of the candidate with `id`, or -1 if absent.
int FindCandidate(const std::vector<TargetCandidate>& candidates, uint64_t id);

// The nearest candidate to `shipPos`, excluding `excludeId` (0 = exclude nothing).
// Returns 0 when there is nothing to select.
uint64_t SelectNearest(const std::vector<TargetCandidate>& candidates,
                       const core::Vec3d& shipPos, uint64_t excludeId = 0);

// Step the selection to the next/previous candidate (dir = +1 / -1), wrapping.
// Candidates are visited in ascending id order so the cycle is deterministic and
// independent of scene iteration order. Returns 0 when the list is empty.
uint64_t CycleTarget(const std::vector<TargetCandidate>& candidates,
                     uint64_t currentId, int dir);

} // namespace gameplay
