// =============================================================================
// render/mesh.cpp — Mesh Implementation
// =============================================================================

#include "mesh.h"
#include "../core/log.h"
#include <cstring>

namespace render
{

// =============================================================================
// Helper: create a committed buffer resource
// =============================================================================
static ComPtr<ID3D12Resource> CreateBuffer(
    ID3D12Device* device,
    uint64_t size,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES initialState,
    const wchar_t* name = nullptr)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = heapType;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = size;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, initialState, nullptr,
        IID_PPV_ARGS(&resource));

    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create buffer (size=%llu, heap=%d): 0x%08X",
                          static_cast<unsigned long long>(size), heapType, hr);
        return nullptr;
    }

    if (name) resource->SetName(name);
    return resource;
}

// =============================================================================
// Upload data from CPU to GPU via staging buffer
// =============================================================================
static void UploadBufferData(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* dest,
    ID3D12Resource* staging,
    const void* data,
    uint64_t dataSize,
    D3D12_RESOURCE_STATES afterState)
{
    // Map staging buffer and copy data
    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 }; // No CPU reads
    HRESULT hr = staging->Map(0, &readRange, &mapped);
    if (FAILED(hr) || !mapped)
    {
        core::Log::Errorf("UploadBufferData: Map() failed (0x%08X)", hr);
        return;
    }
    memcpy(mapped, data, static_cast<size_t>(dataSize));
    staging->Unmap(0, nullptr);

    // Copy staging → destination
    cmdList->CopyBufferRegion(dest, 0, staging, 0, dataSize);

    // Transition destination to final state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = dest;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = afterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

static core::Vec3f SafeNormal(const core::Vec3f& normal, const core::Vec3f& fallback)
{
    if (normal.LengthSq() > 1e-8f)
        return normal.Normalized();
    if (fallback.LengthSq() > 1e-8f)
        return fallback.Normalized();
    return { 0.0f, 1.0f, 0.0f };
}

static void StoreNormal(float out[4], const core::Vec3f& normal)
{
    out[0] = normal.x;
    out[1] = normal.y;
    out[2] = normal.z;
    out[3] = 0.0f;
}

template <typename IndexT>
static void BuildRTTriangleNormals(
    Mesh& mesh,
    const Vertex* vertices, uint32_t vertexCount,
    const IndexT* indices, uint32_t indexCount)
{
    mesh.rtTriangleNormals.clear();
    mesh.rtTriangleNormals.reserve(indexCount / 3);

    for (uint32_t i = 0; i + 2 < indexCount; i += 3)
    {
        uint32_t i0 = static_cast<uint32_t>(indices[i + 0]);
        uint32_t i1 = static_cast<uint32_t>(indices[i + 1]);
        uint32_t i2 = static_cast<uint32_t>(indices[i + 2]);

        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            continue;

        const Vertex& v0 = vertices[i0];
        const Vertex& v1 = vertices[i1];
        const Vertex& v2 = vertices[i2];

        core::Vec3f faceNormal = (v1.position - v0.position).Cross(v2.position - v0.position);
        RTTriangleNormalData tri = {};
        StoreNormal(tri.n0, SafeNormal(v0.normal, faceNormal));
        StoreNormal(tri.n1, SafeNormal(v1.normal, faceNormal));
        StoreNormal(tri.n2, SafeNormal(v2.normal, faceNormal));
        mesh.rtTriangleNormals.push_back(tri);
    }
}

