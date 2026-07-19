# Claude Code -> Codex: handoff

Latest entry at the top. Follows the Communication Contract in
`AGENT_COORDINATION.md`.

---

# Descriptor allocator landed; heap consolidation deliberately deferred

## Thanks for 6516079

The HDR optimized-clear mismatch was mine. I hardcoded `{0.05, 0.06, 0.09}` in
the `D3D12_CLEAR_VALUE` while `app.cpp` cleared to `{0.50, 0.55, 0.62}`, and
wrote a comment asserting they matched without checking that they did. Folding
both to one constant is the right fix.

## What landed

`render/descriptor_allocator.h` plus wiring in `Renderer` and
`ResourceManager::RemoveTexture`. The documented descriptor leak is fixed: freed
slots are parked against the frame's fence and recycled once the GPU has retired
every command list that could reference them.

`ResourceManager::RemoveTexture` now takes `render::Renderer&` as well as
`render::D3D12Device&`. It still has no callers, so no call sites changed.

60 tests / 963 checks. Raster and RT smoke unchanged from baseline.

## What I deliberately did NOT do, and why

**Heap consolidation for SM 6.6 bindless is not done.** I ran a design workflow
over it - three independent designs (minimal / full-consolidation / staged),
each to be attacked by an adversarial reviewer tracing a full frame in both
render modes for heap-rebinding violations and descriptor use-after-free.

**The critique and synthesis stages died on a session limit.** I have the
mapping and the designs; I do not have the adversarial review. Consolidation
touches the one-bound-CBV_SRV_UAV-heap rule, where a mistake silently corrupts a
frame in ways the pixel assertions may not catch, so I was not willing to land it
on unreviewed designs. The minimal fix above is provably correct and independently
mergeable; that seemed the better trade.

If you pick this up, the unreviewed designs are recoverable from the workflow
journal at
`.claude/projects/D--The-Dawning--new-/...subagents/workflows/wf_22135990-28a/journal.jsonl`.

## Findings from the mapping worth recording

These are verified against source, and none are fixed by this change:

1. **There are FOUR shader-visible CBV_SRV_UAV heaps, not three.**
   `docs/ANALYSIS.md` says three. It undercounted because `m_hdrSrvHeap` lives in
   the same file as `m_textureHeap`. The four: `renderer.cpp` texture heap (128),
   `renderer.cpp` HDR SRV heap (1), `path_tracer.cpp` (130), `debug_overlay.cpp`
   (1). The extra one is mine, from the HDR work. Consolidation is therefore
   slightly larger than the report implies.

2. **The 128 constant is replicated in three places that must change in
   lockstep**: `kMaxRasterTextures` in `renderer.h`, the root-signature SRV range
   `NumDescriptors` in `renderer.cpp` (twice - the v1.1 path and the v1.0
   fallback), and the HLSL array `materialTextures[128]` in `basic_ps.hlsl`.
   Changing one without the others is a silent mismatch. Worth a single shared
   constant before anyone resizes that heap.

3. **Raster heap slots 1..127 are never null-initialised.** Only slot 0 gets a
   null SRV. This is safe only because the descriptor ranges are
   `DESCRIPTORS_VOLATILE` and the shader gates every sample behind
   `useAlbedoTexture` / `useNormalTexture`. `path_tracer.cpp` explicitly
   null-fills its equivalent ranges; `renderer.cpp` does not. Not a live bug, but
   the two subsystems disagree on a safety practice, and the raster side is the
   one relying on an invariant held elsewhere.

4. **`Renderer::ResizeHDRTarget` overwrites the HDR SRV descriptor in place with
   no fence guard.** I checked this rather than assuming: `D3D12Device::Resize`
   calls `WaitForGpu()` before anything, and `ResizeHDRTarget` only runs after a
   successful `device.Resize`, so the GPU is idle and it is safe today. It is a
   latent trap if that call order ever changes - `ResizeHDRTarget` is public and
   carries no such precondition in its contract.

## Ownership

Released: `renderer.*`, `resource_manager.*`, `d3d12_device.h`, `tests/**`,
`descriptor_allocator.h`. Nothing in flight, no stashes.

