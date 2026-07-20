// =============================================================================
// render/gpu_draw_records.cpp — per-draw record packing and buffer sizing
// =============================================================================
// See gpu_draw_records.h for why these layouts exist and why nothing validates
// them at runtime. No D3D12 here on purpose: the CPU-only test target links
// this file.
// =============================================================================

#include "gpu_draw_records.h"

#include <algorithm>

namespace render
{

void WriteObjectRecord(ObjectData& out,
                       const core::Mat4x4& world,
                       const core::Mat4x4& normalMatrix,
                       uint32_t recordIndex)
{
    // TRANSPOSE on the way in. core::Mat4x4 is row-major storage with ROW-VECTOR
    // semantics: TransformPoint computes v * M, so component r of the result is
    // sum_k v[k] * m[k][r] - that is, column r of the matrix dotted with the
    // vector. Storing column r as row r of the record lets the shader write
    // dot(row[r], float4(pos, 1)) and get exactly that, with no matrix type and
    // therefore no packing convention to get wrong.
    //
    // The fourth component picks up m[3][r], i.e. the translation, which is why
    // the record needs no separate translation field and why the implicit
    // fourth row of the transpose - always (0,0,0,1) for the affine matrices
    // that reach the renderer - does not have to be stored.
    for (int r = 0; r < 3; ++r)
    {
        out.world[r * 4 + 0] = world.m[0][r];
        out.world[r * 4 + 1] = world.m[1][r];
        out.world[r * 4 + 2] = world.m[2][r];
        out.world[r * 4 + 3] = world.m[3][r];
    }

    // Same transpose, but the translation column is dropped: a normal is a
    // direction, and InverseTranspose3x3 is a 3x3 quantity. Writing 0 rather
    // than leaving the slot alone keeps the record fully defined, which matters
    // for a persistently mapped buffer that is reused across frames without
    // clearing - stale bytes there are how a later shader change becomes a
    // heisenbug.
    for (int r = 0; r < 3; ++r)
    {
        out.normalMatrix[r * 4 + 0] = normalMatrix.m[0][r];
        out.normalMatrix[r * 4 + 1] = normalMatrix.m[1][r];
        out.normalMatrix[r * 4 + 2] = normalMatrix.m[2][r];
        out.normalMatrix[r * 4 + 3] = 0.0f;
    }

    // The witness stamp. Deliberately the LAST thing written and deliberately
    // not derived from anything else in the record: it is the element's own
    // index, so the vertex shader reading it back through the root constant
    // proves the two agree. See "the draw-index witness" in the header.
    out.recordId      = recordIndex;
    out.recordPad[0]  = 0;
    out.recordPad[1]  = 0;
    out.recordPad[2]  = 0;
}

uint32_t RequiredObjectCapacity(uint32_t maxDraws)
{
    return (std::max)(kMinObjectCapacity, 2u * maxDraws);
}

uint32_t RequiredMaterialCapacity(uint32_t maxDraws)
{
    return (std::max)(kMinMaterialCapacity, maxDraws);
}

uint32_t GrownCapacity(uint32_t currentCapacity, uint32_t elementCount)
{
    if (elementCount <= currentCapacity) return currentCapacity;
    // First allocation: exact. See the header - headroom amortises REPEATED
    // growth, and there is nothing to amortise against on the way out of zero.
    // Sizing this exactly is also what keeps a buffer created at a capacity
    // floor actually AT that floor, so the first real frame has to grow.
    if (currentCapacity == 0) return elementCount;
    return elementCount + kCapacityHeadroom;
}

} // namespace render
