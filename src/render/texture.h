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
#include "descriptor_allocator.h"

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
        if (resource || descriptor.IsValid())
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
        descriptor = {};
    }

    ComPtr<ID3D12Resource> resource;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    DescriptorHandle descriptor;

    bool IsValid() const { return resource && width > 0 && height > 0; }

private:
    void MoveFrom(Texture& other) noexcept
    {
        resource = std::move(other.resource);
        width = other.width;
        height = other.height;
        mipCount = other.mipCount;
        format = other.format;
        descriptor = other.descriptor;

        other.width = 0;
        other.height = 0;
        other.mipCount = 1;
        other.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        other.descriptor = {};
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

// Decode an image (PNG/JPEG/etc.) that is already in memory rather than on disk.
// This is the glTF embedded-image case: the bytes come from the GLB's BIN chunk.
// bytes must remain valid for the duration of the call.
Texture CreateTexture2DFromWICMemory(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const uint8_t* bytes,
    size_t byteCount,
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

// Packed occlusion / roughness / metallic in the glTF channel convention:
// AO in R, roughness in G, metallic in B. Alpha is unused and written opaque.
//
// The generated pattern varies roughness and metallic across a checker so the
// ORM path is actually exercised by the demo scene - a material feature that no
// shipped asset uses is untested code, and this project has already been bitten
// by treating "it compiles" as "it works".
//
// baseRoughness/baseMetallic are the values in the darker checker cells;
// altRoughness/altMetallic the lighter ones. AO darkens the cell borders.
std::vector<uint32_t> GenerateCheckerORMTextureRGBA8(
    uint32_t width,
    uint32_t height,
    uint32_t checkerSize,
    float baseRoughness,
    float altRoughness,
    float baseMetallic,
    float altMetallic);

// Emissive mask: bright rectangular panels on a black field, with a dim border
// inside each panel so the mask has structure rather than being a flat fill.
//
// Greyscale by construction - the colour comes from ecs::Material::emissive, so
// one generated mask can drive differently coloured emitters. Black elsewhere,
// which matters: emission is added unconditionally, so a mask that is nonzero
// everywhere would wash the whole surface out.
std::vector<uint32_t> GeneratePanelEmissiveTextureRGBA8(
    uint32_t width,
    uint32_t height,
    uint32_t cellSize,
    float panelFraction);

} // namespace render
