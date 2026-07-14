// =============================================================================
// render/camera.cpp — FPS Camera Implementation
// =============================================================================

#include "camera.h"
#include <algorithm>
#include <cmath>

namespace render
{

void Camera::Init(const core::Vec3f& position, float yawDeg, float pitchDeg)
{
    m_position = position;
    m_yaw = yawDeg;
    m_pitch = pitchDeg;
}

core::Vec3f Camera::Forward() const
{
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
    core::Vec3f up(0.0f, 1.0f, 0.0f);
    return up.Cross(Forward()).Normalized();
}

void Camera::Update(float dt,
                    float mouseDeltaX, float mouseDeltaY,
                    bool forward, bool backward,
                    bool left, bool right,
                    bool up, bool down,
                    bool sprint)
{
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
    core::Vec3f worldUp(0.0f, 1.0f, 0.0f);

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
        m_position += moveDir * (speed / len);
}

core::Mat4x4 Camera::ViewMatrix() const
{
    core::Vec3f target = m_position + Forward();
    return core::Mat4x4::LookAt(m_position, target, { 0.0f, 1.0f, 0.0f });
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
