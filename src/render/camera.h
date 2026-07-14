#pragma once
// =============================================================================
// render/camera.h — FPS Camera
// =============================================================================
// Free-fly camera with mouse look and WASD movement.
// Left-handed: +Z forward, +Y up, +X right.
// Produces view and projection matrices for constant buffer upload.
// =============================================================================

#include "../core/types.h"

namespace render
{

class Camera
{
public:
    void Init(const core::Vec3f& position, float yawDeg, float pitchDeg);

    // Call once per frame with input deltas
    void Update(float dt,
                float mouseDeltaX, float mouseDeltaY,
                bool forward, bool backward,
                bool left, bool right,
                bool up, bool down,
                bool sprint);

    // Matrices
    core::Mat4x4 ViewMatrix() const;
    core::Mat4x4 ProjectionMatrix(float aspectRatio) const;
    core::Mat4x4 ViewProjectionMatrix(float aspectRatio) const;

    // Accessors
    core::Vec3f Position() const { return m_position; }
    core::Vec3f Forward() const;
    core::Vec3f Right() const;
    core::Vec3f Up() const { return { 0.0f, 1.0f, 0.0f }; }

    float Yaw() const { return m_yaw; }
    float Pitch() const { return m_pitch; }

    // Settings
    void SetMoveSpeed(float speed) { m_moveSpeed = speed; }
    void SetSprintMultiplier(float mult) { m_sprintMult = mult; }
    void SetSensitivity(float sens) { m_sensitivity = sens; }
    void SetFOV(float fovDegrees) { m_fovDeg = fovDegrees; }
    void SetClipPlanes(float nearZ, float farZ) { m_nearZ = nearZ; m_farZ = farZ; }

    float GetFOV() const { return m_fovDeg; }
    float GetNearZ() const { return m_nearZ; }
    float GetFarZ() const { return m_farZ; }

private:
    core::Vec3f m_position = { 0.0f, 0.0f, 0.0f };
    float m_yaw = 0.0f;      // Degrees, 0 = looking down +Z
    float m_pitch = 0.0f;    // Degrees, clamped to [-89, 89]

    float m_moveSpeed = 5.0f;
    float m_sprintMult = 3.0f;
    float m_sensitivity = 0.15f;
    float m_fovDeg = 70.0f;
    float m_nearZ = 0.1f;
    float m_farZ = 10000.0f;
};

} // namespace render
