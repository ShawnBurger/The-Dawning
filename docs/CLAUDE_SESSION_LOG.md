# The Dawning — Claude work log

Written 2026-07-20. Covers the work I did on this engine, what it cost, what it
found, and what is still open. Ordered by theme rather than chronology, because
the themes are what transfer.

**Repository is PUBLIC.** Nothing here contains a credential, and nothing in the
tree does either — verified with `git grep` against the key before every push.

---

## 1. Where the engine started and where it is

At the start: a C++20/D3D12 renderer with ECS, DXR path tracing, albedo and normal
maps, and no test suite, no CI, and no way to load a mesh from disk. `ANALYSIS.md`
listed 66 claimed findings, 29 of which survived adversarial verification.

Now: **141 tests / 3271 checks**, green CI, four-cascade shadows, a full PBR
material set in both render paths, a working glTF asset pipeline fed by generated
content, and a constant-buffer architecture that no longer caps the scene at a few
hundred entities.

The roadmap in `MASTER_ENGINE_SPEC.md` puts Phase 0 (repo/CI/tests) and Phase 1
(engine correctness) as complete and Phase 2 (asset pipeline) next. Phase 2 is now
largely built.

---

## 2. What shipped

### Rendering

| Work | Notes |
|---|---|
| Packed ORM maps | Both paths simultaneously. glTF channel convention (AO=R, rough=G, metal=B), modulating scalars rather than replacing them. |
| Emissive maps | Both paths. Greyscale mask + material colour/strength, so one mask drives many emitters. Added after lighting, untouched by occlusion or Fresnel. |
| Directional shadow map | 2048², 3×3 hardware PCF. Light matrix built in **camera-relative** space — the one thing that would have silently broken. |
| Four shadow cascades | One `Texture2DArray`, zero root-signature change. Implemented by Codex from a design I published. |
| HDR scene target + bloom | Linear `R16G16B16A16_FLOAT`, single tone-map resolve. |
| Anisotropic filtering | `MaxAnisotropy=16` was set while the filter was `MIN_MAG_MIP_LINEAR` — inert. Now real. |
| Ray-cone texture LOD | Path tracer had **no** texture LOD; every sample was mip 0. Done, on `claude/rt-texture-lod`, unmerged. |
| Per-object structured buffers | Removed the ~341-entity ceiling. Ring is now 1792 bytes **flat**. |

### Asset pipeline

- **Meshy client** (`tools/meshy/meshy_client.py`) — stdlib only, content-addressed
  so an unchanged prompt never respends, with a credit floor that refuses to start
  a two-stage job it cannot finish. First asset cost 15 credits of 10,300.
- **glTF/GLB importer** — built by Codex, reviewed by me (see §5).
- **GPU bridge** (`src/scene/model_loader.*`) — the step both Codex's importer and
  asset compiler explicitly deferred. Turns an `ImportedModel` into meshes,
  materials and entities, so a generated asset is geometry on screen rather than
  data on disk.
- **Embedded image decode** — `CreateTexture2DFromWICMemory` for glTF images living
  in the GLB's BIN chunk.

### Verification infrastructure

Built from nothing: the unit-test framework and suite, the smoke harness with pixel
assertions and `[SMOKE] key=value` markers, the CI workflow, the shadow-map
readback probe, the constant-ring instrumentation, and the GPU-side draw-index
witness.

---

## 3. Defects found, and the ones worth remembering

Most were found by **measuring**, not by reading.

**The shadow pass allocated constants from the wrong frame's ring.** `BeginFrame`
assigned the frame index and reset the ring offset, but the shadow pass runs
*before* `BeginFrame`. Its per-object constants landed in the previous frame's
buffer. It corrupted nothing only because the regions happened to be disjoint and
three frames in flight gave fence slack — an accident, not a property.

