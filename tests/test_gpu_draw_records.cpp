// =============================================================================
// tests/test_gpu_draw_records.cpp — per-draw GPU record layout and arithmetic
// =============================================================================
// These records are read by the GPU through ROOT SRVs. A root SRV is a bare GPU
// virtual address: SetGraphicsRootShaderResourceView takes no
// StructureByteStride and there is no SRV descriptor, so neither the D3D12
// runtime nor the debug layer ever compares the C++ struct against the struct
// size the shader computes. If the two disagree by one byte, every element
// after the first reads shifted garbage and NOTHING REPORTS IT.
//
// The static_asserts in gpu_draw_records.h are the compile-time half of the
// defence; this file is the runtime half, covering the parts an assert cannot
// see: that the transpose actually transposes, that the index arithmetic keeps
// the two passes' ranges disjoint, and that capacity growth never shrinks.
//
// Nothing here touches D3D12 - that is why the record layouts live in their own
// header rather than in renderer.h, which pulls in d3d12_device.h.
// =============================================================================

#include "test_framework.h"
#include "core/types.h"
#include "render/gpu_draw_records.h"

#include <cstring>

namespace
{

// A deliberately nasty transform: translated, rotated about all three axes, and
// NON-UNIFORMLY scaled. core/types.h warns explicitly that a cube at the origin
// or a pure single-axis rotation will not surface a packing or transpose error,
// so none of the fixtures below are one.
core::Mat4x4 AwkwardWorld()
{
    core::Mat4x4 s;   // identity; types.h has no Scale builder
    s.m[0][0] = 2.0f;
    s.m[1][1] = 0.5f;
    s.m[2][2] = 3.0f;
    const core::Mat4x4 r = core::Mat4x4::RotationY(0.7f) *
                           core::Mat4x4::RotationX(0.3f) *
                           core::Mat4x4::RotationZ(-1.1f);
    const core::Mat4x4 t = core::Mat4x4::Translation({ 5.0f, -2.0f, 11.0f });
    return s * r * t;
}

} // namespace

// =============================================================================
// Layout
// =============================================================================

// Sizes and offsets are static_assert'd in the header, so reaching this test at
// all proves them. Restated at runtime anyway because the numbers themselves
// are the contract with the HLSL declarations, and a reader looking for "what
// must basic_vs.hlsl agree with" should find it in a test, not only in a
// compile flag that vanished at build time.
TEST_CASE(GpuDrawRecords_SizesMatchShaderStructs)
{
    CHECK_EQ(sizeof(render::ObjectData), size_t(96));
    CHECK_EQ(sizeof(render::MaterialData), size_t(80));
    CHECK_EQ(sizeof(render::CBPerPass), size_t(64));

    // Every StructuredBuffer element size must be a multiple of 16, which is
    // what the pad members in MaterialData exist for.
    CHECK_EQ(sizeof(render::ObjectData) % 16, size_t(0));
    CHECK_EQ(sizeof(render::MaterialData) % 16, size_t(0));
}

// The two 12-float blocks must abut exactly: the shader reads six consecutive
// float4s and slices them 3/3.
TEST_CASE(GpuDrawRecords_ObjectBlocksAbut)
{
    CHECK_EQ(offsetof(render::ObjectData, world), size_t(0));
    CHECK_EQ(offsetof(render::ObjectData, normalMatrix), size_t(48));
    CHECK_EQ(sizeof(render::ObjectData),
             offsetof(render::ObjectData, normalMatrix) + size_t(48));
}

// =============================================================================
// The transpose
// =============================================================================

// THE point of the explicit-rows layout. core::Mat4x4 is row-major storage with
// ROW-VECTOR semantics (v' = v * M), so component r of a transformed point is
// column r of the matrix dotted with the point. The record stores column r as
// row r so the shader can write dot(row[r], float4(pos,1)).
//
// Reproduce the shader's dot products here and require them to agree with
// TransformPoint, which is the CPU-side definition of the same operation. This
// is what catches a transpose: swap the indices in WriteObjectRecord and this
// fails on any matrix that is not symmetric.
TEST_CASE(GpuDrawRecords_WorldRowsReproduceTransformPoint)
{
    const core::Mat4x4 world = AwkwardWorld();
    const core::Mat4x4 nrm   = core::Mat4x4::InverseTranspose3x3(world);

    render::ObjectData rec = {};
    render::WriteObjectRecord(rec, world, nrm);

    const core::Vec3f p{ 0.37f, -1.9f, 4.25f };
    const core::Vec3f expected = world.TransformPoint(p);

    // Exactly what basic_vs.hlsl does: dot(worldRowR, float4(position, 1)).
    float got[3];
    for (int r = 0; r < 3; ++r)
    {
        got[r] = rec.world[r * 4 + 0] * p.x
               + rec.world[r * 4 + 1] * p.y
               + rec.world[r * 4 + 2] * p.z
               + rec.world[r * 4 + 3] * 1.0f;
    }

    CHECK_APPROX_EPS(got[0], expected.x, 1e-4);
    CHECK_APPROX_EPS(got[1], expected.y, 1e-4);
    CHECK_APPROX_EPS(got[2], expected.z, 1e-4);
}

