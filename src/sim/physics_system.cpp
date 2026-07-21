// =============================================================================
// sim/physics_system.cpp — Fixed-step flight physics system (ECS glue, GPU-free)
// =============================================================================

#include "physics_system.h"

#include "nbody.h"
#include "rigid_body.h"
#include "thrusters.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sim
{

namespace
{

bool IsFinite(const core::Vec3d& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool IsSafeLocal(const core::Vec3d& value)
{
    return IsFinite(value) && std::fabs(value.x) <= kSectorSize &&
           std::fabs(value.y) <= kSectorSize &&
           std::fabs(value.z) <= kSectorSize;
}

bool IsUsableFrame(const Frame& frame)
{
    return ValidSector(frame.origin) && IsCanonical(frame.origin) &&
           IsFinite(frame.velocity);
}

bool IsKnownOwner(ecs::OrbitOwner owner)
{
    return owner == ecs::OrbitOwner::NBodyActive ||
           owner == ecs::OrbitOwner::OnRails ||
           owner == ecs::OrbitOwner::ForceIntegrated;
}

bool IsValidGravity(const ecs::GravitationalBody& gravity)
{
    return std::isfinite(gravity.mu) && gravity.mu >= 0.0 &&
           std::isfinite(gravity.radius) && gravity.radius >= 0.0 &&
           (!gravity.isSource || gravity.mu > 0.0) &&
           IsKnownOwner(gravity.owner);
}

struct GravityEntity
{
    FrameId frame = kInvalidFrame;
    core::Vec3d localPosition;
    ecs::GravitationalBody gravity;
    ecs::RigidBody* rigidBody = nullptr;
};

struct StagedForce
{
    ecs::RigidBody* rigidBody = nullptr;
    core::Vec3d force;
};

} // namespace

GravityAccumulationResult AccumulateForceIntegratedGravity(
    ecs::Registry& registry, const FrameGraph& frames)
{
    GravityAccumulationResult result;
    ecs::ComponentPool<ecs::GravitationalBody>* gravityPool =
        registry.GetPool<ecs::GravitationalBody>();
    if (!gravityPool)
    {
        result.accepted = true;
        return result;
    }

    std::vector<GravityEntity> entities;
    entities.reserve(gravityPool->Count());
    std::vector<uint64_t> bodyIds;
    bodyIds.reserve(gravityPool->Count());

    for (uint32_t i = 0; i < gravityPool->Count(); ++i)
    {
        const uint32_t entityIndex = gravityPool->EntityAt(i);
        const ecs::GravitationalBody& gravity = gravityPool->DataAt(i);
        if (!registry.HasByIndex<ecs::Transform>(entityIndex) ||
            !registry.HasByIndex<ecs::SpatialFrame>(entityIndex) ||
            !IsValidGravity(gravity))
            return result;

        const ecs::Transform& transform =
            registry.GetByIndex<ecs::Transform>(entityIndex);
        const ecs::SpatialFrame& spatialFrame =
            registry.GetByIndex<ecs::SpatialFrame>(entityIndex);
        const FrameId frame = static_cast<FrameId>(spatialFrame.frameId);
        if (frame >= frames.FrameCount() || !IsSafeLocal(transform.position) ||
            !IsUsableFrame(frames.GetFrame(frame)))
            return result;

        ecs::RigidBody* rigidBody = nullptr;
        if (registry.HasByIndex<ecs::RigidBody>(entityIndex))
            rigidBody = &registry.GetByIndex<ecs::RigidBody>(entityIndex);

        if (gravity.owner == ecs::OrbitOwner::ForceIntegrated)
        {
            if (!rigidBody || !std::isfinite(rigidBody->invMass) ||
                !(rigidBody->invMass > 0.0) ||
                !IsFinite(rigidBody->forceAccum))
                return result;
            const double mass = 1.0 / rigidBody->invMass;
            if (!std::isfinite(mass) || !(mass > 0.0))
                return result;
        }

        GravityEntity entity;
        entity.frame = frame;
        entity.localPosition = transform.position;
        entity.gravity = gravity;
        entity.rigidBody = rigidBody;
        entities.push_back(entity);
        bodyIds.push_back(gravity.bodyId);
    }

    std::sort(bodyIds.begin(), bodyIds.end());
    if (std::adjacent_find(bodyIds.begin(), bodyIds.end()) != bodyIds.end())
        return result;

    for (const GravityEntity& entity : entities)
        if (entity.gravity.isSource)
            ++result.sourceCount;

    std::vector<StagedForce> staged;
    for (const GravityEntity& target : entities)
    {
        if (target.gravity.owner != ecs::OrbitOwner::ForceIntegrated)
            continue;

        std::vector<NBodyParticle> sources;
        sources.reserve(result.sourceCount);
        for (const GravityEntity& source : entities)
        {
            if (!source.gravity.isSource)
                continue;

            const Body sourceBody{ source.frame, source.localPosition, core::Vec3d{} };
            const core::Vec3d sourcePosition =
                frames.ExpressInFrame(sourceBody, target.frame);
            const double softening =
                SofteningLength(source.gravity.mu, source.gravity.radius);
            if (!IsFinite(sourcePosition) || !std::isfinite(softening) ||
                !(softening > 0.0))
                return GravityAccumulationResult{};

            NBodyParticle particle;
            particle.position = sourcePosition;
            particle.mu = source.gravity.mu;
            particle.softening = softening;
            particle.radius = source.gravity.radius;
            particle.bodyId = source.gravity.bodyId;
            particle.isSource = true;
            sources.push_back(particle);
        }

        const double targetSoftening =
            SofteningLength(target.gravity.mu, target.gravity.radius);
        if (!std::isfinite(targetSoftening) || !(targetSoftening > 0.0))
            return GravityAccumulationResult{};

        const core::Vec3d acceleration = GravityAccelerationAt(
            sources, target.localPosition, targetSoftening,
            target.gravity.bodyId);
        const double mass = 1.0 / target.rigidBody->invMass;
        const core::Vec3d nextForce =
            target.rigidBody->forceAccum + acceleration * mass;
        if (!IsFinite(acceleration) || !IsFinite(nextForce))
            return GravityAccumulationResult{};

        staged.push_back(StagedForce{ target.rigidBody, nextForce });
    }

    for (const StagedForce& force : staged)
        force.rigidBody->forceAccum = force.force;

    result.accepted = true;
    result.targetCount = static_cast<uint32_t>(staged.size());
    return result;
}

void StepFlightPhysics(ecs::Registry& registry, double dt, const FlightAssistParams& params)
{
    // Whole-system no-op on a bad step, so the fixed-step accumulator can retry
    // without any body advancing on stale state (matches IntegrateRigidBody).
    if (!(dt > 0.0) || !std::isfinite(dt))
        return;

    // No external accelerations in Stage 2 (gravity is Stage 5, §4). The body
    // origin is the centre of mass, so lever arms are measured from zero.
    const core::Vec3d kZeroForce{ 0.0, 0.0, 0.0 };
    const core::Vec3f kZeroTorque{ 0.0f, 0.0f, 0.0f };
    const core::Vec3f kCoM{ 0.0f, 0.0f, 0.0f };

    registry.Each<ecs::Transform, ecs::RigidBody>(
        [&](uint32_t entityIndex, ecs::Transform& transform, ecs::RigidBody& body)
        {
            if (registry.HasByIndex<ecs::GravitationalBody>(entityIndex) &&
                registry.GetByIndex<ecs::GravitationalBody>(entityIndex).owner !=
                    ecs::OrbitOwner::ForceIntegrated)
            {
                return;
            }

            const bool hasControl   = registry.HasByIndex<ecs::FlightControl>(entityIndex);
            const bool hasThrusters = registry.HasByIndex<ecs::ThrusterSet>(entityIndex);

            if (hasControl)
            {
                const ecs::FlightControl& fc =
                    registry.GetByIndex<ecs::FlightControl>(entityIndex);

                if (fc.mode == ecs::FlightMode::Coupled)
                {
                    // Flight assist has no reactionless fallback. Its desired
                    // wrench is realized only through installed nozzles, and those
                    // throttles are also the exhaust/damage feedback state.
                    if (hasThrusters)
                    {
                        ecs::ThrusterSet& ts =
                            registry.GetByIndex<ecs::ThrusterSet>(entityIndex);
                        const AssistWrench desired = ComputeFlightAssist(
                            body, transform.rotation, fc.linearDemand,
                            fc.angularDemand, params);
                        AllocateThrustersForWrench(
                            ts, desired.worldForce, desired.bodyTorque,
                            transform.rotation, kCoM);
                        const Wrench realized = ComputeWrench(ts, kCoM);
                        AccumulateBodyWrench(body, transform.rotation, realized);
                    }
                }
                else if (hasThrusters)
                {
                    // Decoupled: pilot demand -> greedy allocation -> thruster wrench.
                    ecs::ThrusterSet& ts =
                        registry.GetByIndex<ecs::ThrusterSet>(entityIndex);
                    AllocateThrusters(ts, fc.linearDemand, fc.angularDemand, kCoM);
                    const Wrench w = ComputeWrench(ts, kCoM);
                    AccumulateBodyWrench(body, transform.rotation, w);
                }
                // Decoupled with no thrusters has no actuators -> pure coast.
            }
            else if (hasThrusters)
            {
                // Scripted thrusters (no pilot): realise whatever throttles are set.
                ecs::ThrusterSet& ts = registry.GetByIndex<ecs::ThrusterSet>(entityIndex);
                const Wrench w = ComputeWrench(ts, kCoM);
                AccumulateBodyWrench(body, transform.rotation, w);
            }

            // One semi-implicit-Euler step of the PASSED dt, writing the pose back
            // into Transform and zeroing the accumulators.
            IntegrateRigidBody(transform.position, transform.rotation, body,
                               kZeroForce, kZeroTorque, dt);
        });
}

} // namespace sim
