#pragma once
// =============================================================================
// core/shadow_cascades.h - Cascaded shadow map geometry
// =============================================================================
// The cascade math for the raster directional shadow pass, deliberately GPU-free
// so it compiles into BOTH TheDawningV3 and TheDawningTests. The unit tests
// therefore call the SHIPPED function rather than a paraphrase of it.
//
// That is not cosmetic. tests/test_math.cpp has two cases that read as
// shadow-matrix tests but rebuild LookAt*OrthoLH inline with their own hardcoded
// 24.0f/120.0f - they cover core::Mat4x4, not the renderer, and would keep
// passing if the light-matrix code were deleted outright. A fourth mirror would
// pass while cascades were broken, which is worse than no test at all.
//
// This header includes ONLY types.h and <cstdint>: no d3d12, no renderer.h.
//
// -----------------------------------------------------------------------------
// The construction, and the one property everything rests on
// -----------------------------------------------------------------------------
// Every cascade is an orthographic box centred on the CAMERA-RELATIVE ORIGIN and
// oriented purely by the light direction. Because Mat4x4::LookAt builds
// zAxis = (target - eye).Normalized() with eye = lightDir*d and target = origin,
// both xAxis and yAxis come out perpendicular to lightDir and satisfy
// xAxis.Dot(eye) == yAxis.Dot(eye) == 0. A camera-relative point p therefore has
// light-space x = xAxis.Dot(p), so |x| <= |p| EXACTLY, and likewise for y.
//
// Selecting a cascade by RADIUS with split[c] = extent[c] / kShadowCascadeMargin
// then makes "the selected point lies inside the selected cascade's XY footprint"
// a theorem rather than a hope. The cascade-boundary-fallthrough bug class is
// structurally impossible, which is why the shader needs no XY guard and no
// trial-project-and-retry.
//
// Radial (not view-depth) selection is also what makes the footprints rotation
// invariant: turning the camera does not move a single shadow texel, so unlike
// frustum-fitted cascades these need no stabilisation against rotation at all.
// =============================================================================

#include "types.h"

#include <cstdint>