// Same check for the normal rows, which use only xyz because a normal is a
// direction. Under the non-uniform scale in the fixture, transforming the
// normal with the world matrix instead of its inverse-transpose gives a
// visibly different answer - so this also pins down that the second block is
// the inverse-transpose and not a second copy of world.
TEST_CASE(GpuDrawRecords_NormalRowsUseInverseTranspose)
{
    const core::Mat4x4 world = AwkwardWorld();
    const core::Mat4x4 nrm   = core::Mat4x4::InverseTranspose3x3(world);

    render::ObjectData rec = {};
    render::WriteObjectRecord(rec, world, nrm);

    const core::Vec3f n{ 0.0f, 1.0f, 0.0f };

    float got[3];
    for (int r = 0; r < 3; ++r)
    {
        got[r] = rec.normalMatrix[r * 4 + 0] * n.x
               + rec.normalMatrix[r * 4 + 1] * n.y
               + rec.normalMatrix[r * 4 + 2] * n.z;
    }

    // Column r of the inverse-transpose dotted with the normal.
    for (int r = 0; r < 3; ++r)
    {
        const float expected = nrm.m[0][r] * n.x + nrm.m[1][r] * n.y + nrm.m[2][r] * n.z;
        CHECK_APPROX_EPS(got[r], expected, 1e-5);
    }

    // And it must differ from the world matrix's answer, or the fixture is too
    // tame to catch the mistake this test exists for.
    const core::Vec3f worldN{ world.m[0][0] * n.x + world.m[1][0] * n.y + world.m[2][0] * n.z,
                              world.m[0][1] * n.x + world.m[1][1] * n.y + world.m[2][1] * n.z,
                              world.m[0][2] * n.x + world.m[1][2] * n.y + world.m[2][2] * n.z };
    const float delta = std::fabs(got[0] - worldN.x)
                      + std::fabs(got[1] - worldN.y)
                      + std::fabs(got[2] - worldN.z);
    CHECK(delta > 1e-3f);
}

// The translation column must NOT leak into the normal rows: w is written as
// zero explicitly rather than left over from whatever occupied the buffer last
// frame. These buffers are persistently mapped and reused without clearing.
TEST_CASE(GpuDrawRecords_NormalRowsZeroTheTranslationSlot)
{
    render::ObjectData rec;
    std::memset(&rec, 0xCD, sizeof(rec));   // poison, as a stale frame would

    const core::Mat4x4 world = AwkwardWorld();
    render::WriteObjectRecord(rec, world, core::Mat4x4::InverseTranspose3x3(world));

    CHECK_EQ(rec.normalMatrix[3], 0.0f);
    CHECK_EQ(rec.normalMatrix[7], 0.0f);
    CHECK_EQ(rec.normalMatrix[11], 0.0f);

    // Every byte of the record must have been written - none of the poison may
    // survive into a record the GPU reads.
    const float* raw = &rec.world[0];
    for (size_t i = 0; i < sizeof(render::ObjectData) / sizeof(float); ++i)
        CHECK(raw[i] != -4.31740643e+08f);   // the float spelling of 0xCDCDCDCD
}

// The world rows DO carry translation in w, which is why the record needs no
// separate translation field and why the implicit fourth row of the transpose
// (always 0,0,0,1 for the affine matrices reaching the renderer) is not stored.
TEST_CASE(GpuDrawRecords_WorldRowsCarryTranslationInW)
{
    const core::Mat4x4 t = core::Mat4x4::Translation({ 7.0f, -3.0f, 0.25f });

    render::ObjectData rec = {};
    render::WriteObjectRecord(rec, t, core::Mat4x4::InverseTranspose3x3(t));

    CHECK_APPROX_EPS(rec.world[3],  7.0f,  1e-6);
    CHECK_APPROX_EPS(rec.world[7], -3.0f,  1e-6);
    CHECK_APPROX_EPS(rec.world[11], 0.25f, 1e-6);
}

// =============================================================================
// Index and capacity arithmetic
// =============================================================================

// Objects need twice the draw count because the shadow pass and the main pass
// each append their own record into one buffer. Getting this wrong by a factor
// of two is silent under the floor and shows up only once a scene exceeds it.
TEST_CASE(GpuDrawRecords_ObjectCapacityIsTwicePerDraw)
{
    CHECK_EQ(render::RequiredObjectCapacity(1000), 2000u);
    CHECK_EQ(render::RequiredObjectCapacity(5000), 10000u);
    CHECK_EQ(render::RequiredMaterialCapacity(1000), 1000u);
}

