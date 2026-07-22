#pragma once
// =============================================================================
// gameplay/playable_ship.h - pure player-ship configuration and presentation
// =============================================================================
// This header is deliberately GPU- and platform-free. The app translates its
// keyboard/mouse state into PilotInputSnapshot, while tests can drive the exact
// same mapping, fighter rig, chase-camera pose, and throttle feedback.
// =============================================================================

#include "../core/types.h"
#include "../ecs/components.h"

#include <cmath>
#include <cstdint>

namespace gameplay
{

struct PilotInputSnapshot
{
    bool thrustForward = false;
    bool thrustBackward = false;
    bool strafeLeft = false;
    bool strafeRight = false;
    bool liftUp = false;
    bool liftDown = false;

    bool pitchUp = false;
    bool pitchDown = false;
    bool yawLeft = false;
    bool yawRight = false;
    bool rollLeft = false;
    bool rollRight = false;

    // Pointer axes are normalized pilot demand, not raw pixels. Positive pitch
    // is nose-down (+X body rotation); positive yaw is nose-right (+Y).
    float pointerPitch = 0.0f;
    float pointerYaw = 0.0f;
    bool toggleMode = false;
};

// Platform-neutral movement bindings shared by flight and the player-style
// free camera. The alternate set is where the Windows host supplies arrows;
// keeping that policy out of the flight law gives a future on-foot controller
// the same local-movement contract without importing Win32 key codes.
struct MovementBindingSnapshot
{
    bool primaryForward = false;
    bool primaryBackward = false;
    bool primaryLeft = false;
    bool primaryRight = false;
    bool alternateForward = false;
    bool alternateBackward = false;
    bool alternateLeft = false;
    bool alternateRight = false;
    bool up = false;
    bool down = false;
};

struct LocalMovementInput
{
    bool forward = false;
    bool backward = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
};

inline LocalMovementInput ResolveMovementBindings(
    const MovementBindingSnapshot& bindings)
{
    return {
        bindings.primaryForward || bindings.alternateForward,
        bindings.primaryBackward || bindings.alternateBackward,
        bindings.primaryLeft || bindings.alternateLeft,
        bindings.primaryRight || bindings.alternateRight,
        bindings.up,
        bindings.down,
    };
}

struct PointerSteeringSettings
{
    // Raw-input deltas are integral pixels on Windows. Half a pixel therefore
    // removes only numerical residue while preserving the first real count.
    float deadzonePixels = 0.5f;
    float fullDemandPixels = 28.0f;
    float responseExponent = 1.15f;
};

inline float ClampDemand(float value)
{
    if (!std::isfinite(value)) return 0.0f;
    if (value < -1.0f) return -1.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

inline float ClampThrottle(float value)
{
    if (!std::isfinite(value) || value <= 0.0f) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    return value;
}

inline float DigitalAxis(bool positive, bool negative)
{
    return static_cast<float>(positive ? 1 : 0) -
           static_cast<float>(negative ? 1 : 0);
}

// Converts one fixed-step's accumulated raw mouse delta into a bounded angular
// demand. This is deliberately a rate-style response: when the mouse stops, the
// command stops. A virtual-stick cursor can be added later with a visible HUD
// reticle and an explicit mode setting; silently emulating one with a hidden
// cursor would make recentering impossible to understand.
inline float PointerSteeringDemand(
    float deltaPixels,
    const PointerSteeringSettings& settings = {})
{
    if (!std::isfinite(deltaPixels) || !std::isfinite(settings.deadzonePixels) ||
        !std::isfinite(settings.fullDemandPixels) ||
        !std::isfinite(settings.responseExponent) ||
        settings.deadzonePixels < 0.0f ||
        settings.fullDemandPixels <= settings.deadzonePixels ||
        settings.responseExponent <= 0.0f)
    {
        return 0.0f;
    }

    const float magnitude = std::fabs(deltaPixels);
    if (magnitude <= settings.deadzonePixels)
        return 0.0f;

    float normalized = (magnitude - settings.deadzonePixels) /
                       (settings.fullDemandPixels - settings.deadzonePixels);
    if (normalized > 1.0f)
        normalized = 1.0f;
    const float curved = std::pow(normalized, settings.responseExponent);
    return std::copysign(curved, deltaPixels);
}

inline void ApplyPilotInput(const PilotInputSnapshot& input,
                            ecs::FlightControl& control)
{
    control.linearDemand = {
        DigitalAxis(input.strafeRight, input.strafeLeft),
        DigitalAxis(input.liftUp, input.liftDown),
        DigitalAxis(input.thrustForward, input.thrustBackward),
    };

    control.angularDemand = {
        ClampDemand(DigitalAxis(input.pitchDown, input.pitchUp) + input.pointerPitch),
        ClampDemand(DigitalAxis(input.yawRight, input.yawLeft) + input.pointerYaw),
        DigitalAxis(input.rollRight, input.rollLeft),
    };

    if (input.toggleMode)
    {
        control.mode = control.mode == ecs::FlightMode::Coupled
            ? ecs::FlightMode::Decoupled
            : ecs::FlightMode::Coupled;
    }
}

inline void ClearThrusterThrottles(ecs::ThrusterSet& set)
{
    const uint32_t count = set.count < ecs::ThrusterSet::kMaxThrusters
        ? set.count
        : ecs::ThrusterSet::kMaxThrusters;
    for (uint32_t i = 0; i < count; ++i)
        set.thrusters[i].throttle = 0.0f;
}

inline ecs::RigidBody MakeFighterRigidBody()
{
    constexpr double mass = 12000.0;
    constexpr double width = 3.0;
    constexpr double height = 0.9;
    constexpr double length = 4.4;
    constexpr double oneTwelfthMass = mass / 12.0;

    ecs::RigidBody body;
    body.invMass = 1.0 / mass;
    body.invInertiaDiag = {
        static_cast<float>(1.0 / (oneTwelfthMass * (height * height + length * length))),
        static_cast<float>(1.0 / (oneTwelfthMass * (width * width + length * length))),
        static_cast<float>(1.0 / (oneTwelfthMass * (width * width + height * height))),
    };
    return body;
}

inline ecs::Thruster MakeThruster(const core::Vec3f& position,
                                  const core::Vec3f& direction,
                                  float maxForce)
{
    ecs::Thruster thruster;
    thruster.localPosition = position;
    thruster.localDirection = direction;
    thruster.maxForce = maxForce;
    return thruster;
}

inline ecs::ThrusterSet MakeFighterThrusterSet()
{
    ecs::ThrusterSet set;
    auto add = [&set](const core::Vec3f& position,
                      const core::Vec3f& direction,
                      float maxForce)
    {
        if (set.count < ecs::ThrusterSet::kMaxThrusters)
            set.thrusters[set.count++] = MakeThruster(position, direction, maxForce);
    };

    // Translation through the centre of mass: starboard/port, up/down,
    // main/retro. The line of action of each one passes through the origin.
    add({ -1.5f,  0.0f,  0.0f }, {  1,  0,  0 }, 120000.0f);
    add({  1.5f,  0.0f,  0.0f }, { -1,  0,  0 }, 120000.0f);
    add({  0.0f, -0.45f, 0.0f }, {  0,  1,  0 }, 120000.0f);
    add({  0.0f,  0.45f, 0.0f }, {  0, -1,  0 }, 120000.0f);
    add({  0.0f,  0.0f, -2.2f }, {  0,  0,  1 }, 240000.0f);
    add({  0.0f,  0.0f,  2.2f }, {  0,  0, -1 }, 160000.0f);

    constexpr float rcsForce = 25000.0f;

    // Pitch couples (+X, then -X).
    add({ 0,  0.45f, 0 }, { 0, 0,  1 }, rcsForce);
    add({ 0, -0.45f, 0 }, { 0, 0, -1 }, rcsForce);
    add({ 0,  0.45f, 0 }, { 0, 0, -1 }, rcsForce);
    add({ 0, -0.45f, 0 }, { 0, 0,  1 }, rcsForce);

    // Yaw couples (+Y, then -Y).
    add({ 0, 0,  2.2f }, {  1, 0, 0 }, rcsForce);
    add({ 0, 0, -2.2f }, { -1, 0, 0 }, rcsForce);
    add({ 0, 0,  2.2f }, { -1, 0, 0 }, rcsForce);
    add({ 0, 0, -2.2f }, {  1, 0, 0 }, rcsForce);

    // Roll couples (+Z, then -Z).
    add({  1.5f, 0, 0 }, { 0,  1, 0 }, rcsForce);
    add({ -1.5f, 0, 0 }, { 0, -1, 0 }, rcsForce);
    add({  1.5f, 0, 0 }, { 0, -1, 0 }, rcsForce);
    add({ -1.5f, 0, 0 }, { 0,  1, 0 }, rcsForce);

    return set;
}

struct ThrusterVisualState
{
    ecs::Transform transform;
    float emissiveStrength = 0.0f;
    bool visible = false;
};

inline core::Quatf RotationFromPositiveZ(const core::Vec3f& direction)
{
    const core::Vec3f target = direction.Normalized();
    const float dot = core::Vec3f{ 0.0f, 0.0f, 1.0f }.Dot(target);
    if (dot > 0.9999f) return core::Quatf::Identity();
    if (dot < -0.9999f)
        return core::Quatf::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, core::PI);

    const core::Vec3f cross = core::Vec3f{ 0.0f, 0.0f, 1.0f }.Cross(target);
    return core::Quatf{ cross.x, cross.y, cross.z, 1.0f + dot }.Normalized();
}

inline ThrusterVisualState BuildThrusterVisualState(const ecs::Transform& ship,
                                                     const ecs::Thruster& thruster)
{
    ThrusterVisualState visual;
    const float throttle = ClampThrottle(thruster.throttle);
    visual.visible = throttle > 0.001f;
    visual.emissiveStrength = visual.visible ? 3.0f + 17.0f * throttle : 0.0f;

    const core::Vec3f exhaustDirection = -thruster.localDirection.Normalized();
    const float length = 0.15f + 1.35f * throttle;
    const core::Vec3f localCentre =
        thruster.localPosition + exhaustDirection * (0.08f + length * 0.5f);
    visual.transform.position =
        ship.position + core::Vec3d::FromFloat(ship.rotation.Rotate(localCentre));
    visual.transform.rotation =
        (ship.rotation * RotationFromPositiveZ(exhaustDirection)).Normalized();
    visual.transform.scale = { 0.10f + 0.08f * throttle,
                               0.10f + 0.08f * throttle,
                               length };
    return visual;
}

struct ChaseCameraPose
{
    core::Vec3d position = {};
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
};

inline bool IsFinite(const ChaseCameraPose& pose)
{
    return std::isfinite(pose.position.x) && std::isfinite(pose.position.y) &&
           std::isfinite(pose.position.z) && std::isfinite(pose.yawDegrees) &&
           std::isfinite(pose.pitchDegrees);
}

inline float ShortestAngleDeltaDegrees(float from, float to)
{
    if (!std::isfinite(from) || !std::isfinite(to))
        return 0.0f;
    float delta = std::fmod(to - from, 360.0f);
    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    return delta;
}

inline float WrapDegrees(float angle)
{
    if (!std::isfinite(angle))
        return 0.0f;
    angle = std::fmod(angle, 360.0f);
    if (angle > 180.0f) angle -= 360.0f;
    if (angle < -180.0f) angle += 360.0f;
    return angle;
}

// Frame-rate-independent exponential chase response. Large discontinuities are
// teleports/loads, not camera motion, so they snap instead of dragging the view
// across interplanetary space. Rotation takes the shortest path across +/-180.
inline ChaseCameraPose SmoothChaseCameraPose(
    const ChaseCameraPose& current,
    const ChaseCameraPose& target,
    double dt,
    float responsePerSecond = 12.0f,
    double teleportSnapDistance = 100.0)
{
    if (!IsFinite(target))
        return current;
    if (!IsFinite(current))
        return target;
    if (!std::isfinite(dt) || dt <= 0.0)
        return current;
    if (!std::isfinite(responsePerSecond) || responsePerSecond <= 0.0f ||
        !std::isfinite(teleportSnapDistance) || teleportSnapDistance < 0.0)
    {
        return target;
    }

    const core::Vec3d positionDelta = target.position - current.position;
    if (positionDelta.Length() > teleportSnapDistance)
        return target;

    const double alpha = 1.0 - std::exp(
        -static_cast<double>(responsePerSecond) * dt);
    ChaseCameraPose smoothed;
    smoothed.position = current.position + positionDelta * alpha;
    smoothed.yawDegrees = WrapDegrees(
        current.yawDegrees +
        ShortestAngleDeltaDegrees(current.yawDegrees, target.yawDegrees) *
            static_cast<float>(alpha));
    smoothed.pitchDegrees = current.pitchDegrees +
        (target.pitchDegrees - current.pitchDegrees) * static_cast<float>(alpha);
    return smoothed;
}

// Park the camera at a standoff from a celestial body, framing it. The standoff
// is a MULTIPLE of the body's true radius, so the same call frames a moon or a
// star at a sensible screen size. `azimuthDeg`/`elevationDeg` orbit the vantage
// around the body. The camera looks back at the body's centre. Works at true
// scale (K=1): with reversed-Z the near plane (set by the mode) is the precision
// lever, so a body of radius r renders solid across its whole limb.
inline ChaseCameraPose BuildNearBodyCameraPose(const core::Vec3d& bodyPos,
                                               double bodyRadius,
                                               double standoffRadii = 3.0,
                                               float azimuthDeg = 35.0f,
                                               float elevationDeg = 12.0f)
{
    const double dist = (bodyRadius > 0.0 ? bodyRadius : 1.0) * standoffRadii;
    const double az = static_cast<double>(azimuthDeg) * core::DEG_TO_RAD;
    const double el = static_cast<double>(elevationDeg) * core::DEG_TO_RAD;
    // Unit direction from the body out to the camera.
    const core::Vec3d dir{ std::cos(el) * std::sin(az),
                           std::sin(el),
                           std::cos(el) * std::cos(az) };
    ChaseCameraPose pose;
    pose.position = bodyPos + dir * dist;
    // Look back toward the body: forward = -dir. Matches the yaw/pitch convention
    // BuildChaseCameraPose uses (yaw = atan2(fwd.x, fwd.z), pitch = asin(fwd.y)).
    pose.yawDegrees   = static_cast<float>(std::atan2(-dir.x, -dir.z) * core::RAD_TO_DEG);
    pose.pitchDegrees = static_cast<float>(std::asin(-dir.y) * core::RAD_TO_DEG);
    return pose;
}

inline ChaseCameraPose BuildChaseCameraPose(const ecs::Transform& ship,
                                            float distance = 8.0f,
                                            float height = 2.4f,
    float lookDownDegrees = 8.0f)
{
    const core::Vec3f forward = ship.rotation.Rotate({ 0.0f, 0.0f, 1.0f }).Normalized();

    ChaseCameraPose pose;
    pose.position = ship.position - core::Vec3d::FromFloat(forward) * distance +
                    core::Vec3d{ 0.0, static_cast<double>(height), 0.0 };
    pose.yawDegrees = std::atan2(forward.x, forward.z) * core::RAD_TO_DEG;
    const float vertical = ClampDemand(forward.y);
    pose.pitchDegrees = std::asin(vertical) * core::RAD_TO_DEG - lookDownDegrees;
    return pose;
}

} // namespace gameplay