**The DXR frustum was a hardcoded literal.** `path_trace.hlsl` computed its rays
from `70.0f` under a comment reading "Must match camera FOV". It *did* match, which
is what made it dangerous: change the camera and DXR keeps tracing the old frustum
while raster uses the new one, quietly invalidating every raster/DXR comparison —
the project's main correctness oracle.

**A dead store in the static sampler setup.** The `staticSamplers[]` array was
built one line *before* `RegisterSpace` and `ShaderVisibility` were assigned, so
`s0` shipped with `ShaderVisibility = ALL` instead of `PIXEL`. Harmless — which is
exactly why nothing caught it.

**The constant ring capped the scene at ~341 entities.** Found by adding
instrumentation and then failing to explain a 5.6× gap between the raster and RT
peaks. Working backwards from the byte delta predicted 80 extra entities; the
growth test reports exactly 80. Benign — but it exposed a hard ceiling that
walkable interiors would exceed, and four cascades later pushed the measured
figure to 58% of a 75% gate.

**The path tracer had no texture LOD at all.** Every sample was
`SampleLevel(..., 0)`. Found while chasing a raster/DXR divergence I had just
introduced by enabling anisotropic filtering. The obvious fix — make the DXR
sampler anisotropic too — would have been **inert**, because anisotropy selects
across mips and mip 0 is forced. That is the same mistake the raster side had made.

**Several of my own.** A stale comment claiming `CBPerFrame` was 112 bytes when it
was 176. Two contradictory wrong DWORD counts in one function. A `HighWater()`
staleness guard I introduced by mechanically translating a predicate without
re-deriving whether it still meant anything. Reading `tail`'s exit code through a
pipe instead of git's, and reporting a push as successful when it had not
happened — twice.

---

## 4. The through-line: assertions that cannot fail

The single most repeated finding, across my work and Codex's, is verification that
looks like verification and is not.

**An assertion nobody has watched fail is not evidence.** Concretely, from this
session:

- Deleting the entire shadow pass broke **nothing** any test could see. The map
  stays at its cleared value, every pixel reads fully lit, the frame looks
  plausible, and every test and both smoke modes passed. That is why the shadow
  readback probe exists — and it was verified by deleting the caster draw and
  watching the marker flip to `no`.
- A poison sentinel compared against `-4.31740643e+08` when `0xCDCDCDCD` is
  actually `-4.31602080e+08`, so a "every byte must have been written" loop was
  trivially true for all inputs. The proof of the fix is the good kind: break
  `WriteObjectRecord` so the explicit assertions still pass, watch the new form
  fail, then swap the **old** loop back in against the **same** break and watch it
  report `[ OK ]`.
- A 75% constant-ring gate could not fire on the regression it existed to catch:
  restoring all pre-change traffic reached 7% of capacity.
- A test asserted properties of a `for` loop written inside its own body and
  touched no production code.
- Codex found a design-blessed cascade assertion with no teeth — coverage fraction
  descending per cascade, when all four read 1.0000 — and **deleted it rather than
  ship it green**.
- Reallocation coverage: the first watched-failure attempt left `-RasterOnly`
  completely passing with the fence guard deleted outright, because growth churn
  sat behind an RT-only gate and raster only performed the frame-zero grow. Direct
  proof that *a startup grow is not coverage*.

**A gate that trains people to disable it is worse than no gate.** The golden
capture gate was mis-calibrated twice in one round and its failure message invited
the reader to update the constants — which, on a real regression, retires the only
check pinning a GPU layout. It was **deleted, not recalibrated a third time**,
because it measured the right invariant through the wrong observable: capture
statistics depend on which assets sit in `build/<Config>`, which is build output,
not tracked source.

Its replacement asserts the invariant directly: the vertex shaders write the object
record id they **actually loaded** into a UAV. Writing the loaded value rather than
the root constant is load-bearing — a witness of the constant is a witness of
itself, and would stay perfect while the shader read `objectBuffer[0]`. Proof it is
environment-independent: main reports 18/18 witnessed records because the corridor
asset is loaded, a clean checkout reports 17/17, and both pass. That exact
difference is what broke the old gate.

