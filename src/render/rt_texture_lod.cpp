// =============================================================================
// render/rt_texture_lod.cpp — Ray-cone texture LOD arithmetic (CPU side)
// =============================================================================
// See rt_texture_lod.h for which of these are shipped and which are mirrors of
// shaders/path_trace.hlsl. GPU-free by construction — no D3D12 include.
// =============================================================================

#include "rt_texture_lod.h"

#include <cmath>
#include <cstdint>

namespace render
{

namespace
{

// Twice the area of a triangle in UV space, i.e. the magnitude of the 2D cross
// product of its two edge vectors.
float TwiceUVArea(const float uv0[2], const float uv1[2], const float uv2[2])
{
    const float duv1x = uv1[0] - uv0[0];
    const float duv1y = uv1[1] - uv0[1];
    const float duv2x = uv2[0] - uv0[0];
    const float duv2y = uv2[1] - uv0[1];
    return std::fabs(duv1x * duv2y - duv1y * duv2x);
}

// Twice the area of a triangle in world space, i.e. the length of the 3D cross
// product of its two edge vectors.
float TwiceWorldArea(const float p0[3], const float p1[3], const float p2[3])
{
    const float e1x = p1[0] - p0[0];
    const float e1y = p1[1] - p0[1];
    const float e1z = p1[2] - p0[2];
    const float e2x = p2[0] - p0[0];
    const float e2y = p2[1] - p0[1];
    const float e2z = p2[2] - p0[2];

    const float cx = e1y * e2z - e1z * e2y;
    const float cy = e1z * e2x - e1x * e2z;
    const float cz = e1x * e2y - e1y * e2x;

    return std::sqrt(cx * cx + cy * cy + cz * cz);
}

} // namespace

float PrimaryRayConeSpreadAngle(float tanHalfFovY, uint32_t renderHeight)
{
    // A zero height would be a divide by zero and a zero spread would silently
    // pin every hit to mip 0 — the exact defect ray cones are here to remove.
    // Neither is reachable from a live swap chain, but the path tracer fills
    // this constant before it has proved the output textures exist.
    if (renderHeight == 0 || !(tanHalfFovY > 0.0f))
        return 0.0f;

    return 2.0f * tanHalfFovY / static_cast<float>(renderHeight);
}

float RayConeTriangleLodConstant(
    const float uv0[2], const float uv1[2], const float uv2[2],
    const float p0[3],  const float p1[3],  const float p2[3])
{
    const float uvArea    = TwiceUVArea(uv0, uv1, uv2);
    const float worldArea = TwiceWorldArea(p0, p1, p2);

    if (uvArea < 1e-12f || worldArea < 1e-12f)
        return kRayConeDegenerateLod;

    return 0.5f * std::log2(uvArea / worldArea);
}

float RayConeLodBase(float triangleLodConstant, float coneWidth, float normalDotRayDir)
{
    if (triangleLodConstant <= kRayConeDegenerateLod || coneWidth <= 0.0f)
        return kRayConeDegenerateLod;

    const float grazing = std::fmax(std::fabs(normalDotRayDir), kRayConeGrazingFloor);
    return triangleLodConstant + std::log2(coneWidth / grazing);
}

float RayConeTextureLod(float lodBase, uint32_t texWidth, uint32_t texHeight)
{
    const float texels = static_cast<float>(texWidth) * static_cast<float>(texHeight);
    if (!(texels > 0.0f))
        return 0.0f;

    return std::fmax(lodBase + 0.5f * std::log2(texels), 0.0f);
}

} // namespace render
