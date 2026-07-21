#include "assembly_dynamic_collision_runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace scene
{
namespace
{

constexpr uint64_t kDynamicIdentityBit = uint64_t{ 1 } << 63;
constexpr double kEpsilon = 1.0e-10;
constexpr double kMaximumMagnitude = 1.0e12;
constexpr double kDegreesToRadians =
    3.141592653589793238462643383279502884 / 180.0;

AssemblyDynamicCollisionResult Failure(
    AssemblyDynamicCollisionStatus status,
    std::string error)
{
    AssemblyDynamicCollisionResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

bool Finite(double value)
{
    return std::isfinite(value) && std::abs(value) <= kMaximumMagnitude;
}

bool Finite(const core::Vec3d& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Finite(const core::Vec3f& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool NonZero(const asset::Sha256Digest& digest)
{
    for (uint8_t value : digest.bytes)
        if (value != 0)
            return true;
    return false;
}

core::Vec3f ToFloat3(const std::array<double, 3>& value)
{
    return {
        static_cast<float>(value[0]),
        static_cast<float>(value[1]),
        static_cast<float>(value[2])
    };
}

core::Vec3f ScaleLocal(
    const core::Vec3f& value,
    const core::Vec3f& scale)
{
    return { value.x * scale.x, value.y * scale.y, value.z * scale.z };
}

bool BuildLocalModuleTransform(
    const asset::AssemblyTransform& source,
    ecs::Transform& transform)
{
    for (double value : source.positionMeters)
        if (!Finite(value))
            return false;
    for (double value : source.rotationEulerDegrees)
        if (!Finite(value))
            return false;
    for (double value : source.scale)
    {
        if (!Finite(value) || value <= 0.0 ||
            value > (std::numeric_limits<float>::max)())
            return false;
    }

    transform.position = {
        source.positionMeters[0],
        source.positionMeters[1],
        source.positionMeters[2]
    };
    transform.scale = {
        static_cast<float>(source.scale[0]),
        static_cast<float>(source.scale[1]),
        static_cast<float>(source.scale[2])
    };
    const float pitch = static_cast<float>(
        std::remainder(source.rotationEulerDegrees[0], 360.0) *
        kDegreesToRadians);
    const float yaw = static_cast<float>(
        std::remainder(source.rotationEulerDegrees[1], 360.0) *
        kDegreesToRadians);
    const float roll = static_cast<float>(
        std::remainder(source.rotationEulerDegrees[2], 360.0) *
        kDegreesToRadians);
    transform.rotation = core::Quatf::FromEuler(pitch, yaw, roll).Normalized();
    return Finite(transform.position) && Finite(transform.scale) &&
           Finite(core::Vec3f{
               transform.rotation.x,
               transform.rotation.y,
               transform.rotation.z }) &&
           std::isfinite(transform.rotation.w);
}

bool ReconstructMovingTransform(
    const asset::AssemblyMovingPart& part,
    const ecs::Transform& module,
    double progress,
    ecs::Transform& transformed)
{
    const core::Vec3f sourceAxis = ToFloat3(part.axis);
    if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0 ||
        !Finite(sourceAxis) || sourceAxis.LengthSq() < 1.0e-8f ||
        !Finite(part.travel) || std::abs(part.travel) <= kEpsilon)
        return false;

    transformed = module;
    const core::Vec3f axis = sourceAxis.Normalized();
    if (part.motionType == asset::AssemblyMotionType::Linear)
    {
        const core::Vec3f localTravel = ScaleLocal(
            axis * static_cast<float>(part.travel * progress),
            module.scale);
        transformed.position += core::Vec3d::FromFloat(
            module.rotation.Rotate(localTravel));
    }
    else if (part.motionType == asset::AssemblyMotionType::Rotational)
    {
        if (!Finite(ToFloat3(part.pivotMeters)))
            return false;
        const float radians = static_cast<float>(
            part.travel * progress * kDegreesToRadians);
        const core::Quatf localDelta =
            core::Quatf::FromAxisAngle(axis, radians).Normalized();
        const core::Quatf worldDelta = (
            module.rotation * localDelta * module.rotation.Conjugate()).Normalized();
        const core::Vec3f scaledPivot = ScaleLocal(
            ToFloat3(part.pivotMeters), module.scale);
        const core::Vec3d pivot = module.position + core::Vec3d::FromFloat(
            module.rotation.Rotate(scaledPivot));
        const core::Vec3f offset = (module.position - pivot).ToFloat();
        transformed.position = pivot + core::Vec3d::FromFloat(
            worldDelta.Rotate(offset));
        transformed.rotation =
            (worldDelta * module.rotation).Normalized();
    }
    else
    {
        return false;
    }
    return Finite(transformed.position) && Finite(transformed.scale) &&
           std::isfinite(transformed.rotation.x) &&
           std::isfinite(transformed.rotation.y) &&
           std::isfinite(transformed.rotation.z) &&
           std::isfinite(transformed.rotation.w);
}

bool SocketBasis(
    const asset::AssemblySocket& socket,
    core::Vec3f& right,
    core::Vec3f& up,
    core::Vec3f& forward)
{
    forward = ToFloat3(socket.forward);
    up = ToFloat3(socket.up);
    if (!Finite(forward) || !Finite(up) ||
        forward.LengthSq() < 1.0e-8f || up.LengthSq() < 1.0e-8f)
        return false;
    forward = forward.Normalized();
    right = up.Normalized().Cross(forward);
    if (right.LengthSq() < 1.0e-8f)
        return false;
    right = right.Normalized();
    up = forward.Cross(right).Normalized();
    return up.LengthSq() >= 1.0e-8f;
}

bool BuildPanelBox(
    const asset::AssemblySocket& socket,
    const ecs::Transform& transform,
    double width,
    double height,
    double thickness,
    uint64_t stableId,
    InteriorCollisionBox& box)
{
    core::Vec3f right;
    core::Vec3f up;
    core::Vec3f forward;
    if (!SocketBasis(socket, right, up, forward) ||
        !Finite(ToFloat3(socket.positionMeters)) ||
        !Finite(width) || !Finite(height) || !Finite(thickness) ||
        width <= 0.0 || height <= 0.0 || thickness <= 0.0)
        return false;

    const core::Vec3f localCenter = ScaleLocal(
        ToFloat3(socket.positionMeters), transform.scale);
    const core::Vec3d center = transform.position + core::Vec3d::FromFloat(
        transform.rotation.Rotate(localCenter));
    const core::Vec3f axisRight = transform.rotation.Rotate(ScaleLocal(
        right * static_cast<float>(0.5 * width), transform.scale));
    const core::Vec3f axisUp = transform.rotation.Rotate(ScaleLocal(
        up * static_cast<float>(0.5 * height), transform.scale));
    const core::Vec3f axisForward = transform.rotation.Rotate(ScaleLocal(
        forward * static_cast<float>(0.5 * thickness), transform.scale));
    const core::Vec3d extent{
        std::abs(axisRight.x) + std::abs(axisUp.x) +
            std::abs(axisForward.x),
        std::abs(axisRight.y) + std::abs(axisUp.y) +
            std::abs(axisForward.y),
        std::abs(axisRight.z) + std::abs(axisUp.z) +
            std::abs(axisForward.z)
    };
    if (!Finite(center) || !Finite(extent) || extent.x <= 0.0 ||
        extent.y <= 0.0 || extent.z <= 0.0)
        return false;
    box.minimum = center - extent;
    box.maximum = center + extent;
    box.stableId = stableId;
    box.surfaceFlags = asset::CollisionSurfaceFlags::None;
    return true;
}

uint64_t DynamicStableId(uint32_t movingPartIndex, bool guard)
{
    return kDynamicIdentityBit |
           (static_cast<uint64_t>(movingPartIndex) << 1) |
           static_cast<uint64_t>(guard ? 1u : 0u);
}

bool SameBox(
    const InteriorCollisionBox& lhs,
    const InteriorCollisionBox& rhs)
{
    return lhs.stableId == rhs.stableId && lhs.minimum == rhs.minimum &&
           lhs.maximum == rhs.maximum &&
           lhs.surfaceFlags == rhs.surfaceFlags;
}

bool SameBoxes(
    std::span<const InteriorCollisionBox> lhs,
    std::span<const InteriorCollisionBox> rhs)
{
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), SameBox);
}

AssemblyDynamicCollisionStatus MapCollisionStatus(
    InteriorCollisionStatus status)
{
    switch (status)
    {
    case InteriorCollisionStatus::ResourceLimitExceeded:
        return AssemblyDynamicCollisionStatus::ResourceLimitExceeded;
    case InteriorCollisionStatus::AllocationFailure:
        return AssemblyDynamicCollisionStatus::AllocationFailure;
    case InteriorCollisionStatus::Success:
        return AssemblyDynamicCollisionStatus::Success;
    default:
        return AssemblyDynamicCollisionStatus::CollisionFailure;
    }
}

} // namespace

const char* AssemblyDynamicCollisionStatusName(
    AssemblyDynamicCollisionStatus status)
{
    switch (status)
    {
    case AssemblyDynamicCollisionStatus::Success: return "success";
    case AssemblyDynamicCollisionStatus::InvalidArgument: return "invalid_argument";
    case AssemblyDynamicCollisionStatus::NotInitialized: return "not_initialized";
    case AssemblyDynamicCollisionStatus::TopologyMismatch: return "topology_mismatch";
    case AssemblyDynamicCollisionStatus::InvalidTopology: return "invalid_topology";
    case AssemblyDynamicCollisionStatus::ResourceLimitExceeded: return "resource_limit_exceeded";
    case AssemblyDynamicCollisionStatus::CollisionFailure: return "collision_failure";
    case AssemblyDynamicCollisionStatus::AllocationFailure: return "allocation_failure";
    case AssemblyDynamicCollisionStatus::InternalError: return "internal_error";
    }
    return "unknown";
}

AssemblyDynamicCollisionResult AssemblyDynamicCollisionRuntime::Initialize(
    std::shared_ptr<const asset::CookedAssembly> assembly,
    std::shared_ptr<const InteriorCollisionWorld> staticWorld,
    const AssemblyInteriorRuntime& interior,
    const AssemblyDynamicCollisionConfig& config)
{
    if (IsInitialized())
    {
        return Failure(
            AssemblyDynamicCollisionStatus::InvalidArgument,
            "dynamic collision runtime is already initialized");
    }
    const asset::Sha256Digest* interiorTopology = interior.TopologySha256();
    if (!assembly || !staticWorld || !interiorTopology ||
        !NonZero(assembly->sourceManifestSha256) ||
        *interiorTopology != assembly->sourceManifestSha256 ||
        !Finite(config.panelThicknessMeters) ||
        config.panelThicknessMeters <= 0.0 || config.maxDynamicBoxes == 0 ||
        config.combinedWorldLimits.maxBoxes == 0)
    {
        return Failure(
            AssemblyDynamicCollisionStatus::InvalidArgument,
            "dynamic collision initialization arguments are invalid");
    }
    for (const InteriorCollisionBox& box : staticWorld->Boxes())
    {
        if ((box.stableId & kDynamicIdentityBit) != 0)
        {
            return Failure(
                AssemblyDynamicCollisionStatus::InvalidTopology,
                "static collision uses the reserved dynamic identity range");
        }
    }

    m_assembly = std::move(assembly);
    m_staticWorld = std::move(staticWorld);
    m_config = config;
    std::shared_ptr<const AssemblyInteriorCollisionSnapshot> candidate;
    const AssemblyDynamicCollisionResult built =
        BuildCandidate(interior, 1, candidate);
    if (!built.Succeeded())
    {
        Shutdown();
        return built;
    }
    m_snapshot = std::move(candidate);
    AssemblyDynamicCollisionResult result;
    result.status = AssemblyDynamicCollisionStatus::Success;
    result.changed = true;
    return result;
}

AssemblyDynamicCollisionResult AssemblyDynamicCollisionRuntime::Refresh(
    const AssemblyInteriorRuntime& interior)
{
    if (!m_assembly || !m_staticWorld || !m_snapshot)
    {
        return Failure(
            AssemblyDynamicCollisionStatus::NotInitialized,
            "dynamic collision runtime is not initialized");
    }
    if (m_snapshot->revision == (std::numeric_limits<uint64_t>::max)())
    {
        return Failure(
            AssemblyDynamicCollisionStatus::ResourceLimitExceeded,
            "dynamic collision revision space is exhausted");
    }
    std::shared_ptr<const AssemblyInteriorCollisionSnapshot> candidate;
    const AssemblyDynamicCollisionResult built = BuildCandidate(
        interior, m_snapshot->revision + 1, candidate);
    if (!built.Succeeded())
        return built;
    if (SameBoxes(candidate->dynamicBoxes, m_snapshot->dynamicBoxes))
        return { AssemblyDynamicCollisionStatus::Success, false, {} };
    m_snapshot = std::move(candidate);
    return { AssemblyDynamicCollisionStatus::Success, true, {} };
}

void AssemblyDynamicCollisionRuntime::Shutdown() noexcept
{
    m_snapshot.reset();
    m_staticWorld.reset();
    m_assembly.reset();
    m_config = {};
}

bool AssemblyDynamicCollisionRuntime::RestorePublishedSnapshot(
    std::shared_ptr<const AssemblyInteriorCollisionSnapshot> snapshot) noexcept
{
    if (!m_assembly || !snapshot || !snapshot->collisionWorld ||
        snapshot->revision == 0 ||
        snapshot->topologySha256 != m_assembly->sourceManifestSha256)
        return false;
    m_snapshot = std::move(snapshot);
    return true;
}

AssemblyDynamicCollisionResult AssemblyDynamicCollisionRuntime::BuildCandidate(
    const AssemblyInteriorRuntime& interior,
    uint64_t revision,
    std::shared_ptr<const AssemblyInteriorCollisionSnapshot>& candidate) const
{
    candidate.reset();
    if (!m_assembly || !m_staticWorld || revision == 0)
    {
        return Failure(
            AssemblyDynamicCollisionStatus::NotInitialized,
            "dynamic collision build has no initialized ownership");
    }
    const asset::Sha256Digest* topology = interior.TopologySha256();
    if (!topology || *topology != m_assembly->sourceManifestSha256)
    {
        return Failure(
            AssemblyDynamicCollisionStatus::TopologyMismatch,
            "interior state topology does not match dynamic collision");
    }
    if (interior.InteractionCount() != m_assembly->interactions.size() ||
        interior.PortalCount() != m_assembly->portals.size() ||
        interior.MovingPartCount() != m_assembly->movingParts.size() ||
        !Finite(m_assembly->minimumDoorWidthMeters) ||
        !Finite(m_assembly->minimumClearanceMeters) ||
        m_assembly->minimumDoorWidthMeters <= 0.0 ||
        m_assembly->minimumClearanceMeters <= 0.0)
    {
        return Failure(
            AssemblyDynamicCollisionStatus::InvalidTopology,
            "dynamic collision topology or aperture dimensions are invalid");
    }

    try
    {
        if (m_assembly->movingParts.size() > m_config.maxDynamicBoxes ||
            m_assembly->movingParts.size() >
                (std::numeric_limits<uint64_t>::max)() / 2 ||
            static_cast<uint64_t>(m_assembly->movingParts.size()) * 2 >
                m_config.maxDynamicBoxes)
        {
            return Failure(
                AssemblyDynamicCollisionStatus::ResourceLimitExceeded,
                "dynamic closure box limit is exceeded");
        }

        std::vector<ecs::Transform> modules(m_assembly->modules.size());
        for (size_t i = 0; i < m_assembly->modules.size(); ++i)
        {
            if (!BuildLocalModuleTransform(
                    m_assembly->modules[i].transform, modules[i]))
            {
                return Failure(
                    AssemblyDynamicCollisionStatus::InvalidTopology,
                    "assembly-local module transform is invalid");
            }
        }

        std::vector<InteriorCollisionBox> dynamic;
        dynamic.reserve(m_assembly->movingParts.size() * 2);
        for (size_t portalIndex = 0;
             portalIndex < m_assembly->portals.size();
             ++portalIndex)
        {
            const asset::AssemblyPortal& portal =
                m_assembly->portals[portalIndex];
            if (portal.closureInteraction >= m_assembly->interactions.size())
            {
                return Failure(
                    AssemblyDynamicCollisionStatus::InvalidTopology,
                    "portal closure interaction is invalid");
            }
            const asset::AssemblyInteraction& interaction =
                m_assembly->interactions[portal.closureInteraction];
            if (interaction.portalIndex != portalIndex ||
                interaction.movingPartIndex >= m_assembly->movingParts.size() ||
                interaction.socketIndex >= m_assembly->sockets.size() ||
                interaction.moduleIndex >= m_assembly->modules.size())
            {
                return Failure(
                    AssemblyDynamicCollisionStatus::InvalidTopology,
                    "portal interaction ownership is invalid");
            }
            const asset::AssemblyMovingPart& part =
                m_assembly->movingParts[interaction.movingPartIndex];
            const asset::AssemblySocket& socket =
                m_assembly->sockets[interaction.socketIndex];
            if (part.moduleIndex != interaction.moduleIndex ||
                part.interactionIndex != portal.closureInteraction ||
                socket.moduleIndex != interaction.moduleIndex)
            {
                return Failure(
                    AssemblyDynamicCollisionStatus::InvalidTopology,
                    "moving closure ownership is not reciprocal");
            }

            ecs::Transform moving;
            if (!ReconstructMovingTransform(
                    part,
                    modules[part.moduleIndex],
                    interior.InteractionMotionProgress(
                        portal.closureInteraction),
                    moving))
            {
                return Failure(
                    AssemblyDynamicCollisionStatus::InvalidTopology,
                    "moving closure transform cannot be reconstructed");
            }
            InteriorCollisionBox panel;
            if (!BuildPanelBox(
                    socket,
                    moving,
                    m_assembly->minimumDoorWidthMeters,
                    m_assembly->minimumClearanceMeters,
                    m_config.panelThicknessMeters,
                    DynamicStableId(interaction.movingPartIndex, false),
                    panel))
            {
                return Failure(
                    AssemblyDynamicCollisionStatus::InvalidTopology,
                    "moving closure panel geometry is invalid");
            }
            dynamic.push_back(panel);

            if (!interior.IsPortalTraversable(
                    static_cast<uint32_t>(portalIndex)))
            {
                InteriorCollisionBox guard;
                if (!BuildPanelBox(
                        socket,
                        modules[interaction.moduleIndex],
                        m_assembly->minimumDoorWidthMeters,
                        m_assembly->minimumClearanceMeters,
                        m_config.panelThicknessMeters,
                        DynamicStableId(interaction.movingPartIndex, true),
                        guard))
                {
                    return Failure(
                        AssemblyDynamicCollisionStatus::InvalidTopology,
                        "portal guard geometry is invalid");
                }
                dynamic.push_back(guard);
            }
        }

        std::sort(dynamic.begin(), dynamic.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.stableId < rhs.stableId;
        });
        if (m_staticWorld->BoxCount() >
            m_config.combinedWorldLimits.maxBoxes -
                (std::min)(
                    m_config.combinedWorldLimits.maxBoxes,
                    static_cast<uint64_t>(dynamic.size())))
        {
            return Failure(
                AssemblyDynamicCollisionStatus::ResourceLimitExceeded,
                "combined interior collision box limit is exceeded");
        }

        std::vector<InteriorCollisionBox> combined;
        combined.reserve(m_staticWorld->BoxCount() + dynamic.size());
        combined.insert(
            combined.end(),
            m_staticWorld->Boxes().begin(),
            m_staticWorld->Boxes().end());
        combined.insert(combined.end(), dynamic.begin(), dynamic.end());
        const InteriorCollisionWorldBuildResult built =
            BuildInteriorCollisionWorld(
                combined, m_config.combinedWorldLimits);
        if (!built.Succeeded())
        {
            return Failure(
                MapCollisionStatus(built.status),
                built.error.empty()
                    ? std::string("combined collision publication failed")
                    : built.error);
        }

        std::shared_ptr<AssemblyInteriorCollisionSnapshot> staged(
            new AssemblyInteriorCollisionSnapshot());
        staged->topologySha256 = m_assembly->sourceManifestSha256;
        staged->revision = revision;
        staged->collisionWorld = built.world;
        staged->dynamicBoxes = std::move(dynamic);
        candidate = std::move(staged);
        return { AssemblyDynamicCollisionStatus::Success, true, {} };
    }
    catch (const std::bad_alloc&)
    {
        return Failure(
            AssemblyDynamicCollisionStatus::AllocationFailure,
            "allocation failure while rebuilding dynamic collision");
    }
    catch (...)
    {
        return Failure(
            AssemblyDynamicCollisionStatus::InternalError,
            "unexpected dynamic collision rebuild failure");
    }
}

} // namespace scene