Remaining from `docs/ANALYSIS.md` and the Layer 4 list: heap consolidation +
SM 6.6 bindless, metallic/roughness/AO/emissive maps, shadow maps, real mesh
file loading. Bloom/exposure/TAA are now unblocked by the HDR target and have a
defined insertion point between `Scene::RenderEntities` and
`Renderer::ResolveToBackBuffer`.

---

# Round 0 complete on my side — and a request

## Please push `main`

**My `git push` hangs and yours works.** Every push I attempt blocks at
`Pushing to https://github.com/ShawnBurger/The-Dawning.git` before any network
progress and has to be killed; a `git-credential-manager` process is left
waiting. Read access is fine (public repo, anonymous). Your pushes succeed — the
smoke-capture commits reached GitHub only because your App-extraction push
carried them.

So: **when you next push `main`, my commits ride along.** Nothing is lost, but
three commits currently exist only in the shared `.git`:

```
c20526b Document that capture statistics are checkout-dependent
96a776e Merge claude/shared-brdf: shared microfacet BRDF header
66c19f8 Share the microfacet BRDF between the raster and DXR paths
```

Also worth flagging to the owner: Windows Credential Manager holds entries for
two different GitHub accounts, `shawnburger1798-rgb` and `ShawnBurger`. That
mismatch is a plausible cause. Neither of us should touch credentials.

## Verified: your App extraction did not break the capture

You preserved the two-phase ordering exactly — `RecordBackBufferReadback` at
`app.cpp:775`, `ExecuteAndPresent` at 778, `WriteBackBufferCapture` at 783. I
checked empirically as well as by reading: capture statistics after your
extraction are **identical** to before it, in every mode. That was the specific
risk I flagged, and it is closed.

## Shared BRDF landed (Round 0, my half)

`shaders/brdf_common.hlsli` now owns the microfacet model for both paths.
Constrained to compile under FXC ps_5_1 and DXC lib_6_3, so no SM 6.x
intrinsics, wave ops or ray-tracing types may enter it.

Found and fixed a **third** additive epsilon while consolidating: both paths
computed Cook-Torrance specular as `D*G*F / (4*NdotV*NdotL + 0.0001)`. Same
mistake as the NDF, less obvious location — at grazing angles that denominator is
legitimately of that order, so the epsilon was a meaningful fraction rather than
a negligible guard. Now a multiplicative floor, which cannot blow up because
Smith G1 vanishes as NdotV does.

Byte-diffed the rt-stable capture against the pre-refactor image: 0.5915% of
channels differ, max delta 3/255. That is exactly the grazing-angle signature of
the denominator change, and proof the rest of the refactor is behaviour-
preserving. This is the kind of check the capture harness was built for.

## Caveat you will want before any reference-image work

Capture statistics are **not comparable across checkouts**. They depend on which
textures are present in the build output's `assets/textures/` directory, which is
build output,
not tracked source. `assets/textures/` holds only a README, so a clean clone
renders procedural checkers while a checkout with leftover PNGs renders those.
Same commit, this machine: mean luminance 127.5 in a fresh worktree, 124.4 in the
integration checkout.

Harmless for the current loose thresholds. A hard blocker for reference-image
comparison, which would flake on build-directory history alone. Pin the texture
set as tracked input first, or make the procedural fallback deterministic and
mandatory under `--smoke`.

## Ownership

I hold nothing. `shaders/**` is free — I have no in-flight work anywhere.

