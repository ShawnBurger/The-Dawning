#pragma once
// =============================================================================
// render/rt_texture_lod.h — Ray-cone texture LOD arithmetic (CPU side)
// =============================================================================
// Akenine-Möller, Nilsson, Andersson, Barré-Brisebois, Toth, Karras,
// "Texture Level of Detail Strategies for Real-Time Ray Tracing",
// Ray Tracing Gems (2019), chapter 20.
//
// WHAT IS SHIPPED AND WHAT IS A MIRROR — read this before trusting the tests.
//
//   PrimaryRayConeSpreadAngle() is SHIPPED. render::PathTracer calls it every
//   frame to fill RTPerFrameConstants::primaryConeSpread, which the path tracer
//   reads as g_PrimaryConeSpread. Break it and the rendered image changes.
//
//   The other three functions are a deliberate CPU MIRROR of HLSL that runs
//   only on the GPU (shaders/path_trace.hlsl: RayConeTriangleLodConstant,
//   RayConeLodBase, RayConeTextureLod). They are not called by the engine.
//   A unit test over them pins the arithmetic — the area formulae, the signs,
//   the log base, the 0.5 factors, the degenerate guards — which is where this
//   class of bug actually lives: a sign flip or a log10 reads as "textures are
//   a bit blurry" rather than as a crash. It does NOT pin the shader. If
//   path_trace.hlsl is edited without editing this file, the tests stay green
//   and the image is wrong. Keep the two in step by hand.
//
// This translation unit is GPU-free on purpose (<cstdint> and <cmath> only), so
// TheDawningTests can compile it without dragging D3D12 into a CPU-only test
// binary. Do not add a D3D12 include here.
// =============================================================================

#include <cstdint>

namespace render
{

// Mirror of kRayConeDegenerateLod in shaders/path_trace.hlsl. Sentinel returned
// for a triangle with no usable UV or world area. Large and negative so that
// adding any real 0.5*log2(width*height) still clamps to mip 0, i.e. degenerate
// triangles fall back to sampling mip 0 exactly as they did before ray cones.
inline constexpr float kRayConeDegenerateLod = -64.0f;

// Mirror of kRayConeGrazingFloor. Floor on |dot(rayDir, normal)| so a surface
// seen edge-on does not send the LOD to infinity and collapse to a 1x1 mip.
// The value was calibrated against the raster capture rather than chosen; see
// the table at the declaration in shaders/path_trace.hlsl.
inline constexpr float kRayConeGrazingFloor = 0.1f;

// SHIPPED. Angular spread of the primary ray cone, in radians per unit distance,
// for a pinhole camera: one pixel of output HEIGHT subtends
// 2*tan(fovY/2)/renderHeight. Vertical rather than horizontal because the
// projection derives the horizontal extent from the vertical one times the
// aspect ratio, so height is the axis that is not already scaled.
float PrimaryRayConeSpreadAngle(float tanHalfFovY, uint32_t renderHeight);

// MIRROR. 0.5*log2(uvArea / worldArea) for one triangle.
// Both areas are computed as twice the true triangle area; the factor of two
// cancels in the ratio. UVs are in [0,1] texture space, positions in world
// space. Returns kRayConeDegenerateLod for a degenerate triangle.
float RayConeTriangleLodConstant(
    const float uv0[2], const float uv1[2], const float uv2[2],
    const float p0[3],  const float p1[3],  const float p2[3]);

// MIRROR. Texture-independent part of lambda:
//     triangleLodConstant + log2(coneWidth / max(|normalDotRayDir|, floor))
// Propagates the degenerate sentinel rather than producing a garbage mip.
float RayConeLodBase(float triangleLodConstant, float coneWidth, float normalDotRayDir);

// MIRROR. Per-sample-site term: lodBase + 0.5*log2(texWidth*texHeight), clamped
// at 0. The clamp is what turns the degenerate sentinel back into mip 0.
float RayConeTextureLod(float lodBase, uint32_t texWidth, uint32_t texHeight);

} // namespace render
