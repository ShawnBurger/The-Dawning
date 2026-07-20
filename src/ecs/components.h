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
// Transform — double-precision world position with local rotation/scale
// =============================================================================
struct Transform
{
    core::Vec3d position = { 0.0, 0.0, 0.0 };
    core::Quatf rotation = core::Quatf::Identity();
    core::Vec3f scale    = { 1.0f, 1.0f, 1.0f };

    core::Mat4x4 ToCameraRelativeMatrix(const core::Vec3d& cameraPosition) const
    {
        const core::Vec3f relativePosition = (position - cameraPosition).ToFloat();
        core::Mat4x4 s = core::Mat4x4::Scaling(scale.x, scale.y, scale.z);
        core::Mat4x4 r = core::Mat4x4::FromQuaternion(rotation.Normalized());
        core::Mat4x4 t = core::Mat4x4::Translation(relativePosition);
        return s * r * t;
    }
};

// =============================================================================
// Velocity — linear and angular velocity for physics/animation
// =============================================================================
struct Velocity
{
    core::Vec3f linear  = { 0.0f, 0.0f, 0.0f };
    core::Vec3f angular = { 0.0f, 0.0f, 0.0f };  // Body-local axis * angular rate (rad/s)
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
    core::Color albedo              = core::Color::White();
    float       roughness           = 0.5f;
    float       metallic            = 0.0f;
    uint32_t    albedoTextureHandle = UINT32_MAX;
    uint32_t    normalTextureHandle = UINT32_MAX;

    // Packed occlusion / roughness / metallic, glTF convention: AO in R,
    // roughness in G, metallic in B. One fetch instead of three, and it matches
    // what real asset pipelines already export, so imported materials will not
    // need repacking. When present it MODULATES the scalars above rather than
    // replacing them, which is also what glTF specifies - the scalars stay
    // meaningful as per-instance tints.
    uint32_t    ormTextureHandle = UINT32_MAX;

    // Emissive radiance the surface adds regardless of incident light. The
    // texture tints it per-texel; the colour and strength below scale it, so a
    // single greyscale mask can drive many differently coloured emitters.
    //
    // NOTE: this is emission for SHADING only. Neither path treats emissive
    // surfaces as light sources - the path tracer will see them when a ray
    // happens to land on one, but there is no NEE sampling of emitters, so a
    // bright panel will not illuminate the room. That needs light sampling,
    // which is a separate piece of work.
    core::Color emissive             = core::Color{ 0.0f, 0.0f, 0.0f, 1.0f };
    float       emissiveStrength     = 0.0f;
    uint32_t    emissiveTextureHandle = UINT32_MAX;
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

// =============================================================================
// Flight & Physics (Phase 4, Stage 1) — APPENDED, do not reorder the above.
// =============================================================================
// These are the dynamics-state components for the six-DOF rigid-body flight
// model. They follow docs/research/FLIGHT_PHYSICS_DESIGN.md §3 (the double-
// precision boundary) and §6 (single-source-of-truth pose).
//
// The world POSE is NOT duplicated here: position lives in Transform.position
// (already Vec3d) and orientation in Transform.rotation (already Quatf). The
// integrator is Each<Transform, RigidBody>: it reads the pose from Transform,
// integrates, and writes it back. Duplicating position would create a second
// copy free to diverge — see FLIGHT_PHYSICS_DESIGN.md §6.
//
// Frame conventions (fixed here, load-bearing for every sim function):
//   - forceAccum / linearVelocity  are WORLD frame.
//   - torqueAccum / angularVelocity are BODY frame, which is the natural frame
//     for the diagonal body-space inertia and its gyroscopic coupling.
// The two conventions never mix: linear dynamics live in world space, angular
// dynamics in body space, and the exponential-map orientation update is the
// only bridge between them.
// =============================================================================

// -----------------------------------------------------------------------------
// RigidBody — six-DOF dynamics state (semi-implicit Euler; see sim/rigid_body.h)
// -----------------------------------------------------------------------------
struct RigidBody
{
    // WORLD-frame linear velocity. Vec3d by necessity (FLIGHT_PHYSICS_DESIGN §3):
    // it accumulates directly into the Vec3d Transform.position every step, and
    // orbital conservation laws (½v² − μ/r, r×v) are written over it.
    core::Vec3d linearVelocity = { 0.0, 0.0, 0.0 };

