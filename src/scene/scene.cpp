// =============================================================================
// scene/scene.cpp — Scene Implementation
// =============================================================================

#include "scene.h"
#include "star_system_seed.h"
#include "../ecs/systems.h"
#include "../sim/star_system.h"
#include "../sim/system_instantiate.h"
#include "../core/log.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace scene
{

namespace
{

constexpr uint64_t kSceneHashOffset = 14695981039346656037ull;
constexpr uint64_t kSceneHashPrime  = 1099511628211ull;

void HashSceneBytes(uint64_t& hash, const void* data, size_t byteCount)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < byteCount; ++i)
    {
        hash ^= bytes[i];
        hash *= kSceneHashPrime;
    }
}

template <typename T>
void HashSceneValue(uint64_t& hash, const T& value)
{
    HashSceneBytes(hash, &value, sizeof(value));
}

} // namespace

// =============================================================================
// Init / Shutdown
// =============================================================================
void Scene::Init()
{
    m_resources.Init();
    m_frames.RebuildFromFrames({});
    m_activeFrame = m_frames.CreateFrame(sim::kInvalidFrame, sim::WorldPos{});
    m_masterFrame = m_activeFrame;
    m_coordinateTime = 0.0;
    m_simTick = 0;
    m_ftlCommands.clear();
    m_atmosphereBindings.clear();
    m_clockGravityBindings.clear();
    core::Log::Info("Scene initialized");
}

void Scene::Shutdown(render::D3D12Device& device, render::Renderer& renderer)
{
    if (m_rtReady)
    {
        m_pathTracer.Shutdown();
        m_rtReady = false;
    }
    m_meshToBLAS.Clear();
    m_resources.Shutdown(device, renderer);
    core::Log::Info("Scene shut down");
}

// =============================================================================
// Entity Creation Helpers
// =============================================================================
ecs::Entity Scene::CreateRenderable(const char* name,
                                     MeshHandle mesh,
                                     const ecs::Material& material,
                                     const ecs::Transform& transform)
{
    ecs::Entity e = m_registry.Create();
    if (e.IsNull()) return e;

    m_registry.Assign<ecs::Transform>(e, transform);
    m_registry.Assign<ecs::MeshInstance>(e, ecs::MeshInstance{ mesh.value, true });
    m_registry.Assign<ecs::Material>(e, material);

    if (name)
    {
        ecs::Name n;
        n.Set(name);
        m_registry.Assign<ecs::Name>(e, n);
    }

    return e;
}

ecs::Entity Scene::CreateSpinner(const char* name,
                                  MeshHandle mesh,
                                  const ecs::Material& material,
                                  const ecs::Transform& transform,
                                  float radiansPerSec,
                                  const core::Vec3f& axis)
{
    ecs::Entity e = CreateRenderable(name, mesh, material, transform);
    if (e.IsNull()) return e;

    ecs::RotationSpeed rs;
    rs.radiansPerSecond = radiansPerSec;
    rs.axis = axis;
    m_registry.Assign<ecs::RotationSpeed>(e, rs);

    return e;
}

uint32_t Scene::SeedStarSystem(MeshHandle bodySphere, float meshRadius)
{
    if (m_starSystemActive)
        return 0; // seed once

    // Shift bodyIds into the reserved namespace so the star (id 1 in the builder)
    // does not collide with the ship (id 1) and abort the gravity step.
    const sim::StarSystem system = OffsetStarSystemBodyIds(
        sim::BuildReferenceSystem(), kStarSystemBodyIdBase);

    const uint32_t created =
        sim::InstantiateStarSystem(m_registry, m_masterFrame, system);

    const uint32_t visuals = AttachStarSystemVisuals(
        m_registry, kStarSystemBodyIdBase, bodySphere.value, meshRadius);

    m_starSystemActive = true;
    core::Log::Infof("[SIM] star_system_seeded bodies=%u visuals=%u soi=on",
                     created, visuals);
    return created;
}

