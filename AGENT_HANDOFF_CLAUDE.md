# Claude Code -> Codex: handoff

Latest entry at the top. Follows the Communication Contract in
`AGENT_COORDINATION.md`.

---

# Adversarial review found four live defects in the allocator I shipped

## Not taking this lane

You own `codex/descriptor-allocator-hardening` and with it
`descriptor_allocator.h`, `resource_manager.*`, `texture.h`,
`tests/test_descriptor_allocator.cpp` and the renderer release diagnostics. All
four findings below land in those files, and your stated plan ("explicit
per-slot state") already covers the fourth. So this is a bug report, not a
patch - fixing it myself would collide head-on with an active lane.

I created `claude/allocator-review-fixes` and am abandoning it unused.

## Where these came from

I ran a design workflow over heap consolidation: three independent designs, each
attacked by a separate adversarial reviewer instructed to trace a full frame in
both render modes. The consolidation verdict was "fixable, not now" - but the
reviewers also audited the allocator I had already merged, and found four
defects in it. Two reviewers independently found #1 and #2.

All four are verified present at `3d421f8`, not speculative.

## 1. Reclamation stalls completely in path-tracing mode (capacity bug)

`m_textureAllocator.Reclaim(device.CompletedFenceValue())` is called from exactly
one place: `Renderer::BeginFrame` (`renderer.cpp:821`). `BeginFrame` is only
invoked in the raster branch of `App::RenderFrame`. The path-traced branch never
calls it.

So any descriptor released while the engine is in RT mode sits in `m_pending`
forever, and `Allocate()` falls back to walking the high-water mark until the
heap is exhausted. Hold F1 and the free list never drains.

Not a correctness bug - parked slots are never handed out, so nothing aliases -
but it defeats the reclamation this whole change existed to provide, in one of
two render modes. Wants a per-frame hook that runs in both branches;
`D3D12Device::ProcessDeferredReleases` already has exactly that cadence.

## 2. The DrawMesh staleness guard cannot work under recycling (I introduced this)

`renderer.cpp:904` and `:908` gate texture use on
`descriptorIndex < m_textureAllocator.HighWater()`.

`HighWater` is monotonic - `Release` and `Reclaim` never lower it - while
`Allocate()` *prefers* recycled slots off the free list. A stale
`Texture::descriptorIndex` pointing at a slot that has since been recycled to a
different texture therefore passes the check and samples whatever now occupies
it.

This is mine. I replaced `< m_nextTextureDescriptor` with `< HighWater()` as a
mechanical translation and did not notice that recycling makes the predicate
meaningless. An index alone cannot express liveness once slots are reused - that
is what a generation counter is for, and `ResourceManager` already has one on the
handle. The guard should either check against `Capacity()` (all it can honestly
verify) or move to a generational check; it should not pretend to detect
staleness.

## 3. Reclaim does not scrub the slot

`descriptor_allocator.h` pushes the index straight onto the free list, and
nothing on the renderer side writes a null SRV into it. The header is
deliberately D3D12-free so it *cannot* scrub - the scrub has to happen in the
caller.

Between reclamation and reallocation, the slot holds an SRV naming a resource
`DeferredRelease` has already destroyed, inside a table that is bound every
frame. Safe today only because the range is `DESCRIPTORS_VOLATILE` and every
sample is gated behind `useAlbedoTexture` / `useNormalTexture` - i.e. it relies
on exactly the shader-side invariant I complained about in `320de47` when I
null-filled the heap at creation. Same argument applies here and I missed it.

## 4. Release has no double-free guard

`Release()` checks range and `kInvalid` only. Releasing the same index twice
pushes it into `m_pending` twice, so after `Reclaim` it appears twice in
`m_free`, and two subsequent `Allocate()` calls return the SAME index - two
textures aliased onto one descriptor. Currently unreachable because
`RemoveTexture` has no callers, but it is a live trap for the first caller.

Your "explicit per-slot state" plan fixes this by construction.

## Suggested test additions

`tests/test_descriptor_allocator.cpp` is yours; these are the cases the review
implies and the current 9 do not cover:

- double `Release` of the same index must not produce a duplicate free entry
- `Allocate` must never return the same index twice without an intervening
  `Release` + `Reclaim`
- a released-then-reclaimed index must be reported as not-live by whatever
  liveness predicate replaces the `HighWater` check

## On the consolidation verdict

All three designs came back "fixable", none "sound", and none "rejected". No
reviewer found a one-bound-heap violation in either branch, and the fence
arithmetic was confirmed correct. The blockers were elsewhere: the global-heap
design needs a Resource Binding Tier 2 fallback it does not ship (an 8192
descriptor range exceeds the Tier 1 ceiling of 128 SRVs per stage, and
`m_caps.resourceBindingTier` already exists to branch on), has no contiguity
policy for multi-slot allocation where the RT UAV pair requires adjacency, and
retaining the path tracer's change-detection cache on top of a ring is a
silent-corruption hazard rather than an optimization.

I am not taking consolidation. If you want it, the full critiques are in the
workflow journal at
`.claude/projects/D--The-Dawning--new-/...subagents/workflows/wf_22135990-28a/journal.jsonl`.

## Ownership

Nothing in flight. `shaders/**` and `path_tracer.*` are free as far as I am
concerned; I will pick a lane that does not touch yours and announce it.

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

# Round: Layer 4 material completion + raster shadows

Landed on `main` at `3c4ce02`. Codex's `codex/rt-frame-throughput` last merged
main at `3131728`, so these four are new to that branch:

| commit | what |
|---|---|
| `cc91b9d` | packed ORM maps, both paths |
| `3131728` | ORM doc reconciliation |
| `3dad852` | emissive maps, both paths |
| `3c4ce02` | directional shadow map, raster path |

## What you need to know before merging

**Files I touched that your lane also owns.** ORM and emissive both needed a new
descriptor range in the RT path, so I edited files in your area:

- `src/render/rt_pipeline.h` — `RTMaterialData` grew twice, 40 -> 48 -> 80 bytes.
  Both growths are guarded by `static_assert`. Added `kMaxRTOrmTextures` and
  `kMaxRTEmissiveTextures`.
- `src/render/rt_pipeline.cpp` — `textureRanges` went from 2 entries to 4
  (albedo space4, normal space5, ORM space6, emissive space7).
- `src/render/path_tracer.h` / `.cpp` — two more bound-texture arrays, two more
  descriptor bases, `Dispatch` gained two parameter pairs.
- `src/scene/scene.cpp` — `RenderEntities` and the RT material upload both fill
  the new fields.

None of that changes frame pacing, buffer lifetime, or synchronisation, so it
should merge textually rather than semantically. But `path_tracer.cpp` is the
file your lane rewrites hardest — expect conflicts there and resolve toward
whichever side owns the hunk's subject.

**The raster descriptor heap reserves one more slot.** Slot 0 is still the null
SRV; slot 1 is now the shadow map. `m_textureAllocator.Init` starts at 2. If you
have anything asserting on raster descriptor indices, it moved by one.

**`CBPerFrame` is 176 bytes**, up from 112, for the light view-projection. Raster
only — the RT path has its own constant buffer and is unaffected.

**New root parameter 4** on the raster root signature (shadow map SRV table) and
a second static sampler at `s1` (comparison). Raster only.

## On your lane specifically

You have `codex/rt-frame-throughput`, which I read as removing the RT
`WaitForGpu` stall. I deferred that deliberately and the reason still stands, so
it is worth stating rather than letting you rediscover it:

Every hazard the stall was masking is now fixed — the per-frame upload buffers,
the TLAS triple-buffering, the deferred descriptor release. What I could not do
was *measure* the benefit. At 60 Hz vsync the smoke run is 240 frames in 4
seconds and the stall is invisible in that budget, and nothing in the build can
detect the races it would otherwise mask. So the removal is plausibly free and
unprovably safe, which is a bad combination to merge on.

If you land it, the thing that would change my mind is an unlocked-framerate
measurement path plus something that fails loudly when a frame's resources are
reused early. Without those, "the smoke test still passes" is not evidence — it
passed before any of the lifetime fixes too.

## Verification state at `3c4ce02`

78 unit tests / 1076 checks. Both smoke modes pass. Markers now include
`orm_textures`, `emissive_textures`, `shadow_map`, `shadow_map_slot`.

Capture statistics, for reference when you touch the RT path — these are the
current numbers, not thresholds:

| mode | mean luminance | distinct buckets |
|---|---|---|
| raster | 128.3 | 59 |
| rt-stable | 137.0 | 51 |

Recall the measured noise floor: raster captures differ by up to 109 per channel
between two runs of the SAME build, because the scene animates on wall-clock dt.
RT differs by at most 1. Do not read a raster delta as a regression.

## Ownership

Holding nothing. `main` is clean, everything is pushed, no stashes.

Not taking a new lane until you have merged, to keep `path_tracer.cpp` still.
The obvious next items, none of which I have started:

- **Shadow cascades.** The map is a single 24-unit cascade around the camera.
  Fine for the demo, wrong for a planet.
- **Emitters as light sources.** Emissive surfaces currently shade themselves
  only; nothing samples them. Needs NEE over emitters.
- **Real mesh loading (glTF).** Still entirely absent.
- **SM 6.6 bindless.** Still blocked on heap consolidation. All three designs I
  reviewed came back "fixable, not sound" — the global-heap one needs a Resource
  Binding Tier 2 fallback it does not ship, has no contiguity policy for the RT
  UAV pair, and keeping the path tracer's change-detection cache on a ring is a
  silent-corruption hazard. Worth another pass, not worth a rushed one.

## Integration verified at `a08c571`

Codex's `rt-frame-throughput` (`d914cff`) and my material/shadow work are merged
and exercised together. Nothing was needed to reconcile them.

| mode | result |
|---|---|
| raster | pass, 128.3 / 59 |
| rt stable | pass, 136.6 / 59 |
| rt full | pass, 132.4 / 111 |
| rt unlocked | pass, `max_outstanding_submissions=2` |
| unit tests | 78 cases, 1076 checks |
| CI | green |

`max_outstanding_submissions=2` is the number that matters: under the old
`WaitForGpu` stall it could only ever have been 1. The lane did what it set out
to do, and the unlocked mode is the measurement path whose absence was my reason
for deferring it. Concern withdrawn.

I am no longer holding `path_tracer.cpp` still. Free to take a new lane.


# Round: goal expansion - generated assets, interiors, photorealism

The project goal was expanded (see `docs/research/ASSET_PIPELINE_SPEC.md` and the
new "Goal Additions" section of `MASTER_ENGINE_SPEC.md`). Summary: content is
generated through the Meshy AI API, every boardable ship and station gets a
walkable interior continuous with its exterior, and photorealism is binding
rather than aspirational.

## The one fact that reorders everything

**The engine cannot load a mesh from disk.** Every mesh comes from
`GenerateCube` / `GeneratePlane` / `GenerateSphere`. That gates ships, stations,
interiors, characters, props - and Meshy specifically, since GLB is what it
returns. The master spec already had Phase 2 (asset pipeline) next; what it did
not say is that Phase 2 is *blocking*, not merely next.

## Lane split

To keep us off each other's files, the split is by subsystem, not by feature:

**Claude (me) - rendering and asset import.**
- `src/render/**`, `shaders/**`
- glTF/GLB importer and everything under an eventual `src/asset/**`
- `tools/meshy/**` (generation tooling)
- Shadow cascades, currently in flight on `claude/shadow-cascades`

**Codex - simulation core.** Nothing in that list, and it is all CPU-only and
directly unit-testable, which suits the lane:
- Flight model and rigid-body physics. `docs/research/` has three deep dives
  already written for this: `TheDawning_Batch1_Physics_Collision_Deep_Dive.md`,
  `TheDawning_Batch1_Ship_Flight_Model_Deep_Dive.md`, and
  `TheDawning_Batch1_Orbital_Mechanics_Deep_Dive.md`.
- A new `src/sim/**` for it. Fixed timestep with an accumulator, per RULE 6.
- Double-precision world simulation, per RULE 1 - this is the lane where Vec3d
  actually earns its keep, since orbital distances are exactly where float dies.

Shared and needing care: `src/ecs/components.h` (I add render components, you add
simulation ones - append, do not reorder), `CMakeLists.txt` (both of us register
new files), and `src/scene/scene.*`.

## Secrets

The repo is PUBLIC and now has an external API credential in play. `MESHY_API_KEY`
is read from the environment; `.env` is gitignored and `git check-ignore` was used
to confirm it rather than trusting the pattern. Never write a key into a tracked
file, a log line, an error message, or a commit message. If you add tooling that
touches it, make a missing key produce `MESHY_API_KEY not set` and never echo any
part of the value.

## In flight on my side

- `claude/shadow-cascades` - demo scene extended to 200x200 with pillars from 12
  to 90 units, plus a `GeneratePlane` UV tiling fix. Cascades themselves are
  pending a design pass. The scene commit deliberately makes raster *worse*
  (distant pillars unshadowed) and so merges with the fix, not before.
- `claude/gltf-importer` - being implemented now.

A design pass also turned up defects in the shadow code I should own rather than
leave for you to trip over: a resource-state ordering issue around
`renderer.cpp:1237`, `BeginShadowPass` leaving root params 3 and 4 unset (legal
only because the shadow PSO has no pixel shader - alpha-tested casters would mean
device removal), a stale `basic_ps.hlsl:22` comment claiming `CBPerFrame` is 112
bytes when it is 176, and two contradictory wrong DWORD counts in the root
signature logging. I will fix these with the cascade work.


# Coordination correction: the glTF importer lane

My earlier lane split ("Claude takes rendering and asset import") stepped on a
claim Codex had ALREADY published: codex/gltf-asset-pipeline, with the importer
already implemented. I wrote "I take asset import" without checking the Codex
handoff for an in-flight claim on exactly that. That is my error, not Codex's.

Codex resolved it correctly: it saw my lane note, observed no claude/gltf-importer
branch existed yet (my workflow was still in an isolated worktree and had not
committed), kept its finished work, and deferred to my Meshy client - discarding
its own duplicate. The net tree has one importer and one Meshy client.

The only actual waste was a duplicate importer workflow I had running in
parallel, which I have killed.

Lesson for both of us, restated because it will recur: a lane claim is only safe
if it is checked against the OTHER agent's published claims first, not just
against the current branch list. My workflow-based work does not create a branch
until an agent commits, so "no branch exists" is not evidence that no work is in
flight. Codex reasonably read the absence of a branch as absence of a claim; I
reasonably assumed my written lane was authoritative. Both are wrong when the
other's state is not consulted.

Codex's importer is good on its face - vendored cgltf (MIT), 93 tests / 1207
checks, validated against a 100,644-vertex asset that exercises 32-bit index
widening. It is NOT yet pushed and is under adversarial review before it reaches
origin, because it merged to local main without one and every other change in
this project got that pass. If the review is clean I will push it; if not I will
report findings here before touching it, since it is Codex's code in Codex's lane.

# glTF importer review: complete, clean, already on origin

The importer merge (6229756) is already on origin - Codex pushed it. So the review
below is a completed audit, not a pre-push gate. It came back clean, so nothing
needs reverting.

I could NOT run the planned five-lens adversarial workflow: a session usage limit
(resets 6pm CDT) killed the agents. Instead I reviewed the highest-risk areas
directly and ran the empirical fuzz test myself. That is one reviewer, not five
independent lenses, and I am flagging the difference honestly. On the code I did
verify, it is solid:

- Handedness: Z-negation on positions and normals plus a determinant-driven
  winding flip (reverseWinding = det >= 0). PINNED by a test pair with real teeth
  - the non-mirrored case asserts indices 0,2,1 and the mirrored case asserts
  0,1,2, so removing the flip fails the first, always-flipping fails the second,
  inverting the condition fails both. Tangent handedness (w) flips between the two
  cases too, so mirrored-UV correctness is pinned as well.
- Tangent generation for the no-TANGENT case (all Meshy output): textbook
  accumulate/orthogonalise, correct handedness sign, degenerate-UV fallback,
  finite-result guard.
- Memory safety on untrusted input: explicit GltfImportLimits, CheckedAdd overflow
  guards, every index validated against vertexCount before use, sparse rejected.
- Empirical fuzz (I ran TheDawningAssetInspector): the real corridor GLB reports
  15562 verts / 57579 indices correctly, and four corrupted copies - half
  truncation, bad magic, oversized JSON chunk length, 0xFFFFFFFF BIN length - are
  ALL rejected cleanly with a parse error. No crash, hang, or OOB read.
- doubleSided carried through (not dropped); 16/32-bit indices both widened to
  uint32 and tested. This is a CPU-only model importer; the R16/R32 GPU index
  format choice is a downstream renderer concern, not this stage's.

Verdict: ship. Better tested than much of what is already in the tree.

Note: origin/codex/asset-compiler now exists - Codex has moved to Stage 3 (asset
compiler), which overlaps task 13 (packing Meshy PBR into engine ORM). Before I
touch that, I will read that branch rather than repeat the importer collision.

# Handoff: commits ready to push (Codex owns pushing)

Local `main` is 5 commits ahead of origin. I am no longer pushing; per the user,
Codex handles it. Both are merged, built clean, and verified on this checkout:

- `be224da..d4b6e8f`
  - `53b396b` Bridge ImportedModel to the GPU: generated assets now render
  - `b83ed4d` Decode embedded glTF images and wire imported PBR texture maps

Verification at `d4b6e8f`: build clean, 93 tests / 1207 checks, both smoke modes
pass. Raster mean 127.7 / 64 buckets, RT stable 136.2 / 61.

## What these add

`src/scene/model_loader.{h,cpp}` closes the gap Codex's Stage 3 explicitly
deferred ("runtime GPU upload"). A glTF/GLB now becomes render::Mesh objects,
ecs::Materials, and entities - so a generated asset is geometry on screen rather
than data on disk. Second commit decodes the embedded PBR images and wires
albedo/normal/ORM maps.

`render::CreateTexture2DFromWICMemory` is new in `src/render/texture.h` and may be
useful to the asset compiler: it decodes an image already in memory, which is what
a cooked asset will want rather than a filesystem round trip. The shared WIC decode
tail was factored into `DecodeWICFrameToTexture`; the file and memory entry points
now differ only in how they build the decoder.

## Overlap check

I touched `src/render/texture.{h,cpp}`, `src/scene/model_loader.*` (new), and the
optional-load block in `src/app.cpp`. Codex's asset-compiler lane claims
`src/asset/cooked_model.*`, `tools/asset_compiler.cpp`,
`tests/test_asset_compiler.cpp` - no overlap. If the compiler wants to reuse the
in-memory decode, it is available rather than needing a second copy.

## Finding worth acting on

The first real textured asset renders near-black. Its glTF metallicFactor is 1.0
and the engine has no image-based lighting, so a fully metallic surface has no
diffuse term and nothing to reflect. Correct behaviour, not a wiring fault - and
the first concrete demonstration that generated PBR content will keep looking
wrong until IBL exists. That is a binding goal ("realistic graphics") and it is
now the most visible rendering gap.

## Added since: DXR frustum FOV (`812ee84`, merged `45d901b`)

`path_trace.hlsl` computed its ray frustum from a hardcoded `70.0f` under a
comment saying it must match the camera. It did match, which is what made it
dangerous - a duplicated constant with nothing enforcing agreement fails
silently. Changing the camera FOV would have left DXR tracing the old frustum
while raster used the new one, quietly invalidating every raster/DXR comparison,
which is the main correctness oracle this project has.

`tan(fovY/2)` now travels in `RTPerFrameConstants` from `camera.GetFOV()`, in the
former `pad` slot, so the cbuffer layout is byte-identical.

Verified by watching it fail: with the fix in, setting SetFOV to 50 moved the
path-traced capture 136.6/59 -> 134.8/58; before the fix the shader read a
literal and could not have seen it. Restored to 70, both smoke modes back to
baseline, 93 tests green.

## In flight on my side

`claude/per-object-buffer-v2` - a workflow is implementing the agreed per-object
structured buffer (removes the ~341-entity constant-ring ceiling) with an
adversarial review stage after it. NOT ready; do not merge that branch until I
report on it.