// =============================================================================
// CreateMesh — upload vertices + indices to VRAM
// =============================================================================
Mesh CreateMesh(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const Vertex* vertices, uint32_t vertexCount,
    const uint16_t* indices, uint32_t indexCount,
    ComPtr<ID3D12Resource>& outVertexUpload,
    ComPtr<ID3D12Resource>& outIndexUpload)
{
    Mesh mesh;
    mesh.vertexCount = vertexCount;
    mesh.indexCount = indexCount;
    BuildRTTriangleNormals(mesh, vertices, vertexCount, indices, indexCount);

    uint64_t vbSize = static_cast<uint64_t>(vertexCount) * sizeof(Vertex);
    uint64_t ibSize = static_cast<uint64_t>(indexCount) * sizeof(uint16_t);

    // Create destination buffers in VRAM (DEFAULT heap)
    mesh.vertexBuffer = CreateBuffer(device, vbSize,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, L"MeshVertexBuffer");
    mesh.indexBuffer = CreateBuffer(device, ibSize,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, L"MeshIndexBuffer");

    if (!mesh.vertexBuffer || !mesh.indexBuffer)
    {
        core::Log::Error("Failed to create mesh GPU buffers");
        return mesh;
    }

    // Create staging buffers on UPLOAD heap
    outVertexUpload = CreateBuffer(device, vbSize,
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    outIndexUpload = CreateBuffer(device, ibSize,
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

    if (!outVertexUpload || !outIndexUpload)
    {
        core::Log::Error("Failed to create mesh staging upload buffers");
        return mesh;
    }

    // Upload vertex data
    UploadBufferData(cmdList, mesh.vertexBuffer.Get(), outVertexUpload.Get(),
                     vertices, vbSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // Upload index data
    UploadBufferData(cmdList, mesh.indexBuffer.Get(), outIndexUpload.Get(),
                     indices, ibSize, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    // Set up buffer views
    mesh.vbView.BufferLocation = mesh.vertexBuffer->GetGPUVirtualAddress();
    mesh.vbView.SizeInBytes    = static_cast<UINT>(vbSize);
    mesh.vbView.StrideInBytes  = sizeof(Vertex);

    mesh.ibView.BufferLocation = mesh.indexBuffer->GetGPUVirtualAddress();
    mesh.ibView.SizeInBytes    = static_cast<UINT>(ibSize);
    mesh.ibView.Format         = DXGI_FORMAT_R16_UINT;

    core::Log::Infof("Mesh created: %u verts, %u indices (VB=%llu bytes, IB=%llu bytes)",
                     vertexCount, indexCount,
                     static_cast<unsigned long long>(vbSize),
                     static_cast<unsigned long long>(ibSize));
    return mesh;
}

// =============================================================================
// CreateMesh32 — 32-bit index variant for large meshes
// =============================================================================
Mesh CreateMesh32(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const Vertex* vertices, uint32_t vertexCount,
    const uint32_t* indices, uint32_t indexCount,
    ComPtr<ID3D12Resource>& outVertexUpload,
    ComPtr<ID3D12Resource>& outIndexUpload)
{
    Mesh mesh;
    mesh.vertexCount = vertexCount;
    mesh.indexCount = indexCount;
    mesh.indexFormat = DXGI_FORMAT_R32_UINT;
    BuildRTTriangleNormals(mesh, vertices, vertexCount, indices, indexCount);

    uint64_t vbSize = static_cast<uint64_t>(vertexCount) * sizeof(Vertex);
    uint64_t ibSize = static_cast<uint64_t>(indexCount) * sizeof(uint32_t);

    mesh.vertexBuffer = CreateBuffer(device, vbSize,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, L"MeshVertexBuffer32");
    mesh.indexBuffer = CreateBuffer(device, ibSize,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, L"MeshIndexBuffer32");

    if (!mesh.vertexBuffer || !mesh.indexBuffer)
    {
        core::Log::Error("Failed to create mesh GPU buffers (32-bit)");
        return mesh;
    }

    outVertexUpload = CreateBuffer(device, vbSize,
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    outIndexUpload = CreateBuffer(device, ibSize,
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

    if (!outVertexUpload || !outIndexUpload)
    {
        core::Log::Error("Failed to create mesh staging upload buffers (32-bit)");
        return mesh;
    }

    UploadBufferData(cmdList, mesh.vertexBuffer.Get(), outVertexUpload.Get(),
                     vertices, vbSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    UploadBufferData(cmdList, mesh.indexBuffer.Get(), outIndexUpload.Get(),
                     indices, ibSize, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    mesh.vbView.BufferLocation = mesh.vertexBuffer->GetGPUVirtualAddress();
    mesh.vbView.SizeInBytes    = static_cast<UINT>(vbSize);
    mesh.vbView.StrideInBytes  = sizeof(Vertex);

    mesh.ibView.BufferLocation = mesh.indexBuffer->GetGPUVirtualAddress();
    mesh.ibView.SizeInBytes    = static_cast<UINT>(ibSize);
    mesh.ibView.Format         = DXGI_FORMAT_R32_UINT;

    core::Log::Infof("Mesh created (32-bit idx): %u verts, %u indices", vertexCount, indexCount);
    return mesh;
}

// =============================================================================
// GenerateCube — unit cube, CW winding for LH front-face
// =============================================================================
MeshData GenerateCube(const core::Color& color)
{
    MeshData data;

    // 24 vertices (4 per face, unique normals per face)
    // CW winding when viewed from outside = front face in LH
    const float h = 0.5f;

    auto addFace = [&](
        core::Vec3f p0, core::Vec3f p1, core::Vec3f p2, core::Vec3f p3,
        core::Vec3f normal)
    {
        uint16_t base = static_cast<uint16_t>(data.vertices.size());

        data.vertices.push_back({ p0, normal, color, { 0.0f, 1.0f } });
        data.vertices.push_back({ p1, normal, color, { 0.0f, 0.0f } });
        data.vertices.push_back({ p2, normal, color, { 1.0f, 0.0f } });
        data.vertices.push_back({ p3, normal, color, { 1.0f, 1.0f } });

        data.indices.push_back(base + 0);
        data.indices.push_back(base + 2);
        data.indices.push_back(base + 1);
        data.indices.push_back(base + 0);
        data.indices.push_back(base + 3);
        data.indices.push_back(base + 2);
    };

    // Front face (+Z) — looking at it from +Z, CW is: BL, TL, TR, BR
    addFace({ -h, -h,  h }, { -h,  h,  h }, {  h,  h,  h }, {  h, -h,  h },
            {  0,  0,  1 });

    // Back face (-Z) — looking at it from -Z, CW is: BR, TR, TL, BL
    addFace({  h, -h, -h }, {  h,  h, -h }, { -h,  h, -h }, { -h, -h, -h },
            {  0,  0, -1 });

    // Right face (+X)
    addFace({  h, -h,  h }, {  h,  h,  h }, {  h,  h, -h }, {  h, -h, -h },
            {  1,  0,  0 });

    // Left face (-X)
    addFace({ -h, -h, -h }, { -h,  h, -h }, { -h,  h,  h }, { -h, -h,  h },
            { -1,  0,  0 });

    // Top face (+Y)
    addFace({ -h,  h,  h }, { -h,  h, -h }, {  h,  h, -h }, {  h,  h,  h },
            {  0,  1,  0 });

    // Bottom face (-Y)
    addFace({ -h, -h, -h }, { -h, -h,  h }, {  h, -h,  h }, {  h, -h, -h },
            {  0, -1,  0 });

    return data;
}

// =============================================================================
// GeneratePlane — XZ plane
// =============================================================================
MeshData GeneratePlane(float width, float depth, uint32_t subdivX, uint32_t subdivZ,
                       const core::Color& color)
{
    MeshData data;

    // Clamp to sane ranges
    if (subdivX < 1) subdivX = 1;
    if (subdivZ < 1) subdivZ = 1;
    if (subdivX > 512) subdivX = 512;
    if (subdivZ > 512) subdivZ = 512;
    if (width <= 0.0f) width = 1.0f;
    if (depth <= 0.0f) depth = 1.0f;

    float halfW = width * 0.5f;
    float halfD = depth * 0.5f;
    float dx = width / static_cast<float>(subdivX);
    float dz = depth / static_cast<float>(subdivZ);

    // Generate vertices
    for (uint32_t z = 0; z <= subdivZ; z++)
    {
        for (uint32_t x = 0; x <= subdivX; x++)
        {
            float px = -halfW + static_cast<float>(x) * dx;
            float pz = -halfD + static_cast<float>(z) * dz;
            float u = static_cast<float>(x) / static_cast<float>(subdivX);
            float v = static_cast<float>(z) / static_cast<float>(subdivZ);

            data.vertices.push_back({
                { px, 0.0f, pz },
                { 0.0f, 1.0f, 0.0f },
                color,
                { u, v }
            });
        }
    }

    // Generate indices (CW winding for LH)
    uint32_t cols = subdivX + 1;
    for (uint32_t z = 0; z < subdivZ; z++)
    {
        for (uint32_t x = 0; x < subdivX; x++)
        {
            uint16_t tl = static_cast<uint16_t>(z * cols + x);
            uint16_t tr = static_cast<uint16_t>(tl + 1);
            uint16_t bl = static_cast<uint16_t>((z + 1) * cols + x);
            uint16_t br = static_cast<uint16_t>(bl + 1);

            // CW: tl, bl, br then tl, br, tr
            data.indices.push_back(tl);
            data.indices.push_back(bl);
            data.indices.push_back(br);
            data.indices.push_back(tl);
            data.indices.push_back(br);
            data.indices.push_back(tr);
        }
    }

    return data;
}

// =============================================================================
// GenerateSphere — UV sphere
// =============================================================================
MeshData GenerateSphere(float radius, uint32_t slices, uint32_t stacks,
                        const core::Color& color)
{
    MeshData data;

    // Clamp to sane ranges
    if (slices < 3) slices = 3;
    if (stacks < 2) stacks = 2;
    if (slices > 256) slices = 256;
    if (stacks > 128) stacks = 128;
    if (radius <= 0.0f) radius = 0.5f;

    const float pi = core::PI;

    // Generate vertices
    for (uint32_t stack = 0; stack <= stacks; stack++)
    {
        float phi = pi * static_cast<float>(stack) / static_cast<float>(stacks);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (uint32_t slice = 0; slice <= slices; slice++)
        {
            float theta = 2.0f * pi * static_cast<float>(slice) / static_cast<float>(slices);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            core::Vec3f normal(sinPhi * cosTheta, cosPhi, sinPhi * sinTheta);
            core::Vec3f pos = normal * radius;
            float u = static_cast<float>(slice) / static_cast<float>(slices);
            float v = static_cast<float>(stack) / static_cast<float>(stacks);

            data.vertices.push_back({ pos, normal, color, { u, v } });
        }
    }

    // Generate indices (CW winding for LH)
    uint32_t cols = slices + 1;
    for (uint32_t stack = 0; stack < stacks; stack++)
    {
        for (uint32_t slice = 0; slice < slices; slice++)
        {
            uint16_t tl = static_cast<uint16_t>(stack * cols + slice);
            uint16_t tr = static_cast<uint16_t>(tl + 1);
            uint16_t bl = static_cast<uint16_t>((stack + 1) * cols + slice);
            uint16_t br = static_cast<uint16_t>(bl + 1);

            data.indices.push_back(tl);
            data.indices.push_back(br);
            data.indices.push_back(bl);
            data.indices.push_back(tl);
            data.indices.push_back(tr);
            data.indices.push_back(br);
        }
    }

    return data;
}

} // namespace render
