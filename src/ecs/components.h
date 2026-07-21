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

// =============================================================================
// Orbital / N-body gravity (Sim Stage 1) — APPENDED, do not reorder the above.
// =============================================================================
// The dynamics-state components for the N-body orbital core. They follow
// docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md — the "DECISION REVISION"
// (N-body-default in the active system, Kepler rails as a distant LOD) and §5
// (one owner per body per step). See src/sim/nbody.h and src/sim/kepler.h.
//
// These are plain data (doubles + ids). They are consumed by the pure functions
// in sim/nbody.h and sim/kepler.h exactly the way ecs::RigidBody is consumed by
// sim/rigid_body.h — sim includes ecs; ecs never includes sim, so no cycle.
//
// World POSE is NOT duplicated here: the orbital position lives in
// Transform.position (Vec3d) and the orbital velocity in RigidBody.linearVelocity
// (Vec3d), the same single-source-of-truth carriers §4.4 makes the on-rails <->
// N-body handoff continuous through. GravitationalBody/OrbitState add only the
// gravitational parameters, the LOD owner token, and the analytic rails.
// =============================================================================

// The level-of-detail owner of a body's motion THIS step. Exactly one owner per
// body per step (RELATIVISTIC_SIM_ARCHITECTURE.md §5.1, revised): a body is
// either integrated by the N-body Forest-Ruth stepper OR advanced on analytic
// Kepler rails, never both. Debug-asserted in the stepper. An OnRails body must
// receive NO separate N-body force (the pull is already in its ellipse) — the
// double-count negative control the design demands.
enum class OrbitOwner : uint32_t
{
    NBodyActive = 0, // full N-body gravity this step (the active-system default)
    OnRails     = 1, // analytic Kepler propagation around its primary this step
};

// -----------------------------------------------------------------------------
// OrbitalElements — the classical osculating orbit an on-rails body propagates.
// -----------------------------------------------------------------------------
// Angles in radians, distance in metres. The anomaly stored is the TRUE anomaly
// at the epoch (OrbitState::epoch), NOT the mean anomaly: true anomaly is exact
// in the (e->1, M->0) critical region the research (PHYSICS_RESEARCH_REFERENCE §1)
// flags, where mean-anomaly seeding is worst-conditioned. semiMajorAxis is
// negative for hyperbolic orbits (e>1), the standard convention.
struct OrbitalElements
{
    double semiMajorAxis    = 0.0; // a  (m); a<0 for hyperbolic
    double eccentricity     = 0.0; // e  (0<=e<1 ellipse, ~1 parabola, >1 hyperbola)
    double inclination      = 0.0; // i  (rad)
    double longitudeAscNode = 0.0; // Omega (rad)
    double argPeriapsis     = 0.0; // omega (rad)
    double trueAnomaly      = 0.0; // nu    (rad) AT EPOCH
};

// -----------------------------------------------------------------------------
// GravitationalBody — a body that produces and/or feels Newtonian gravity.
// -----------------------------------------------------------------------------
struct GravitationalBody
{
    // Gravitational parameter mu = G*M (m^3 s^-2). This is the quantity the force
    // law g = -mu*r/(r^2+eps^2)^1.5 and the future GR clock both use, and it is
    // known far more precisely than G and M separately. mass = mu / G. A test
    // particle (ship, missile, debris) may carry mu = 0: it FEELS gravity but is
    // a negligible source (see isSource).
    double mu = 0.0;

    // Physical radius (m). Feeds the shared softening floor
    // eps = max(radius, r_s + eps0) and close-encounter/collision detection.
    double radius = 0.0;

    // STABLE identity. The force summation is done in ascending bodyId order so
    // floating-point non-associativity cannot make the result depend on iteration
    // accident (RELATIVISTIC_SIM_ARCHITECTURE.md revision, "Determinism"). Two ids
    // must never collide within one active system.
    uint64_t bodyId = 0;

    // true  => a massive contributor: produces gravity AND feels it.
    // false => a test particle: feels gravity from sources, produces none. Its
    //          reaction on the sources is dropped (standard test-particle limit),
    //          which is exact in the mu->0 limit and keeps ships/debris O(N) not
    //          O(N^2). A test particle under THRUST is not stepped here at all —
    //          it goes through the rigid-body lane (see sim/nbody.h operator split).
    bool isSource = true;

    // LOD owner THIS step. See OrbitOwner.
    OrbitOwner owner = OrbitOwner::NBodyActive;
};

// -----------------------------------------------------------------------------
// OrbitState — the analytic rails an on-rails body rides, plus its primary.
// -----------------------------------------------------------------------------
// Valid/used when GravitationalBody::owner == OnRails. On promotion (player
// enters the system) the N-body state is SEEDED from these elements at the
// promotion instant; on demotion osculating elements are FIT back into here from
// the current (r,v) — both continuous in position and velocity by construction
// (sim/nbody.h Promote/Demote).
struct OrbitState
{
    OrbitalElements elements;         // the osculating orbit, defined at `epoch`
    double          primaryMu = 0.0;  // mu of the primary this body orbits
    uint64_t        primaryBodyId = 0;// stable id of the primary
    double          epoch = 0.0;      // coordinate time at which `elements` hold
};