---

## 5. Working alongside Codex

Two agents, one repository. What actually happened:

- **The glTF importer collided.** I wrote a lane split claiming asset import
  without checking Codex's already-published `codex/gltf-asset-pipeline` claim,
  under which the importer was already finished. **My error.** Codex resolved it
  correctly — kept its complete work, deferred to my Meshy client, discarded its
  own duplicate. I killed my duplicate workflow.
- **I reviewed Codex's importer** rather than trusting green, and it passed: the
  handedness winding flip *and* the tangent handedness sign are both pinned by a
  mirror/non-mirror test pair that fails if either breaks. I fuzzed it myself —
  the real corridor GLB parses correctly and four corrupted copies (truncation,
  bad magic, oversized JSON length, `0xFFFFFFFF` BIN length) are all rejected
  cleanly with no crash.
- **Codex implemented shadow cascades** from a design I published, on my branch.
  Reasonable collaboration; also my declared lane.
- **Shared build directory corrupts builds.** Concurrent `cmake` produced
  `C1041` PDB-contention errors. Transient, but it presents as a compile error.
- **Currently blocked**: an unresolved merge of
  `codex/per-object-buffer-integration` sits in the shared checkout — a third
  duplication, this time of the structured-buffer work. I have not touched it, and
  have committed nothing, because any commit would silently complete that merge.

**Lesson:** a lane claim is only safe if checked against the other agent's
*published claims*, not against the branch list. Workflow-based work creates no
branch until an agent commits, so "no branch exists" is not evidence that no work
is in flight.

---

## 6. Deliberately not done

- **Emitters are not light sources.** Emissive surfaces shade themselves; nothing
  samples them. A bright panel does not illuminate the room.
- **No image-based lighting.** The first real textured asset renders near-black
  because its `metallicFactor` is 1.0 — a fully metallic surface has no diffuse
  term and nothing to reflect. Correct behaviour, and the concrete demonstration
  that generated PBR content stays wrong until IBL exists.
- **Normal-map filtering is unsolved.** Ray-cone LOD moved DXR's mean luminance
  *away* from raster, isolated to the normal map: filtering averages tangent-space
  normals toward flat and discards sub-pixel variance acting as roughness. Raster
  has the identical defect. The fix is Toksvig or LEAN/CLEAN mapping.
- **SM 6.6 bindless**, blocked on heap consolidation — all three designs reviewed
  came back "fixable, not sound".
- **Reference-image testing.** Raster captures differ by up to 109 per channel
  between two runs of the *same* build, because the scene animates on wall-clock
  dt. That noise floor invalidates naive golden-image comparison.

---

## 7. Open items

| # | Item | State |
|---|---|---|
| — | Reconcile `codex/per-object-buffer-integration` with main | **blocks everything**, needs a human decision |
| 12 | Ray-cone texture LOD | done on `claude/rt-texture-lod`, unmerged |
| 18 | Image-based lighting | not started; most visible remaining gap |
| 22 | Witness the material index; gate the witness's per-vertex cost | not started |
| — | Toksvig/LEAN normal-map filtering | not started |
| — | Emitters as light sources (NEE over emitters) | not started |

---

## 8. Numbers worth keeping

| Measurement | Value |
|---|---|
| Constant ring, before | 768 bytes per shadowed entity → ~341 entity ceiling |
| Constant ring, after | 1792 bytes **flat**, ~1% of capacity |
| Ring with 4 cascades, pre-fix | 58% of a 75% gate on the 97-entity run |
| Texture tables | 128 raster / 64 per DXR channel ≈ 32 PBR materials |
| Raster capture noise floor | up to 109 per channel, same build, consecutive runs |
| RT capture noise floor | 1 per channel |
| Distant-ground contrast vs raster | 4.876 → 1.694 (raster 1.582) after ray cones |
| First generated asset | 15,562 verts / 19,193 tris, 8.7 MB GLB + ~12 MB textures, 15 credits |
