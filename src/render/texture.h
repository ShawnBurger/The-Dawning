#pragma once
// =============================================================================
// render/texture.h - D3D12 texture resource helpers
// =============================================================================
// Layer 4 starts with explicit texture resources. File decoders will sit above
// this layer; this file owns raw RGBA8 upload into DEFAULT heap textures.
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include "../core/types.h"

using Microsoft::WRL::ComPtr;

namespace render
{

struct Texture
{
    ComPtr<ID3D12Resource> resource;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t descriptorIndex = UINT32_MAX;

    bool IsValid() const { return resource && width > 0 && height > 0; }
};

Texture CreateTexture2DFromRGBA8(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const uint32_t* pixels,
    uint32_t width,
    uint32_t height,
    ComPtr<ID3D12Resource>& outUpload,
    const wchar_t* name = nullptr);

std::vector<uint32_t> GenerateCheckerTextureRGBA8(
    uint32_t width,
    uint32_t height,
    uint32_t checkSize,
    core::Color a,
    core::Color b);

} // namespace render
