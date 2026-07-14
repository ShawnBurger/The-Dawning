// =============================================================================
// render/shader_utils.cpp — Shader Compilation Implementation
// =============================================================================
// Two compilation backends:
//   - D3DCompile for SM 5.1 (vs_5_1, ps_5_1)
//   - DXC (dxcompiler.dll) for SM 6.0+ and ray tracing libraries (lib_6_3+)
//
// DXC is loaded on-demand via LoadLibrary when a lib_6_* or *_6_* target is
// requested. Requires dxcompiler.dll + dxil.dll in the executable directory
// or system PATH.
// =============================================================================

#include "shader_utils.h"
#include "../core/log.h"
#include <dxcapi.h>
#include <fstream>
#include <vector>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace render
{

// =============================================================================
// D3DCompile flags (SM 5.1)
// =============================================================================
static UINT GetCompileFlags()
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    return flags;
}

// =============================================================================
// Check if target requires DXC (SM 6.0+)
// =============================================================================
static bool RequiresDXC(const char* target)
{
    if (!target) return false;
    // lib_6_*, vs_6_*, ps_6_*, cs_6_*, ms_6_*, as_6_*
    const char* p = target;
    while (*p && *p != '_') p++;
    if (*p == '_') p++;
    return (*p == '6' || *p == '7');
}

// =============================================================================
// DXC Compilation (lazy-loaded)
// =============================================================================
static HMODULE           s_dxcModule = nullptr;
static DxcCreateInstanceProc s_DxcCreateInstance = nullptr;

static bool EnsureDXC()
{
    if (s_DxcCreateInstance) return true;

    s_dxcModule = LoadLibraryW(L"dxcompiler.dll");
    if (!s_dxcModule)
    {
        core::Log::Error("Failed to load dxcompiler.dll — DXC not available");
        core::Log::Error("Copy dxcompiler.dll + dxil.dll to the executable directory");
        return false;
    }

    s_DxcCreateInstance = (DxcCreateInstanceProc)GetProcAddress(s_dxcModule, "DxcCreateInstance");
    if (!s_DxcCreateInstance)
    {
        core::Log::Error("Failed to find DxcCreateInstance in dxcompiler.dll");
        FreeLibrary(s_dxcModule);
        s_dxcModule = nullptr;
        return false;
    }

    core::Log::Info("DXC (DirectX Shader Compiler) loaded");
    return true;
}

static ComPtr<ID3DBlob> CompileWithDXC(
    const wchar_t* filePath,
    const char* entryPoint,
    const char* target)
{
    if (!EnsureDXC()) return nullptr;

    // Create DXC instances
    ComPtr<IDxcUtils> pUtils;
    ComPtr<IDxcCompiler3> pCompiler;

    HRESULT hr = s_DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&pUtils));
    if (FAILED(hr)) { core::Log::Error("DxcCreateInstance(Utils) failed"); return nullptr; }

    hr = s_DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&pCompiler));
    if (FAILED(hr)) { core::Log::Error("DxcCreateInstance(Compiler) failed"); return nullptr; }

    // Create include handler
    ComPtr<IDxcIncludeHandler> pIncludeHandler;
    pUtils->CreateDefaultIncludeHandler(&pIncludeHandler);

    // Load source file
    ComPtr<IDxcBlobEncoding> pSource;
    hr = pUtils->LoadFile(filePath, nullptr, &pSource);
    if (FAILED(hr))
    {
        core::Log::Errorf("DXC: failed to load file (0x%08X)", hr);
        return nullptr;
    }

    // Convert target and entry point to wide strings
    wchar_t wTarget[32];
    wchar_t wEntry[64];
    MultiByteToWideChar(CP_UTF8, 0, target, -1, wTarget, 32);

    bool isLibrary = (strncmp(target, "lib_", 4) == 0);
    if (!isLibrary && entryPoint && entryPoint[0])
        MultiByteToWideChar(CP_UTF8, 0, entryPoint, -1, wEntry, 64);

    // Build arguments
    std::vector<LPCWSTR> args;
    args.push_back(filePath); // Source name for error messages

    if (!isLibrary && entryPoint && entryPoint[0])
    {
        args.push_back(L"-E");
        args.push_back(wEntry);
    }

    args.push_back(L"-T");
    args.push_back(wTarget);

#ifdef _DEBUG
    args.push_back(L"-Zi");  // Debug info
    args.push_back(L"-Od");  // No optimization
