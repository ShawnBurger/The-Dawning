#pragma once
// =============================================================================
// render/mesh.h — Mesh & Geometry Buffer Management
// =============================================================================
// Vertex format: position (float3) + normal (float3) + color (float4) + uv (float2)
// = 48 bytes per vertex. Uses classic input layout for Layer 2.
//
// Static meshes are uploaded to DEFAULT heap (VRAM) via staging upload buffer.
// Dynamic meshes would use persistently-mapped upload buffers (future).
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include "../core/types.h"

using Microsoft::WRL::ComPtr;

namespace render
{

// =============================================================================
// Vertex format
// =============================================================================
struct Vertex
{
    core::Vec3f position;
    core::Vec3f normal;
    core::Color color;
    core::Vec2f uv;
};

// Per-triangle normals consumed by the DXR closest-hit shader. Stored as
// float4-aligned rows to keep the CPU/GPU structured-buffer layout simple.
struct RTTriangleNormalData
{
    float n0[4];
    float n1[4];
    float n2[4];
};

// Per-triangle UVs consumed by the DXR closest-hit shader. These mirror the
// triangle normal metadata so ray hits can reconstruct material texture coords.
struct RTTriangleUVData
{
    float uv0[4];
    float uv1[4];
    float uv2[4];
};

// Per-triangle positions consumed by the DXR closest-hit shader for tangent
// frame reconstruction when sampling normal maps.
struct RTTrianglePositionData
{
    float p0[4];
    float p1[4];
    float p2[4];
};

// Per-TLAS-instance metadata consumed by the DXR closest-hit shader.
struct RTInstanceData
{
    uint32_t triangleNormalOffset = 0;
    uint32_t triangleUVOffset = 0;
    uint32_t trianglePositionOffset = 0;
    uint32_t pad = 0;

    // Object-to-world normal matrix, i.e. InverseTranspose3x3 of the world matrix,
    // stored TRANSPOSED as three float4 rows so the shader can produce component i
    // with a plain dot(row[i].xyz, objectNormal). Normals are covectors: the
    // closest-hit shader previously used ObjectToWorld3x4() directly, which skews
    // them under non-uniform scale while the raster path (renderer.cpp ->
    // basic_vs.hlsl) transforms them correctly, so the two paths disagreed.
    //
    // Explicit float4 rows rather than a matrix type: StructuredBuffer elements do
    // not get the column-major reinterpretation that cbuffer matrices do, and
    // spelling out the rows keeps the layout unambiguous on both sides.
    // Identity by default.
    float normalMatrix[12] = { 1.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f, 0.0f,
                               0.0f, 0.0f, 1.0f, 0.0f };
};
static_assert(sizeof(RTInstanceData) == 64,
              "RTInstanceData must match struct InstanceData in shaders/path_trace.hlsl");

// Input layout description matching the Vertex struct
const D3D12_INPUT_ELEMENT_DESC kVertexLayout[] =
{
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,        0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
constexpr uint32_t kVertexLayoutCount = _countof(kVertexLayout);

// =============================================================================
// Mesh — owns GPU vertex and index buffers
// =============================================================================
struct Mesh
{
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    D3D12_INDEX_BUFFER_VIEW  ibView = {};
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT; // R16_UINT or R32_UINT
    std::vector<RTTriangleNormalData> rtTriangleNormals;
    std::vector<RTTriangleUVData> rtTriangleUVs;
    std::vector<RTTrianglePositionData> rtTrianglePositions;

    bool IsValid() const { return vertexBuffer && indexBuffer && indexCount > 0; }
};

// =============================================================================
// Mesh creation — uploads CPU data to GPU VRAM via staging buffers
// =============================================================================

// Create a mesh from vertex and index data. Uses the provided command list
// for the upload copy. Caller must execute the command list and wait for
// GPU completion before the returned staging buffers go out of scope.
//
// Usage:
//   ComPtr<ID3D12Resource> vbUpload, ibUpload;
//   Mesh mesh = CreateMesh(device, cmdList, verts, indices, vbUpload, ibUpload);
//   // Execute command list, wait for GPU...
//   // vbUpload and ibUpload can now be released
//
Mesh CreateMesh(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const Vertex* vertices, uint32_t vertexCount,
    const uint16_t* indices, uint32_t indexCount,
    ComPtr<ID3D12Resource>& outVertexUpload,
    ComPtr<ID3D12Resource>& outIndexUpload);

// 32-bit index variant — for meshes with >65535 vertices
Mesh CreateMesh32(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const Vertex* vertices, uint32_t vertexCount,
    const uint32_t* indices, uint32_t indexCount,
    ComPtr<ID3D12Resource>& outVertexUpload,
    ComPtr<ID3D12Resource>& outIndexUpload);

// =============================================================================
// Primitive generators — return CPU-side vertex/index data
// =============================================================================

struct MeshData
{
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
};

// Unit cube centered at origin, side length 1.0
// CW winding (front-facing in LH with FrontCounterClockwise=FALSE)
MeshData GenerateCube(const core::Color& color = core::Color::White());

// XZ plane centered at origin.
//
// uvTiles is how many times the texture repeats across the plane. It defaults to
// 1 for backward compatibility, but that couples texel density to plane SIZE:
// enlarging a plane stretches its texture rather than covering more of it. Pass
// width / <desired world units per tile> to keep density constant instead. The
// static sampler wraps, so values above 1 tile correctly.
MeshData GeneratePlane(float width, float depth, uint32_t subdivX, uint32_t subdivZ,
                       const core::Color& color = core::Color::White(),
                       float uvTiles = 1.0f);

// UV sphere centered at origin
MeshData GenerateSphere(float radius, uint32_t slices, uint32_t stacks,
                        const core::Color& color = core::Color::White());

} // namespace render
