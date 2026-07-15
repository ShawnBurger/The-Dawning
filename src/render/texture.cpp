// =============================================================================
// render/texture.cpp - D3D12 texture resource helpers
// =============================================================================

#include "texture.h"
#include "../core/log.h"
#include <cstring>

namespace render
{

static uint32_t AlignTo(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint8_t ToByte(float value)
{
    value = core::Saturate(value);
    return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

static uint32_t PackRGBA8(core::Color color)
{
    uint32_t r = ToByte(color.r);
    uint32_t g = ToByte(color.g);
    uint32_t b = ToByte(color.b);
    uint32_t a = ToByte(color.a);
    return r | (g << 8) | (b << 16) | (a << 24);
}

Texture CreateTexture2DFromRGBA8(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const uint32_t* pixels,
    uint32_t width,
    uint32_t height,
    ComPtr<ID3D12Resource>& outUpload,
    const wchar_t* name)
{
    Texture texture;
    if (!device || !cmdList || !pixels || width == 0 || height == 0)
    {
        core::Log::Error("CreateTexture2DFromRGBA8: invalid input");
        return texture;
    }

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc = { 1, 0 };
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&texture.resource));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create texture %ux%u: 0x%08X", width, height, hr);
        return texture;
    }

    if (name) texture.resource->SetName(name);

    const uint32_t srcPitch = width * 4;
    const uint32_t uploadPitch = AlignTo(srcPitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t uploadSize = static_cast<uint64_t>(uploadPitch) * height;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc = { 1, 0 };
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    hr = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&outUpload));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create texture upload buffer: 0x%08X", hr);
        texture.resource.Reset();
        return texture;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = outUpload->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr) || !mapped)
    {
        core::Log::Errorf("Failed to map texture upload buffer: 0x%08X", hr);
        texture.resource.Reset();
        outUpload.Reset();
        return texture;
    }

    const uint8_t* src = reinterpret_cast<const uint8_t*>(pixels);
    for (uint32_t y = 0; y < height; ++y)
    {
        std::memcpy(mapped + static_cast<size_t>(y) * uploadPitch,
                    src + static_cast<size_t>(y) * srcPitch,
                    srcPitch);
    }
    outUpload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = outUpload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width = width;
    srcLoc.PlacedFootprint.Footprint.Height = height;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = uploadPitch;
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    texture.width = width;
    texture.height = height;
    texture.format = DXGI_FORMAT_R8G8B8A8_UNORM;

    core::Log::Infof("Texture created: %ux%u RGBA8", width, height);
    return texture;
}

std::vector<uint32_t> GenerateCheckerTextureRGBA8(
    uint32_t width,
    uint32_t height,
    uint32_t checkSize,
    core::Color a,
    core::Color b)
{
    if (width == 0) width = 1;
    if (height == 0) height = 1;
    if (checkSize == 0) checkSize = 1;

    std::vector<uint32_t> pixels(width * height);
    const uint32_t ca = PackRGBA8(a);
    const uint32_t cb = PackRGBA8(b);

    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            bool odd = ((x / checkSize) + (y / checkSize)) & 1u;
            pixels[static_cast<size_t>(y) * width + x] = odd ? cb : ca;
        }
    }

    return pixels;
}

} // namespace render
