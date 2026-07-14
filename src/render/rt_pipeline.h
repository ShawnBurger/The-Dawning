#pragma once
// =============================================================================
// render/rt_pipeline.h — DXR Ray Tracing Pipeline
// =============================================================================
// Manages:
//   - Global root signature (TLAS, output UAV, per-frame CB, bindless buffers)
//   - DXR state object (DXIL libraries, hit groups, shader config, pipeline config)
//   - Shader Binding Table (SBT) layout with proper alignment
//
// Architecture: megakernel path tracer (single DispatchRays, iterative bounces)
// Future: SER integration, ReSTIR, DLSS Ray Reconstruction
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace render
{

// =============================================================================
// Per-frame constants for the path tracer (uploaded each frame)
// =============================================================================
struct RTPerFrameConstants
{
    float invViewProj[16];       // Inverse view-projection matrix
    float cameraPos[4];          // Camera world position (w unused)
    float lightDir[4];           // Directional light direction (w unused)
    float lightColor[4];         // Light color + intensity (w unused)
    float ambientColor[4];       // Ambient color (w unused)
    uint32_t frameIndex;         // For random seed
    uint32_t maxBounces;         // Max path tracing bounces
    uint32_t renderWidth;        // Output texture width
    uint32_t renderHeight;       // Output texture height
};

// =============================================================================
// Per-instance material data (in a StructuredBuffer, indexed by InstanceID)
// =============================================================================
struct RTMaterialData
{
    float albedo[4];
    float roughness;
    float metallic;
    float pad[2];
};

// =============================================================================
// RTPipeline
// =============================================================================
class RTPipeline
{
public:
    bool Init(ID3D12Device5* device);
    void Shutdown();

    // Build the shader table for a given number of scene instances
    bool BuildShaderTable(ID3D12Device5* device, uint32_t instanceCount);

    // Access
    ID3D12StateObject*     GetStateObject() const  { return m_stateObject.Get(); }
    ID3D12RootSignature*   GetGlobalRootSig() const { return m_globalRootSig.Get(); }

    // Shader table addresses for DispatchRays
    D3D12_GPU_VIRTUAL_ADDRESS RayGenAddress() const;
    uint64_t                  RayGenSize() const;
    D3D12_GPU_VIRTUAL_ADDRESS MissAddress() const;
    uint64_t                  MissSize() const;
    uint64_t                  MissStride() const;
    D3D12_GPU_VIRTUAL_ADDRESS HitGroupAddress() const;
    uint64_t                  HitGroupSize() const;
    uint64_t                  HitGroupStride() const;

private:
    bool CreateGlobalRootSignature(ID3D12Device5* device);
    bool CreateStateObject(ID3D12Device5* device);

    ComPtr<ID3D12RootSignature> m_globalRootSig;
    ComPtr<ID3D12StateObject>   m_stateObject;

    // Shader identifiers (32 bytes each, retrieved after state object creation)
    uint8_t m_rayGenID[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = {};
    uint8_t m_missID[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = {};
    uint8_t m_shadowMissID[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = {};
    uint8_t m_hitGroupID[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = {};
    uint8_t m_shadowHitGroupID[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = {};

    // Shader table buffer
    ComPtr<ID3D12Resource> m_shaderTable;
    uint64_t m_shaderTableSize = 0;

    // SBT layout offsets
    uint64_t m_rayGenOffset = 0;
    uint64_t m_rayGenEntrySize = 0;
    uint64_t m_missOffset = 0;
    uint64_t m_missEntrySize = 0;
    uint32_t m_missCount = 0;
    uint64_t m_hitGroupOffset = 0;
    uint64_t m_hitGroupEntrySize = 0;
    uint32_t m_hitGroupCount = 0;
};

} // namespace render
