// =============================================================================
// render/d3d12_device.cpp — D3D12 Device Implementation
// =============================================================================

#include "d3d12_device.h"
#include "../core/log.h"
#include <d3d12sdklayers.h>
#include <cstdint>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace render
{

// =============================================================================
// Helpers
// =============================================================================
static const char* HrToString(HRESULT hr)
{
    switch (hr)
    {
    case S_OK:                              return "S_OK";
    case DXGI_ERROR_DEVICE_REMOVED:         return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET:           return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_INVALID_CALL:           return "DXGI_ERROR_INVALID_CALL";
    case E_OUTOFMEMORY:                     return "E_OUTOFMEMORY";
    case E_INVALIDARG:                      return "E_INVALIDARG";
    default:                                return "UNKNOWN";
    }
}

#define CHECK_HR(hr, msg) \
    if (FAILED(hr)) { core::Log::Errorf("%s: %s (0x%08X)", msg, HrToString(hr), hr); return false; }

// =============================================================================
// Init
// =============================================================================
bool D3D12Device::Init(HWND hwnd, int width, int height, bool enableDebugLayer)
{
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    // DRED must be configured BEFORE device creation
    if (enableDebugLayer) SetupDRED();

    if (!CreateDevice(enableDebugLayer)) return false;

    // Probe GPU capabilities after device creation
    ProbeCapabilities();

    if (!CreateCommandObjects())         return false;
    if (!CreateSwapChain(hwnd))          return false;
    if (!CreateRTVs())                   return false;
    if (!CreateDepthBuffer())            return false;

    core::Log::Infof("D3D12 initialized: %dx%d, %d back buffers", m_width, m_height, kFrameCount);
    return true;
}

// =============================================================================
// Device & Adapter
// =============================================================================
bool D3D12Device::CreateDevice(bool enableDebugLayer)
{
    HRESULT hr;

    // Enable debug layer in debug builds
    if (enableDebugLayer)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            core::Log::Info("D3D12 debug layer enabled");

            // Enable GPU-based validation (slower but catches more errors)
            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debugController.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(FALSE); // Enable for deep debugging, disable for perf
            }
        }
    }

    // Create DXGI factory
    UINT factoryFlags = enableDebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
    CHECK_HR(hr, "CreateDXGIFactory2");

    // Select adapter: prefer high-performance discrete GPU
    ComPtr<IDXGIAdapter1> adapter1;
    hr = m_factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&adapter1));
    if (FAILED(hr))
    {
        // Fallback: enumerate adapters manually
        for (UINT i = 0; m_factory->EnumAdapters1(i, &adapter1) != DXGI_ERROR_NOT_FOUND; i++)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter1->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter1.Get(), D3D_FEATURE_LEVEL_12_0,
                                             __uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
            adapter1.Reset();
        }
    }

    if (!adapter1)
    {
        core::Log::Error("No suitable D3D12 adapter found");
        return false;
    }

    adapter1.As(&m_adapter);

    // Log adapter info
    DXGI_ADAPTER_DESC1 adapterDesc;
    adapter1->GetDesc1(&adapterDesc);
    char adapterName[128];
    WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);
    core::Log::Infof("GPU: %s (VRAM: %llu MB)",
        adapterName,
        static_cast<unsigned long long>(adapterDesc.DedicatedVideoMemory / (1024 * 1024)));

    // Create device
    hr = D3D12CreateDevice(adapter1.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    CHECK_HR(hr, "D3D12CreateDevice");

    if (FAILED(m_device.As(&m_device5)))
    {
        core::Log::Warn("ID3D12Device5 unavailable; DXR path tracing disabled");
    }

    // Set up info queue to break on errors in debug
    if (enableDebugLayer)
    {
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(m_device.As(&infoQueue)))
        {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        }
    }

    return true;
}

