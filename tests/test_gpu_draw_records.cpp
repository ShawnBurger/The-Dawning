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
    CHECK_EQ(sizeof(render::ObjectData), size_t(112));
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
    // The witness stamp starts exactly where the second matrix block ends.
    CHECK_EQ(offsetof(render::ObjectData, recordId),
             offsetof(render::ObjectData, normalMatrix) + size_t(48));
    CHECK_EQ(offsetof(render::ObjectData, recordId), size_t(96));
    CHECK_EQ(offsetof(render::ObjectData, recordPad), size_t(100));
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
    render::WriteObjectRecord(rec, world, nrm, 0u);

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
    render::WriteObjectRecord(rec, world, nrm, 0u);

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
    const core::Mat4x4 world = AwkwardWorld();
    const core::Mat4x4 normal = core::Mat4x4::InverseTranspose3x3(world);

    render::ObjectData rec;
    std::memset(&rec, 0xCD, sizeof(rec));   // poison, as a stale frame would
    render::WriteObjectRecord(rec, world, normal, 0u);

    CHECK_EQ(rec.normalMatrix[3], 0.0f);
    CHECK_EQ(rec.normalMatrix[7], 0.0f);
    CHECK_EQ(rec.normalMatrix[11], 0.0f);

    // EVERY BYTE of the record must have been written - none of the poison may
    // survive into a record the GPU reads, because these buffers are
    // persistently mapped and reused across frames without clearing.
    //
    // Done by writing the SAME record twice from two DIFFERENT poison patterns
    // and comparing the results, rather than by testing each float against the
    // float spelling of one pattern. The literal spelling is how this assertion
    // was dead before: it read -4.31740643e+08f, while 0xCDCDCDCD is actually
    // -4.31602080e+08f, so every comparison was trivially true and the loop
    // could not fail for any input. Differencing two poisons needs no magic
    // constant at all - an unwritten byte keeps whichever poison it started
    // with, so the two records disagree there and memcmp catches it. It also
    // catches unwritten bytes inside PADDING, which a float-by-float walk over
    // the named members cannot reach.
    render::ObjectData other;
    std::memset(&other, 0xAB, sizeof(other));
    render::WriteObjectRecord(other, world, normal, 0u);

    CHECK(std::memcmp(&rec, &other, sizeof(render::ObjectData)) == 0);
}

// The world rows DO carry translation in w, which is why the record needs no
// separate translation field and why the implicit fourth row of the transpose
// (always 0,0,0,1 for the affine matrices reaching the renderer) is not stored.
TEST_CASE(GpuDrawRecords_WorldRowsCarryTranslationInW)
{
    const core::Mat4x4 t = core::Mat4x4::Translation({ 7.0f, -3.0f, 0.25f });

    render::ObjectData rec = {};
    render::WriteObjectRecord(rec, t, core::Mat4x4::InverseTranspose3x3(t), 0u);

    CHECK_APPROX_EPS(rec.world[3],  7.0f,  1e-6);
    CHECK_APPROX_EPS(rec.world[7], -3.0f,  1e-6);
    CHECK_APPROX_EPS(rec.world[11], 0.25f, 1e-6);
}

// =============================================================================
// The draw-index witness stamp
// =============================================================================

