#pragma once
// =============================================================================
// render/shader_utils.h — Shader Compilation Utilities
// =============================================================================
// Supports two compilation backends:
//   - D3DCompile (d3dcompiler_47.dll) for SM 5.1 shaders (vs_5_1, ps_5_1)
//   - DXC (dxcompiler.dll) for SM 6.0+ and ray tracing libraries (lib_6_3+)
// DXC is loaded on demand when a lib_6_* target is requested.
// =============================================================================

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace render
{

// Compile a shader from an HLSL file. Returns null blob on failure (errors logged).
// Automatically selects D3DCompile or DXC based on the target profile.
// For DXR shaders, pass entryPoint="" and target="lib_6_3" (or higher).
ComPtr<ID3DBlob> CompileShaderFromFile(
    const wchar_t* filePath,
    const char* entryPoint,
    const char* target,         // e.g., "vs_5_1", "ps_5_1", "lib_6_3"
    const D3D_SHADER_MACRO* defines = nullptr);

// Compile a shader from source string.
ComPtr<ID3DBlob> CompileShaderFromSource(
    const char* source,
    size_t sourceSize,
    const char* sourceName,
    const char* entryPoint,
    const char* target,
    const D3D_SHADER_MACRO* defines = nullptr);

} // namespace render
