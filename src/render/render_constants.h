#pragma once
// =============================================================================
// render/render_constants.h — single source of truth for depth-pipeline config.
// =============================================================================
// The raster main pass uses REVERSED-Z on the D32_FLOAT depth buffer: the far
// plane maps to 0 and the near plane to 1 (paired with the reversed-Z infinite
// projection in core::Mat4x4::PerspectiveFovLH_ReverseZ). Three things must agree
// or depth silently breaks — the projection, the CLEAR value, and the depth TEST
// direction. The clear value and comparison func live here so the device layer
// (which owns the depth resource's optimized clear) and the renderer (which
// issues ClearDepthStencilView and builds the opaque PSO) cannot drift apart.
//
// The shadow / ortho pass is deliberately NOT reversed (it is OrthoLH and gains
// nothing); it keeps its own forward-Z clear (1.0) and LESS test locally.
// =============================================================================

#include <d3d12.h>

namespace render
{

// Reversed-Z: clear depth to the FAR value (0), and keep a fragment when its
// depth is GREATER than what is stored (nearer, since near=1 > far=0).
inline constexpr float                 kMainDepthClear    = 0.0f;
inline constexpr D3D12_COMPARISON_FUNC kMainDepthCompare  = D3D12_COMPARISON_FUNC_GREATER;

} // namespace render