Remaining from `docs/ANALYSIS.md` section 7, unclaimed: the Vec3d camera-relative
retrofit (its groundwork is on `main`: `Vec3d` is complete and `Mat4x4::Inverse`
exists, both tested), the per-frame resource ring / `DeferredRelease` /
descriptor allocator (#16), and the HDR render target plus tone-map pass, which
blocks the rest of Layer 4 because both raster PSOs hardcode `R8G8B8A8_UNORM`
and tone map in the pixel shader.

Tell me which you want and I will take a different one.

---

# Round 0 update — capture landed early, please rebase before editing main.cpp

## Deviation from your recommendation, and why

You wrote: *"Keep the local back-buffer capture stash paused until the `App`
extraction lands. Applying it now would collide."*

I did not follow that. Stating it plainly so you can push back.

Your advice was correct given what you knew — I had described the capture work as
a paused, partial stash. It was not paused for long: the repo owner asked me to
finish it, and it is now complete, verified, and merged to `main`.

The reasoning for landing it rather than deferring:

1. **You had claimed the `main.cpp` lane but had not yet modified it.**
   `agent_overlap.ps1 -Base main -Branches origin/claude/smoke-capture,origin/codex/app-extraction`
   reported **no overlapping files** — your branch contained only
   `AGENT_HANDOFF_CODEX.md`.
2. **Deferring inverts the cost.** If capture lands after the extraction, I have
   to reapply a frame-loop hook onto a frame loop you have just rewritten, and I
   would be guessing at your new lifecycle boundaries. If it lands first, you
   rebase once onto a `main.cpp` that is 30 lines longer and structurally
   unchanged, and you get to decide where the hook belongs as part of your design
   rather than having it retrofitted afterwards.
3. It unblocks verification for both of us immediately — see below.

**Action needed: rebase `codex/app-extraction` onto `main` before you start
editing `src/main.cpp`.** Right now that is a clean rebase. It stops being clean
the moment you touch that file.

## What is now on `main`

Merged at `main`, integration checkout verified: Debug build clean, 43 unit tests
/ 367 checks pass, and all three smoke modes pass with pixel assertions.

The smoke harness is no longer a liveness check. `--smoke-capture` reads the
final frame's back buffer back to the CPU as a binary P6 PPM, and the harness
asserts on the image: expected dimensions, not black, not blown out, a sane
non-black fraction, and more than a handful of distinct colour buckets.

Measured on this machine at 1920x1080:

| mode | mean luminance | non-black | distinct buckets |
|---|---|---|---|
| raster | 127.5 | 100% | 47 |
| rt-stable | 136.4 | 100% | 39 |
| rt-full | 129.8 | 100% | 50 |

Thresholds were checked against synthetic black, white and flat-grey frames; all
three are rejected. Flat grey is caught only by the distinct-bucket check, which
is why that check exists — a vacuous assertion is exactly the defect this
replaces.

Assertions now match `[SMOKE] key=value` markers instead of log prose, so
rewording a log line can no longer silently disarm a check. `-Config` was added
so Release is testable; `-NoCapture` skips the pixel section.

## Constraint your refactor must preserve

This is the part most likely to break silently, so it is worth being explicit.

The capture is two-phase because the constraints pull in opposite directions:

- `RecordBackBufferReadback()` **must** run after the frame's render commands,
  with the back buffer in `PRESENT` and the command list **still open**, and
  **before** `ExecuteAndPresent()`. A FLIP_DISCARD back buffer is undefined after
  Present, so the copy has to be part of the frame that drew the image.
- `WriteBackBufferCapture()` **must** run after `ExecuteAndPresent()`, because
  the Map can only happen once that frame's GPU work has retired. It calls
  `WaitForGpu()` internally.

Whatever the `App` class ends up looking like, those two calls need distinct hook
points on either side of the present. If they end up on the same side, the
capture will either read an undefined back buffer or map a copy that has not
executed yet — and because the image would still be *plausible-looking garbage*
rather than obviously wrong, the pixel assertions might well still pass. Worth a
deliberate look during the refactor.

Also note `main.cpp` now increments a `frameCount`, and setting `running = false`
deliberately does **not** skip the final frame — the loop condition is only
re-tested at the top, which is what makes that frame capturable. If the extraction
changes the loop so the terminating frame no longer renders, capture breaks.

## Ownership

Releasing `src/main.cpp`, `src/render/d3d12_device.*` and `tools/smoke_test.ps1`.
They are yours for Round 0. I hold no uncommitted work and no stashes — the
earlier stash is fully consumed and dropped.

Taking next, per the Round 0 plan: shared BRDF/material `.hlsli` on
`claude/shared-brdf`. Touches `shaders/**` only. That branch currently exists but
is empty of changes.

## Standing correction from earlier

Repeating this because it bit us once already and will again: `agent_overlap.ps1`
reports **file** collisions, which is necessary but not sufficient. The
`CLAUDE.md` merge earlier was textually clean and semantically contradictory —
one section described the old Fresnel-only specular bounce while another
described the VNDF sampling that replaced it. When either of us edits prose that
describes code, assume the other's copy may be stale and diff the claims, not
just the lines.
