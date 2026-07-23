// =============================================================================
// gameplay/docking.cpp — see docking.h
// =============================================================================
#include "docking.h"

#include <algorithm>
#include <cmath>

namespace gameplay
{
namespace
{
double Dot(const core::Vec3d& a, const core::Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double Len(const core::Vec3d& a) { return std::sqrt(Dot(a, a)); }
double ClampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

double MaxApproachSpeed(double d, const DockingPort& port)
{
    // Constant-deceleration braking curve that reaches ~0 at the capture point,
    // clamped to [vMin, vCruise]. vMin keeps the ship creeping in to make contact.
    const double dEff = d - port.captureRadius;
    if (dEff <= 0.0) return port.vMin;
    const double v = std::sqrt(2.0 * port.approachAccel * dEff);
    return ClampD(v, port.vMin, port.vCruise);
}

DockApproach ComputeApproach(const core::Vec3d& shipPos, const core::Vec3d& shipVelRelPort,
                             const core::Vec3d& shipForward, const core::Vec3d& shipUp,
                             const DockingPort& port)
{
    DockApproach a;
    // s points from the port to the ship. `shipVelRelPort` is the ship velocity in
    // the port's frame (the caller subtracts the station velocity).
    const core::Vec3d s{ shipPos.x - port.position.x,
                         shipPos.y - port.position.y,
                         shipPos.z - port.position.z };
    a.range = Len(s);
    a.along = Dot(s, port.approachAxis);

    const core::Vec3d lat{ s.x - a.along * port.approachAxis.x,
                           s.y - a.along * port.approachAxis.y,
                           s.z - a.along * port.approachAxis.z };
    a.lateral = Len(lat);

    a.coneError = (a.range > 1e-6) ? std::acos(ClampD(a.along / a.range, -1.0, 1.0)) : 0.0;
    a.alignError = std::acos(ClampD(-Dot(shipForward, port.approachAxis), -1.0, 1.0));
    a.rollError  = std::acos(ClampD(Dot(shipUp, port.up), -1.0, 1.0));

    // closingSpeed = -(V_rel . s)/range: positive when the range is shrinking.
    a.closingSpeed = (a.range > 1e-6) ? -(Dot(shipVelRelPort, s) / a.range) : 0.0;
    a.maxSpeed     = MaxApproachSpeed(a.along, port);

    a.inCorridor = (a.along > 0.0) && (a.coneError <= port.coneHalfAngle);
    a.overspeed  = a.closingSpeed > a.maxSpeed + 0.5; // small hysteresis

    a.captured = (a.along <= port.captureRadius) && (a.along > -0.5 * port.captureRadius) &&
                 (a.lateral <= port.lateralTol) && (a.alignError <= port.alignTol) &&
                 (a.rollError <= port.rollTol) && (std::fabs(a.closingSpeed) <= port.speedTol);
    return a;
}

DockState StepDockState(DockState current, const DockApproach& a,
                        const DockingPort& port, double engageRange)
{
    switch (current)
    {
        case DockState::Idle:
            if (a.range <= engageRange && a.inCorridor) return DockState::Approaching;
            return DockState::Idle;

        case DockState::Approaching:
            if (a.range > engageRange * 1.5)              return DockState::Idle;
            if (!a.inCorridor || a.overspeed)             return DockState::Hold;
            if (a.captured)                               return DockState::Docked;
            // Close to the port and roughly centred -> switch to fine alignment.
            if (a.along <= port.captureRadius * 3.0 && a.lateral <= port.lateralTol * 1.5)
                return DockState::Aligning;
            return DockState::Approaching;

        case DockState::Aligning:
            if (a.captured)                               return DockState::Docked;
            if (!a.inCorridor || a.overspeed)             return DockState::Hold;
            // Drifted back out of the fine-approach zone -> back to the corridor run.
            if (a.along > port.captureRadius * 4.0)       return DockState::Approaching;
            return DockState::Aligning;

        case DockState::Hold:
            if (a.range > engageRange * 1.5)              return DockState::Idle;
            if (a.inCorridor && !a.overspeed)             return DockState::Approaching;
            return DockState::Hold;

        case DockState::Docked:
            return DockState::Docked; // leaves only on an explicit undock request

        case DockState::Undocking:
            // Cleared the fine-approach envelope -> free flight. Keyed on RANGE (not
            // along), matching every other envelope exit: `along` is the axial
            // projection and stays small if the ship pushes off sideways or noses back
            // at the port, which would otherwise strand the ship in Undocking forever.
            // captureRadius*4 is the same fine-approach boundary the Aligning case uses,
            // so undock completes as soon as the ship is clear of the collar (the push
            // opens the range at once, so re-capture cannot fire before it clears).
            if (a.range > port.captureRadius * 4.0)       return DockState::Idle;
            return DockState::Undocking;
    }
    return DockState::Idle;
}

} // namespace gameplay