// =============================================================================
// Command Queue, Allocators, Command List, Fence
// =============================================================================
bool D3D12Device::CreateCommandObjects()
{
    HRESULT hr;

    // Command queue (direct — supports all command types)
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_cmdQueue));
    CHECK_HR(hr, "CreateCommandQueue");
    m_cmdQueue->SetName(L"MainDirectQueue");

    // One allocator per frame (can't reset until GPU finishes with it)
    const wchar_t* allocNames[] = { L"CmdAllocator[0]", L"CmdAllocator[1]", L"CmdAllocator[2]" };
    for (uint32_t i = 0; i < kFrameCount; i++)
    {
        hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&m_cmdAllocators[i]));
        CHECK_HR(hr, "CreateCommandAllocator");
        m_cmdAllocators[i]->SetName(allocNames[i]);
    }

    // Command list (starts in recording state, associated with frame 0's allocator)
    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      m_cmdAllocators[0].Get(), nullptr,
                                      IID_PPV_ARGS(&m_cmdList));
    CHECK_HR(hr, "CreateCommandList");
    m_cmdList->SetName(L"MainCommandList");

    if (FAILED(m_cmdList.As(&m_cmdList4)))
    {
        core::Log::Warn("ID3D12GraphicsCommandList4 unavailable; DXR path tracing disabled");
    }

    // Close immediately — we'll reset it at the start of each frame
    m_cmdList->Close();

    // Fence for CPU/GPU synchronization
    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    CHECK_HR(hr, "CreateFence");
    m_fence->SetName(L"FrameFence");

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
    {
        core::Log::Error("Failed to create fence event");
        return false;
    }

    for (uint32_t i = 0; i < kFrameCount; i++)
        m_fenceValues[i] = 0;
    m_globalFenceValue = 0;

    core::Log::Info("Command objects created (all named for DRED)");
    return true;
}

// =============================================================================
// Swap Chain
// =============================================================================
bool D3D12Device::CreateSwapChain(HWND hwnd)
{
    HRESULT hr;

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width       = static_cast<UINT>(m_width);
    scDesc.Height      = static_cast<UINT>(m_height);
    scDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.Stereo      = FALSE;
    scDesc.SampleDesc  = { 1, 0 };
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = kFrameCount;
    scDesc.Scaling     = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // Best for modern GPUs
    scDesc.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags       = m_caps.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    hr = m_factory->CreateSwapChainForHwnd(m_cmdQueue.Get(), hwnd, &scDesc,
                                            nullptr, nullptr, &swapChain1);
    CHECK_HR(hr, "CreateSwapChainForHwnd");

    // Disable Alt+Enter fullscreen toggle (we'll handle it ourselves later)
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = swapChain1.As(&m_swapChain);
    CHECK_HR(hr, "SwapChain QueryInterface");

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    core::Log::Infof("Swap chain created (FLIP_DISCARD, tearing %s)",
                     m_caps.tearingSupported ? "enabled" : "disabled");
    return true;
}

// =============================================================================
// Render Target Views
// =============================================================================
bool D3D12Device::CreateRTVs()
{
    HRESULT hr;

    // Create RTV descriptor heap (if not already created)
    if (!m_rtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = kFrameCount;
        rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
        CHECK_HR(hr, "CreateDescriptorHeap (RTV)");
        m_rtvDescSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // Get swap chain buffers and create RTVs
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const wchar_t* bbNames[] = { L"BackBuffer[0]", L"BackBuffer[1]", L"BackBuffer[2]" };
    for (uint32_t i = 0; i < kFrameCount; i++)
    {
        hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        CHECK_HR(hr, "GetBuffer (swap chain)");
        m_renderTargets[i]->SetName(bbNames[i]);
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescSize;
    }

    return true;
}

// =============================================================================
// Depth/Stencil Buffer
// =============================================================================
bool D3D12Device::CreateDepthBuffer()
{
    HRESULT hr;

    // DSV descriptor heap
    if (!m_dsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap));
        CHECK_HR(hr, "CreateDescriptorHeap (DSV)");
    }

    // Depth buffer resource
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width            = static_cast<UINT64>(m_width);
    depthDesc.Height           = static_cast<UINT>(m_height);
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels        = 1;
    depthDesc.Format           = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc       = { 1, 0 };
    depthDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format               = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth   = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue, IID_PPV_ARGS(&m_depthBuffer));
    CHECK_HR(hr, "CreateCommittedResource (depth buffer)");
    m_depthBuffer->SetName(L"DepthBuffer");

    // Create DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc,
                                      m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

