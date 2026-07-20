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
#include <cstddef>          // offsetof — see the RTPerFrameConstants assertions
#include "d3d12_device.h"   // kFrameCount

using Microsoft::WRL::ComPtr;

namespace render
{

constexpr uint32_t kMaxRTAlbedoTextures = 64;
constexpr uint32_t kMaxRTNormalTextures = 64;
constexpr uint32_t kMaxRTOrmTextures    = 64;
constexpr uint32_t kMaxRTEmissiveTextures = 64;

// =============================================================================
// Per-frame constants for the path tracer (uploaded each frame)
// =============================================================================
struct RTPerFrameConstants
{
    float viewProj[16];          // Reserved for future inverse-VP/reprojection work
    float cameraPos[4];          // Camera world position (w unused)
    float cameraRight[4];        // Camera right vector (w unused)
    float cameraUp[4];           // Camera up vector (w unused)
    float cameraForward[4];      // Camera forward vector (w unused)
    float lightDir[4];           // Directional light direction (w unused)
    float lightColor[4];         // Light color + intensity (w unused)
    float ambientColor[4];       // Ambient color (w unused)
    uint32_t frameIndex;         // Accumulation index; resets on camera/quality change
    uint32_t maxBounces;         // Max path tracing bounces
    uint32_t renderWidth;        // Output texture width
    uint32_t renderHeight;       // Output texture height
    uint32_t samplesPerPixel;    // Camera/path samples per pixel per frame
    uint32_t stablePreview;      // Non-zero enables deterministic preview fill
    uint32_t seedIndex;          // Wall-clock dispatch counter — RNG decorrelation only
    // tan(fovY/2), supplied by the camera rather than hardcoded in the shader.
    // path_trace.hlsl used to compute this from a literal 70 degrees with a
    // comment reading "Must match camera FOV" - a duplicated constant with
    // nothing enforcing it. They did match, which is exactly why the drift would
    // have been silent: change Camera::SetFOV and the DXR path keeps tracing the
    // old frustum while raster follows the new one, so the two paths disagree
    // about which pixels see what. Occupies the former alignment pad, so the
    // layout is unchanged.
    float tanHalfFovY;
    // Primary ray-cone spread angle, radians per unit distance, from
    // render::PrimaryRayConeSpreadAngle(tanHalfFovY, renderHeight). The shader
    // could derive this from the two fields above in one line; it is passed
    // instead so the formula lives in a GPU-free translation unit that
    // tests/test_rt_texture_lod.cpp can actually compile and pin. See
    // rt_texture_lod.h for exactly how much of the LOD path that does and does
    // not cover. Sits at offset 208, the start of a fresh 16-byte cbuffer row,
    // so no earlier field moves and HLSL packing needs no explicit padding.
    float primaryConeSpread;

    // -------------------------------------------------------------------------
    // 212..223 — MANDATORY PADDING. Do not delete to be tidy.
    // -------------------------------------------------------------------------
    // primaryConeSpread ends the struct at 212, which is NOT a 16-byte boundary.
    // HLSL places a float4 array on the next one, 224; C++ would place iblSH at
    // 212. That is a 12-byte SHEAR with no compile error on either side - the SH
    // coefficients would be read one row late and three floats offset, producing
    // a smooth, plausible, wrong ambient that reads as "the sky needs tuning".
    //
    // This is the same class of layout trap CBPerFrame's packoffset assertions
    // guard on the raster side, and the offsetof assertions below are what make
    // it a build error rather than an image artefact.
    float pad0[3];

    // -------------------------------------------------------------------------
    // Image-based lighting, consumed by the STABLE PREVIEW only.
    // -------------------------------------------------------------------------
    // Byte-identical in meaning to CBPerFrame::iblSH / iblParams on the raster
    // side, because both paths now call the SAME shaders/ibl_common.hlsli with
    // the same inputs. The full path tracer ignores every field here: it samples
    // DawningSkyRadiance on every miss, which is the integral these coefficients
    // approximate, and substituting the approximation for the reference is
    // exactly the divergence this stage exists to remove rather than create.
    //
    // Nine L2 coefficients with the Lambertian transfer constants already folded
    // in by core::PackIrradianceCoefficients. w is unused padding per row.
    float iblSH[9][4];              // 224..367
    // x = cube mip count, y = intensity, z = enable (the kill switch and the
    // negative control's lever), w = probe write gate.
    float iblParams[4];             // 368..383
};

// Must stay byte-identical to cbuffer PerFrameConstants in shaders/path_trace.hlsl.
// A silent mismatch here misaligns every field after the divergence point.
static_assert(sizeof(RTPerFrameConstants) == 384,
              "RTPerFrameConstants layout changed — update path_trace.hlsl to match");
// The two that catch the 212 -> 224 shear specifically. Deleting pad0[3] leaves
// sizeof() at 372 and both of these fail at BUILD time, which is the only place
// this defect is cheap to find.
static_assert(offsetof(RTPerFrameConstants, iblSH) == 224,
              "HLSL rounds float4 g_IblSH[9] to offset 224; pad0[3] is what makes "
              "the C++ struct agree. Do not remove it.");
static_assert(offsetof(RTPerFrameConstants, iblParams) == 368,
              "g_IblParams must follow the nine SH rows exactly");

// =============================================================================
// Per-instance material data (in a StructuredBuffer, indexed by InstanceID)
// =============================================================================
struct RTMaterialData
{
    float albedo[4];
    float roughness;
    float metallic;
    uint32_t albedoTextureIndex;
    uint32_t useAlbedoTexture;
    uint32_t normalTextureIndex;
    uint32_t useNormalTexture;
    uint32_t ormTextureIndex;
    uint32_t useOrmTexture;
    float emissive[3];
    float emissiveStrength;
    uint32_t emissiveTextureIndex;
    uint32_t useEmissiveTexture;
    uint32_t pad0;
    uint32_t pad1;
};

// Must stay byte-identical to struct MaterialData in shaders/path_trace.hlsl.
// StructuredBuffer elements pack tightly, so every member being 4 bytes means
// the C++ and HLSL layouts agree only as long as both lists match exactly.
static_assert(sizeof(RTMaterialData) == 80,
              "RTMaterialData layout changed - update MaterialData in path_trace.hlsl");

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
    bool                   HasShaderTable() const { return m_shaderTable != nullptr; }

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
    uint32_t m_shaderTableInstanceCount = UINT32_MAX;

    // Retired shader tables, kept alive across kFrameCount rebuilds.
    //
    // BuildShaderTable early-outs unless the instance count changes, so this is
    // not a per-frame hazard - but when the scene's topology DOES change, the
    // old table is replaced while a previously recorded DispatchRays may still
    // reference it. Dropping the ComPtr there is a use-after-free.
    //
    // A ring rather than a fence-tagged queue because this class holds only an
    // ID3D12Device5*, not the D3D12Device wrapper that owns the fence, and
    // plumbing the wrapper in for a rare event is not worth the coupling.
    // Rebuilds are infrequent and a table is a few hundred bytes, so holding the
    // last kFrameCount of them is cheap and unconditionally safe: by the time a
    // slot is overwritten, kFrameCount further rebuilds have occurred, each at
    // least a frame apart.
    ComPtr<ID3D12Resource> m_retiredShaderTables[kFrameCount];
    uint32_t               m_retiredShaderTableSlot = 0;

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