void Scene::DestroyEntity(ecs::Entity entity)
{
    ClearAtmosphereBinding(entity);
    ClearClockGravityBinding(entity);
    std::erase_if(m_ftlCommands,
                  [&](const sim::FtlCommand& command)
                  { return command.entity == entity; });
    m_registry.Destroy(entity);
}

// =============================================================================
// Phase 1: Update Systems
// =============================================================================
sim::SimulationStepResult Scene::UpdateSystems(double dt)
{
    sim::SimulationStepConfig config;
    config.activeFrame = m_activeFrame;
    config.masterFrame = m_masterFrame;
    config.coordinateTime = m_coordinateTime;
    config.flightAssist = m_flightAssist;
    config.closeEncounters = m_closeEncounters;
    config.enableSoiTransitions = m_starSystemActive;
    config.soiHysteresis = m_soiHysteresis;
    config.ftlCommands = m_ftlCommands;
    config.atmosphereBindings = m_atmosphereBindings;
    config.clockGravityBindings = m_clockGravityBindings;

    sim::SimulationStepResult result =
        sim::StepSimulation(m_registry, m_frames, dt, config);
    m_ftlCommands.clear();
    if (!result.accepted)
        return result;

    // Legacy Velocity/RotationSpeed components remain useful for simple render
    // props. Their systems explicitly skip RigidBody-owned transforms.
    SystemVelocity(dt);
    SystemRotation(dt);
    m_coordinateTime += dt;
    ++m_simTick;

    // Collisions can destroy entities below Scene::DestroyEntity. Keep the
    // host-owned persistent bindings synchronized with the resulting registry.
    std::erase_if(m_atmosphereBindings,
                  [&](const sim::AtmosphereBinding& binding)
                  { return !m_registry.IsAlive(binding.entity); });
    std::erase_if(m_clockGravityBindings,
                  [&](const sim::ClockGravityBinding& binding)
                  { return !m_registry.IsAlive(binding.entity); });
    for (sim::ClockGravityBinding& binding : m_clockGravityBindings)
    {
        const auto remap = std::find_if(
            result.bodyIdRemaps.begin(), result.bodyIdRemaps.end(),
            [&](const auto& candidate)
            { return candidate.first == binding.primaryBodyId; });
        if (remap != result.bodyIdRemaps.end())
            binding.primaryBodyId = remap->second;
    }
    return result;
}

void Scene::QueueFtlCommand(const sim::FtlCommand& command)
{
    m_ftlCommands.push_back(command);
}

void Scene::SetAtmosphereBinding(const sim::AtmosphereBinding& binding)
{
    const auto it = std::find_if(
        m_atmosphereBindings.begin(), m_atmosphereBindings.end(),
        [&](const sim::AtmosphereBinding& current)
        { return current.entity == binding.entity; });
    if (it == m_atmosphereBindings.end())
        m_atmosphereBindings.push_back(binding);
    else
        *it = binding;
}

void Scene::ClearAtmosphereBinding(ecs::Entity entity)
{
    std::erase_if(m_atmosphereBindings,
                  [&](const sim::AtmosphereBinding& binding)
                  { return binding.entity == entity; });
}

void Scene::SetClockGravityBinding(const sim::ClockGravityBinding& binding)
{
    const auto it = std::find_if(
        m_clockGravityBindings.begin(), m_clockGravityBindings.end(),
        [&](const sim::ClockGravityBinding& current)
        { return current.entity == binding.entity; });
    if (it == m_clockGravityBindings.end())
        m_clockGravityBindings.push_back(binding);
    else
        *it = binding;
}

void Scene::ClearClockGravityBinding(ecs::Entity entity)
{
    std::erase_if(m_clockGravityBindings,
                  [&](const sim::ClockGravityBinding& binding)
                  { return binding.entity == entity; });
}

sim::SnapshotBuildResult Scene::BuildSimulationSnapshot(double fixedDt) const
{
    return sim::BuildSnapshot(m_registry, m_frames, m_coordinateTime, fixedDt,
                              m_simTick, m_masterFrame);
}

