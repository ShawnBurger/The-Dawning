#pragma once
// =============================================================================
// render/gpu_draw_records.h — GPU-facing per-draw record layouts
// =============================================================================
// The structs the raster path writes into its per-draw StructuredBuffers, plus
// the pure arithmetic that fills and sizes them.
//
// DELIBERATELY FREE OF D3D12. renderer.h pulls in d3d12_device.h, which the
// CPU-only unit-test target cannot link. Splitting the layouts and the record
// packing out here is what lets tests/test_gpu_draw_records.cpp assert the
// sizes, the offsets, the transpose and the capacity arithmetic without a
// device - and those asserts are the ONLY guard that exists, see below.
//
// WHY THESE ARE NOT CONSTANT BUFFERS ANY MORE. They were CBPerObject (b0) and
// CBMaterial (b2), one 256-byte slice of a fixed 256 KB ring each, per draw.
// With the shadow pass uploading a second per-object copy that came to 768
// bytes per shadowed entity per frame, making the ring the scene's scaling
// surface at a hard ceiling near 341 entities. They now live in growable,
// kFrameCount-instanced upload buffers indexed by a per-draw root constant.
//
// NOTHING VALIDATES THESE LAYOUTS AT RUNTIME. A root SRV is a bare GPU virtual
// address: SetGraphicsRootShaderResourceView takes no StructureByteStride and
// there is no SRV descriptor for the debug layer to cross-check against the
// shader's computed struct size. A one-byte disagreement between these structs
// and their HLSL counterparts reads shifted garbage for every element after the
// first, silently. The static_asserts below and the unit tests beside them are
// the whole of the defence.
// =============================================================================

#include "../core/types.h"
#include <cstdint>
#include <cstddef>   // offsetof

