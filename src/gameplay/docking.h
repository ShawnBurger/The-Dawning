#pragma once
// =============================================================================
// gameplay/docking.h — docking-port geometry, approach guidance, state machine
// =============================================================================
// Pure CPU (GPU-free) so the tests drive the shipped code, like gameplay/targeting
// and flight_control. A docking PORT is a frame (position + outward approach axis +
// up); the ship flies down the approach corridor while a closing-rate governor caps
// speed vs distance, then nulls attitude/roll, then soft-docks. The app owns the
// port and the DockState; this module computes the guidance (DockApproach) and
// advances the state machine. Rendering (the alignment widget, closing-rate cue)
// lives in the HUD layer. Precedent: Elite ATC + pad alignment, Star Citizen ILS
// ring/crossbars, X4 guide rings (see the ship-feel research).
// =============================================================================

#include "core/types.h"

#include <cstdint>

namespace gameplay
{

// A docking port: a frame in world space. `approachAxis` is the OUTWARD unit normal
// (the direction a ship must come from); the ship noses in ANTI-parallel to it.
struct DockingPort
{
    core::Vec3d position{ 0, 0, 0 };
    core::Vec3d approachAxis{ 0, 0, 1 };   // unit, outward
    core::Vec3d up{ 0, 1, 0 };             // unit, defines roll alignment
    double      captureRadius   = 12.0;    // metres; soft-dock distance along the axis
    double      coneHalfAngle   = 0.5236;  // rad (~30 deg): approach-corridor half-angle
    double      lateralTol      = 8.0;     // metres off centreline to capture
    double      alignTol        = 0.15;    // rad: nose-in error to capture
    double      rollTol         = 0.35;    // rad: roll error to capture
    double      speedTol        = 3.0;     // m/s: max closing speed to capture
    double      vCruise         = 60.0;    // m/s: governor cap far out
    double      vMin            = 1.5;     // m/s: terminal creep so contact happens
    double      approachAccel   = 1.5;     // m/s^2: governor braking curve
};

// Derived guidance for the ship's current pose relative to a port.
struct DockApproach
{
    double along        = 0.0;   // distance out along +approachAxis (want > 0)
    double lateral      = 0.0;   // perpendicular offset from the centreline (m)
    double range        = 0.0;   // straight-line ship->port distance (m)
    double coneError    = 0.0;   // rad off the corridor centreline (from the port)
    double alignError   = 0.0;   // rad: ship nose vs anti-approach-axis (0 = nosed in)
    double rollError    = 0.0;   // rad: ship up vs port up
    double closingSpeed = 0.0;   // m/s toward the port (+ = closing)
    double maxSpeed     = 0.0;   // governor limit at this distance (m/s)
    bool   inCorridor   = false; // along > 0 && coneError <= coneHalfAngle
    bool   overspeed    = false; // closingSpeed > maxSpeed (the "reduce speed" cue)
    bool   captured     = false; // all capture tolerances met
};

// The dock/undock lifecycle. Hold is the "too fast / out of corridor" state that
// drives the HUD "REDUCE SPEED / REALIGN" cue.
enum class DockState : uint8_t
{
    Idle = 0,     // no port engaged / out of range
    Approaching,  // in the corridor, flying down the governor curve
    Aligning,     // near the port, nulling attitude + roll on the centreline
    Hold,         // out of corridor or overspeed — recover before continuing
    Docked,       // soft-docked / latched
    Undocking,    // pushing back out along +approachAxis to a safe distance
};

// The governor: the maximum closing speed allowed at distance `d` along the axis —
// a constant-deceleration braking curve that stops at the capture point, clamped to
// [vMin, vCruise]. d is the along-axis distance to the port.
double MaxApproachSpeed(double d, const DockingPort& port);

// Compute the guidance for a ship pose relative to a port. `shipVelRelPort` is the
// ship velocity in the PORT's frame (the caller subtracts the station velocity), so
// the closing speed is correct for a moving/orbiting station. `shipForward`/`shipUp`
// are unit vectors.
DockApproach ComputeApproach(const core::Vec3d& shipPos, const core::Vec3d& shipVelRelPort,
                             const core::Vec3d& shipForward, const core::Vec3d& shipUp,
                             const DockingPort& port);

// Advance the dock state machine one tick given the current state and guidance.
// `engageRange` is the range within which Idle transitions to Approaching. Docked is
// entered by capture and left only by an explicit undock request (which sets the state
// to Undocking and pushes the ship off); Undocking then returns to Idle by RANGE once
// the ship is clear of the fine-approach envelope (port.captureRadius * 4), so pushing
// off sideways or nosing back at the port cannot strand it.
DockState StepDockState(DockState current, const DockApproach& a,
                        const DockingPort& port, double engageRange);

} // namespace gameplay
