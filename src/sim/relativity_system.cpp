// =============================================================================
// sim/relativity_system.cpp - frame-aware ECS proper-time adapter
// =============================================================================

#include "relativity_system.h"

#include <cmath>
#include <unordered_map>
#include <vector>

namespace sim
{

namespace
{

bool IsFinite(const Vec3d& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool IsSafeLocal(const Vec3d& value)
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

struct GravitySource
{
    FrameId frame = kInvalidFrame;
    Vec3d localPosition;
    double mu = 0.0;
    double radius = 0.0;
    bool isSource = false;
};

struct StagedClock
{
    ecs::RelativisticClock* destination = nullptr;
    ecs::RelativisticClock value;
};

} // namespace

RelativisticClockStepResult StepRelativisticClocks(
    ecs::Registry& registry,
    const FrameGraph& frames,
    FrameId masterFrame,
    double dt,
    std::span<const ClockGravityBinding> gravityBindings)
{
    RelativisticClockStepResult result;
    if (!(dt > 0.0) || !std::isfinite(dt))
        return result;

    auto* clockPool = registry.GetPool<ecs::RelativisticClock>();
    if (!clockPool)
    {
        result.accepted = true;
        return result;
    }
    if (masterFrame >= frames.FrameCount() ||
        !IsUsableFrame(frames.GetFrame(masterFrame)))
        return result;

    std::unordered_map<uint32_t, uint64_t> bindingByEntity;
    bindingByEntity.reserve(gravityBindings.size() * 2u);
    for (const ClockGravityBinding& binding : gravityBindings)
    {
        if (!registry.IsAlive(binding.entity) ||
            !registry.Has<ecs::RelativisticClock>(binding.entity) ||
            binding.primaryBodyId == kNoClockGravitySource ||
            !bindingByEntity.emplace(
                binding.entity.Index(), binding.primaryBodyId).second)
            return result;
    }

    std::unordered_map<uint64_t, GravitySource> sources;
    auto* gravityPool = registry.GetPool<ecs::GravitationalBody>();
    if (gravityPool)
    {
        sources.reserve(gravityPool->Count() * 2u);
        for (uint32_t i = 0; i < gravityPool->Count(); ++i)
        {
            const uint32_t entityIndex = gravityPool->EntityAt(i);
            const ecs::GravitationalBody& gravity = gravityPool->DataAt(i);
            if (!registry.HasByIndex<ecs::Transform>(entityIndex) ||
                !registry.HasByIndex<ecs::SpatialFrame>(entityIndex) ||
                !std::isfinite(gravity.mu) || gravity.mu < 0.0 ||
                !std::isfinite(gravity.radius) || gravity.radius < 0.0 ||
                (gravity.isSource && !(gravity.mu > 0.0)))
                return result;

            const ecs::Transform& transform =
                registry.GetByIndex<ecs::Transform>(entityIndex);
            const FrameId frame = static_cast<FrameId>(
                registry.GetByIndex<ecs::SpatialFrame>(entityIndex).frameId);
            if (frame >= frames.FrameCount() ||
                !IsUsableFrame(frames.GetFrame(frame)) ||
                !IsSafeLocal(transform.position))
                return result;

            GravitySource source{
                frame, transform.position, gravity.mu, gravity.radius,
                gravity.isSource,
            };
            if (!sources.emplace(gravity.bodyId, source).second)
                return result;
        }
    }

    std::vector<StagedClock> staged;
    staged.reserve(clockPool->Count());
    for (uint32_t i = 0; i < clockPool->Count(); ++i)
    {
        const uint32_t entityIndex = clockPool->EntityAt(i);
        ecs::RelativisticClock& clock = clockPool->DataAt(i);
        if (!registry.HasByIndex<ecs::Transform>(entityIndex) ||
            !registry.HasByIndex<ecs::SpatialFrame>(entityIndex) ||
            !registry.HasByIndex<ecs::RigidBody>(entityIndex) ||
            !std::isfinite(clock.coordinateTime) ||
            !std::isfinite(clock.properTimeDeviation) ||
            clock.coordinateTime < 0.0 || clock.properTimeDeviation > 0.0)
            return result;

        const ecs::Transform& transform =
            registry.GetByIndex<ecs::Transform>(entityIndex);
        const ecs::RigidBody& rigidBody =
            registry.GetByIndex<ecs::RigidBody>(entityIndex);
        const FrameId frame = static_cast<FrameId>(
            registry.GetByIndex<ecs::SpatialFrame>(entityIndex).frameId);
        if (frame >= frames.FrameCount() ||
            !IsUsableFrame(frames.GetFrame(frame)) ||
            !IsSafeLocal(transform.position) ||
            !IsFinite(rigidBody.linearVelocity))
            return result;

        const Body body{ frame, transform.position, rigidBody.linearVelocity };
        const double beta = BetaInMasterFrame(frames, body, masterFrame);
        if (!std::isfinite(beta) || beta < 0.0)
            return result;

        double factorMinusOne = SRDilationFactorMinusOne(beta);
        const auto binding = bindingByEntity.find(entityIndex);
        if (binding != bindingByEntity.end())
        {
            const auto source = sources.find(binding->second);
            if (source == sources.end() || !source->second.isSource ||
                !(source->second.mu > 0.0))
                return result;
            const Body sourceBody{
                source->second.frame, source->second.localPosition, Vec3d{},
            };
            const WorldPos bodyWorld = frames.ResolveWorldPos(body);
            const WorldPos sourceWorld = frames.ResolveWorldPos(sourceBody);
            const Vec3d separation = Separation(sourceWorld, bodyWorld);
            const double distance = std::hypot(
                separation.x, separation.y, separation.z);
            const double softening = SofteningLength(
                source->second.mu, source->second.radius);
            if (!std::isfinite(distance) || !std::isfinite(softening) ||
                !(softening > 0.0))
                return result;
            factorMinusOne = CombinedDilationFactorMinusOne(
                beta, source->second.mu, distance, softening);
            ++result.gravitationalClockCount;
        }
        if (!std::isfinite(factorMinusOne) || factorMinusOne < -1.0 ||
            factorMinusOne > 0.0)
            return result;

        ecs::RelativisticClock next = clock;
        AdvanceClock(next, factorMinusOne, dt);
        if (!std::isfinite(next.coordinateTime) ||
            !std::isfinite(next.properTimeDeviation) ||
            next.coordinateTime != clock.coordinateTime + dt)
            return RelativisticClockStepResult{};
        staged.push_back(StagedClock{ &clock, next });
    }

    for (const StagedClock& next : staged)
        *next.destination = next.value;

    result.accepted = true;
    result.clockCount = static_cast<uint32_t>(staged.size());
    return result;
}

} // namespace sim