namespace render
{

// =============================================================================
// ObjectData — per-draw transform record, 96 bytes
// =============================================================================
// Matches `struct ObjectData` in shaders/basic_vs.hlsl and shaders/shadow_vs.hlsl.
//
// EXPLICIT float4 ROWS, NOT float4x4, and that is load bearing rather than
// stylistic. render/mesh.h records that StructuredBuffer elements do NOT get
// the column-major reinterpretation cbuffer matrices do, and it records it
// because this tree already shipped exactly that bug once, on
// RTInstanceData::normalMatrix. The engine's transforms are correct only
// because a row-major CPU memcpy and HLSL's column-major cbuffer packing
// transpose each other and cancel (core/types.h). Moving a matrix out of a
// cbuffer removes one half of that cancellation, so relying on a
// `column_major` annotation here would be betting the whole engine's geometry
// on a modifier being honoured in a context an in-tree note says it does not
// apply to. Spelling the rows out removes the question.
//
// Both blocks are stored TRANSPOSED - row r holds column r of the source
// Mat4x4 - so the shader produces component r with a plain
// dot(row[r], float4(pos, 1)). Same idiom scene.cpp already uses for
// RTInstanceData::normalMatrix.
//
// The view-projection is NOT here. It lives in CBPerPass, uploaded once per
// pass. That is what lets the shadow pass and the main pass share one record
// shape, and what makes each future shadow cascade cost 256 flat bytes rather
// than another 96 bytes per entity per cascade.
struct ObjectData
{
    // Camera-relative object-to-world. Every world matrix reaching the renderer
    // is already camera-relative (CLAUDE.md RULE 1) and affine, so the implicit
    // fourth row of the transpose is always (0,0,0,1) and is not stored.
    float world[12];          //  0..47
    // InverseTranspose3x3 of that matrix, same transposed shape, w component 0.
    // Normals are covectors: reusing `world` skews them under non-uniform scale.
    float normalMatrix[12];   // 48..95
};
static_assert(sizeof(ObjectData) == 96,
              "ObjectData must match struct ObjectData in basic_vs.hlsl / shadow_vs.hlsl");
static_assert(offsetof(ObjectData, world) == 0, "ObjectData.world must sit at byte 0");
static_assert(offsetof(ObjectData, normalMatrix) == 48,
              "ObjectData.normalMatrix must sit at byte 48");

// =============================================================================
// MaterialData — per-draw material record, 80 bytes
// =============================================================================
// Matches `struct MaterialData` in shaders/basic_ps.hlsl. Field for field the
// old CBMaterial: every 16-byte block is exactly filled and no member straddles
// a boundary, so cbuffer packing and StructuredBuffer tight packing produce
// identical offsets. The bytes do not move; only the register does. The two pad
// members are what keep the size a multiple of 16.
//
// The DXR path already ships this exact 80-byte shape as a StructuredBuffer
// element (shaders/path_trace.hlsl), so this is a proven layout rather than a
// new one.
struct MaterialData
{
    float albedo[4];                //  0..15
    float roughness;                // 16
    float metallic;                 // 20
    uint32_t useAlbedoTexture;      // 24
    uint32_t useNormalTexture;      // 28
    uint32_t albedoTextureIndex;    // 32
    uint32_t normalTextureIndex;    // 36
    uint32_t useOrmTexture;         // 40
    uint32_t ormTextureIndex;       // 44
    float emissive[3];              // 48..59
    float emissiveStrength;         // 60
    uint32_t useEmissiveTexture;    // 64
    uint32_t emissiveTextureIndex;  // 68
    uint32_t materialPad0;          // 72
    uint32_t materialPad1;          // 76
};
static_assert(sizeof(MaterialData) == 80,
              "MaterialData must match struct MaterialData in basic_ps.hlsl");
static_assert(offsetof(MaterialData, albedo) == 0, "");
static_assert(offsetof(MaterialData, roughness) == 16, "");
static_assert(offsetof(MaterialData, metallic) == 20, "");
static_assert(offsetof(MaterialData, useAlbedoTexture) == 24, "");
static_assert(offsetof(MaterialData, useNormalTexture) == 28, "");
static_assert(offsetof(MaterialData, albedoTextureIndex) == 32, "");
static_assert(offsetof(MaterialData, normalTextureIndex) == 36, "");
static_assert(offsetof(MaterialData, useOrmTexture) == 40, "");
static_assert(offsetof(MaterialData, ormTextureIndex) == 44, "");
static_assert(offsetof(MaterialData, emissive) == 48, "");
static_assert(offsetof(MaterialData, emissiveStrength) == 60, "");
static_assert(offsetof(MaterialData, useEmissiveTexture) == 64, "");
static_assert(offsetof(MaterialData, emissiveTextureIndex) == 68, "");
static_assert(offsetof(MaterialData, materialPad0) == 72, "");
static_assert(offsetof(MaterialData, materialPad1) == 76, "");

// GPU smoke evidence written by the vertex shaders. The probe hashes the
// fields the GPU actually loaded, so it catches both a bad per-draw index and a
// CPU/HLSL layout disagreement. A marker of index + 1 distinguishes an unwritten
// slot from a legitimate zero hash.
struct DrawProbeRecord
{
    uint32_t objectHash;
    uint32_t objectMarker;
    uint32_t materialHash;
    uint32_t materialMarker;
};
static_assert(sizeof(DrawProbeRecord) == 16,
              "DrawProbeRecord must match the RWByteAddressBuffer layout in the raster shaders");

uint32_t HashObjectData(const ObjectData& record);
uint32_t HashMaterialData(const MaterialData& record);

// =============================================================================
// CBPerPass — per-PASS view-projection, 64 bytes
// =============================================================================
// Matches `cbuffer CBPerPass : register(b4)`. Uploaded twice per frame: the
// light matrix in BeginShadowPass, the camera matrix in BeginFrame.
//
// Deliberately still a cbuffer float4x4 on the shader side. That keeps today's
// proven row-major-memcpy / column-major-packing / mul(M, v) cancellation,
// which every frame the engine has ever rendered verifies. All the new layout
// risk is concentrated in ObjectData, where explicit rows remove it.
struct CBPerPass
{
    float viewProj[16];
};
static_assert(sizeof(CBPerPass) == 64,
              "CBPerPass must match cbuffer CBPerPass (b4) in basic_vs.hlsl / shadow_vs.hlsl");

// =============================================================================
// Record packing and buffer sizing — pure arithmetic, unit tested
// =============================================================================

// Writes the transposed affine world matrix and its inverse-transpose into a
// record. `world` and `normalMatrix` are core::Mat4x4 in the engine's
// row-major-storage / row-vector-semantics convention (core/types.h), so
// column r of the source becomes row r of the record and the shader's
// dot(row[r], p) reproduces the row-vector product p * M componentwise.
void WriteObjectRecord(ObjectData& out,
                       const core::Mat4x4& world,
                       const core::Mat4x4& normalMatrix);

// Element counts the two per-draw buffers need for a frame drawing at most
// `maxDraws` meshes.
//
// Objects need TWICE the draw count because the shadow pass and the main pass
// each append their own record: the shadow pass occupies [0, N) and the main
// pass [N, 2N). Those ranges are DISJOINT rather than shared, deliberately.
// Sharing was tempting - both passes walk the same pool in the same order with
// identical filters, so draw i is the same entity in both - but that parity is
// a coincidence of two loops, enforced nowhere. If shadow casting ever gains a
// filter the main pass lacks (frustum culling, a castsShadow flag, an LOD cut),
// shared indices would silently desynchronise and every object would render
// with a different entity's transform. With disjoint ranges the two loops
// matter only for SIZING, where being wrong costs an oversized allocation
// rather than wrong geometry.
//
// The floors keep the GPU virtual address non-null in an empty scene, which
// removes any need to reason about binding a null root SRV during DrawSky.
constexpr uint32_t kMinObjectCapacity   = 256;
constexpr uint32_t kMinMaterialCapacity = 128;

uint32_t RequiredObjectCapacity(uint32_t maxDraws);
uint32_t RequiredMaterialCapacity(uint32_t maxDraws);

// Capacity a FrameStructuredBuffer grows to when `elementCount` is requested.
// Never shrinks: shrinking would mean destroying a buffer a recorded command
// list may still reference. Growth is geometric (at least 1.5x) so a steadily
// expanding scene does not recreate all kFrameCount resources at a fixed rate.
constexpr uint32_t kCapacityHeadroom = 64;
uint32_t GrownCapacity(uint32_t currentCapacity, uint32_t elementCount);

// =============================================================================
// DrawRecordCursor — the object buffer's allocation state machine
// =============================================================================
// Renderer OWNS one of these and calls nothing else to allocate object record
// indices. It lives here, away from D3D12, for one reason: the alternative was
// a cursor spelled as a bare `uint32_t m_objectCursor` incremented inline at
// four sites in renderer.cpp, which cannot be tested without a device - and the
// test that claimed to cover it instead re-implemented the increments inside
// the test body and asserted on its own local variable. That test could not
// fail for any change to the renderer, because it never called the renderer.
//
// The logic is no longer trivial enough to leave untested. It has three
// distinct operations and a cascade rewind:
//
//   Reset()      once per frame, above every pass
//   BeginPass()  marks where the current pass's range starts
//   Rewind()     returns to that mark - every shadow cascade does this, because
//                the casters are walked once per cascade and the records are
//                cascade-independent (the light matrix lives in CBPerPass)
//   Allocate()   hands out one index, or fails when capacity is exhausted
//
// Allocate REPORTS failure rather than clamping. A clamped index is a valid
// index into someone else's record, which renders wrong geometry silently; a
// reported failure lets the caller skip the draw and log.
class DrawRecordCursor
{
public:
    // Start of a frame. Both the cursor and the pass mark return to zero.
    void Reset() { m_next = 0; m_passStart = 0; }

    // Start of a pass. The mark is where Rewind() returns to.
    void BeginPass() { m_passStart = m_next; }

    // Start of a cascade within the current pass: re-issue the same indices the
    // previous cascade used. Safe only because the records are byte-identical
    // across cascades - see Renderer::BeginShadowCascade.
    void Rewind() { m_next = m_passStart; }

    // Hands out the next index. Returns false and leaves the cursor untouched
    // when `capacity` is exhausted, so a caller that skips the draw stays
    // consistent with one that does not.
    bool Allocate(uint32_t capacity, uint32_t& outIndex)
    {
        if (m_next >= capacity) return false;
        outIndex = m_next;
        ++m_next;
        return true;
    }

    uint32_t Next() const { return m_next; }
    uint32_t PassStart() const { return m_passStart; }
    // Records the current pass has issued, i.e. the width of its range.
    uint32_t RecordsThisPass() const { return m_next - m_passStart; }

private:
    uint32_t m_next      = 0;
    uint32_t m_passStart = 0;
};

} // namespace render