// recordId must be the index the caller allocated, verbatim. This is the CPU
// half of the GPU-side assertion described in gpu_draw_records.h: the vertex
// shaders write back the recordId they LOADED, so a stamp that did not match
// the slot would make the witness lie in the safe direction - it would report
// correct indexing while the GPU read the wrong element.
//
// Distinct indices must produce distinct stamps and identical geometry, which
// is what makes the witness a check on INDEXING rather than on the transforms:
// the two records below differ in exactly one field.
TEST_CASE(GpuDrawRecords_RecordIdStampsTheAllocatedIndex)
{
    const core::Mat4x4 world  = AwkwardWorld();
    const core::Mat4x4 normal = core::Mat4x4::InverseTranspose3x3(world);

    render::ObjectData a = {};
    render::ObjectData b = {};
    render::WriteObjectRecord(a, world, normal, 0u);
    render::WriteObjectRecord(b, world, normal, 41u);

    CHECK_EQ(a.recordId, 0u);
    CHECK_EQ(b.recordId, 41u);

    // Same geometry, so everything before the stamp is byte-identical.
    CHECK(std::memcmp(&a, &b, offsetof(render::ObjectData, recordId)) == 0);

    // The pad is written, not inherited. gpu_draw_records.h explains why the
    // three uints are spelled out on both sides rather than left implicit.
    CHECK_EQ(b.recordPad[0], 0u);
    CHECK_EQ(b.recordPad[1], 0u);
    CHECK_EQ(b.recordPad[2], 0u);
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

// The floors keep the buffers non-empty in an empty scene, so Renderer::Init can
// create them before any frame and Renderer::BeginFrame's root SRV binds are
// never skipped. They do NOT exist to avoid a null root SRV in DrawSky - the sky
// shaders read neither structured buffer; see gpu_draw_records.h.
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
// Drives render::DrawRecordCursor - the SAME object Renderer::DrawMesh,
// DrawMeshShadow, BeginShadowPass and BeginShadowCascade allocate through. The
// previous version of this test incremented a local `uint32_t cursor` in its own
// for-loops and asserted on that, so it exercised no production code and could
// not fail for any renderer change whatsoever.
TEST_CASE(GpuDrawRecords_PassRangesAreDisjoint)
{
    const uint32_t drawsPerPass = 97;
    const uint32_t capacity = render::RequiredObjectCapacity(drawsPerPass);

    render::DrawRecordCursor cursor;
    cursor.Reset();

    // ---- shadow pass, as App::RenderFrame drives it -------------------------
    cursor.BeginPass();
    const uint32_t shadowFirst = cursor.Next();
    for (uint32_t i = 0; i < drawsPerPass; ++i)
    {
        uint32_t index = 0xFFFFFFFFu;
        CHECK(cursor.Allocate(capacity, index));
        CHECK_EQ(index, shadowFirst + i);
    }
    const uint32_t shadowRecords = cursor.RecordsThisPass();

    // ---- main pass ----------------------------------------------------------
    cursor.BeginPass();
    const uint32_t mainFirst = cursor.Next();
    for (uint32_t i = 0; i < drawsPerPass; ++i)
    {
        uint32_t index = 0xFFFFFFFFu;
        CHECK(cursor.Allocate(capacity, index));
        CHECK_EQ(index, mainFirst + i);
    }
    const uint32_t mainRecords = cursor.RecordsThisPass();

    CHECK_EQ(shadowFirst, 0u);
    CHECK_EQ(mainFirst, drawsPerPass);          // the ranges do not overlap
    CHECK(mainFirst >= shadowFirst + shadowRecords);
    CHECK_EQ(cursor.Next(), 2 * drawsPerPass);

    // The parity invariant the smoke harness asserts on.
    CHECK_EQ(shadowRecords, mainRecords);

    // And it all fits, which is the whole reason for the 2x.
    CHECK(cursor.Next() <= capacity);
}

// The four shadow cascades share ONE set of object records. App::RenderFrame
// walks the casters once per cascade, so without the rewind the object buffer
// would need (kShadowCascadeCount + 1) x maxDraws rather than 2x, and
// RequiredObjectCapacity would be wrong by a factor of 2.5.
//
// This is what makes the cascade count free in per-object memory: the records
// are cascade-independent because the light view-projection moved out of
// ObjectData and into CBPerPass.
TEST_CASE(GpuDrawRecords_ShadowCascadesReuseOneRecordRange)
{
    const uint32_t drawsPerPass = 97;
    const uint32_t cascades     = 4;
    const uint32_t capacity     = render::RequiredObjectCapacity(drawsPerPass);

    render::DrawRecordCursor cursor;
    cursor.Reset();
    cursor.BeginPass();

    for (uint32_t c = 0; c < cascades; ++c)
    {
        cursor.Rewind();                 // what BeginShadowCascade does
        for (uint32_t i = 0; i < drawsPerPass; ++i)
        {
            uint32_t index = 0xFFFFFFFFu;
            CHECK(cursor.Allocate(capacity, index));
            // Every cascade hands out the SAME indices as the first.
            CHECK_EQ(index, i);
        }
    }

    // Four cascades cost exactly what one does.
    CHECK_EQ(cursor.Next(), drawsPerPass);
    CHECK_EQ(cursor.RecordsThisPass(), drawsPerPass);

    // And the main pass still starts immediately after, so 2x holds.
    cursor.BeginPass();
    uint32_t mainIndex = 0xFFFFFFFFu;
    CHECK(cursor.Allocate(capacity, mainIndex));
    CHECK_EQ(mainIndex, drawsPerPass);
}

// Allocate must REPORT exhaustion, not clamp. A clamped index is a valid index
// into another draw's record: the scene renders with wrong transforms and
// nothing logs. The renderer relies on the false return to skip the draw.
TEST_CASE(GpuDrawRecords_CursorReportsExhaustionRatherThanClamping)
{
    render::DrawRecordCursor cursor;
    cursor.Reset();
    cursor.BeginPass();

    const uint32_t capacity = 3;
    for (uint32_t i = 0; i < capacity; ++i)
    {
        uint32_t index = 0xFFFFFFFFu;
        CHECK(cursor.Allocate(capacity, index));
        CHECK_EQ(index, i);
    }

    // One past the end: fails, and does NOT advance - so a caller that skips the
    // draw leaves the cursor exactly where a caller that never asked would.
    uint32_t overflow = 0xFFFFFFFFu;
    CHECK(!cursor.Allocate(capacity, overflow));
    CHECK_EQ(overflow, 0xFFFFFFFFu);        // untouched, not clamped to capacity-1
    CHECK_EQ(cursor.Next(), capacity);

    // Still refuses on a second attempt rather than wrapping.
    CHECK(!cursor.Allocate(capacity, overflow));
    CHECK_EQ(cursor.Next(), capacity);
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

    // Grow, then shrink the demand back down. The capacity must HOLD: shrinking
    // would mean destroying a buffer frames in flight may still be reading.
    uint32_t cap = render::kMinObjectCapacity;
    cap = render::GrownCapacity(cap, render::RequiredObjectCapacity(11));
    const uint32_t afterSmall = cap;
    cap = render::GrownCapacity(cap, render::RequiredObjectCapacity(400));
    const uint32_t afterGrowth = cap;
    cap = render::GrownCapacity(cap, render::RequiredObjectCapacity(11));

    CHECK(afterGrowth > afterSmall);
    CHECK_EQ(cap, afterGrowth);   // did not shrink back
}

// The FIRST allocation is exact - no headroom - and that is what makes a buffer
// created at a capacity floor actually sit at that floor.
//
// This is not a stylistic detail. Renderer::Init creates both per-draw buffers
// at kMinObjectCapacity / kMinMaterialCapacity so that the first real frame has
// to grow past them, which is what puts the reallocate-and-DeferredRelease swap
// on the DEFAULT verification path instead of behind -ForceGrow. If the initial
// allocation silently picked up kCapacityHeadroom, a 4-element floor would
// become a 68-element buffer, the demo scene's 34 records would fit inside it,
// and the whole arrangement would quietly revert to "never grows".
TEST_CASE(GpuDrawRecords_FirstAllocationIsExactSoTheFloorsAreReal)
{
    CHECK_EQ(render::GrownCapacity(0, render::kMinObjectCapacity),
             render::kMinObjectCapacity);
    CHECK_EQ(render::GrownCapacity(0, render::kMinMaterialCapacity),
             render::kMinMaterialCapacity);
    CHECK_EQ(render::GrownCapacity(0, 256), 256u);

    // Headroom returns the moment there is an existing allocation to grow FROM,
    // because from then on it is amortising a real reallocation.
    CHECK_EQ(render::GrownCapacity(render::kMinObjectCapacity, 256),
             256u + render::kCapacityHeadroom);
}

// THE COVERAGE INVARIANT, asserted here so it cannot silently rot: the smoke
// scene MUST cross the growth path, twice.
//
// This test replaces one that asserted the exact opposite. Its predecessor was
// an "honest note" recording that the smoke scene's peak of 91 renderables sat
// under a 256-element floor, so EnsureFrameStructuredBuffer early-outed on every
// frame of every smoke run and its D3D12 reallocate-and-DeferredRelease branch -
// the only code in this design that can use-after-free with three frames in
// flight - was exercised by neither smoke mode. Documenting that gap was better
// than not knowing about it, but the gap was the bug. The floors are now smaller
// than any real scene and these are the numbers that keep them that way.
TEST_CASE(GpuDrawRecords_SmokeSceneForcesTwoGrowsByConstruction)
{
    // What the running demo scene actually is, and what the growth churn adds.
    // Cross-checked against the smoke harness: shadow_records / main_records are
    // 17, and App::ApplySmokeRTMutationStress creates 80 RTGrowth_* entities at
    // frame 8 and destroys them at frame 16.
    const uint32_t demoRenderables   = 17;
    const uint32_t grownRenderables  = demoRenderables + 80;   // 97

    // ---- Renderer::Init: exactly the floors, no headroom -------------------
    uint32_t objectCap   = render::GrownCapacity(0, render::kMinObjectCapacity);
    uint32_t materialCap = render::GrownCapacity(0, render::kMinMaterialCapacity);
    CHECK_EQ(objectCap,   render::kMinObjectCapacity);
    CHECK_EQ(materialCap, render::kMinMaterialCapacity);

    // ---- Frame one: the demo scene must NOT fit ----------------------------
    // This is the assertion that keeps the floors honest. If someone raises them
    // back over the scene size, this is what fails.
    const uint32_t frameOneObjects   = render::RequiredObjectCapacity(demoRenderables);
    const uint32_t frameOneMaterials = render::RequiredMaterialCapacity(demoRenderables);
    CHECK(frameOneObjects   > objectCap);
    CHECK(frameOneMaterials > materialCap);

    objectCap   = render::GrownCapacity(objectCap,   frameOneObjects);
    materialCap = render::GrownCapacity(materialCap, frameOneMaterials);
    CHECK_EQ(objectCap,   frameOneObjects   + render::kCapacityHeadroom);
    CHECK_EQ(materialCap, frameOneMaterials + render::kCapacityHeadroom);

    // ---- Frame eight: +80 entities must not fit either ----------------------
    // The SECOND grow, and the one that matters: it happens mid-run with earlier
    // frames still executing, which is the only condition under which a missing
    // deferred-release fence can be observed. kCapacityHeadroom is 64 and the
    // churn adds 80 renderables = 160 object records, so the headroom cannot
    // absorb it - but that is arithmetic worth pinning rather than assuming,
    // since a larger headroom or a smaller churn would silently swallow it.
    const uint32_t grownObjects   = render::RequiredObjectCapacity(grownRenderables);
    const uint32_t grownMaterials = render::RequiredMaterialCapacity(grownRenderables);
    CHECK(grownObjects   > objectCap);
    CHECK(grownMaterials > materialCap);

    // ---- And back down: the capacity holds when the churn is destroyed ------
    const uint32_t afterGrowthObjects = render::GrownCapacity(objectCap, grownObjects);
    CHECK_EQ(render::GrownCapacity(afterGrowthObjects, frameOneObjects),
             afterGrowthObjects);
}

// A floor of 4 object records is 2 draws, so ANY scene with 3 or more
// renderables outgrows it on frame one. Stated as a test rather than a comment
// because "the first frame must grow" is the property the whole default-path
// coverage rests on, and it is a property of the NUMBER, not of the demo scene.
TEST_CASE(GpuDrawRecords_FloorsAreBelowAnyRealScene)
{
    CHECK(render::kMinObjectCapacity   > 0u);   // the buffer must always exist
    CHECK(render::kMinMaterialCapacity > 0u);

    // Three renderables is already too many for the floors to hold.
    CHECK(render::RequiredObjectCapacity(3)   > render::kMinObjectCapacity);
    CHECK(render::RequiredMaterialCapacity(3) > render::kMinMaterialCapacity);
}
