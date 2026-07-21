// =============================================================================
// sim/orbit_system.cpp - passive orbital ECS adapter
// =============================================================================

#include "orbit_system.h"

#include "nbody.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

bool IsFinite(const core::Vec3f& value)
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

bool IsKnownOwner(ecs::OrbitOwner owner)
{
    return owner == ecs::OrbitOwner::NBodyActive ||
           owner == ecs::OrbitOwner::OnRails ||
           owner == ecs::OrbitOwner::ForceIntegrated;
}

bool IsValidGravity(const ecs::GravitationalBody& gravity)
{
    return IsKnownOwner(gravity.owner) && std::isfinite(gravity.mu) &&
           gravity.mu >= 0.0 && std::isfinite(gravity.radius) &&
           gravity.radius >= 0.0 &&
           (!gravity.isSource || gravity.mu > 0.0);
}

bool IsValidRigidBody(const ecs::RigidBody& body)
{
    return IsFinite(body.linearVelocity) && IsFinite(body.angularVelocity) &&
           IsFinite(body.forceAccum) && IsFinite(body.torqueAccum) &&
           std::isfinite(body.invMass) && body.invMass >= 0.0;
}

bool IsValidOrbit(const ecs::OrbitState& orbit)
{
    const ecs::OrbitalElements& e = orbit.elements;
    const double values[] = {
        e.semiMajorAxis, e.eccentricity, e.inclination,
        e.longitudeAscNode, e.argPeriapsis, e.trueAnomaly,
        orbit.primaryMu, orbit.epoch,
    };
    for (double value : values)
        if (!std::isfinite(value))
            return false;

    if (!(orbit.primaryMu > 0.0) || !(e.eccentricity >= 0.0) ||
        e.eccentricity == 1.0)
        return false;
    if ((e.eccentricity < 1.0 && !(e.semiMajorAxis > 0.0)) ||
        (e.eccentricity > 1.0 && !(e.semiMajorAxis < 0.0)))
        return false;

    const double p = e.semiMajorAxis *
        (1.0 - e.eccentricity * e.eccentricity);
    const double denominator = 1.0 + e.eccentricity * std::cos(e.trueAnomaly);
    return std::isfinite(p) && p > 0.0 &&
           std::isfinite(denominator) && denominator > 0.0;
}

struct EntityRecord
{
    uint32_t entityIndex = UINT32_MAX;
    FrameId frame = kInvalidFrame;
    Vec3d position;
    ecs::RigidBody body;
    ecs::GravitationalBody gravity;
    bool hasOrbit = false;
    ecs::OrbitState orbit;
};

struct WorldState
{
    WorldPos position;
    Vec3d velocity;
};

struct StagedBody
{
    uint32_t entityIndex = UINT32_MAX;
    Vec3d position;
    Vec3d velocity;
    double invMass = 0.0;
    bool updateGravity = false;
    double mu = 0.0;
    double radius = 0.0;
    bool isSource = false;
};

bool IsFinite(const StateVector& state)
{
    return IsFinite(state.position) && IsFinite(state.velocity);
}

} // namespace