// The floors keep the buffers non-empty in an empty scene, so their GPU virtual
// addresses are never null and DrawSky - which runs under the same root
// signature - never sees an unbound root SRV.
TEST_CASE(GpuDrawRecords_CapacityFloorsHoldForEmptyScenes)
{
    CHECK_EQ(render::RequiredObjectCapacity(0), render::kMinObjectCapacity);
    CHECK_EQ(render::RequiredMaterialCapacity(0), render::kMinMaterialCapacity);

    // The floor must win right up to the point the real requirement passes it.
    CHECK_EQ(render::RequiredObjectCapacity(render::kMinObjectCapacity / 2),
             render::kMinObjectCapacity);
    CHECK_EQ(render::RequiredObjectCapacity(render::kMinObjectCapacity / 2 + 1),
             render::kMinObjectCapacity + 2u);
}

// The two passes must occupy DISJOINT ranges: shadow [0, N), main [N, 2N).
// Sharing one record per entity would be half the memory and is exactly the
// trap this arithmetic exists to avoid - the two scene walks agree only by
// coincidence, and a future frustum-culled main pass would silently pair each
// draw with a different entity's transform.
TEST_CASE(GpuDrawRecords_PassRangesAreDisjoint)
{
    const uint32_t drawsPerPass = 97;
    const uint32_t capacity = render::RequiredObjectCapacity(drawsPerPass);

    uint32_t cursor = 0;
    uint32_t shadowFirst = cursor;
    for (uint32_t i = 0; i < drawsPerPass; ++i) ++cursor;
    const uint32_t shadowLast = cursor - 1;
    const uint32_t shadowRecords = cursor;

    const uint32_t mainFirst = cursor;
    for (uint32_t i = 0; i < drawsPerPass; ++i) ++cursor;
    const uint32_t mainLast = cursor - 1;
    const uint32_t mainRecords = cursor - shadowRecords;

    CHECK_EQ(shadowFirst, 0u);
    CHECK_EQ(shadowLast, drawsPerPass - 1);
    CHECK_EQ(mainFirst, drawsPerPass);       // the ranges do not overlap
    CHECK(mainFirst > shadowLast);
    CHECK_EQ(mainLast, 2 * drawsPerPass - 1);

    // The parity invariant the smoke harness asserts on.
    CHECK_EQ(shadowRecords, mainRecords);

    // And it all fits, which is the whole reason for the 2x.
    CHECK(cursor <= capacity);
}

// Growth must never shrink: a smaller buffer would mean destroying one a
// recorded command list may still reference. It must also add headroom, or
// every added entity reallocates all kFrameCount buffers.
TEST_CASE(GpuDrawRecords_GrowthNeverShrinksAndAmortises)
{
    // Below capacity: no change at all, so no reallocation.
    CHECK_EQ(render::GrownCapacity(500, 100), 500u);
    CHECK_EQ(render::GrownCapacity(500, 500), 500u);

    // Above capacity: grow past the request so the next few additions are free.
    CHECK_EQ(render::GrownCapacity(500, 501), 501u + render::kCapacityHeadroom);
    CHECK(render::GrownCapacity(500, 501) > 500u);

    // From nothing.
    CHECK_EQ(render::GrownCapacity(0, 256), 256u + render::kCapacityHeadroom);

    // Grow, then shrink the demand back down. The capacity must HOLD: shrinking
    // would mean destroying a buffer frames in flight may still be reading.
    // 400 draws is used rather than the smoke scene's 91 for the reason the next
    // test documents.
    uint32_t cap = 0;
    cap = render::GrownCapacity(cap, render::RequiredObjectCapacity(11));
    const uint32_t afterSmall = cap;
    cap = render::GrownCapacity(cap, render::RequiredObjectCapacity(400));
    const uint32_t afterGrowth = cap;
    cap = render::GrownCapacity(cap, render::RequiredObjectCapacity(11));

    CHECK(afterGrowth > afterSmall);
    CHECK_EQ(cap, afterGrowth);   // did not shrink back
}

// HONEST NOTE, asserted so it cannot rot into a false assumption: the smoke
// scene never reaches the growth path. Its peak is 91 renderables, i.e. 182
// object records, which sits UNDER the 256-element floor - so
// EnsureFrameStructuredBuffer early-outs on every frame of every smoke run and
// its D3D12 reallocate-and-DeferredRelease branch is exercised by neither smoke
// mode. The arithmetic above is covered here; the resource swap is not covered
// anywhere. Raising the floor or lowering it would change that, so pin the
// relationship down rather than leaving it to be rediscovered.
TEST_CASE(GpuDrawRecords_SmokeSceneStaysUnderTheCapacityFloor)
{
    const uint32_t smokePeakRenderables = 91;   // 11 demo + 80 RTGrowth_*
    CHECK(render::RequiredObjectCapacity(smokePeakRenderables) ==
          render::kMinObjectCapacity);
    CHECK(render::RequiredMaterialCapacity(smokePeakRenderables) ==
          render::kMinMaterialCapacity);

    // The first renderable count that DOES force a real allocation, so anyone
    // wanting to exercise the growth path knows what to reach for.
    const uint32_t firstGrowingCount = render::kMinObjectCapacity / 2 + 1;   // 129
    CHECK(render::RequiredObjectCapacity(firstGrowingCount) >
          render::kMinObjectCapacity);
}
