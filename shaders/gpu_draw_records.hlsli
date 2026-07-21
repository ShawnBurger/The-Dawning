#ifndef DAWNING_GPU_DRAW_RECORDS_HLSLI
#define DAWNING_GPU_DRAW_RECORDS_HLSLI

// Keep these byte-identical with ObjectData and MaterialData in
// src/render/gpu_draw_records.h. Root SRVs carry no StructureByteStride, so the
// runtime cannot validate these layouts for us. All three raster stages include
// this file to remove shader-to-shader drift; the draw probe validates the
// remaining CPU-to-HLSL boundary on a real GPU during raster smoke tests.
//
// THE PROBE IS SPLIT ACROSS STAGES ON PURPOSE. The object words are written by
// the vertex shaders, which is where objectBuffer is consumed; the material
// words are written by the pixel shader, which is where materialBuffer is
// consumed. Witnessing a buffer from a stage that is not the one that uses it
// witnesses the probe's own read - it stays perfectly green while the consuming
// stage reads element 0.
struct ObjectData
{
    float4 worldRow0;      //  0..15
    float4 worldRow1;      // 16..31
    float4 worldRow2;      // 32..47
    float4 normalRow0;     // 48..63
    float4 normalRow1;     // 64..79
    float4 normalRow2;     // 80..95
    uint   recordId;       // 96..99   the element's own index, stamped by the CPU
    uint3  recordPad;      // 100..111 explicit, so the stride is 112 by construction
};

struct MaterialData
{
    float4 albedo;
    float  roughness;
    float  metallic;
    uint   useAlbedoTexture;
    uint   useNormalTexture;
    uint   albedoTextureIndex;
    uint   normalTextureIndex;
    uint   useOrmTexture;
    uint   ormTextureIndex;
    float3 emissive;
    float  emissiveStrength;
    uint   useEmissiveTexture;
    uint   emissiveTextureIndex;
    uint   recordId;       // 72..75   the element's own index, stamped by the CPU
    uint   materialPad;    // 76..79
};

// FNV-1a over every 32-bit word of the record, in declaration order. Must stay
// in step with HashWords in src/render/gpu_draw_records.cpp, which hashes the
// CPU struct's bytes the same way - including the recordId and the padding, so
// the two sides only agree when the whole element agrees.
uint DawningHashWord(uint hash, uint word)
{
    return (hash ^ word) * 16777619u;
}

uint DawningHashObjectData(ObjectData record)
{
    uint hash = 2166136261u;
    hash = DawningHashWord(hash, asuint(record.worldRow0.x));
    hash = DawningHashWord(hash, asuint(record.worldRow0.y));
    hash = DawningHashWord(hash, asuint(record.worldRow0.z));
    hash = DawningHashWord(hash, asuint(record.worldRow0.w));
    hash = DawningHashWord(hash, asuint(record.worldRow1.x));
    hash = DawningHashWord(hash, asuint(record.worldRow1.y));
    hash = DawningHashWord(hash, asuint(record.worldRow1.z));
    hash = DawningHashWord(hash, asuint(record.worldRow1.w));
    hash = DawningHashWord(hash, asuint(record.worldRow2.x));
    hash = DawningHashWord(hash, asuint(record.worldRow2.y));
    hash = DawningHashWord(hash, asuint(record.worldRow2.z));
    hash = DawningHashWord(hash, asuint(record.worldRow2.w));
    hash = DawningHashWord(hash, asuint(record.normalRow0.x));
    hash = DawningHashWord(hash, asuint(record.normalRow0.y));
    hash = DawningHashWord(hash, asuint(record.normalRow0.z));
    hash = DawningHashWord(hash, asuint(record.normalRow0.w));
    hash = DawningHashWord(hash, asuint(record.normalRow1.x));
    hash = DawningHashWord(hash, asuint(record.normalRow1.y));
    hash = DawningHashWord(hash, asuint(record.normalRow1.z));
    hash = DawningHashWord(hash, asuint(record.normalRow1.w));
    hash = DawningHashWord(hash, asuint(record.normalRow2.x));
    hash = DawningHashWord(hash, asuint(record.normalRow2.y));
    hash = DawningHashWord(hash, asuint(record.normalRow2.z));
    hash = DawningHashWord(hash, asuint(record.normalRow2.w));
    hash = DawningHashWord(hash, record.recordId);
    hash = DawningHashWord(hash, record.recordPad.x);
    hash = DawningHashWord(hash, record.recordPad.y);
    return DawningHashWord(hash, record.recordPad.z);
}

