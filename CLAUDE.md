# CLAUDE.md - The Dawning V3 Agent Instructions

## Project

The Dawning V3 is a Windows-only C++20 / Direct3D 12 engine baseline.
The current source tree is the canonical V3 engine; older archives are
historical references only and must not be merged directly into this tree.

Read these files before editing:

- `README.md`
- `AGENT_COORDINATION.md`
- The relevant `src/`, `shaders/`, and `tools/` files for the task

## Current Baseline

- ECS-driven D3D12 raster renderer.
- Optional DXR path tracing path when GPU support and runtime DXC DLLs exist.
- Layer 4 material path with albedo and normal textures.
- Raster and path tracing display now share tone mapping / sky helper behavior.
- Build output is `build\Debug\TheDawningV3.exe`.

## Required Commands

Build from the repository root:

```bat
SETUP_AND_BUILD.bat
```

Run smoke tests after rendering changes:

```powershell
.\tools\smoke_test.cmd -RasterOnly -Seconds 1.5 -TimeoutSeconds 8
.\tools\smoke_test.cmd -Seconds 3 -TimeoutSeconds 12
.\tools\smoke_test.cmd -FullQuality -Seconds 3 -TimeoutSeconds 12
```

If path tracing fails to initialize, confirm these files are next to the exe:

- `build\Debug\dxcompiler.dll`
- `build\Debug\dxil.dll`

## Parallel Agent Rules

Use Git worktrees for parallel work. Do not have Codex and Claude Code edit the
same physical checkout at the same time.

- Codex branches use `codex/<task-slug>`.
- Claude branches use `claude/<task-slug>`.
- Local worktrees live under `.agents\worktrees\` and are ignored by Git.
- Use `tools\agent_worktree.ps1` to create an isolated checkout.
- Use `tools\agent_status.ps1` and `tools\agent_overlap.ps1` before merging.

## Engineering Rules

1. Keep V3 compile-clean before adding features.
2. Make small, verifiable changes.
3. Prefer existing engine patterns over new abstractions.
4. Use proper `.h` / `.cpp` splits. Avoid header-only implementations except
   tiny inlines or templates.
5. Always include `<cstdint>` when using fixed-width integer types.
6. Use double precision (`Vec3d`) for world positions. Convert to `Vec3f` only
   after camera-relative subtraction.
7. The engine uses a left-handed coordinate system. `+Z` is forward,
   `LookAt` is LH, projection maps Z to `[0, 1]`, and back-face culling is CW.
8. The logger is not thread-safe. Call it only from the main thread until the
   engine has a job-safe logging path.
9. Do not use copyrighted source terms. Existing rename map: Starfleet to
   Vanguard, LCARS to HELIX, Phaser to Arc Lance, Photon Torpedo to Fusion
   Torpedo, Dilithium to Helion, Federation to Commonwealth.
