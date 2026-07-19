#pragma once
// =============================================================================
// render/d3d12_device.h — D3D12 Device & Swap Chain
// =============================================================================
// Manages all D3D12 infrastructure:
//   - DXGI factory and adapter selection (prefers discrete GPU)
//   - D3D12 device at Feature Level 12.0
//   - Direct command queue, allocators, and command list
//   - Swap chain with triple buffering (FLIP_DISCARD)
//   - RTV and DSV descriptor heaps
//   - Depth buffer
//   - Fence-based CPU/GPU synchronization
//
// The frame cycle is:
//   1. WaitForPreviousFrame() — CPU waits until GPU finishes frame N-2
//   2. ResetCommandList() — reset allocator and command list for current frame
//   3. ... record commands ...
//   4. ExecuteAndPresent() — close list, execute, present, signal fence
// =============================================================================

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "deferred_release.h"
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace render
{

static constexpr uint32_t kFrameCount = 3;  // Triple buffering

// =============================================================================
// GPU Capability Database
// =============================================================================
// Probed at startup so the rest of the engine can branch on features
// without scattering CheckFeatureSupport calls everywhere.
struct GpuCapabilities
{
    // Adapter info
    char adapterName[128] = {};
    uint64_t dedicatedVideoMemoryMB = 0;

    // Feature levels / shader models
    D3D_FEATURE_LEVEL maxFeatureLevel = D3D_FEATURE_LEVEL_12_0;
    uint32_t highestShaderModel = 60;     // 60 = SM6.0, 65 = SM6.5, 66 = SM6.6, etc.

    // Rendering features
    bool meshShaders = false;
    bool raytracing = false;              // DXR 1.0+
    bool raytracingTier1_1 = false;       // DXR 1.1 (inline raytracing)
    bool variableRateShading = false;     // VRS Tier 1+
    bool samplerFeedback = false;
    bool enhancedBarriers = false;

    // Resource binding
    uint32_t resourceBindingTier = 1;     // 1, 2, or 3
    bool bindlessResources = false;       // Tier 3 = true bindless

    // Presentation
    bool tearingSupported = false;        // DXGI_FEATURE_PRESENT_ALLOW_TEARING
    bool hdrSupported = false;

    // Diagnostics
    bool dredSupported = false;           // Device Removed Extended Data
};

class D3D12Device
{
public:
    bool Init(HWND hwnd, int width, int height, bool enableDebugLayer = true);
    void Shutdown();

    // Frame lifecycle
    void WaitForCurrentFrame();
    void ResetCommandList();
    void ExecuteAndPresent(bool vsync = true);

    // Resize swap chain (call when window resizes)
    bool Resize(int newWidth, int newHeight);

    // -------------------------------------------------------------------------
    // Deferred GPU resource release
    // -------------------------------------------------------------------------
    // Dropping a ComPtr to a GPU resource frees it immediately, but up to
    // kFrameCount frames may still be in flight with command lists referencing
    // it. Hand it here instead: the resource is retained until the GPU has
    // passed the fence value signalled at the end of the frame in which it was
    // retired, then released.
    //
    // Takes IUnknown so it works for resources, heaps, PSOs and root signatures
    // alike. Safe to call mid-frame; that is the entire point.
    //
    // Releases are processed automatically once per frame from MoveToNextFrame,
    // and flushed during Shutdown after a full WaitForGpu.
    void DeferredRelease(ComPtr<IUnknown> resource);
    void ProcessDeferredReleases();

    // Fence values for subsystems that defer their own reclamation (e.g. the
    // descriptor allocator, which parks freed heap indices rather than owning a
    // COM object it could hand to DeferredRelease).
    //
    // PendingFenceValue is the value that will be signalled at the END of the
    // current frame - the same +1 DeferredRelease uses, and for the same reason:
    // work recorded during this frame is covered by the next signal, not the
    // last one. Tagging with CompletedFenceValue would release a frame early.
    uint64_t PendingFenceValue()   const { return m_globalFenceValue + 1; }
    uint64_t CompletedFenceValue() const
    {
        return m_fence ? m_fence->GetCompletedValue() : 0;
    }
    uint32_t PendingDeferredReleaseCount() const
    {
        return static_cast<uint32_t>(m_deferredReleases.Size());
    }

    // Barrier helpers
    void TransitionResource(ID3D12Resource* resource,
                            D3D12_RESOURCE_STATES before,
                            D3D12_RESOURCE_STATES after);

    // -------------------------------------------------------------------------
    // Back-buffer capture (smoke test "eyes")
    // -------------------------------------------------------------------------
    // Two-phase because the copy has to be recorded inside the frame that drew
    // the image (a FLIP_DISCARD back buffer is undefined after Present) while the
    // Map can only happen once that frame's GPU work has retired.
    //
    //   1. RecordBackBufferReadback() — call after the frame's render commands,
    //      with the back buffer in PRESENT state and the command list still open.
    //      Leaves the back buffer in PRESENT so ExecuteAndPresent() stays legal.
    //   2. ExecuteAndPresent()
    //   3. WriteBackBufferCapture(path) — waits for the GPU, maps the readback
    //      buffer and writes a binary P6 PPM.
    //
    // Both return false and log via core::Log::Errorf on any failure.
    bool RecordBackBufferReadback();
    bool WriteBackBufferCapture(const char* path);
    bool HasPendingCapture() const { return m_capturePending; }

    // Accessors
    ID3D12Device*              Device()   const { return m_device.Get(); }
    ID3D12GraphicsCommandList* CmdList()  const { return m_cmdList.Get(); }
    ID3D12Device5*             Device5()  const { return m_device5.Get(); }
    ID3D12GraphicsCommandList4* CmdList4() const { return m_cmdList4.Get(); }
    ID3D12CommandQueue*        CmdQueue() const { return m_cmdQueue.Get(); }

    D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DSV() const;
    ID3D12Resource*             CurrentBackBuffer() const { return m_renderTargets[m_frameIndex].Get(); }

    uint32_t FrameIndex() const { return m_frameIndex; }
    int      Width()  const { return m_width; }
    int      Height() const { return m_height; }
    bool     IsDeviceLost() const { return m_deviceLost; }

    const GpuCapabilities& Caps() const { return m_caps; }

    // Wait for all GPU work to complete (used during shutdown/resize)
    void WaitForGpu();

private:
    bool CreateDevice(bool enableDebugLayer);
    bool CreateCommandObjects();
    bool CreateSwapChain(HWND hwnd);
    bool CreateRTVs();
    bool CreateDepthBuffer();
    void ProbeCapabilities();
    void SetupDRED();
    void MoveToNextFrame();

    // DXGI
    ComPtr<IDXGIFactory6>    m_factory;
    ComPtr<IDXGIAdapter4>    m_adapter;

    // Device
    ComPtr<ID3D12Device>     m_device;
    ComPtr<ID3D12Device5>    m_device5;

    // Command objects
    ComPtr<ID3D12CommandQueue>          m_cmdQueue;
    ComPtr<ID3D12CommandAllocator>      m_cmdAllocators[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList>   m_cmdList;
    ComPtr<ID3D12GraphicsCommandList4>  m_cmdList4;

    // Swap chain
    ComPtr<IDXGISwapChain4>  m_swapChain;
    ComPtr<ID3D12Resource>   m_renderTargets[kFrameCount];
    uint32_t                 m_frameIndex = 0;
    HWND                     m_hwnd = nullptr;

    // Descriptors
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    uint32_t                     m_rtvDescSize = 0;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

    // Depth buffer
    ComPtr<ID3D12Resource>   m_depthBuffer;

    // Back-buffer capture (READBACK heap; only allocated if a capture is requested)
    ComPtr<ID3D12Resource>   m_captureBuffer;
    uint64_t                 m_captureBufferSize = 0;
    uint64_t                 m_captureRowPitch = 0;   // 256-aligned, != width * 4
    uint32_t                 m_captureWidth = 0;
    uint32_t                 m_captureHeight = 0;
    bool                     m_capturePending = false;

    // Fence
    ComPtr<ID3D12Fence>      m_fence;
    HANDLE                   m_fenceEvent = nullptr;
    uint64_t                 m_fenceValues[kFrameCount] = {};
    uint64_t                 m_globalFenceValue = 0;

    // Ordering lives in DeferredReleaseQueue (render/deferred_release.h), which
    // has no D3D12 dependency and is unit tested. This class supplies the fence.
    DeferredReleaseQueue<ComPtr<IUnknown>> m_deferredReleases;

    // Dimensions
    int m_width = 0;
    int m_height = 0;
    bool m_deviceLost = false;

    // Capabilities
    GpuCapabilities m_caps;
};

} // namespace render
