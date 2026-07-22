# Repository Deep Audit - 2026-07-22

## Scope

This audit reviewed the integrated engine at base commit `17e621b` across core,
ECS, gameplay, simulation, terrain, scene assembly, asset parsing, renderer,
shaders, tools, and tests. The work was isolated on
`codex/repo-deep-audit`; the dirty production-asset worktree was not modified.

The review combined:

- clean Debug and Release builds at warning level 4;
- all C++ and Python contract tests;
- MSVC whole-program static analysis;
- an AddressSanitizer build of the complete CPU test executable;
- raster, stable-DXR, full-DXR, resize, structured-buffer growth, GPU-validation,
  production-content, flight-control, Surface-startup, and F4 mode-cycle runs;
- manual review of ownership, arithmetic, parsing, state, and probe boundaries.

## Corrected Defects

| Area | Defect | Correction |
| --- | --- | --- |
| Test registration | Python files could exist under `tests/test_*.py` without ever entering CTest. | Added a discovery-level `python_contract_suite`; inspector-specific tests remain independently registered. |
| Terrain mesh | `gridN > 256` overflowed the 16-bit index domain; negative input was only partially bounded. | Clamp the grid to `[2, 256]` and verify the maximum emitted index is `UINT16_MAX`. |
| Terrain selector | Recursive selection could exceed `maxLeaves`, unsafe shifts accepted hostile levels, and malformed floating-point configuration could enter unstable refinement. | Replaced recursion with deterministic error-priority refinement, exact leaf accounting, bounded levels/counts, finite configuration normalization, saturating error math, and coverage tests. |
| Surface camera modes | Four-element label arrays indexed a five-mode enum, making Surface an out-of-bounds read. | Centralized compile-time labels for every mode and asserted enum/table parity. |
| Surface lifecycle | Terrain framing existed only when Surface was the startup mode; cycling to Surface with F4 could show no terrain. | Prepare verified Moon framing after the system is seeded and on Surface entry; release streamed cache resources when leaving. |
| Runtime star-system state | Normal play seeded the system but HUD, lighting, and sky decisions still tested the smoke command-line flag. | Exposed `Scene::HasStarSystem()` as runtime truth and retained the option only where it defines a smoke profile. |
| Terrain residency | Streamed patches accumulated without a bound and could release GPU buffers without a fence-aware path. | Added a 2,048-entry/stale-frame cache policy and fence-retired eviction/shutdown release. |
| GPU draw verification | Planet and terrain vertex shaders consumed `ObjectData` while opting out of the advertised per-draw probe. Surface runs therefore hid hundreds of unverified reads. | Wired both shaders and draw calls into the probe; Surface now verifies all 380 main records with zero mismatches. |
| Probe reporting | `draw_probe` combined draw-record correctness with a separate cascade-blend result, obscuring which contract failed in non-demo scenes. | Report draw records and cascade blending independently while preserving the strict combined failure in the demo smoke profile. |
| Mesh upload | Null/empty input, view-size overflow, out-of-range indices, and partial staging outputs were accepted or could leak ambiguous state. | Validate before allocation, reject invalid indices/sizes, log map failures, reset outputs on entry, and publish staging resources transactionally. |
| Texture loading | DDS used potentially unaligned struct reads; several dimensions and size products could overflow or narrow; failed calls could retain stale upload outputs. | Use `memcpy` header reads, checked 64-bit surface math, D3D extent limits, WIC DWORD bounds, size-safe procedural allocations, and entry-time output reset. |
| ECS sparse sets | Direct out-of-domain indices could drive capacity overflow; reallocations did not preserve state if component copy failed. | Enforce the entity index domain, use bounded growth, and stage allocations/copies with RAII before publishing. |
| Job system | Partial thread construction could terminate during unwind; ceiling division overflowed; queue allocation failure could leave `Wait()` hung; an empty callback could terminate a worker. | Join partially created workers, use overflow-safe partition math, repair pending counts on enqueue failure, and treat an empty callback as a no-op. |
| Renderer capacity | Doubling a hostile draw count wrapped the object capacity to a small value. | Saturate the capacity computation so allocation fails explicitly instead of under-allocating. |
| Static-analysis hygiene | Several immutable numeric bounds, signed indices, and the Windows entry point generated avoidable analyzer ambiguity. | Made bounds constant expressions, used size-correct indexing, simplified minimum selection, and added WinMain SAL annotations. |

## Verification

All completed gates passed unless explicitly noted:

- Debug full build: passed.
- Release full build: passed.
- Debug CTest: 7/7 passed.
- Release CTest: 7/7 passed.
- C++ unit executable: 567/567 passed.
- Python discovery: 61 passed, 3 skipped by environment-specific inspector contracts.
- AddressSanitizer CPU suite: 567/567 passed with no sanitizer report.
- MSVC `/analyze`, complete application: zero warnings.
- Release raster smoke: passed.
- Release stable-DXR smoke: passed.
- Release full-DXR smoke: passed.
- Release Frontier Courier plus flight smoke: passed.
- Release resize/forced-growth raster stress: 34 reallocations, 31 while frames were in flight; passed.
- Debug GPU-validation raster stress: passed.
- Debug GPU-validation stable-DXR stress: passed.
- Release Surface smoke: 26 shadow records and 380 main records, all distinct with zero mismatches; passed.
- Automated F4 injection from Ship through Orrery, NearBody, and Free into Surface: all transitions observed; final Surface draw probe passed with 380 main records.
- `git diff --check`: passed.

The attempted Release GPU-validation run was intentionally rejected by the
smoke harness because Release does not enable the D3D12 debug interface. The same
stress profile passed under Debug, where GPU validation is available; no Release
coverage is claimed for that unavailable layer.

## Residual Risks

- Surface terrain is still a prototype: fixed initial framing, no split/merge
  hysteresis, and no crack skirts between mixed LOD neighbors. The selector and
  residency boundaries are now safe, but production planetary traversal remains
  future work.
- The F4 lifecycle path was exercised by automated Windows message injection,
  not registered as a portable CTest. A dedicated input-replay harness would make
  that regression suitable for CI.
- Allocation-failure recovery is code-reviewed and exception-safe, but there is
  no deterministic allocator fault-injection suite yet.
- DDS, KTX, cooked assets, and glTF have contract and negative tests, but not a
  coverage-guided fuzzing campaign.
- DXR validation proves this machine and driver path. Cross-vendor GPU coverage
  remains a CI/infrastructure task.

## Audit Conclusion

No unresolved correctness failure was found in the audited integrated branch
after the fixes above. The engine now has stronger negative boundaries, complete
Python test discovery, bounded terrain streaming, truthful GPU draw verification,
and consistent runtime wiring for the seeded star system and Surface mode. The
remaining items are explicit production milestones rather than hidden regressions.
