// =============================================================================
// render/texture.cpp - D3D12 texture resource helpers
// =============================================================================

#include "texture.h"
#include "../core/log.h"
#include <cstring>
#include <fstream>

namespace render
{

static constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a)
        | (static_cast<uint32_t>(b) << 8)
        | (static_cast<uint32_t>(c) << 16)
        | (static_cast<uint32_t>(d) << 24);
}

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

static bool IsBlockCompressed(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static uint32_t BitsPerPixel(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return 32;
    case DXGI_FORMAT_R8_UNORM:
        return 8;
    default:
        return 0;
    }
}

static uint32_t BCBlockBytes(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return 8;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 16;
    default:
        return 0;
    }
}

static bool ComputeSurfaceInfo(
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    uint32_t& rowBytes,
    uint32_t& numRows,
    uint64_t& totalBytes)
{
    if (width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN)
        return false;

    if (IsBlockCompressed(format))
    {
        uint32_t blockBytes = BCBlockBytes(format);
        if (blockBytes == 0) return false;
        rowBytes = ((width + 3u) / 4u) * blockBytes;
        numRows = (height + 3u) / 4u;
    }
    else
    {
        uint32_t bpp = BitsPerPixel(format);
        if (bpp == 0) return false;
        rowBytes = (width * bpp + 7u) / 8u;
        numRows = height;
    }

    totalBytes = static_cast<uint64_t>(rowBytes) * numRows;
    return true;
}

