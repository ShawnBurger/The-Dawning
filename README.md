# The Dawning V3

The Dawning V3 is the canonical C++20/D3D12 engine baseline for this repository.
Older `SpaceEngineD3D12`, V1, and V2 archives are historical references only.
Do not merge old snapshots directly into this source tree.

## Current Baseline

- Windows-only, Visual Studio 2022, x64.
- CMake project targeting C++20.
- D3D12 raster renderer with ECS-driven scene rendering.
- Optional DXR path tracing path when the GPU and runtime DLLs support it.
- Source lives in `src/`; runtime shaders live in `shaders/`.

## Build

From the repository root:

```bat
SETUP_AND_BUILD.bat
```

The script first tries `cmake` from `PATH`, then common Visual Studio bundled
CMake locations.

Manual equivalent:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

If `cmake` is not on `PATH`, the Visual Studio bundled executable is commonly:

```text
C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

Build output:

```text
build\Debug\TheDawningV3.exe
```

## Path Tracing Runtime

Raster mode builds and runs without extra DLLs. For DXR/path tracing shader
compilation at runtime, copy these next to `TheDawningV3.exe`:

- `dxcompiler.dll`
- `dxil.dll`

They can come from the Microsoft DirectX Shader Compiler release or the
Microsoft.Direct3D.DXC NuGet package.

## Runtime Controls

- `F1`: toggle raster/path tracing.
- `F2`: toggle path tracing quality.
- `F3`: toggle the debug overlay.
- `WASD` + mouse: move the camera after clicking into the window.
- `ESC`: release mouse capture, then quit.

## Development Rule

Keep V3 compile-clean before adding features. Prefer small, verified changes:
build after each meaningful edit, then move to the next layer.