// =============================================================================
// Frame Lifecycle
// =============================================================================

void D3D12Device::WaitForCurrentFrame()
{
    // Wait until the GPU has finished processing this frame index's commands
    uint64_t fenceValue = m_fenceValues[m_frameIndex];
    if (m_fence->GetCompletedValue() < fenceValue)
    {
        m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void D3D12Device::ResetCommandList()
{
    m_cmdAllocators[m_frameIndex]->Reset();
    m_cmdList->Reset(m_cmdAllocators[m_frameIndex].Get(), nullptr);
}

void D3D12Device::ExecuteAndPresent(bool vsync)
{
    // Close command list. This is the most valuable HRESULT in the frame: a recording
    // error surfaces here as a loggable failure, whereas submitting a list that failed
    // to close turns it into an opaque device removal.
    HRESULT closeHr = m_cmdList->Close();
    if (FAILED(closeHr))
    {
        core::Log::Errorf("Command list Close failed: %s (0x%08X) - skipping submit",
                          HrToString(closeHr), closeHr);
        return;
    }

    // Execute
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    // Present
    UINT syncInterval = vsync ? 1 : 0;
    UINT presentFlags = (!vsync && m_caps.tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = m_swapChain->Present(syncInterval, presentFlags);
    if (FAILED(hr))
    {
        core::Log::Errorf("Present failed: %s (0x%08X)", HrToString(hr), hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED)
        {
            HRESULT reason = m_device->GetDeviceRemovedReason();
            core::Log::Errorf("Device removed reason: 0x%08X", reason);

            // Retrieve DRED data if available
            ComPtr<ID3D12DeviceRemovedExtendedData> dredData;
            if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&dredData))))
            {
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
                if (SUCCEEDED(dredData->GetAutoBreadcrumbsOutput(&breadcrumbs)))
                {
                    const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
                    while (node)
                    {
                        // pLastBreadcrumbValue is legitimately null for nodes whose
                        // breadcrumb buffer was not resident at removal. Dereferencing
                        // it here would crash inside the post-mortem handler and destroy
                        // the very diagnostics we came for.
                        const bool haveCount = (node->pLastBreadcrumbValue != nullptr);
                        const unsigned completed =
                            haveCount ? static_cast<unsigned>(*node->pLastBreadcrumbValue) : 0u;

                        char name[128] = "(unnamed)";
                        if (node->pCommandListDebugNameW)
                        {
                            WideCharToMultiByte(CP_UTF8, 0, node->pCommandListDebugNameW, -1,
                                                name, sizeof(name), nullptr, nullptr);
                        }

                        if (haveCount)
                        {
                            core::Log::Errorf("  DRED Breadcrumb: CmdList='%s', completed %u/%u ops",
                                              name, completed, node->BreadcrumbCount);
                        }
                        else
                        {
                            core::Log::Errorf("  DRED Breadcrumb: CmdList='%s', completed ?/%u ops "
                                              "(breadcrumb buffer not resident)",
                                              name, node->BreadcrumbCount);
                        }
                        node = node->pNext;
                    }
                }

                D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
                if (SUCCEEDED(dredData->GetPageFaultAllocationOutput(&pageFault)))
                {
                    core::Log::Errorf("  DRED Page Fault VA: 0x%llX",
                                      static_cast<unsigned long long>(pageFault.PageFaultVA));
                }
            }
        }
    }

    // Only advance frame if device is still healthy
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        m_deviceLost = true;
        core::Log::Error("Device lost — frame advancement halted");
        return;
    }

    // Signal fence and move to next frame
    MoveToNextFrame();
}

void D3D12Device::MoveToNextFrame()
{
    // Signal the fence for the current frame
    m_globalFenceValue++;
    m_fenceValues[m_frameIndex] = m_globalFenceValue;
    m_cmdQueue->Signal(m_fence.Get(), m_globalFenceValue);

    // Advance to next frame
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Wait for the next frame's previous work to complete
    WaitForCurrentFrame();
}