sim::SnapshotApplyResult Scene::ApplySimulationSnapshot(
    const sim::SimSnapshot& snapshot)
{
    sim::SnapshotApplyResult result =
        sim::ApplySnapshot(m_registry, m_frames, snapshot);
    if (!result.accepted)
        return result;

    m_coordinateTime = snapshot.coordinateTimeEpoch;
    m_simTick = snapshot.simTick;
    m_masterFrame = snapshot.masterFrame;
    m_activeFrame = snapshot.masterFrame;
    m_ftlCommands.clear();
    InvalidatePathTraceHistory();
    return result;
}

void Scene::InvalidatePathTraceHistory()
{
    m_pathTracer.InvalidateAccumulation();
}

// =============================================================================
// System: Velocity - integrates entities that have Transform + Velocity
// =============================================================================
void Scene::SystemVelocity(double dt)
{
    ecs::systems::IntegrateVelocities(m_registry, dt);
}

// =============================================================================
// System: Rotation — spins entities that have Transform + RotationSpeed
// =============================================================================
void Scene::SystemRotation(double dt)
{
    ecs::systems::IntegrateRotations(m_registry, dt);
}

// =============================================================================
// Phase 2: Render Entities
// =============================================================================
// Iterates all entities with Transform + MeshInstance + Material and issues
// draw calls through the renderer. Checks mesh handle validity and visibility.
// =============================================================================
uint32_t Scene::MeshInstanceCount() const
{
    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    return meshPool ? meshPool->Count() : 0u;
}

void Scene::RenderShadowCasters(render::D3D12Device& device,
                               render::Renderer& renderer,
                               const core::Vec3d& cameraPosition)
{
    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        uint32_t entityIdx = meshPool->EntityAt(i);
        const auto& meshInst = meshPool->DataAt(i);

        if (!meshInst.visible) continue;
        if (!m_registry.HasByIndex<ecs::Transform>(entityIdx)) continue;
        if (!m_registry.HasByIndex<ecs::Material>(entityIdx)) continue;

        const auto& transform = m_registry.GetByIndex<ecs::Transform>(entityIdx);

        MeshHandle handle(meshInst.meshHandle);
        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || !gpuMesh->IsValid()) continue;

        renderer.DrawMeshShadow(device, *gpuMesh,
                                transform.ToCameraRelativeMatrix(cameraPosition, m_renderScale));
    }
}

void Scene::RenderEntities(render::D3D12Device& device,
                           render::Renderer& renderer,
                           const core::Vec3d& cameraPosition)
{
    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        uint32_t entityIdx = meshPool->EntityAt(i);
        const auto& meshInst = meshPool->DataAt(i);

        // Skip invisible entities
        if (!meshInst.visible) continue;

        // Must also have Transform and Material
        if (!m_registry.HasByIndex<ecs::Transform>(entityIdx)) continue;
        if (!m_registry.HasByIndex<ecs::Material>(entityIdx)) continue;

        const auto& transform = m_registry.GetByIndex<ecs::Transform>(entityIdx);
        const auto& material  = m_registry.GetByIndex<ecs::Material>(entityIdx);

        // Look up the actual GPU mesh via handle
        MeshHandle handle(meshInst.meshHandle);
        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || !gpuMesh->IsValid()) continue;

        const render::Texture* albedoTexture = nullptr;
        if (material.albedoTextureHandle != UINT32_MAX)
            albedoTexture = m_resources.GetTexture(TextureHandle(material.albedoTextureHandle));
        const render::Texture* normalTexture = nullptr;
        if (material.normalTextureHandle != UINT32_MAX)
            normalTexture = m_resources.GetTexture(TextureHandle(material.normalTextureHandle));
        const render::Texture* ormTexture = nullptr;
        if (material.ormTextureHandle != UINT32_MAX)
            ormTexture = m_resources.GetTexture(TextureHandle(material.ormTextureHandle));

        const render::Texture* emissiveTexture = nullptr;
        if (material.emissiveTextureHandle != UINT32_MAX)
            emissiveTexture =
                m_resources.GetTexture(TextureHandle(material.emissiveTextureHandle));

        const core::Mat4x4 worldMatrix =
            transform.ToCameraRelativeMatrix(cameraPosition, m_renderScale);

        // Issue draw call
        renderer.DrawMesh(device, *gpuMesh, worldMatrix,
                          material.albedo, material.roughness, material.metallic,
                          albedoTexture, normalTexture, ormTexture, emissiveTexture,
                          material.emissive, material.emissiveStrength);
    }
}