    // BODY-frame angular velocity (rad/s). Bounded O(1–10), never a world
    // position, so Vec3f — matching Velocity.angular and the deep-dive.
    core::Vec3f angularVelocity = { 0.0f, 0.0f, 0.0f };

    // Reciprocal mass. double (not float) so it multiplies the Vec3d velocity
    // update in uniform double arithmetic (FLIGHT_PHYSICS_DESIGN §3). 0 == a
    // static / infinite-mass body: force produces no linear acceleration.
    double invMass = 0.0;

    // BODY-frame reciprocal principal moments of inertia (1/Ixx, 1/Iyy, 1/Izz).
    // Diagonal-only for the first slice (§2.5): distinct entries already produce
    // the full non-commuting tumble, so no Mat3x3 is needed. A zero entry locks
    // that axis (infinite inertia).
    core::Vec3f invInertiaDiag = { 0.0f, 0.0f, 0.0f };

    // Per-step wrench accumulators, zeroed by the integrator after each step.
    core::Vec3d forceAccum  = { 0.0, 0.0, 0.0 };   // WORLD frame, newtons
    core::Vec3f torqueAccum = { 0.0f, 0.0f, 0.0f }; // BODY frame, newton-metres

    // Reserved for render-side interpolation (FLIGHT_PHYSICS_DESIGN §6). NOT read
    // by the simulation — reading interpolated pose back into the sim would break
    // determinism. Present now so adding interpolation later is not a component
    // change. Unused in Stage 1.
    core::Vec3d prevPosition = { 0.0, 0.0, 0.0 };
    core::Quatf prevRotation = core::Quatf::Identity();
};

// -----------------------------------------------------------------------------
// Thruster — one discrete thruster mounted at a body-local offset.
// -----------------------------------------------------------------------------
// Force = localDirection * maxForce * throttle; torque about the centre of mass
// = (localPosition − centreOfMass) × force  (Ship deep-dive PART 2). All fields
// are body-local geometry, comfortably inside float (a ship is tens of metres).
// localDirection is expected to be unit length; it is used as-is, not
// renormalised, so the force formula matches the design exactly.
struct Thruster
{
    core::Vec3f localPosition  = { 0.0f, 0.0f, 0.0f }; // offset from body origin, metres
    core::Vec3f localDirection = { 0.0f, 0.0f, 1.0f }; // unit thrust direction (body)
    float       maxForce       = 0.0f;                  // newtons at full throttle
    float       throttle       = 0.0f;                  // commanded [0,1]
};

// -----------------------------------------------------------------------------
// ThrusterSet — a small fixed-capacity bank of thrusters for one body.
// -----------------------------------------------------------------------------
// Fixed capacity keeps the component POD (no heap, direct memcpy for save/load).
// 32 covers the Ship deep-dive's 24-nozzle layout with headroom.
struct ThrusterSet
{
    static constexpr uint32_t kMaxThrusters = 32;

    Thruster thrusters[kMaxThrusters] = {};
    uint32_t count = 0; // number of live entries in thrusters[]
};

// -----------------------------------------------------------------------------
// FlightControl — six-axis pilot demand plus the flight-assist mode.
// -----------------------------------------------------------------------------
// Data only for Stage 1. The control-demand → wrench mapping and the coupled
// auto-brake are FLIGHT_PHYSICS_DESIGN Stage 2/3 (deferred); nothing consumes
// this component yet. Each demand axis is normalised pilot intent in [-1, 1].
enum class FlightMode : uint32_t
{
    Coupled   = 0, // flight assist on: auto-brake toward zero when stick centred
    Decoupled = 1, // raw Newtonian: no assist
};

struct FlightControl
{
    core::Vec3f linearDemand  = { 0.0f, 0.0f, 0.0f }; // strafe X / lift Y / thrust Z (body)
    core::Vec3f angularDemand = { 0.0f, 0.0f, 0.0f }; // pitch X / yaw Y / roll Z (body)
    FlightMode  mode          = FlightMode::Coupled;
};

} // namespace ecs