#else
    args.push_back(L"-O3");  // Max optimization
#endif

    // Compile
    DxcBuffer sourceBuffer = {};
    sourceBuffer.Ptr      = pSource->GetBufferPointer();
    sourceBuffer.Size     = pSource->GetBufferSize();
    sourceBuffer.Encoding = 0;

    ComPtr<IDxcResult> pResult;
    hr = pCompiler->Compile(&sourceBuffer,
                             args.data(), static_cast<UINT32>(args.size()),
                             pIncludeHandler.Get(),
                             IID_PPV_ARGS(&pResult));

    // Check for errors
    ComPtr<IDxcBlobUtf8> pErrors;
    pResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr);
    if (pErrors && pErrors->GetStringLength() > 0)
    {
        core::Log::Errorf("DXC compile output: %s",
                          static_cast<const char*>(pErrors->GetBufferPointer()));
    }

    HRESULT compileStatus;
    pResult->GetStatus(&compileStatus);
    if (FAILED(compileStatus))
    {
        char narrowPath[256];
        WideCharToMultiByte(CP_UTF8, 0, filePath, -1, narrowPath, sizeof(narrowPath), nullptr, nullptr);
        core::Log::Errorf("DXC: failed to compile %s [%s] (0x%08X)",
                          narrowPath, target, compileStatus);
        return nullptr;
    }

    // Get compiled bytecode
    ComPtr<IDxcBlob> pBytecode;
    pResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pBytecode), nullptr);

    // Wrap in ID3DBlob for compatibility with the rest of the engine
    ComPtr<ID3DBlob> blob;
    D3DCreateBlob(pBytecode->GetBufferSize(), &blob);
    memcpy(blob->GetBufferPointer(), pBytecode->GetBufferPointer(),
           pBytecode->GetBufferSize());

    char narrowPath[256];
    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, narrowPath, sizeof(narrowPath), nullptr, nullptr);
    core::Log::Infof("DXC compiled: %s [%s] (%zu bytes)",
                     narrowPath, target, blob->GetBufferSize());
    return blob;
}

// =============================================================================
// Public API — routes to D3DCompile or DXC based on target
// =============================================================================
ComPtr<ID3DBlob> CompileShaderFromFile(
    const wchar_t* filePath,
    const char* entryPoint,
    const char* target,
    const D3D_SHADER_MACRO* defines)
{
    // Route to DXC for SM 6.0+ targets
    if (RequiresDXC(target))
        return CompileWithDXC(filePath, entryPoint, target);

    // D3DCompile path for SM 5.1 and below
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompileFromFile(
        filePath, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint, target, GetCompileFlags(), 0,
        &bytecode, &errors);

    if (errors && errors->GetBufferSize() > 0)
    {
        core::Log::Errorf("Shader compile output: %s",
                          static_cast<const char*>(errors->GetBufferPointer()));
    }

    if (FAILED(hr))
    {
        char narrowPath[256];
        WideCharToMultiByte(CP_UTF8, 0, filePath, -1, narrowPath, sizeof(narrowPath), nullptr, nullptr);
        core::Log::Errorf("Failed to compile shader: %s [%s] (0x%08X)",
                          narrowPath, entryPoint, hr);
        return nullptr;
    }

    char narrowPath[256];
    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, narrowPath, sizeof(narrowPath), nullptr, nullptr);
    core::Log::Infof("Compiled shader: %s [%s] (%zu bytes)",
                     narrowPath, entryPoint, bytecode->GetBufferSize());
    return bytecode;
}

ComPtr<ID3DBlob> CompileShaderFromSource(
    const char* source,
    size_t sourceSize,
    const char* sourceName,
    const char* entryPoint,
    const char* target,
    const D3D_SHADER_MACRO* defines)
{
    // DXC from source not implemented yet — use D3DCompile for SM 5.1
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompile(
        source, sourceSize, sourceName,
        defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint, target, GetCompileFlags(), 0,
        &bytecode, &errors);

    if (errors && errors->GetBufferSize() > 0)
    {
        core::Log::Errorf("Shader compile output: %s",
                          static_cast<const char*>(errors->GetBufferPointer()));
    }

    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to compile shader from source: %s [%s] (0x%08X)",
                          sourceName, entryPoint, hr);
        return nullptr;
    }

    return bytecode;
}

} // namespace render
