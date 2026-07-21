#pragma once

#include "assembly_runtime_resources.h"
#include "model_loader.h"
#include "../asset/runtime_content_manifest.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace scene
{

enum class AssemblyRuntimeHostStatus : uint8_t
{
    Success,
    InvalidState,
    ManifestFailure,
    AssemblyFailure,
    CoverageFailure,
    ModelFailure,
    ResourceFailure,
    CatalogFailure,
    PreparationFailure,
    CommitFailure,
    AllocationFailure,
    InternalError
};

const char* AssemblyRuntimeHostStatusName(AssemblyRuntimeHostStatus status);

struct AssemblyRuntimeHostResult
{
    AssemblyRuntimeHostStatus status = AssemblyRuntimeHostStatus::InvalidState;
    std::string error;

    bool Succeeded() const
    {
        return status == AssemblyRuntimeHostStatus::Success;
    }
};

// Owns one data-driven assembly from manifest load through explicit ECS and GPU
// resource teardown. BeginLoad records uploads on the caller's currently open
// command list. CommitAfterUploadRetirement must be called only after that list
// has executed and the GPU copy work has retired.
class AssemblyRuntimeHost final
{
public:
    AssemblyRuntimeHost() = default;
    AssemblyRuntimeHost(const AssemblyRuntimeHost&) = delete;
    AssemblyRuntimeHost& operator=(const AssemblyRuntimeHost&) = delete;

    AssemblyRuntimeHostResult BeginLoad(
        Scene& scene,
        render::D3D12Device& device,
        render::Renderer& renderer,
        const std::filesystem::path& manifestPath);

    AssemblyRuntimeHostResult CommitAfterUploadRetirement(Scene& scene);

    bool Shutdown(
        Scene& scene,
        render::D3D12Device& device,
        render::Renderer& renderer) noexcept;

    bool IsPending() const { return m_pending; }
    bool IsLive() const { return m_instance && m_instance->IsAlive(); }
    const AssemblyInstance* Instance() const { return m_instance.get(); }
    const asset::RuntimeContentManifest& Manifest() const { return m_manifest; }

private:
    void ReleaseState(
        Scene& scene,
        render::D3D12Device& device,
        render::Renderer& renderer) noexcept;

    asset::RuntimeContentManifest m_manifest;
    std::shared_ptr<const asset::CookedAssembly> m_assembly;
    std::vector<LoadedModelResources> m_models;
    std::shared_ptr<AssemblyRuntimeResourceOwners> m_owners;
    std::unique_ptr<asset::AssemblyResourceCatalogStore> m_catalog;
    std::shared_ptr<const PreparedAssemblyPlan> m_plan;
    std::shared_ptr<AssemblyInstance> m_instance;
    bool m_pending = false;
};

} // namespace scene