// =============================================================================
// Path Tracer Init
// =============================================================================
bool Scene::InitPathTracer(render::D3D12Device& device)
{
    if (!m_pathTracer.Init(device))
    {
        core::Log::Error("Failed to initialize path tracer");
        return false;
    }
    m_rtReady = true;
    return true;
}

bool Scene::ResizePathTracer(render::D3D12Device& device, uint32_t width, uint32_t height)
{
    if (!m_rtReady) return true;   // Nothing to resize; not a failure.
    return m_pathTracer.Resize(device.Device5(), width, height);
}

// =============================================================================
// Ensure BLAS exists for each unique mesh
// =============================================================================
void Scene::EnsureBLAS(render::D3D12Device& device)
{
    auto* dev5 = device.Device5();
    auto* cmd4 = device.CmdList4();
    if (!dev5 || !cmd4) return;

    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        const auto& meshInst = meshPool->DataAt(i);
        MeshHandle handle(meshInst.meshHandle);
        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || !gpuMesh->IsValid()) continue;

        // A recycled ResourceManager slot has a different full handle and must
        // build a new BLAS rather than inheriting the old mesh's geometry.
        if (!m_meshToBLAS.Contains(handle))
        {
            uint32_t blasIdx = m_pathTracer.GetAcceleration().BuildBLAS(dev5, cmd4, *gpuMesh);
            if (blasIdx != UINT32_MAX)
                m_meshToBLAS.Set(handle, blasIdx);
        }
    }
}

// =============================================================================
// Build Acceleration Structures (called once per frame before path tracing)
// =============================================================================
void Scene::BuildAccelerationStructures(render::D3D12Device& device,
                                         const core::Vec3d& cameraPosition)
{
    if (!m_rtReady) return;

    // Runtime mesh additions must receive a BLAS before TLAS extraction. The
    // startup path also calls EnsureBLAS explicitly, so this is normally a
    // cheap cache walk and only records work for newly observed handles.
    EnsureBLAS(device);

    auto* dev5 = device.Device5();
    auto* cmd4 = device.CmdList4();
    if (!dev5 || !cmd4) return;

    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    // Collect TLAS instances from all renderable entities
    std::vector<render::TLASInstance> instances;
    instances.reserve(meshPool->Count());

    uint32_t instanceID = 0;
    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        uint32_t entityIdx = meshPool->EntityAt(i);
        const auto& meshInst = meshPool->DataAt(i);

        if (!meshInst.visible) continue;
        if (!m_registry.HasByIndex<ecs::Transform>(entityIdx)) continue;
        if (!m_registry.HasByIndex<ecs::Material>(entityIdx)) continue;

        MeshHandle handle(meshInst.meshHandle);
        uint32_t blasIdx = UINT32_MAX;
        if (!m_meshToBLAS.TryGet(handle, blasIdx))
            continue;

        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || gpuMesh->rtTriangleNormals.empty() ||
            gpuMesh->rtTriangleUVs.size() != gpuMesh->rtTriangleNormals.size() ||
            gpuMesh->rtTrianglePositions.size() != gpuMesh->rtTriangleNormals.size())
            continue;

        const auto& blas = m_pathTracer.GetAcceleration().GetBLAS(blasIdx);

        const auto& transform = m_registry.GetByIndex<ecs::Transform>(entityIdx);
        const core::Mat4x4 worldMat =
            transform.ToCameraRelativeMatrix(cameraPosition, m_renderScale);

        render::TLASInstance inst = {};
        // Convert the engine's row-vector matrix to DXR's 3x4 instance
        // transform. Engine translation lives in row 3; DXR expects it in
        // the final column of the 3x4 descriptor.
        const float* m = worldMat.Data();
        inst.transform[0][0] = m[0]; inst.transform[0][1] = m[4]; inst.transform[0][2] = m[8];  inst.transform[0][3] = m[12];
        inst.transform[1][0] = m[1]; inst.transform[1][1] = m[5]; inst.transform[1][2] = m[9];  inst.transform[1][3] = m[13];
        inst.transform[2][0] = m[2]; inst.transform[2][1] = m[6]; inst.transform[2][2] = m[10]; inst.transform[2][3] = m[14];

        inst.instanceID    = instanceID;
        inst.instanceMask  = 0xFF;
        inst.hitGroupOffset = instanceID * 2; // 2 ray types per instance
        inst.instanceFlags = 0;
        inst.blasAddress   = blas.gpuAddress;

        instances.push_back(inst);
        instanceID++;
    }

    if (instances.empty()) return;

    // Build TLAS
    m_pathTracer.GetAcceleration().BuildTLAS(device,
        instances.data(), static_cast<uint32_t>(instances.size()));

    // Build shader table if instance count changed
    m_pathTracer.GetPipeline().BuildShaderTable(dev5,
        static_cast<uint32_t>(instances.size()));
}

