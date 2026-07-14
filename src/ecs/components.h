#pragma once
// =============================================================================
// ecs/components.h — Engine Component Types
// =============================================================================
// All components are plain data structs (POD-like). No virtual functions,
// no heap allocations, no pointers to external resources.
// GPU resource references use handles (indices into resource manager pools).
// =============================================================================

#include "../core/types.h"
#include <cstdint>

namespace ecs
{

// =============================================================================
// Transform — local position/rotation/scale
// =============================================================================
struct Transform
{
    core::Vec3f position = { 0.0f, 0.0f, 0.0f };
    core::Quatf rotation = core::Quatf::Identity();
    core::Vec3f scale    = { 1.0f, 1.0f, 1.0f };

    core::Mat4x4 ToMatrix() const
    {
        core::Mat4x4 s = core::Mat4x4::Scaling(scale.x, scale.y, scale.z);
        core::Mat4x4 r = core::Mat4x4::FromQuaternion(rotation.Normalized());
        core::Mat4x4 t = core::Mat4x4::Translation(position);
        return s * r * t;
    }
};

// =============================================================================
// Velocity — linear and angular velocity for physics/animation
// =============================================================================
struct Velocity
{
    core::Vec3f linear  = { 0.0f, 0.0f, 0.0f };
    core::Vec3f angular = { 0.0f, 0.0f, 0.0f };  // Euler rates (rad/s)
};

// =============================================================================
// MeshInstance — reference to a mesh in the resource manager
// =============================================================================
struct MeshInstance
{
    uint32_t meshHandle = UINT32_MAX;  // Index into ResourceManager mesh pool
    bool     visible = true;
};

// =============================================================================
// Material — PBR material properties
// =============================================================================
struct Material
{
    core::Color albedo    = core::Color::White();
    float       roughness = 0.5f;
    float       metallic  = 0.0f;
};

// =============================================================================
// RotationSpeed — simple spinning behavior
// =============================================================================
struct RotationSpeed
{
    float radiansPerSecond = 1.0f;
    core::Vec3f axis       = { 0.0f, 1.0f, 0.0f };  // Rotation axis
};

// =============================================================================
// Parent — entity hierarchy (stores parent entity index)
// =============================================================================
struct Parent
{
    uint32_t entityIndex = UINT32_MAX;
};

// =============================================================================
// Name — debug/display name for entities
// =============================================================================
struct Name
{
    char text[48] = {};

    void Set(const char* str)
    {
        int i = 0;
        if (str)
            while (str[i] && i < 47) { text[i] = str[i]; i++; }
        text[i] = '\0';
    }
};

// =============================================================================
// Tag components — zero-size markers
// =============================================================================
struct ActiveTag { uint8_t _pad = 0; };

} // namespace ecs