void D3D12Device::WaitForGpu()
{
    // Signal and wait for all frames
    m_globalFenceValue++;
    m_cmdQueue->Signal(m_fence.Get(), m_globalFenceValue);

    m_fence->SetEventOnCompletion(m_globalFenceValue, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);

    // Update all fence values
    for (uint32_t i = 0; i < kFrameCount; i++)
        m_fenceValues[i] = m_globalFenceValue;
}

// =============================================================================
// Resize
// =============================================================================
bool D3D12Device::Resize(int newWidth, int newHeight)
{
    if (newWidth <= 0 || newHeight <= 0) return false;

    // Never touch a dead swap chain. Without this, a resize arriving during
    // device-removed state calls ResizeBuffers on an object that cannot service it.
    if (m_deviceLost)
    {
        core::Log::Warn("Resize ignored: device is lost");
        return false;
    }

    core::Log::Infof("Resizing swap chain: %dx%d", newWidth, newHeight);

    // Wait for all frames to complete
    WaitForGpu();

    // Release back buffer references
    for (uint32_t i = 0; i < kFrameCount; i++)
        m_renderTargets[i].Reset();
    m_depthBuffer.Reset();

    // Resize swap chain buffers
    HRESULT hr = m_swapChain->ResizeBuffers(kFrameCount,
        static_cast<UINT>(newWidth), static_cast<UINT>(newHeight),
        DXGI_FORMAT_R8G8B8A8_UNORM,
        m_caps.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    CHECK_HR(hr, "ResizeBuffers");

    m_width = newWidth;
    m_height = newHeight;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Recreate RTVs and depth buffer
    if (!CreateRTVs())       return false;
    if (!CreateDepthBuffer()) return false;

    // Reset fence values
    for (uint32_t i = 0; i < kFrameCount; i++)
        m_fenceValues[i] = m_globalFenceValue;

    return true;
}

// =============================================================================
// Barrier Helper
// =============================================================================
void D3D12Device::TransitionResource(ID3D12Resource* resource,
                                      D3D12_RESOURCE_STATES before,
                                      D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &barrier);
}

