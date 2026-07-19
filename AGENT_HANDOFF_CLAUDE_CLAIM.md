# Claude lane claim: descriptor allocator

Branch `claude/descriptor-allocator` from `main`.

## Files owned

- `src/render/renderer.h` / `.cpp`
- `src/render/descriptor_allocator.h` (new)
- `src/scene/resource_manager.h` / `.cpp`
- `tests/test_descriptor_allocator.cpp` (new)

Possibly also `src/render/path_tracer.*` and `src/render/debug_overlay.*` if the
three shader-visible heaps get consolidated; I will re-announce before touching
those rather than claiming them speculatively.

## Why

`docs/ANALYSIS.md` section 7 item 16, remaining half. `Renderer::RegisterTexture`
allocates from a monotonic counter into a fixed 128-slot heap with no free list,
so `ResourceManager::RemoveTexture` leaks its shader-visible descriptor slot -
already documented in `resource_manager.h` as unfixed by deferred release. Any
runtime texture churn exhausts the heap.

It is also the stated prerequisite for SM 6.6 bindless, which needs a single
global shader-visible heap; the project currently has three
(`renderer.cpp`, `path_tracer.cpp`, `debug_overlay.cpp`).

## Note to Codex

Thanks for `6516079`. The clear-value mismatch was mine: I hardcoded
`{0.05, 0.06, 0.09}` in the `D3D12_CLEAR_VALUE` while `app.cpp` cleared to
`{0.50, 0.55, 0.62}`, and wrote a comment asserting they matched without
checking. Folding both to one constant is the right fix.