// =============================================================================
// Relativity (Sim Stage 2) — APPENDED, do not reorder the above.
// =============================================================================
// The state a relativistic body needs, per docs/research/
// RELATIVISTIC_SIM_ARCHITECTURE.md §3 (momentum-space dynamics) and §2 (proper
// time). Consumed by the pure functions in src/sim/relativity.h exactly the way
// RigidBody is consumed by sim/rigid_body.h — sim includes ecs; ecs never
// includes sim, so no cycle. These add NO call site; the ship-slice lane wires
// them.
//
// WHY MOMENTUM IS A STORED COMPONENT AND NOT DERIVED FROM RigidBody.linearVelocity
// (the §3.3 decision, made concrete):
//   The architecture (§3.1) advances relativistic MOMENTUM `p += F·dt` and
//   RECOVERS velocity `v = p / sqrt(m² + (|p|/c)²)`. Storing `p` as the
//   authoritative dynamical state — rather than re-deriving it from a stored
//   velocity each step — buys two things a velocity store cannot:
//     1. |v| < c is a STRUCTURAL property of the stored state. Any finite `p`
//        recovers a strictly sub-c `v`; there is no γ to clamp.
//     2. γ = sqrt(1 + (|p|/(mc))²) is precise at ARBITRARY boost. Recovering γ
//        from a velocity stored near c would form 1 − v²/c², a catastrophic
//        cancellation that loses all precision exactly where dilation matters.
//   §3.3 says linearVelocity "stays a Vec3d ... and p is derived/stored
//   ALONGSIDE it" — so momentum lives here as its own field, and the recovered
//   `v` is written back into RigidBody.linearVelocity each step for the position
//   integrator and the orbital invariants (½v²−μ/r, r×v) that are written over
//   velocity. RECONCILIATION CONTRACT: in the relativistic lane RelativisticBody
//   .momentum is authoritative and RigidBody.linearVelocity is a per-step
//   projection of it; a body uses EITHER the Newtonian velocity lane OR the
//   relativistic momentum lane, never writing both independently — the same
//   one-owner discipline OrbitOwner enforces for the N-body/rails split.
// =============================================================================

// -----------------------------------------------------------------------------
// RelativisticBody — momentum-space dynamical state (see sim/relativity.h).
// -----------------------------------------------------------------------------
struct RelativisticBody
{
    // Relativistic momentum p = γ m v (kg·m/s), WORLD/frame vector like
    // RigidBody.linearVelocity. This is the AUTHORITATIVE state the force step
    // advances (p += F·dt). |v| < c is structural because velocity is recovered
    // as v = p / sqrt(m² + (|p|/c)²) for whatever finite p this holds. Vec3d by
    // RULE 1: it is a world-frame dynamical vector, and its magnitude spans the
    // full non-relativistic-to-ultrarelativistic range in double.
    core::Vec3d momentum = { 0.0, 0.0, 0.0 };

    // Rest mass m (kg), > 0 for a relativistic body. The recovery radicand
    // m² + (|p|/c)² is strictly positive for m > 0, which is WHY the recovery
    // cannot go imaginary or divide by zero. Equals 1/RigidBody.invMass when the
    // body is also a rigid body; stored here so the relativistic functions are
    // pure in (p, m) and need no RigidBody.
    double restMass = 1.0;
};

// -----------------------------------------------------------------------------
// RelativisticClock — the proper-time sidecar (see sim/relativity.h, §2.2).
// -----------------------------------------------------------------------------
// Proper time is accumulated as the DEVIATION Σ(dτ − dt), NEVER as τ += dτ:
// near β=0 and r=∞ the dilation factor is ~1.0 and dτ − dt is a ~1e-9 residual
// that would drown if summed onto a coordinate-time-magnitude running total.
// The deviation is kept in its own double so the tiny signal survives a long
// trip; proper time is RECONSTRUCTED as coordinateTime + deviation on demand
// (sim::ProperTime), which is not the same as accumulating τ directly — the
// residual is never rounded against the large magnitude during accumulation.
struct RelativisticClock
{
    double coordinateTime       = 0.0; // Σ dt : elapsed coordinate time (master frame)
    double properTimeDeviation  = 0.0; // Σ (dτ − dt) : the isolated dilation residual (≤ 0)
};

// -----------------------------------------------------------------------------
// SpatialFrame - opt-in reference-frame ownership for precision-safe entities.
// -----------------------------------------------------------------------------
// ecs deliberately does not include sim/reference_frame.h, so this stores the
// underlying sim::FrameId as its uint32_t representation. When this component is
// present, Transform.position, RigidBody.linearVelocity and
// RigidBody.prevPosition are local to this frame. Entities without it retain the
// original world-space convention above.
struct SpatialFrame
{
    uint32_t frameId = UINT32_MAX;
};

} // namespace ecs