uint DawningHashMaterialData(MaterialData record)
{
    uint hash = 2166136261u;
    hash = DawningHashWord(hash, asuint(record.albedo.x));
    hash = DawningHashWord(hash, asuint(record.albedo.y));
    hash = DawningHashWord(hash, asuint(record.albedo.z));
    hash = DawningHashWord(hash, asuint(record.albedo.w));
    hash = DawningHashWord(hash, asuint(record.roughness));
    hash = DawningHashWord(hash, asuint(record.metallic));
    hash = DawningHashWord(hash, record.useAlbedoTexture);
    hash = DawningHashWord(hash, record.useNormalTexture);
    hash = DawningHashWord(hash, record.albedoTextureIndex);
    hash = DawningHashWord(hash, record.normalTextureIndex);
    hash = DawningHashWord(hash, record.useOrmTexture);
    hash = DawningHashWord(hash, record.ormTextureIndex);
    hash = DawningHashWord(hash, asuint(record.emissive.x));
    hash = DawningHashWord(hash, asuint(record.emissive.y));
    hash = DawningHashWord(hash, asuint(record.emissive.z));
    hash = DawningHashWord(hash, asuint(record.emissiveStrength));
    hash = DawningHashWord(hash, record.useEmissiveTexture);
    hash = DawningHashWord(hash, record.emissiveTextureIndex);
    hash = DawningHashWord(hash, record.recordId);
    return DawningHashWord(hash, record.materialPad);
}

// One 48-byte slot per OBJECT record. The first 16 bytes are object/material
// hashes and markers; bytes 16..47 carry the cascade-blend witness. Both passes
// address it by the object index, and the two
// passes take disjoint ranges of the object buffer, so the shadow pass's slots
// and the main pass's slots never collide.
//
// InterlockedOr rather than a plain store. Every invocation of a draw computes
// the same value, and OR is idempotent for identical values, so the result is
// deterministic without needing to nominate one "first" vertex or pixel. When
// indexing breaks, invocations disagree and the OR produces a value that is
// neither operand - still a mismatch, which is all the reader needs.
#define DAWNING_PROBE_STRIDE 48u

void DawningWriteObjectProbe(RWByteAddressBuffer probe, uint objectIndex, ObjectData obj)
{
    const uint slot = objectIndex * DAWNING_PROBE_STRIDE;
    uint ignored;
    probe.InterlockedOr(slot + 0u, DawningHashObjectData(obj), ignored);
    // The marker comes from INSIDE the loaded record, never from objectIndex. A
    // marker built from the root constant would be a witness of itself.
    probe.InterlockedOr(slot + 4u, obj.recordId + 1u, ignored);
}

void DawningWriteMaterialProbe(RWByteAddressBuffer probe, uint objectIndex, MaterialData mat)
{
    const uint slot = objectIndex * DAWNING_PROBE_STRIDE;
    uint ignored;
    probe.InterlockedOr(slot + 8u, DawningHashMaterialData(mat), ignored);
    probe.InterlockedOr(slot + 12u, mat.recordId + 1u, ignored);
}

uint DawningQuantizeShadow(float value)
{
    return (uint)round(saturate(value) * 255.0f);
}

void DawningWriteShadowBlendProbe(RWByteAddressBuffer probe,
                                  uint objectIndex,
                                  uint pairBit,
                                  float expected,
                                  float output,
                                  float primary)
{
    const uint slot = objectIndex * DAWNING_PROBE_STRIDE;
    const uint expectedQ8 = DawningQuantizeShadow(expected);
    const uint outputQ8 = DawningQuantizeShadow(output);
    const uint primaryQ8 = DawningQuantizeShadow(primary);
    uint ignored;
    probe.InterlockedAdd(slot + 16u, 1u, ignored);
    probe.InterlockedOr(slot + 20u, pairBit, ignored);
    probe.InterlockedAdd(slot + 24u, expectedQ8, ignored);
    probe.InterlockedAdd(slot + 28u, outputQ8, ignored);
    probe.InterlockedAdd(slot + 32u, primaryQ8, ignored);
    probe.InterlockedAdd(slot + 36u,
                         (expectedQ8 > primaryQ8) ? (expectedQ8 - primaryQ8)
                                                  : (primaryQ8 - expectedQ8),
                         ignored);
    probe.InterlockedAdd(slot + 40u, (expectedQ8 == outputQ8) ? 0u : 1u, ignored);
}

#endif
