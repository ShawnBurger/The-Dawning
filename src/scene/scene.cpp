// =============================================================================
// scene/scene.cpp — Scene Implementation
// =============================================================================

#include "scene.h"
#include "../core/log.h"

namespace scene
{

// =============================================================================
// Init / Shutdown
// =============================================================================
void Scene::Init()
{
    m_resources.Init();
    core::Log::Info("Scene initialized");
}

void Scene::Shutdown()
{
    if (m_rtReady)
    {
        m_pathTracer.Shutdown();
        m_rtReady = false;
    }
    m_resources.Shutdown();
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

void Scene::DestroyEntity(ecs::Entity entity)
{
    m_registry.Destroy(entity);
}

// =============================================================================
// Phase 1: Update Systems
// =============================================================================
void Scene::UpdateSystems(float dt)
{
    SystemRotation(dt);
    // Future systems: velocity integration, physics, AI, etc.
}

// =============================================================================
// System: Rotation — spins entities that have Transform + RotationSpeed
// =============================================================================
void Scene::SystemRotation(float dt)
{
    m_registry.Each<ecs::Transform, ecs::RotationSpeed>(
        [dt](uint32_t /*entityIdx*/, ecs::Transform& transform, ecs::RotationSpeed& spin)
        {
            float angle = spin.radiansPerSecond * dt;
            core::Quatf delta = core::Quatf::FromAxisAngle(spin.axis, angle);
            transform.rotation = (transform.rotation * delta).Normalized();
        }
    );
}

// =============================================================================
// Phase 2: Render Entities
// =============================================================================
// Iterates all entities with Transform + MeshInstance + Material and issues
// draw calls through the renderer. Checks mesh handle validity and visibility.
// =============================================================================
void Scene::RenderEntities(render::D3D12Device& device, render::Renderer& renderer)
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

        // Compute world matrix from transform
        core::Mat4x4 worldMatrix = transform.ToMatrix();

        // Issue draw call
        renderer.DrawMesh(device, *gpuMesh, worldMatrix,
                          material.albedo, material.roughness, material.metallic,
                          albedoTexture, normalTexture);
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
        uint32_t meshIdx = handle.Index();

        // Grow mapping if needed
        if (meshIdx >= m_meshToBLAS.size())
            m_meshToBLAS.resize(meshIdx + 1, UINT32_MAX);

        // Build BLAS if not already built for this mesh
        if (m_meshToBLAS[meshIdx] == UINT32_MAX)
        {
            const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
            if (!gpuMesh || !gpuMesh->IsValid()) continue;

            uint32_t blasIdx = m_pathTracer.GetAcceleration().BuildBLAS(dev5, cmd4, *gpuMesh);
            if (blasIdx != UINT32_MAX)
                m_meshToBLAS[meshIdx] = blasIdx;
        }
    }
}

// =============================================================================
// Build Acceleration Structures (called once per frame before path tracing)
// =============================================================================
void Scene::BuildAccelerationStructures(render::D3D12Device& device)
{
    if (!m_rtReady) return;

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
        uint32_t meshIdx = handle.Index();

        if (meshIdx >= m_meshToBLAS.size() || m_meshToBLAS[meshIdx] == UINT32_MAX)
            continue;

        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || gpuMesh->rtTriangleNormals.empty() ||
            gpuMesh->rtTriangleUVs.size() != gpuMesh->rtTriangleNormals.size() ||
            gpuMesh->rtTrianglePositions.size() != gpuMesh->rtTriangleNormals.size())
            continue;

        uint32_t blasIdx = m_meshToBLAS[meshIdx];
        const auto& blas = m_pathTracer.GetAcceleration().GetBLAS(blasIdx);

        const auto& transform = m_registry.GetByIndex<ecs::Transform>(entityIdx);
        core::Mat4x4 worldMat = transform.ToMatrix();

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
    m_pathTracer.GetAcceleration().BuildTLAS(dev5, cmd4,
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
    render::RTQualityMode qualityMode)
{
    if (!m_rtReady) return;

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
        uint32_t meshIdx = handle.Index();

        if (meshIdx >= m_meshToBLAS.size() || m_meshToBLAS[meshIdx] == UINT32_MAX)
            continue;

        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || gpuMesh->rtTriangleNormals.empty() ||
            gpuMesh->rtTriangleUVs.size() != gpuMesh->rtTriangleNormals.size() ||
            gpuMesh->rtTrianglePositions.size() != gpuMesh->rtTriangleNormals.size())
            continue;

        const auto& mat = m_registry.GetByIndex<ecs::Material>(entityIdx);
        const uint32_t albedoTextureIndex =
            resolveTextureIndex(mat.albedoTextureHandle, albedoTextures,
                                render::kMaxRTAlbedoTextures, "albedo");
        const uint32_t normalTextureIndex =
            resolveTextureIndex(mat.normalTextureHandle, normalTextures,
                                render::kMaxRTNormalTextures, "normal");

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
        materials.push_back(rtMat);

        render::RTInstanceData rtInstance = {};
        rtInstance.triangleNormalOffset = static_cast<uint32_t>(triangleNormals.size());
        rtInstance.triangleUVOffset = static_cast<uint32_t>(triangleUVs.size());
        rtInstance.trianglePositionOffset = static_cast<uint32_t>(trianglePositions.size());
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
                          static_cast<uint32_t>(materials.size()),
                          qualityMode);
}

void Scene::CopyPathTraceToBackBuffer(render::D3D12Device& device)
{
    if (m_rtReady)
        m_pathTracer.CopyToBackBuffer(device);
}

} // namespace scene