// =============================================================================
// Descriptor Accessors
// =============================================================================
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::CurrentRTV() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_frameIndex * m_rtvDescSize);
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::DSV() const
{
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

// =============================================================================
// DRED — Device Removed Extended Data
// =============================================================================
// Must be called BEFORE D3D12CreateDevice. Configures breadcrumbs and page fault
// reporting so that when a GPU crash occurs, we get actionable diagnostics
// instead of just "device removed."
void D3D12Device::SetupDRED()
{
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
    {
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        m_caps.dredSupported = true;
        core::Log::Info("DRED enabled (auto-breadcrumbs + page fault reporting)");
    }
    else
    {
        core::Log::Warn("DRED not available on this system (requires Windows 10 1903+)");
    }
}

// =============================================================================
// GPU Capability Probing
// =============================================================================
void D3D12Device::ProbeCapabilities()
{
    // Adapter info (already partially captured in CreateDevice, fill struct here)
    if (m_adapter)
    {
        DXGI_ADAPTER_DESC1 desc;
        ComPtr<IDXGIAdapter1> adapter1;
        if (SUCCEEDED(m_adapter.As(&adapter1)))
        {
            adapter1->GetDesc1(&desc);
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                m_caps.adapterName, sizeof(m_caps.adapterName), nullptr, nullptr);
            m_caps.dedicatedVideoMemoryMB = desc.DedicatedVideoMemory / (1024 * 1024);
        }
    }

    // Shader model
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {};
    // Try highest first, fall back
    D3D_SHADER_MODEL models[] = {
        D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0
    };
    for (auto sm : models)
    {
        shaderModel.HighestShaderModel = sm;
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                        &shaderModel, sizeof(shaderModel))))
        {
            m_caps.highestShaderModel = (static_cast<uint32_t>(shaderModel.HighestShaderModel >> 4) * 10)
                                      + static_cast<uint32_t>(shaderModel.HighestShaderModel & 0xF);
            break;
        }
    }

    // Feature level
    D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0
    };
    featureLevels.NumFeatureLevels = _countof(levels);
    featureLevels.pFeatureLevelsRequested = levels;
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,
                    &featureLevels, sizeof(featureLevels))))
    {
        m_caps.maxFeatureLevel = featureLevels.MaxSupportedFeatureLevel;
    }

    // D3D12 Options for mesh shaders, raytracing, VRS, etc.
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                    &options7, sizeof(options7))))
    {
        m_caps.meshShaders = (options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1);
        m_caps.samplerFeedback = (options7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                    &options5, sizeof(options5))))
    {
        m_caps.raytracing = m_device5 && (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);
        m_caps.raytracingTier1_1 = m_device5 && (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,
                    &options6, sizeof(options6))))
    {
        m_caps.variableRateShading = (options6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1);
    }

    // Resource binding tier
    D3D12_FEATURE_DATA_D3D12_OPTIONS options0 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                    &options0, sizeof(options0))))
    {
        m_caps.resourceBindingTier = static_cast<uint32_t>(options0.ResourceBindingTier);
        m_caps.bindlessResources = (options0.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3);
    }

    // Tearing support (for variable refresh rate / unlocked FPS)
    BOOL tearingSupported = FALSE;
    if (m_factory)
    {
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(m_factory.As(&factory5)))
        {
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearingSupported, sizeof(tearingSupported))))
            {
                m_caps.tearingSupported = (tearingSupported == TRUE);
            }
        }
    }

    // Enhanced barriers (D3D12 Options12)
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,
                    &options12, sizeof(options12))))
    {
        m_caps.enhancedBarriers = (options12.EnhancedBarriersSupported == TRUE);
    }

    // HDR output support (check primary output for ST2084/HDR10 color space)
    if (m_adapter)
    {
        ComPtr<IDXGIOutput> output;
        if (SUCCEEDED(m_adapter->EnumOutputs(0, &output)))
        {
            ComPtr<IDXGIOutput6> output6;
            if (SUCCEEDED(output.As(&output6)))
            {
                DXGI_OUTPUT_DESC1 outputDesc;
                if (SUCCEEDED(output6->GetDesc1(&outputDesc)))
                {
                    m_caps.hdrSupported = (outputDesc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                }
            }
        }
    }

    // Log capability summary
    core::Log::Info("=== GPU Capabilities ===");
    core::Log::Infof("  Adapter: %s (%llu MB VRAM)", m_caps.adapterName,
                     static_cast<unsigned long long>(m_caps.dedicatedVideoMemoryMB));
    // D3D_FEATURE_LEVEL_12_0/12_1/12_2 are 0xc000/0xc100/0xc200 - the minor version
    // lives in bits 8-11, not 0-3. Masking the low nibble reported "12.0" on every
    // adapter, including 12_2 / DX12 Ultimate parts.
    core::Log::Infof("  Feature Level: 12.%d", ((m_caps.maxFeatureLevel >> 8) & 0xF));
    core::Log::Infof("  Shader Model: %d.%d", m_caps.highestShaderModel / 10,
                     m_caps.highestShaderModel % 10);
    core::Log::Infof("  Resource Binding Tier: %d%s", m_caps.resourceBindingTier,
                     m_caps.bindlessResources ? " (bindless)" : "");
    core::Log::Infof("  Mesh Shaders: %s", m_caps.meshShaders ? "YES" : "no");
    core::Log::Infof("  Raytracing: %s%s",
                     m_caps.raytracing ? "YES" : "no",
                     m_caps.raytracingTier1_1 ? " (Tier 1.1)" : "");
    core::Log::Infof("  VRS: %s", m_caps.variableRateShading ? "YES" : "no");
    core::Log::Infof("  Enhanced Barriers: %s", m_caps.enhancedBarriers ? "YES" : "no");
    core::Log::Infof("  HDR: %s", m_caps.hdrSupported ? "YES" : "no");
    core::Log::Infof("  Tearing: %s", m_caps.tearingSupported ? "YES" : "no");
    core::Log::Infof("  DRED: %s", m_caps.dredSupported ? "YES" : "no");
    core::Log::Info("========================");
}

// =============================================================================
// Shutdown
// =============================================================================
void D3D12Device::Shutdown()
{
    WaitForGpu();

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    // ComPtr releases handle cleanup automatically
    core::Log::Info("D3D12 device shut down");
}

} // namespace render
