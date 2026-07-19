// =============================================================================
// render/d3d12_device.cpp — D3D12 Device Implementation
// =============================================================================

#include "d3d12_device.h"
#include "../core/log.h"
#include <d3d12sdklayers.h>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <vector>

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

static bool IsDeviceRemovalError(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
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
    hr = m_cmdList->Close();
    CHECK_HR(hr, "Close initial command list");

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

bool D3D12Device::WaitForFenceValue(uint64_t fenceValue, const char* context)
{
    if (m_deviceLost || !m_fence || !m_fenceEvent)
        return false;

    uint64_t completedValue = m_fence->GetCompletedValue();
    if (completedValue == UINT64_MAX)
    {
        core::Log::Errorf("Fence reported device removal while waiting for %s", context);
        m_deviceLost = true;
        return false;
    }

    if (completedValue >= fenceValue)
        return true;

    const HRESULT eventHr = m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
    bool eventArmed = SUCCEEDED(eventHr);
    if (FAILED(eventHr))
    {
        if (IsDeviceRemovalError(eventHr))
        {
            core::Log::Errorf("SetEventOnCompletion (%s) reported device loss: %s (0x%08X)",
                              context, HrToString(eventHr), eventHr);
            m_deviceLost = true;
            return false;
        }

        // Event registration can fail for reasons unrelated to device removal.
        // The fence remains queryable, so retain the safety guarantee by polling
        // instead of returning early and allowing callers to destroy live GPU data.
        core::Log::Warnf("SetEventOnCompletion (%s) failed; polling fence instead: %s (0x%08X)",
                         context, HrToString(eventHr), eventHr);
    }

    // Do not disappear into an infinite OS wait. A device can be removed after
    // event registration; polling lets us observe D3D12's UINT64_MAX sentinel
    // and leave shutdown/recovery paths instead of waiting on an event that may
    // never be signaled.
    for (;;)
    {
        if (eventArmed)
        {
            const DWORD waitResult = WaitForSingleObject(m_fenceEvent, 1000);
            if (waitResult == WAIT_OBJECT_0)
                return true;

            if (waitResult != WAIT_TIMEOUT)
            {
                core::Log::Warnf("Fence event wait (%s) failed; polling fence instead: "
                                 "result=0x%08X error=%lu",
                                 context, waitResult, GetLastError());
                eventArmed = false;
            }
        }
        else
        {
            Sleep(1);
        }

        completedValue = m_fence->GetCompletedValue();
        if (completedValue == UINT64_MAX)
        {
            core::Log::Errorf("Device removed while waiting for %s", context);
            m_deviceLost = true;
            return false;
        }
        if (completedValue >= fenceValue)
            return true;
    }
}

bool D3D12Device::WaitForCurrentFrame()
{
    // Wait until the GPU has finished processing this frame index's commands.
    return WaitForFenceValue(m_fenceValues[m_frameIndex], "the current frame");
}

bool D3D12Device::ResetCommandList()
{
    if (m_deviceLost || !m_cmdAllocators[m_frameIndex] || !m_cmdList)
        return false;

    HRESULT hr = m_cmdAllocators[m_frameIndex]->Reset();
    if (FAILED(hr))
    {
        core::Log::Errorf("Command allocator Reset failed: %s (0x%08X)",
                          HrToString(hr), hr);
        if (IsDeviceRemovalError(hr))
            m_deviceLost = true;
        return false;
    }

    hr = m_cmdList->Reset(m_cmdAllocators[m_frameIndex].Get(), nullptr);
    if (FAILED(hr))
    {
        core::Log::Errorf("Command list Reset failed: %s (0x%08X)",
                          HrToString(hr), hr);
        if (IsDeviceRemovalError(hr))
            m_deviceLost = true;
        return false;
    }

    return true;
}

bool D3D12Device::ExecuteAndPresent(bool vsync)
{
    if (m_deviceLost || !m_cmdList || !m_cmdQueue || !m_swapChain)
        return false;

    // Close command list. This is the most valuable HRESULT in the frame: a recording
    // error surfaces here as a loggable failure, whereas submitting a list that failed
    // to close turns it into an opaque device removal.
    HRESULT closeHr = m_cmdList->Close();
    if (FAILED(closeHr))
    {
        core::Log::Errorf("Command list Close failed: %s (0x%08X) - skipping submit",
                          HrToString(closeHr), closeHr);
        if (IsDeviceRemovalError(closeHr))
            m_deviceLost = true;
        return false;
    }

    // Execute
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    // Present
    UINT syncInterval = vsync ? 1 : 0;
    UINT presentFlags = (!vsync && m_caps.tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = m_swapChain->Present(syncInterval, presentFlags);
    const bool presentSucceeded = SUCCEEDED(hr);
    if (!presentSucceeded)
    {
        core::Log::Errorf("Present failed: %s (0x%08X)", HrToString(hr), hr);
        if (IsDeviceRemovalError(hr))
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
    if (IsDeviceRemovalError(hr))
    {
        m_deviceLost = true;
        core::Log::Error("Device lost — frame advancement halted");
        return false;
    }

    // Signal fence and move to next frame
    if (!MoveToNextFrame())
        return false;

    return presentSucceeded;
}

// =============================================================================
// Deferred GPU resource release
// =============================================================================
void D3D12Device::DeferredRelease(ComPtr<IUnknown> resource)
{
    if (!resource) return;

    // Tag with the fence value that will be signalled at the END of the current
    // frame, not the last one already signalled. MoveToNextFrame increments
    // m_globalFenceValue and then signals it, so the value covering commands
    // recorded during this frame is m_globalFenceValue + 1 as observed here.
    // Using m_globalFenceValue instead would free the resource one frame early,
    // while the command list recorded this frame still references it.
    m_deferredReleases.Push(m_globalFenceValue + 1, std::move(resource));
}

void D3D12Device::ProcessDeferredReleases()
{
    if (m_deferredReleases.Empty()) return;

    // A lost device never advances its fence again, so drain unconditionally
    // rather than leaking the queue to process exit; nothing can be executing.
    if (m_deviceLost)
    {
        m_deferredReleases.Clear();
        return;
    }

    m_deferredReleases.Process(m_fence ? m_fence->GetCompletedValue() : 0);
}

bool D3D12Device::MoveToNextFrame()
{
    if (m_deviceLost || !m_cmdQueue || !m_fence || !m_swapChain)
        return false;

    // Signal the fence for the current frame
    m_globalFenceValue++;
    m_fenceValues[m_frameIndex] = m_globalFenceValue;
    const HRESULT signalHr = m_cmdQueue->Signal(m_fence.Get(), m_globalFenceValue);
    if (FAILED(signalHr))
    {
        core::Log::Errorf("Frame fence Signal failed: %s (0x%08X)",
                          HrToString(signalHr), signalHr);
        if (IsDeviceRemovalError(signalHr))
            m_deviceLost = true;
        return false;
    }

    // Advance to next frame
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Wait for the next frame's previous work to complete
    if (!WaitForCurrentFrame())
        return false;

    // Once per frame, after the wait, so the fence has advanced as far as it is
    // going to this frame. Callers never have to remember to do this.
    ProcessDeferredReleases();
    return true;
}

bool D3D12Device::WaitForGpu()
{
    // A removed device will never advance this fence. Waiting here during
    // shutdown used to hang the process indefinitely after a device-loss exit.
    if (m_deviceLost || !m_cmdQueue || !m_fence || !m_fenceEvent)
        return false;

    // Signal and wait for all frames. A non-removal Signal failure provides no
    // ordering point at all, so retry while the device reports healthy rather
    // than pretending device loss and force-releasing potentially live objects.
    const uint64_t idleFenceValue = m_globalFenceValue + 1;
    uint32_t signalAttempts = 0;
    for (;;)
    {
        const HRESULT signalHr = m_cmdQueue->Signal(m_fence.Get(), idleFenceValue);
        if (SUCCEEDED(signalHr))
            break;

        const HRESULT removalReason = m_device
            ? m_device->GetDeviceRemovedReason()
            : E_FAIL;
        if (IsDeviceRemovalError(signalHr) || FAILED(removalReason))
        {
            core::Log::Errorf("GPU fence Signal failed after device loss: %s "
                              "(0x%08X), removal reason 0x%08X",
                              HrToString(signalHr), signalHr, removalReason);
            m_deviceLost = true;
            return false;
        }

        ++signalAttempts;
        if (signalAttempts == 1 || signalAttempts % 100 == 0)
        {
            core::Log::Warnf("GPU fence Signal failed while device remains healthy; "
                             "retrying (%u): %s (0x%08X)",
                             signalAttempts, HrToString(signalHr), signalHr);
        }
        if (signalAttempts >= 500)
        {
            core::Log::Error("GPU queue stayed healthy but could not signal an idle fence; "
                             "terminating without unwinding GPU resources");
            std::abort();
        }
        Sleep(10);
    }

    m_globalFenceValue = idleFenceValue;

    if (!WaitForFenceValue(m_globalFenceValue, "GPU idle"))
        return false;

    // Update all fence values
    for (uint32_t i = 0; i < kFrameCount; i++)
        m_fenceValues[i] = m_globalFenceValue;

    ProcessDeferredReleases();
    return true;
}

// =============================================================================
// Resize
// =============================================================================
bool D3D12Device::HasValidFrameTargets() const
{
    if (!m_depthBuffer)
        return false;

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (!m_renderTargets[i])
            return false;
    }

    return true;
}

bool D3D12Device::RebuildFrameTargets()
{
    for (uint32_t i = 0; i < kFrameCount; ++i)
        m_renderTargets[i].Reset();
    m_depthBuffer.Reset();

    if (!CreateRTVs())
        return false;
    if (!CreateDepthBuffer())
        return false;

    return true;
}

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

    if (newWidth == m_width && newHeight == m_height)
    {
        if (HasValidFrameTargets())
            return true;

        core::Log::Warn("Frame targets are incomplete; rebuilding without resizing the swap chain");
        if (!WaitForGpu())
            return false;
        return RebuildFrameTargets();
    }

    core::Log::Infof("Resizing swap chain: %dx%d", newWidth, newHeight);

    // Wait for all frames to complete
    if (!WaitForGpu())
        return false;

    const int oldWidth = m_width;
    const int oldHeight = m_height;

    // Release back buffer references
    for (uint32_t i = 0; i < kFrameCount; i++)
        m_renderTargets[i].Reset();
    m_depthBuffer.Reset();

    // Resize swap chain buffers
    HRESULT hr = m_swapChain->ResizeBuffers(kFrameCount,
        static_cast<UINT>(newWidth), static_cast<UINT>(newHeight),
        DXGI_FORMAT_R8G8B8A8_UNORM,
        m_caps.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr))
    {
        core::Log::Errorf("ResizeBuffers: %s (0x%08X)", HrToString(hr), hr);
        if (IsDeviceRemovalError(hr))
        {
            m_deviceLost = true;
            return false;
        }

        // The back-buffer ComPtrs had to be released before ResizeBuffers. Try to
        // reacquire the previous buffers and recreate their depth target. Rendering
        // remains skipped unless that repair succeeds, and the window keeps the
        // resize request pending for another attempt.
        m_width = oldWidth;
        m_height = oldHeight;
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        if (RebuildFrameTargets())
            core::Log::Warn("Restored previous frame targets after resize failure");
        else
            core::Log::Error("Failed to restore previous frame targets after resize failure");
        return false;
    }

    m_width = newWidth;
    m_height = newHeight;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Recreate RTVs and depth buffer. If either allocation fails, m_width and
    // m_height intentionally remain at the new swap-chain size. The next retry
    // enters the same-size repair path above instead of calling ResizeBuffers a
    // second time against partially rebuilt resources.
    if (!RebuildFrameTargets())
        return false;

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
// Back-Buffer Capture
// =============================================================================
// Phase 1: record the back buffer → READBACK copy into the frame's command list.
// The destination row pitch is D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256) aligned
// and is therefore wider than width * 4 for most resolutions; the writer below
// walks rows by that pitch rather than assuming a packed image.
bool D3D12Device::RecordBackBufferReadback()
{
    ID3D12Resource* backBuffer = CurrentBackBuffer();
    if (!backBuffer)
    {
        core::Log::Error("Back-buffer capture: no current back buffer");
        return false;
    }
    if (!m_device || !m_cmdList)
    {
        core::Log::Error("Back-buffer capture: device or command list unavailable");
        return false;
    }

    const D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
    if (bbDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        core::Log::Errorf("Back-buffer capture: unexpected format %d (expected R8G8B8A8_UNORM)",
                          static_cast<int>(bbDesc.Format));
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT   numRows      = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalBytes   = 0;
    m_device->GetCopyableFootprints(&bbDesc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

    if (totalBytes == 0 || numRows == 0 || footprint.Footprint.RowPitch == 0)
    {
        core::Log::Error("Back-buffer capture: GetCopyableFootprints returned an empty footprint");
        return false;
    }

    // Allocate (or grow) the readback buffer.
    if (!m_captureBuffer || m_captureBufferSize < totalBytes)
    {
        m_captureBuffer.Reset();
        m_captureBufferSize = 0;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width            = totalBytes;
        bufDesc.Height           = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels        = 1;
        bufDesc.Format           = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc       = { 1, 0 };
        bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&m_captureBuffer));
        CHECK_HR(hr, "CreateCommittedResource (back-buffer capture readback)");
        m_captureBuffer->SetName(L"BackBufferCaptureReadback");
        m_captureBufferSize = totalBytes;
    }

    // PRESENT → COPY_SOURCE, copy, COPY_SOURCE → PRESENT. The caller is about to
    // Present, so the back buffer must be handed back in the state it arrived in.
    TransitionResource(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource       = m_captureBuffer.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource        = backBuffer;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    m_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    TransitionResource(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);

    m_captureRowPitch = footprint.Footprint.RowPitch;
    m_captureWidth    = footprint.Footprint.Width;
    m_captureHeight   = footprint.Footprint.Height;
    m_capturePending  = true;
    return true;
}

// Phase 2: drain the GPU, map the readback buffer, write a binary P6 PPM.
// P6 is deliberate: three bytes per pixel, an ASCII header, no dependencies, and
// every image tool plus a dozen lines of PowerShell can read it.
bool D3D12Device::WriteBackBufferCapture(const char* path)
{
    if (!path || !*path)
    {
        core::Log::Error("Back-buffer capture: no output path given");
        return false;
    }
    if (!m_capturePending || !m_captureBuffer)
    {
        core::Log::Error("Back-buffer capture: no readback was recorded");
        return false;
    }
    if (m_deviceLost)
    {
        core::Log::Error("Back-buffer capture: device lost before readback completed");
        return false;
    }

    // The copy was recorded in the frame that has since been submitted; make sure
    // it has actually retired before touching the mapped bytes.
    if (!WaitForGpu())
    {
        core::Log::Error("Back-buffer capture: GPU did not retire the readback copy");
        return false;
    }

    const uint64_t readBytes = m_captureRowPitch * static_cast<uint64_t>(m_captureHeight);

    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(readBytes) };
    void* mapped = nullptr;
    HRESULT hr = m_captureBuffer->Map(0, &readRange, &mapped);
    CHECK_HR(hr, "Map (back-buffer capture readback)");
    if (!mapped)
    {
        core::Log::Error("Back-buffer capture: Map returned a null pointer");
        return false;
    }

    FILE* file = nullptr;
    const errno_t openErr = fopen_s(&file, path, "wb");
    if (openErr != 0 || !file)
    {
        const D3D12_RANGE noWrite = { 0, 0 };
        m_captureBuffer->Unmap(0, &noWrite);
        core::Log::Errorf("Back-buffer capture: failed to open '%s' for writing (errno %d)",
                          path, static_cast<int>(openErr));
        return false;
    }

    bool ok = fprintf(file, "P6\n%u %u\n255\n", m_captureWidth, m_captureHeight) > 0;

    // Drop alpha row by row. Source rows are m_captureRowPitch apart, not width*4.
    const auto* base = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> rgbRow(static_cast<size_t>(m_captureWidth) * 3);
    for (uint32_t y = 0; ok && y < m_captureHeight; y++)
    {
        const uint8_t* srcRow = base + static_cast<size_t>(y * m_captureRowPitch);
        for (uint32_t x = 0; x < m_captureWidth; x++)
        {
            rgbRow[x * 3 + 0] = srcRow[x * 4 + 0];
            rgbRow[x * 3 + 1] = srcRow[x * 4 + 1];
            rgbRow[x * 3 + 2] = srcRow[x * 4 + 2];
        }
        ok = fwrite(rgbRow.data(), 1, rgbRow.size(), file) == rgbRow.size();
    }

    if (ferror(file)) ok = false;
    if (fclose(file) != 0) ok = false;

    const D3D12_RANGE noWrite = { 0, 0 };
    m_captureBuffer->Unmap(0, &noWrite);

    if (!ok)
    {
        core::Log::Errorf("Back-buffer capture: failed to write '%s'", path);
        return false;
    }

    m_capturePending = false;
    core::Log::Infof("Back buffer captured: %s (%ux%u, row pitch %llu)",
                     path, m_captureWidth, m_captureHeight,
                     static_cast<unsigned long long>(m_captureRowPitch));
    core::Log::Infof("[SMOKE] capture=ok file=%s w=%u h=%u", path, m_captureWidth, m_captureHeight);
    return true;
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
    const bool gpuIdle = WaitForGpu();
    if (!gpuIdle && !m_deviceLost && m_cmdQueue && m_fence)
        core::Log::Warn("D3D12 shutdown continuing after GPU idle wait failed");

    // Clear unconditionally before the fence and event are torn down. The normal
    // path is GPU-idle; after device loss, no command stream can make progress and
    // fence-based processing would otherwise retain the queue until destruction.
    if (!m_deferredReleases.Empty())
    {
        core::Log::Infof("Releasing %zu deferred GPU resource(s) at shutdown",
                         m_deferredReleases.Size());
        m_deferredReleases.Clear();
    }

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    // ComPtr releases handle cleanup automatically
    core::Log::Info("D3D12 device shut down");
}

} // namespace render