static Texture CreateTexture2DFromMemory(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const uint8_t* data,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    uint32_t srcRowBytes,
    uint32_t srcNumRows,
    ComPtr<ID3D12Resource>& outUpload,
    const wchar_t* name)
{
    Texture texture;
    if (!device || !cmdList || !data || width == 0 || height == 0 ||
        format == DXGI_FORMAT_UNKNOWN || srcRowBytes == 0 || srcNumRows == 0)
    {
        core::Log::Error("CreateTexture2DFromMemory: invalid input");
        return texture;
    }

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
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

    const uint32_t uploadPitch = AlignTo(srcRowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t uploadSize = static_cast<uint64_t>(uploadPitch) * srcNumRows;

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

    for (uint32_t y = 0; y < srcNumRows; ++y)
    {
        std::memcpy(mapped + static_cast<size_t>(y) * uploadPitch,
                    data + static_cast<size_t>(y) * srcRowBytes,
                    srcRowBytes);
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
    srcLoc.PlacedFootprint.Footprint.Format = format;
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
    texture.mipCount = 1;
    texture.format = format;

    core::Log::Infof("Texture created: %ux%u format=%u", width, height, static_cast<uint32_t>(format));
    return texture;
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
    return CreateTexture2DFromMemory(
        device,
        cmdList,
        reinterpret_cast<const uint8_t*>(pixels),
        width,
        height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        width * 4,
        height,
        outUpload,
        name);
}

#pragma pack(push, 1)
struct DDSPixelFormat
{
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask;
    uint32_t gBitMask;
    uint32_t bBitMask;
    uint32_t aBitMask;
};

struct DDSHeader
{
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DDSPixelFormat ddspf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};

struct DDSHeaderDXT10
{
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
#pragma pack(pop)

static DXGI_FORMAT FormatFromDDSHeader(const DDSHeader& header, const uint8_t* bytes, size_t size, size_t& dataOffset)
{
    static constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
    static constexpr uint32_t DDPF_FOURCC = 0x4;
    static constexpr uint32_t DDPF_RGB = 0x40;

    const DDSPixelFormat& pf = header.ddspf;
    if ((pf.flags & DDPF_FOURCC) != 0)
    {
        switch (pf.fourCC)
        {
        case MakeFourCC('D', 'X', 'T', '1'): return DXGI_FORMAT_BC1_UNORM;
        case MakeFourCC('D', 'X', 'T', '3'): return DXGI_FORMAT_BC2_UNORM;
        case MakeFourCC('D', 'X', 'T', '5'): return DXGI_FORMAT_BC3_UNORM;
        case MakeFourCC('A', 'T', 'I', '1'): return DXGI_FORMAT_BC4_UNORM;
        case MakeFourCC('B', 'C', '4', 'U'): return DXGI_FORMAT_BC4_UNORM;
        case MakeFourCC('B', 'C', '4', 'S'): return DXGI_FORMAT_BC4_SNORM;
        case MakeFourCC('A', 'T', 'I', '2'): return DXGI_FORMAT_BC5_UNORM;
        case MakeFourCC('B', 'C', '5', 'U'): return DXGI_FORMAT_BC5_UNORM;
        case MakeFourCC('B', 'C', '5', 'S'): return DXGI_FORMAT_BC5_SNORM;
        case MakeFourCC('D', 'X', '1', '0'):
            if (size < dataOffset + sizeof(DDSHeaderDXT10))
                return DXGI_FORMAT_UNKNOWN;
            {
                const auto* dx10 = reinterpret_cast<const DDSHeaderDXT10*>(bytes + dataOffset);
                dataOffset += sizeof(DDSHeaderDXT10);
                if (dx10->arraySize != 1 || dx10->resourceDimension != 3)
                    return DXGI_FORMAT_UNKNOWN;
                return static_cast<DXGI_FORMAT>(dx10->dxgiFormat);
            }
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    }

    if ((pf.flags & DDPF_RGB) != 0 && pf.rgbBitCount == 32)
    {
        if (pf.rBitMask == 0x000000ff && pf.gBitMask == 0x0000ff00 &&
            pf.bBitMask == 0x00ff0000 && pf.aBitMask == 0xff000000)
        {
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        }

        if (pf.rBitMask == 0x00ff0000 && pf.gBitMask == 0x0000ff00 &&
            pf.bBitMask == 0x000000ff && pf.aBitMask == 0xff000000)
        {
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        }

        if ((pf.flags & DDPF_ALPHAPIXELS) == 0 &&
            pf.rBitMask == 0x00ff0000 && pf.gBitMask == 0x0000ff00 &&
            pf.bBitMask == 0x000000ff)
        {
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        }
    }

    if ((pf.flags & DDPF_RGB) != 0 && pf.rgbBitCount == 8 && pf.rBitMask == 0x000000ff)
        return DXGI_FORMAT_R8_UNORM;

    return DXGI_FORMAT_UNKNOWN;
}

Texture CreateTexture2DFromDDSFile(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const char* filePath,
    ComPtr<ID3D12Resource>& outUpload,
    const wchar_t* name)
{
    Texture texture;
    if (!filePath || !filePath[0])
        return texture;

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file)
    {
        core::Log::Warnf("DDS texture not found: %s", filePath);
        return texture;
    }

    std::streamsize fileSize = file.tellg();
    if (fileSize <= 0)
    {
        core::Log::Errorf("DDS texture is empty: %s", filePath);
        return texture;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), fileSize))
    {
        core::Log::Errorf("Failed to read DDS texture: %s", filePath);
        return texture;
    }

    static constexpr uint32_t kDDSMagic = MakeFourCC('D', 'D', 'S', ' ');
    if (bytes.size() < sizeof(uint32_t) + sizeof(DDSHeader))
    {
        core::Log::Errorf("DDS texture too small: %s", filePath);
        return texture;
    }

    uint32_t magic = *reinterpret_cast<const uint32_t*>(bytes.data());
    if (magic != kDDSMagic)
    {
        core::Log::Errorf("Invalid DDS magic: %s", filePath);
        return texture;
    }

    const auto* header = reinterpret_cast<const DDSHeader*>(bytes.data() + sizeof(uint32_t));
    if (header->size != sizeof(DDSHeader) || header->ddspf.size != sizeof(DDSPixelFormat))
    {
        core::Log::Errorf("Invalid DDS header: %s", filePath);
        return texture;
    }

    size_t dataOffset = sizeof(uint32_t) + sizeof(DDSHeader);
    DXGI_FORMAT format = FormatFromDDSHeader(*header, bytes.data(), bytes.size(), dataOffset);
    if (format == DXGI_FORMAT_UNKNOWN)
    {
        core::Log::Errorf("Unsupported DDS format: %s", filePath);
        return texture;
    }

    uint32_t rowBytes = 0;
    uint32_t numRows = 0;
    uint64_t totalBytes = 0;
    if (!ComputeSurfaceInfo(header->width, header->height, format, rowBytes, numRows, totalBytes))
    {
        core::Log::Errorf("Could not compute DDS surface info: %s", filePath);
        return texture;
    }

    if (bytes.size() < dataOffset + totalBytes)
    {
        core::Log::Errorf("DDS texture data is truncated: %s", filePath);
        return texture;
    }

    texture = CreateTexture2DFromMemory(
        device,
        cmdList,
        bytes.data() + dataOffset,
        header->width,
        header->height,
        format,
        rowBytes,
        numRows,
        outUpload,
        name);

    if (texture.IsValid())
        core::Log::Infof("DDS loaded: %s (%ux%u, format=%u)",
                         filePath, texture.width, texture.height, static_cast<uint32_t>(format));
    return texture;
}

bool WriteCheckerDDSTextureRGBA8(
    const char* filePath,
    uint32_t width,
    uint32_t height,
    uint32_t checkSize,
    core::Color a,
    core::Color b)
{
    if (!filePath || !filePath[0])
        return false;

    auto pixels = GenerateCheckerTextureRGBA8(width, height, checkSize, a, b);
    std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        core::Log::Warnf("Could not write starter DDS texture: %s", filePath);
        return false;
    }

    static constexpr uint32_t kDDSMagic = MakeFourCC('D', 'D', 'S', ' ');
    static constexpr uint32_t DDSD_CAPS = 0x1;
    static constexpr uint32_t DDSD_HEIGHT = 0x2;
    static constexpr uint32_t DDSD_WIDTH = 0x4;
    static constexpr uint32_t DDSD_PITCH = 0x8;
    static constexpr uint32_t DDSD_PIXELFORMAT = 0x1000;
    static constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
    static constexpr uint32_t DDPF_RGB = 0x40;
    static constexpr uint32_t DDSCAPS_TEXTURE = 0x1000;

    DDSHeader header = {};
    header.size = sizeof(DDSHeader);
    header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH;
    header.height = height;
    header.width = width;
    header.pitchOrLinearSize = width * 4;
    header.mipMapCount = 1;
    header.ddspf.size = sizeof(DDSPixelFormat);
    header.ddspf.flags = DDPF_RGB | DDPF_ALPHAPIXELS;
    header.ddspf.rgbBitCount = 32;
    header.ddspf.rBitMask = 0x000000ff;
    header.ddspf.gBitMask = 0x0000ff00;
    header.ddspf.bBitMask = 0x00ff0000;
    header.ddspf.aBitMask = 0xff000000;
    header.caps = DDSCAPS_TEXTURE;

    out.write(reinterpret_cast<const char*>(&kDDSMagic), sizeof(kDDSMagic));
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * sizeof(uint32_t)));
    if (!out)
    {
        core::Log::Warnf("Failed while writing starter DDS texture: %s", filePath);
        return false;
    }

    core::Log::Infof("Starter DDS texture written: %s", filePath);
    return true;
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
