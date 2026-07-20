#ifndef DAWNING_GPU_DRAW_RECORDS_HLSLI
#define DAWNING_GPU_DRAW_RECORDS_HLSLI

// Keep these byte-identical with ObjectData and MaterialData in
// src/render/gpu_draw_records.h. Root SRVs carry no StructureByteStride, so the
// runtime cannot validate these layouts for us. Both raster stages include this
// file to remove shader-to-shader drift; the draw probe validates the remaining
// CPU-to-HLSL boundary on a real GPU during raster smoke tests.
struct ObjectData
{
    float4 worldRow0;
    float4 worldRow1;
    float4 worldRow2;
    float4 normalRow0;
    float4 normalRow1;
    float4 normalRow2;
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
    uint2  materialPad;
};

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
    return DawningHashWord(hash, asuint(record.normalRow2.w));
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
    hash = DawningHashWord(hash, record.materialPad.x);
    return DawningHashWord(hash, record.materialPad.y);
}

#endif