namespace core
{

// Number of cascades. Changing this is NOT a one-line edit: the split, texel and
// fade tables are uploaded as float4 and declared as float4 in basic_ps.hlsl, so
// another count means repacking them as float4[(N+3)/4] and indexing [i/4][i%4].
// A static_assert in render/renderer.h pins this.
inline constexpr uint32_t kShadowCascadeCount = 4;

// Shadow map resolution, per cascade slice. Lives here rather than on class
// Renderer so the tests and the shader define can both reach it.
inline constexpr uint32_t kShadowMapSize = 2048;

// The legacy single-cascade extent and depth range. Retained as named constants
// because cascade 0 is deliberately bit-identical to them (see
// ShadowCascadeDepthRange), which is what makes the near-field diff exactly zero
// and gives any visual regression a clean bisect anchor.
inline constexpr float kShadowExtent     = 24.0f;
inline constexpr float kShadowDepthRange = 120.0f;

// Per-cascade orthographic HALF-extent, in world units.
//
// A fixed table, NOT a fit to the camera frustum. This is the single biggest
// simplification in the design and it is deliberate:
//   1. Radial selection already makes the footprints rotation-invariant, so
//      there is nothing about camera orientation left to fit.
//   2. The camera's far plane is 10000, and fitting to it produces absurd
//      cascades - a shadow-specific reach has to be chosen by hand regardless.
//      This table IS the tunable.
//   3. A frustum fit needs near/far/fov/aspect plumbed into the shadow path,
//      which is the exact edit that historically becomes "let me also centre the
//      cascades on camera.Position()" - narrowing a Vec3d into a matrix while
//      every world matrix around it stays camera-relative. Keeping the fit
//      argument-free means the only camera datum that ever enters is the Vec3d
//      used for the snap residual below.
inline constexpr float kShadowCascadeExtent[kShadowCascadeCount] =
    { 24.0f, 65.0f, 175.0f, 470.0f };

// Selection margin. The theorem needs slack for the snap displacement
// (<= sqrt(2) texels), the shader's normal offset (<= 4 texels at grazing) and
// the 3x3 PCF half-width (1.5 texels) - about 7 texels. Worst-case projected
// |x|/E works out at 1/1.05 + (sqrt(2)*2 + 4*2 + 1.5*2)/2048 = 0.9591, leaving
// ~42 texels of slack. An order of magnitude of headroom.
inline constexpr float kShadowCascadeMargin = 1.05f;

// Fraction of a cascade's split radius at which its outer blend band begins.
// Uploaded from the start so the constant-buffer byte layout never churns;
// consumed only if the optional cross-cascade blend is ever enabled.
inline constexpr float kShadowCascadeFadeFraction = 0.85f;

// Outer radius at which cascade c stops being selected, in world units.
float ShadowCascadeSplitRadius(uint32_t cascade);

// World units covered by one shadow-map texel in cascade c.
float ShadowCascadeTexelWorld(uint32_t cascade);

// Depth of cascade c's orthographic slab, in world units.
//
// Expressed against the two legacy constants rather than a fresh magic 5.0, so
// cascade 0 lands on exactly 120.0 with no clamp. Fixing this ratio is also what
// makes ONE shared shadow PSO sufficient across four cascades whose texel sizes
// differ by 19.6x: for a float depth format D3D12 scales RasterizerState.DepthBias
// by the exponent of the primitive's maximum z, so the constant term is roughly a
// fixed NDC offset - i.e. a world bias proportional to depthRange, hence to
// extent, hence to texelWorld. Bias-per-texel stays constant across cascades and
// the existing DepthBias=2000 / SlopeScaledDepthBias=2.5 carry over unchanged.
float ShadowCascadeDepthRange(uint32_t cascade);

// Inner edge of cascade c's outer blend band.
float ShadowCascadeFadeLo(uint32_t cascade);

// CPU twin of the shader's cascade selector. Written with the IDENTICAL
// descending-`<` structure as the three `if` statements in basic_ps.hlsl, so the
// unit tests constrain the shader's arithmetic rather than a paraphrase of it.
// Beyond the last split this still returns the last cascade; the sampler's
// OPAQUE_WHITE border then reads as lit, with no second spherical cutoff to
// disagree with the square footprint boundary.
uint32_t SelectShadowCascade(float radialDistance);

// Build cascade c's camera-relative light view-projection matrix.
//
// RULE 1 (CLAUDE.md) - THE SANCTIONED EXCEPTION, AND ITS EXACT SHAPE.
// cameraPosition enters this function in DOUBLE and is used for exactly one
// thing: to quantise the cascade centre onto a lattice fixed to the WORLD
// origin. No absolute world position is ever narrowed to float. The only
// narrowing performed is of a RESIDUAL - a sum of two terms each bounded by one
// texel (<= 0.46 world units for the largest cascade) - so the rule holds by
// construction rather than by anyone remembering to subtract the camera first.
//
// Reconstructing an absolute snapped position from the light basis and narrowing
// THAT is the classic violation, and it is not a theoretical one: at ~1e7 units
// the basis's own orthonormality error alone costs ~1 world unit, about 55
// texels. The residual form is immune because the basis error multiplies a
// quantity bounded by half a unit, not by 1e7.
//
// Why the snap is needed at all: under camera-relative rendering the camera is
// pinned at the origin and the world slides beneath it, so quantising a
// camera-relative coordinate quantises nothing - the grid moves with the thing
// being quantised, and the snap becomes a no-op that still looks like it works.
// The lattice has to be anchored to the world origin, which is why the absolute
// camera position is the input.
//
// Depth is deliberately NOT snapped: the same matrix produces both the stored
// depth and the reference depth, so a continuous shift of the depth mapping
// cancels exactly in the comparison and snapping it would only spend precision.
Mat4x4 BuildShadowCascadeMatrix(const Vec3f& lightDir,
                                uint32_t cascade,
                                const Vec3d& cameraPosition);

} // namespace core