PassiveOrbitStepResult StepPassiveOrbits(
    ecs::Registry& registry,
    const FrameGraph& frames,
    FrameId activeFrame,
    double coordinateTime,
    double dt,
    const CloseEncounterConfig& collisionConfig)
{
    PassiveOrbitStepResult result;
    if (!(dt > 0.0) || !std::isfinite(dt) ||
        !std::isfinite(coordinateTime) ||
        !std::isfinite(coordinateTime + dt) ||
        !IsValidCloseEncounterConfig(collisionConfig))
        return result;

    auto* gravityPool = registry.GetPool<ecs::GravitationalBody>();
    if (!gravityPool)
    {
        result.accepted = true;
        return result;
    }

    std::vector<EntityRecord> records;
    records.reserve(gravityPool->Count());
    std::unordered_map<uint64_t, size_t> recordById;
    recordById.reserve(gravityPool->Count() * 2u);

    bool needsActiveFrame = false;
    for (uint32_t i = 0; i < gravityPool->Count(); ++i)
    {
        const uint32_t entityIndex = gravityPool->EntityAt(i);
        const ecs::GravitationalBody& gravity = gravityPool->DataAt(i);
        if (!IsValidGravity(gravity) ||
            !registry.HasByIndex<ecs::Transform>(entityIndex) ||
            !registry.HasByIndex<ecs::SpatialFrame>(entityIndex) ||
            !registry.HasByIndex<ecs::RigidBody>(entityIndex))
            return result;

        const ecs::Transform& transform =
            registry.GetByIndex<ecs::Transform>(entityIndex);
        const ecs::SpatialFrame& spatial =
            registry.GetByIndex<ecs::SpatialFrame>(entityIndex);
        const ecs::RigidBody& body =
            registry.GetByIndex<ecs::RigidBody>(entityIndex);
        const FrameId frame = static_cast<FrameId>(spatial.frameId);
        if (frame >= frames.FrameCount() || !IsUsableFrame(frames.GetFrame(frame)) ||
            !IsSafeLocal(transform.position) || !IsValidRigidBody(body))
            return result;
        if (!recordById.emplace(gravity.bodyId, records.size()).second)
            return result;

        EntityRecord record;
        record.entityIndex = entityIndex;
        record.frame = frame;
        record.position = transform.position;
        record.body = body;
        record.gravity = gravity;
        if (gravity.owner == ecs::OrbitOwner::OnRails)
        {
            if (!registry.HasByIndex<ecs::OrbitState>(entityIndex))
                return result;
            record.hasOrbit = true;
            record.orbit = registry.GetByIndex<ecs::OrbitState>(entityIndex);
            if (!IsValidOrbit(record.orbit))
                return result;
            ++result.railBodyCount;
        }
        else if (gravity.owner == ecs::OrbitOwner::NBodyActive)
        {
            needsActiveFrame = true;
            ++result.nbodyBodyCount;
        }
        records.push_back(record);
    }

    if (needsActiveFrame &&
        (activeFrame >= frames.FrameCount() ||
         !IsUsableFrame(frames.GetFrame(activeFrame))))
        return PassiveOrbitStepResult{};

    std::vector<NBodyParticle> particles;
    particles.reserve(result.nbodyBodyCount);
    std::unordered_map<uint64_t, size_t> nbodyRecordById;
    nbodyRecordById.reserve(result.nbodyBodyCount * 2u);
    for (size_t i = 0; i < records.size(); ++i)
    {
        const EntityRecord& record = records[i];
        if (record.gravity.owner != ecs::OrbitOwner::NBodyActive)
            continue;

        const Body source{ record.frame, record.position,
                           record.body.linearVelocity };
        NBodyParticle particle;
        particle.position = frames.ExpressInFrame(source, activeFrame);
        particle.velocity = frames.ResolveWorldVel(source) -
                            frames.GetFrame(activeFrame).velocity;
        particle.mu = record.gravity.mu;
        particle.radius = record.gravity.radius;
        particle.softening = SofteningLength(particle.mu, particle.radius);
        particle.bodyId = record.gravity.bodyId;
        particle.isSource = record.gravity.isSource;
        if (!IsSafeLocal(particle.position) || !IsFinite(particle.velocity) ||
            !std::isfinite(particle.softening) || !(particle.softening > 0.0))
            return PassiveOrbitStepResult{};
        nbodyRecordById.emplace(particle.bodyId, i);
        particles.push_back(particle);
    }

    CloseEncounterReport collisionReport;
    StepNBodyCollisional(particles, dt, collisionConfig, collisionReport);

    std::unordered_set<uint64_t> survivorIds;
    survivorIds.reserve(particles.size() * 2u);
    for (const NBodyParticle& particle : particles)
    {
        if (!survivorIds.insert(particle.bodyId).second ||
            !IsSafeLocal(particle.position) || !IsFinite(particle.velocity) ||
            !std::isfinite(particle.mu) || particle.mu < 0.0 ||
            !std::isfinite(particle.radius) || particle.radius < 0.0)
            return PassiveOrbitStepResult{};
    }

    std::unordered_map<uint64_t, uint64_t> parent;
    parent.reserve(nbodyRecordById.size() * 2u);
    for (const auto& entry : nbodyRecordById)
        parent.emplace(entry.first, entry.first);
    std::function<uint64_t(uint64_t)> findRoot = [&](uint64_t id) -> uint64_t {
        uint64_t current = id;
        while (parent[current] != current)
            current = parent[current];
        const uint64_t root = current;
        current = id;
        while (parent[current] != current)
        {
            const uint64_t next = parent[current];
            parent[current] = root;
            current = next;
        }
        return root;
    };
    for (const CollisionEvent& event : collisionReport.events)
    {
        if (!event.merged)
            continue;
        if (parent.find(event.survivorId) == parent.end())
            return PassiveOrbitStepResult{};
        for (uint64_t absorbed : event.absorbedIds)
        {
            if (parent.find(absorbed) == parent.end())
                return PassiveOrbitStepResult{};
            const uint64_t a = findRoot(event.survivorId);
            const uint64_t b = findRoot(absorbed);
            const uint64_t lo = (std::min)(a, b);
            const uint64_t hi = (std::max)(a, b);
            parent[hi] = lo;
        }
    }

    std::unordered_map<uint64_t, std::vector<uint64_t>> groups;
    groups.reserve(parent.size());
    for (const auto& entry : parent)
        groups[findRoot(entry.first)].push_back(entry.first);
    // Each group's members are pushed while iterating the unordered_map `parent`,
    // i.e. in hash-bucket order. The merge below sums inertial masses over the
    // group, and IEEE addition is non-associative, so a 3+-body merge's summed
    // mass — hence the survivor's invMass and its whole subsequent trajectory —
    // would depend on STL bucket layout and differ across toolchains
    // (libstdc++/libc++/MSVC iterate uint64_t keys differently). Sort each group
    // ascending by bodyId so the sum follows the id-sorted-summation discipline
    // the force path (nbody.cpp DeterministicOrder) already enforces everywhere.
    for (auto& entry : groups)
        std::sort(entry.second.begin(), entry.second.end());

    std::vector<StagedBody> staged;
    staged.reserve(particles.size() + result.railBodyCount);
    std::unordered_map<uint64_t, WorldState> worldStates;
    worldStates.reserve(records.size() * 2u);

    for (const NBodyParticle& particle : particles)
    {
        const auto binding = nbodyRecordById.find(particle.bodyId);
        if (binding == nbodyRecordById.end())
            return PassiveOrbitStepResult{};
        const EntityRecord& record = records[binding->second];
        const Body stepped{ activeFrame, particle.position, particle.velocity };
        // Preserve the integrator's local result bit-exactly when the body is
        // already in the active frame. Resolving through a large WorldPos and
        // subtracting the same origin can discard tiny post-step displacement.
        const Body local = (record.frame == activeFrame)
            ? stepped
            : frames.Reparent(stepped, record.frame);
        if (!IsSafeLocal(local.localPos) || !IsFinite(local.localVel))
            return PassiveOrbitStepResult{};

        double invMass = record.body.invMass;
        const auto group = groups.find(particle.bodyId);
        if (group != groups.end() && group->second.size() > 1)
        {
            bool infiniteMass = false;
            double totalMass = 0.0;
            for (uint64_t memberId : group->second)
            {
                const EntityRecord& member =
                    records[nbodyRecordById.at(memberId)];
                if (member.body.invMass == 0.0)
                {
                    infiniteMass = true;
                    break;
                }
                totalMass += 1.0 / member.body.invMass;
                if (!std::isfinite(totalMass))
                    return PassiveOrbitStepResult{};
            }
            invMass = infiniteMass ? 0.0 : 1.0 / totalMass;
        }

        staged.push_back(StagedBody{
            record.entityIndex, local.localPos, local.localVel, invMass, true,
            particle.mu, particle.radius, particle.isSource,
        });
        worldStates.emplace(
            particle.bodyId,
            WorldState{ frames.ResolveWorldPos(stepped),
                        frames.ResolveWorldVel(stepped) });
    }

    for (const EntityRecord& record : records)
    {
        if (record.gravity.owner != ecs::OrbitOwner::ForceIntegrated)
            continue;
        const Body body{ record.frame, record.position,
                         record.body.linearVelocity };
        worldStates.emplace(
            record.gravity.bodyId,
            WorldState{ frames.ResolveWorldPos(body),
                        frames.ResolveWorldVel(body) });
    }

    const double targetTime = coordinateTime + dt;
    std::unordered_map<uint64_t, uint8_t> railMarks;
    railMarks.reserve(result.railBodyCount * 2u);
    // Rail bodies whose primary was absorbed by this step's merge need their stored
    // OrbitState.primaryBodyId repointed to the survivor so LATER steps (where the
    // absorbed primary entity no longer exists) still resolve. Staged here, applied
    // in the commit phase only on the accepted path. {entityIndex, survivorBodyId}.
    std::vector<std::pair<uint32_t, uint64_t>> primaryRepoints;
    std::function<bool(uint64_t)> resolveRail = [&](uint64_t bodyId) -> bool {
        if (worldStates.find(bodyId) != worldStates.end())
            return true;
        const auto found = recordById.find(bodyId);
        if (found == recordById.end())
            return false;
        const EntityRecord& record = records[found->second];
        if (record.gravity.owner != ecs::OrbitOwner::OnRails || !record.hasOrbit)
            return false;
        uint8_t& mark = railMarks[bodyId];
        if (mark == 1)
            return false;
        if (mark == 2)
            return true;
        mark = 1;

        // Follow the primary through this step's merge. If the primary was an
        // NBodyActive source that was absorbed as a NON-survivor, its bodyId is
        // gone from worldStates and its stored record still reads NBodyActive, so
        // a direct resolveRail(primaryId) returns false and stalls the ENTIRE step
        // — permanently, since nothing commits and the next step reproduces the
        // identical merge (the review's HIGH finding). The union-find root is the
        // cluster's min bodyId, which is exactly the collision survivor and IS a
        // surviving particle already in worldStates. Only consult findRoot for ids
        // that are in `parent` (N-body bodies); operator[] would otherwise insert.
        const uint64_t primaryId = record.orbit.primaryBodyId;
        const uint64_t effectivePrimary =
            (parent.find(primaryId) != parent.end()) ? findRoot(primaryId)
                                                      : primaryId;
        if (!resolveRail(effectivePrimary))
            return false;
        const WorldState& primary = worldStates.at(effectivePrimary);
        // If the primary merged away, repoint this rail body's OrbitState to the
        // survivor for future steps. NOTE (documented residual for a follow-up):
        // primaryMu and the osculating elements are left as-is, so PromoteFromRails
        // keeps propagating around the survivor with the pre-merge mu. For the
        // dominant case — a heavy primary absorbing a light body — the survivor's
        // world position and mass are ~unchanged, so the error is small and bounded;
        // the exact fix is to re-fit the elements (DemoteToRails) about the survivor.
        if (effectivePrimary != primaryId)
            primaryRepoints.push_back({ record.entityIndex, effectivePrimary });
        const StateVector relative = PromoteFromRails(
            record.orbit, targetTime - record.orbit.epoch);
        if (!IsFinite(relative))
            return false;

        const WorldPos worldPosition = Translate(primary.position, relative.position);
        const Vec3d worldVelocity = primary.velocity + relative.velocity;
        const Vec3d localPosition = Separation(
            frames.GetFrame(record.frame).origin, worldPosition);
        const Vec3d localVelocity = worldVelocity -
                                    frames.GetFrame(record.frame).velocity;
        if (!ValidSector(worldPosition) || !IsCanonical(worldPosition) ||
            !IsSafeLocal(localPosition) || !IsFinite(localVelocity))
            return false;

        staged.push_back(StagedBody{
            record.entityIndex, localPosition, localVelocity,
            record.body.invMass, false, 0.0, 0.0, false,
        });
        worldStates.emplace(bodyId, WorldState{ worldPosition, worldVelocity });
        mark = 2;
        return true;
    };

    for (const EntityRecord& record : records)
        if (record.gravity.owner == ecs::OrbitOwner::OnRails &&
            !resolveRail(record.gravity.bodyId))
            return PassiveOrbitStepResult{};

    std::vector<ecs::Entity> absorbedEntities;
    for (const auto& binding : nbodyRecordById)
    {
        if (survivorIds.find(binding.first) != survivorIds.end())
            continue;
        const ecs::Entity entity =
            registry.EntityAtIndex(records[binding.second].entityIndex);
        if (entity.IsNull())
            return PassiveOrbitStepResult{};
        absorbedEntities.push_back(entity);
    }

    for (const StagedBody& next : staged)
    {
        ecs::Transform& transform =
            registry.GetByIndex<ecs::Transform>(next.entityIndex);
        ecs::RigidBody& body =
            registry.GetByIndex<ecs::RigidBody>(next.entityIndex);
        body.prevPosition = transform.position;
        body.prevRotation = transform.rotation;
        transform.position = next.position;
        body.linearVelocity = next.velocity;
        body.invMass = next.invMass;
        body.forceAccum = Vec3d{};
        if (next.updateGravity)
        {
            ecs::GravitationalBody& gravity =
                registry.GetByIndex<ecs::GravitationalBody>(next.entityIndex);
            gravity.mu = next.mu;
            gravity.radius = next.radius;
            gravity.isSource = next.isSource;
        }
    }
    // Repoint rail bodies whose primary merged away, so subsequent steps resolve
    // against the survivor instead of the destroyed absorbed primary. Applied here
    // (accepted path only) alongside the other staged writes; the rail entity itself
    // survives, so its OrbitState component is live.
    for (const auto& repoint : primaryRepoints)
        registry.GetByIndex<ecs::OrbitState>(repoint.first).primaryBodyId =
            repoint.second;

    for (ecs::Entity entity : absorbedEntities)
        registry.Destroy(entity);

    result.accepted = true;
    result.destroyedEntityCount =
        static_cast<uint32_t>(absorbedEntities.size());
    result.collisions = std::move(collisionReport);
    return result;
}

} // namespace sim
