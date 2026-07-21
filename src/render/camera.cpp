// =============================================================================
// render/camera.cpp — FPS Camera Implementation
// =============================================================================

#include "camera.h"
#include <algorithm>
#include <cmath>

namespace render
{

void Camera::Init(const core::Vec3d& position, float yawDeg, float pitchDeg)
{
    m_position = position;
    m_yaw = yawDeg;
    m_pitch = pitchDeg;
    m_hasExplicitBasis = false;
}

bool Camera::InitBasis(const core::Vec3d& position,
                       const core::Vec3f& forward,
                       const core::Vec3f& up)
{
    const bool finite =
        std::isfinite(position.x) && std::isfinite(position.y) &&
        std::isfinite(position.z) && std::isfinite(forward.x) &&
        std::isfinite(forward.y) && std::isfinite(forward.z) &&
        std::isfinite(up.x) && std::isfinite(up.y) && std::isfinite(up.z);
    if (!finite || forward.LengthSq() < 1.0e-8f ||
        up.LengthSq() < 1.0e-8f)
    {
        return false;
    }

    const core::Vec3f normalizedForward = forward.Normalized();
    const core::Vec3f right =
        up.Normalized().Cross(normalizedForward).Normalized();
    if (right.LengthSq() < 1.0e-8f)
        return false;
    const core::Vec3f normalizedUp =
        normalizedForward.Cross(right).Normalized();
    if (normalizedUp.LengthSq() < 1.0e-8f)
        return false;

    m_position = position;
    m_explicitForward = normalizedForward;
    m_explicitUp = normalizedUp;
    m_yaw = std::atan2(normalizedForward.x, normalizedForward.z) *
        core::RAD_TO_DEG;
    m_pitch = std::asin((std::max)(
        -1.0f, (std::min)(1.0f, normalizedForward.y))) * core::RAD_TO_DEG;
    m_hasExplicitBasis = true;
    return true;
}

core::Vec3f Camera::Forward() const
{
    if (m_hasExplicitBasis)
        return m_explicitForward;
    float yawRad   = m_yaw * core::DEG_TO_RAD;
    float pitchRad = m_pitch * core::DEG_TO_RAD;
    float cosPitch = std::cos(pitchRad);
    return core::Vec3f(
        std::sin(yawRad) * cosPitch,
        std::sin(pitchRad),
        std::cos(yawRad) * cosPitch
    ).Normalized();
}

core::Vec3f Camera::Right() const
{
    // Right = Up x Forward (LH cross product)
    return Up().Cross(Forward()).Normalized();
}

core::Vec3f Camera::Up() const
{
    return m_hasExplicitBasis
        ? m_explicitUp
        : core::Vec3f{ 0.0f, 1.0f, 0.0f };
}

void Camera::Update(float dt,
                    float mouseDeltaX, float mouseDeltaY,
                    bool forward, bool backward,
                    bool left, bool right,
                    bool up, bool down,
                    bool sprint)
{
    m_hasExplicitBasis = false;

    // Mouse look
    m_yaw   += mouseDeltaX * m_sensitivity;
    m_pitch -= mouseDeltaY * m_sensitivity;  // Invert Y: moving mouse up looks up

    // Clamp pitch to prevent flipping
    m_pitch = (std::max)(-89.0f, (std::min)(89.0f, m_pitch));

    // Wrap yaw
    if (m_yaw > 360.0f)  m_yaw -= 360.0f;
    if (m_yaw < -360.0f) m_yaw += 360.0f;

    // Movement
    float speed = m_moveSpeed * (sprint ? m_sprintMult : 1.0f) * dt;

    core::Vec3f fwd = Forward();
    core::Vec3f rt  = Right();
    const core::Vec3f worldUp(0.0f, 1.0f, 0.0f);

    core::Vec3f moveDir(0.0f, 0.0f, 0.0f);
    if (forward)  moveDir += fwd;
    if (backward) moveDir -= fwd;
    if (right)    moveDir += rt;
    if (left)     moveDir -= rt;
    if (up)       moveDir += worldUp;
    if (down)     moveDir -= worldUp;

    // Normalize to prevent faster diagonal movement
    float len = moveDir.Length();
    if (len > 0.001f)
        m_position += core::Vec3d::FromFloat(moveDir) * static_cast<double>(speed / len);
}

core::Mat4x4 Camera::ViewMatrix() const
{
    // Scene transforms are camera-relative before narrowing to float, so the
    // GPU camera always sits at the local origin.
    const core::Vec3f origin = { 0.0f, 0.0f, 0.0f };
    return core::Mat4x4::LookAt(origin, Forward(), Up());
}

core::Mat4x4 Camera::ProjectionMatrix(float aspectRatio) const
{
    return core::Mat4x4::PerspectiveFovLH(
        m_fovDeg * core::DEG_TO_RAD,
        aspectRatio,
        m_nearZ, m_farZ);
}

core::Mat4x4 Camera::ViewProjectionMatrix(float aspectRatio) const
{
    return ViewMatrix() * ProjectionMatrix(aspectRatio);
}

} // namespace render
