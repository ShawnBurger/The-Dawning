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
#include <utility>
#include <vector>
#include "../core/types.h"

using Microsoft::WRL::ComPtr;

namespace render
{

struct Texture
{
    Texture() = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    Texture(Texture&& other) noexcept
    {
        MoveFrom(other);
    }

    Texture& operator=(Texture&&) = delete;

    // Texture cannot retire an existing descriptor/resource by itself because
    // that requires Renderer and D3D12Device fence ownership. Adoption therefore
    // succeeds only into an empty value; callers must explicitly retire first.
    bool Adopt(Texture&& other) noexcept
    {
        if (this == &other)
            return true;
        if (resource || descriptorIndex != UINT32_MAX)
            return false;

        MoveFrom(other);
        return true;
    }

    // Call only after the resource and descriptor have been handed to their
    // fence-aware owners.
    void ResetAfterRetirement() noexcept
    {
        resource.Reset();
        width = 0;
        height = 0;
        mipCount = 1;
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
        descriptorIndex = UINT32_MAX;
    }

    ComPtr<ID3D12Resource> resource;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t descriptorIndex = UINT32_MAX;

    bool IsValid() const { return resource && width > 0 && height > 0; }

private:
    void MoveFrom(Texture& other) noexcept
    {
        resource = std::move(other.resource);
        width = other.width;
        height = other.height;
        mipCount = other.mipCount;
        format = other.format;
        descriptorIndex = other.descriptorIndex;

        other.width = 0;
        other.height = 0;
        other.mipCount = 1;
        other.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        other.descriptorIndex = UINT32_MAX;
    }
};

Texture CreateTexture2DFromRGBA8(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const uint32_t* pixels,
    uint32_t width,
    uint32_t height,
    ComPtr<ID3D12Resource>& outUpload,
    const wchar_t* name = nullptr);

Texture CreateTexture2DFromDDSFile(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const char* filePath,
    ComPtr<ID3D12Resource>& outUpload,
    const wchar_t* name = nullptr);

Texture CreateTexture2DFromKTXFile(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const char* filePath,
    ComPtr<ID3D12Resource>& outUpload,
    const wchar_t* name = nullptr);

Texture CreateTexture2DFromWICFile(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const char* filePath,
    ComPtr<ID3D12Resource>& outUpload,
    const wchar_t* name = nullptr);

bool WriteCheckerDDSTextureRGBA8(
    const char* filePath,
    uint32_t width,
    uint32_t height,
    uint32_t checkSize,
    core::Color a,
    core::Color b);

std::vector<uint32_t> GenerateCheckerTextureRGBA8(
    uint32_t width,
    uint32_t height,
    uint32_t checkSize,
    core::Color a,
    core::Color b);

std::vector<uint32_t> GenerateWaveNormalTextureRGBA8(
    uint32_t width,
    uint32_t height,
    float frequency,
    float strength);

} // namespace render