// =============================================================================
// Dispatch Path Tracing
// =============================================================================
void Scene::PathTraceEntities(
    render::D3D12Device& device,
    const render::Camera& camera,
    const core::Vec3f& lightDir,
    const core::Vec3f& lightColor,
    const core::Vec3f& ambientColor,
    render::RTQualityMode qualityMode,
    const render::RTEnvironmentInputs& environment)
{
    if (!m_rtReady) return;

    const core::Vec3d& cameraPosition = camera.Position();
    uint64_t sceneSignature = kSceneHashOffset;
    HashSceneValue(sceneSignature, lightDir.x);
    HashSceneValue(sceneSignature, lightDir.y);
    HashSceneValue(sceneSignature, lightDir.z);
    HashSceneValue(sceneSignature, lightColor.x);
    HashSceneValue(sceneSignature, lightColor.y);
    HashSceneValue(sceneSignature, lightColor.z);
    HashSceneValue(sceneSignature, ambientColor.x);
    HashSceneValue(sceneSignature, ambientColor.y);
    HashSceneValue(sceneSignature, ambientColor.z);

    // Collect materials in instance order (matching TLAS instance IDs)
    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    std::vector<render::RTMaterialData> materials;
    std::vector<render::RTInstanceData> instanceData;
    std::vector<render::RTTriangleNormalData> triangleNormals;
    std::vector<render::RTTriangleUVData> triangleUVs;
    std::vector<render::RTTrianglePositionData> trianglePositions;
    std::vector<const render::Texture*> albedoTextures;
    std::vector<const render::Texture*> normalTextures;
    std::vector<const render::Texture*> ormTextures;
    std::vector<const render::Texture*> emissiveTextures;
    materials.reserve(meshPool->Count());
    instanceData.reserve(meshPool->Count());

    auto resolveTextureIndex = [&](uint32_t handleValue,
                                   std::vector<const render::Texture*>& textures,
                                   uint32_t maxTextures,
                                   const char* label) -> uint32_t
    {
        if (handleValue == UINT32_MAX)
            return UINT32_MAX;

        const render::Texture* texture = m_resources.GetTexture(TextureHandle(handleValue));
        if (!texture || !texture->IsValid())
            return UINT32_MAX;

        for (uint32_t i = 0; i < textures.size(); ++i)
        {
            if (textures[i] == texture)
                return i;
        }

        if (textures.size() >= maxTextures)
        {
            core::Log::Warnf("Path tracer %s texture table is full; material texture skipped", label);
            return UINT32_MAX;
        }

        textures.push_back(texture);
        return static_cast<uint32_t>(textures.size() - 1);
    };

    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        uint32_t entityIdx = meshPool->EntityAt(i);
        const auto& meshInst = meshPool->DataAt(i);

        if (!meshInst.visible) continue;
        if (!m_registry.HasByIndex<ecs::Transform>(entityIdx)) continue;
        if (!m_registry.HasByIndex<ecs::Material>(entityIdx)) continue;

        MeshHandle handle(meshInst.meshHandle);
        if (!m_meshToBLAS.Contains(handle))
            continue;

        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || gpuMesh->rtTriangleNormals.empty() ||
            gpuMesh->rtTriangleUVs.size() != gpuMesh->rtTriangleNormals.size() ||
            gpuMesh->rtTrianglePositions.size() != gpuMesh->rtTriangleNormals.size())
            continue;

        const auto& mat = m_registry.GetByIndex<ecs::Material>(entityIdx);
        const auto& instTransform = m_registry.GetByIndex<ecs::Transform>(entityIdx);

        HashSceneValue(sceneSignature, entityIdx);
        HashSceneValue(sceneSignature, meshInst.meshHandle);
        HashSceneValue(sceneSignature, instTransform.position.x);
        HashSceneValue(sceneSignature, instTransform.position.y);
        HashSceneValue(sceneSignature, instTransform.position.z);
        HashSceneValue(sceneSignature, instTransform.rotation.x);
        HashSceneValue(sceneSignature, instTransform.rotation.y);
        HashSceneValue(sceneSignature, instTransform.rotation.z);
        HashSceneValue(sceneSignature, instTransform.rotation.w);
        HashSceneValue(sceneSignature, instTransform.scale.x);
        HashSceneValue(sceneSignature, instTransform.scale.y);
        HashSceneValue(sceneSignature, instTransform.scale.z);
        HashSceneValue(sceneSignature, mat.albedo.r);
        HashSceneValue(sceneSignature, mat.albedo.g);
        HashSceneValue(sceneSignature, mat.albedo.b);
        HashSceneValue(sceneSignature, mat.albedo.a);
        HashSceneValue(sceneSignature, mat.roughness);
        HashSceneValue(sceneSignature, mat.metallic);
        HashSceneValue(sceneSignature, mat.albedoTextureHandle);
        HashSceneValue(sceneSignature, mat.normalTextureHandle);
        HashSceneValue(sceneSignature, mat.ormTextureHandle);
        HashSceneValue(sceneSignature, mat.emissive.r);
        HashSceneValue(sceneSignature, mat.emissive.g);
        HashSceneValue(sceneSignature, mat.emissive.b);
        HashSceneValue(sceneSignature, mat.emissive.a);
        HashSceneValue(sceneSignature, mat.emissiveStrength);
        HashSceneValue(sceneSignature, mat.emissiveTextureHandle);
        const uint32_t albedoTextureIndex =
            resolveTextureIndex(mat.albedoTextureHandle, albedoTextures,
                                render::kMaxRTAlbedoTextures, "albedo");
        const uint32_t ormTextureIndex =
            resolveTextureIndex(mat.ormTextureHandle, ormTextures,
                                render::kMaxRTOrmTextures, "orm");
        const uint32_t normalTextureIndex =
            resolveTextureIndex(mat.normalTextureHandle, normalTextures,
                                render::kMaxRTNormalTextures, "normal");
        const uint32_t emissiveTextureIndex =
            resolveTextureIndex(mat.emissiveTextureHandle, emissiveTextures,
                                render::kMaxRTEmissiveTextures, "emissive");

        render::RTMaterialData rtMat = {};
        rtMat.albedo[0]  = mat.albedo.r;
        rtMat.albedo[1]  = mat.albedo.g;
        rtMat.albedo[2]  = mat.albedo.b;
        rtMat.albedo[3]  = mat.albedo.a;
        rtMat.roughness   = mat.roughness;
        rtMat.metallic    = mat.metallic;
        rtMat.albedoTextureIndex = albedoTextureIndex == UINT32_MAX ? 0u : albedoTextureIndex;
        rtMat.useAlbedoTexture = albedoTextureIndex == UINT32_MAX ? 0u : 1u;
        rtMat.normalTextureIndex = normalTextureIndex == UINT32_MAX ? 0u : normalTextureIndex;
        rtMat.useNormalTexture = normalTextureIndex == UINT32_MAX ? 0u : 1u;
        rtMat.ormTextureIndex = ormTextureIndex == UINT32_MAX ? 0u : ormTextureIndex;
        rtMat.useOrmTexture = ormTextureIndex == UINT32_MAX ? 0u : 1u;
        rtMat.emissive[0] = mat.emissive.r;
        rtMat.emissive[1] = mat.emissive.g;
        rtMat.emissive[2] = mat.emissive.b;
        rtMat.emissiveStrength = mat.emissiveStrength;
        rtMat.emissiveTextureIndex =
            emissiveTextureIndex == UINT32_MAX ? 0u : emissiveTextureIndex;
        rtMat.useEmissiveTexture = emissiveTextureIndex == UINT32_MAX ? 0u : 1u;
        materials.push_back(rtMat);

        render::RTInstanceData rtInstance = {};
        rtInstance.triangleNormalOffset = static_cast<uint32_t>(triangleNormals.size());
        rtInstance.triangleUVOffset = static_cast<uint32_t>(triangleUVs.size());
        rtInstance.trianglePositionOffset = static_cast<uint32_t>(trianglePositions.size());

        // Normal matrix for this instance, stored transposed so the closest-hit
        // shader can evaluate component i as dot(row[i].xyz, objectNormal). Uses the
        // same InverseTranspose3x3 the raster path uses, so both paths now shade
        // identically under non-uniform scale.
        {
            const core::Mat4x4 instWorld =
                instTransform.ToCameraRelativeMatrix(cameraPosition, m_renderScale);
            const core::Mat4x4 normalMat = core::Mat4x4::InverseTranspose3x3(instWorld);
            for (int row = 0; row < 3; ++row)
            {
                rtInstance.normalMatrix[row * 4 + 0] = normalMat.m[0][row];
                rtInstance.normalMatrix[row * 4 + 1] = normalMat.m[1][row];
                rtInstance.normalMatrix[row * 4 + 2] = normalMat.m[2][row];
                rtInstance.normalMatrix[row * 4 + 3] = 0.0f;
            }
        }

        instanceData.push_back(rtInstance);

        triangleNormals.insert(triangleNormals.end(),
                               gpuMesh->rtTriangleNormals.begin(),
                               gpuMesh->rtTriangleNormals.end());
        triangleUVs.insert(triangleUVs.end(),
                           gpuMesh->rtTriangleUVs.begin(),
                           gpuMesh->rtTriangleUVs.end());
        trianglePositions.insert(trianglePositions.end(),
                                 gpuMesh->rtTrianglePositions.begin(),
                                 gpuMesh->rtTrianglePositions.end());
    }

    m_pathTracer.Dispatch(device, camera, lightDir, lightColor, ambientColor,
                          materials.data(), static_cast<uint32_t>(materials.size()),
                          instanceData.data(), static_cast<uint32_t>(instanceData.size()),
                          triangleNormals.data(), static_cast<uint32_t>(triangleNormals.size()),
                          triangleUVs.data(), static_cast<uint32_t>(triangleUVs.size()),
                          trianglePositions.data(), static_cast<uint32_t>(trianglePositions.size()),
                          albedoTextures.data(), static_cast<uint32_t>(albedoTextures.size()),
                          normalTextures.data(), static_cast<uint32_t>(normalTextures.size()),
                          ormTextures.data(), static_cast<uint32_t>(ormTextures.size()),
                          emissiveTextures.data(),
                          static_cast<uint32_t>(emissiveTextures.size()),
                          static_cast<uint32_t>(materials.size()),
                          sceneSignature,
                          qualityMode,
                          environment);
}

void Scene::CopyPathTraceToBackBuffer(render::D3D12Device& device)
{
    if (m_rtReady)
        m_pathTracer.CopyToBackBuffer(device);
}

} // namespace scene
