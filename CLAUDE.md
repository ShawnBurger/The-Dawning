# CLAUDE.md — The Dawning V3 V3 Engine (Layer-by-Layer Rebuild)

## PROJECT

The Dawning is a D3D12 space game engine built from scratch in C++20.
It is a fresh rebuild taking a layer-by-layer approach: each layer is brought
up far enough for the next one to stand on, then revisited as needed.

Verification is a unit-test target covering core math and ECS invariants plus
one end-to-end smoke test (`tools/smoke_test.ps1`). That is not comprehensive
coverage — most of the render and DXR layers are exercised only by the smoke
test, and there is no CI. Read "DONE" in the roadmap below as "runs, and later
layers depend on it", not as "finished, hardened, or fully tested".

## CURRENT STATE: Layer 3+RT (ECS + Full Path Tracing)

Layer 3 provides ECS architecture; the RT extension adds full DXR path tracing:
- Sparse-set ECS with generational entity IDs, type-indexed component pools
- Handle-based ResourceManager for meshes and materials
- Scene class with phase-based update/render systems
- DXR 1.1 ray tracing pipeline:
  - BLAS built per unique mesh (PREFER_FAST_TRACE, opaque geometry)
  - TLAS rebuilt every frame from ECS entity transforms
  - State object: ray generation + primary closest-hit + shadow hit + 2 miss shaders
  - Shader Binding Table with proper 64/32-byte alignment
  - Global root signature, 10 parameters, 18 of 64 DWORDs: TLAS SRV (t0),
    output UAV table (u0-u1), per-frame CB (b0), material StructuredBuffer (t1),
    triangle normals (t2), instance metadata (t3), triangle UVs (t4), triangle
    positions (t5), an SRV table for albedo (t0/space4), normal (t0/space5),
    ORM (t0/space6), emissive (t0/space7) textures and the prefiltered
    environment cube (t0/space8), and a root UAV for the IBL consumption probe
    (u0/space4). Two static samplers: s0 for material textures, s1 (trilinear,
    CLAMP) for the environment cube
  - Per-instance material lookup: global material StructuredBuffer indexed by
    InstanceID(). This is a root SRV, not SM 6.6 bindless — no
    ResourceDescriptorHeap anywhere in the project
- Path tracing shader (path_trace.hlsl):
  - Multi-bounce iterative path tracing (max bounces driven by the quality mode)
  - Cook-Torrance BRDF (GGX NDF, Smith geometry, Fresnel-Schlick) for direct
    lighting under NEE
  - Next Event Estimation with shadow rays (ACCEPT_FIRST_HIT optimization)
  - Cosine-weighted hemisphere sampling for diffuse bounces
  - Russian roulette path termination. The estimator is still not unbiased:
    throughput is hard-clamped to 4.0 (path_trace.hlsl) and samples are
    firefly-clamped to a fixed luminance ceiling before accumulation
  - PCG hash RNG, each dimension hashed in sequence. Seeded from a wall-clock
    dispatch counter (`seedIndex`), NOT the accumulation index — the two are
    separate fields precisely because the accumulation index resets on camera
    motion and would otherwise pin every moving frame to one random sequence
  - Temporal accumulation: progressive running mean weighted 1/(n+1) over the
    accumulation index, which the CPU resets to 0 on any camera or quality
    change. Equal weight per frame, so variance falls as 1/n while the view is
    still. No reprojection — a moving camera restarts accumulation
  - Firefly clamping against a fixed per-sample luminance ceiling, applied before
    the sample enters the mean. NaN/Inf samples are dropped rather than
    propagated into the history buffer
  - Indirect specular uses GGX VNDF importance sampling; with a separable Smith
    G the Monte Carlo weight reduces to F * G1(NdotL). Rays sampled below the
    horizon terminate the path
- DXC shader compiler integration (dxcompiler.dll loaded on-demand for lib_6_3+)
- Dual render mode: F1 toggles between rasterization and path tracing
- RT output copied to back buffer (future: DLSS Ray Reconstruction)
- Title bar shows render mode + FPS + entity count

## BUILD

All commands run from the repository root. Do not hardcode an absolute path —
the repo is relocatable.

```batch
SETUP_AND_BUILD.bat
```

`SETUP_AND_BUILD.bat` locates `cmake` on `PATH`, then falls back to the common
Visual Studio 2022 bundled locations, and runs the two steps below. Manual
equivalent:

```batch
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

Output: `build\Debug\TheDawningV3.exe`. Runtime log: `build\Debug\TheDawning.log`.

Smoke test (after building, from the repository root):

```batch
tools\smoke_test.cmd
```

## LAYER ROADMAP

Layer 1: Kernel (DONE) — window, D3D12, timer, input, math, DRED, capabilities
Layer 2: Geometry Pipeline (DONE) — shaders, PSO, mesh upload, camera, lit scene
Layer 3: Core Architecture (DONE) — ECS, resource manager, entity-driven rendering
Layer 3+RT: Path Tracing (DONE) — DXR pipeline, BLAS/TLAS, multi-bounce path tracer
Layer 4: Material System (PARTIAL) — see below. README.md's "Layer 4 material
         path" refers to this partial state, not a finished layer.
  Done:    KTX v1 / PNG (WIC) / DDS loaders with procedural fallbacks, CPU mip
           generation for RGBA sources, albedo + normal maps, Cook-Torrance GGX
           raster shading, indexed shader-visible texture tables in both the
           raster and DXR paths, packed occlusion/roughness/metallic (ORM) maps
           in the glTF channel convention modulating the material scalars in
           both paths, emissive maps in both paths, bloom with a parameterised
           exposure, and a linear
           R16G16B16A16_FLOAT scene target resolved to the back buffer by a
           dedicated tone-map pass
           (shaders/tonemap_ps.hlsl). Tone mapping happens exactly once, not per
           material shader; post-process passes insert between
           Scene::RenderEntities and Renderer::ResolveToBackBuffer
           Directional shadows are CASCADED: 4 slices of one 2048x2048
           Texture2DArray (64 MiB), fixed half-extents {24, 65, 175, 470},
           selected radially in the pixel shader, with a world-anchored
           double-precision texel snap so distant edges do not crawl as the
           camera translates. Reach is ~448 world units, up from 24. Cascade 0 is
           bit-identical to the old single cascade, so the near field is
           unchanged. Cascades cost NOTHING per entity in the constant ring:
           per-object and per-material data live in structured buffers indexed
           by a root constant, and the light view-projection is a per-PASS
           cbuffer, so each cascade costs one flat 256-byte upload and the four
           cascades share one set of object records. Ring peak is 1792 bytes,
           flat, in both smoke modes
           BOTH smoke modes validate draw records ON THE GPU through one merged
           probe at u0/space4, 16 bytes per object record. Each slot carries two
           independent claims: a HASH of every field of the record the shader
           loaded, which catches CPU/HLSL layout drift, and a MARKER read out of
           the record's own recordId field, which catches wrong-element
           indexing. Neither covers the other - shifted garbage still comes from
           the right element, and two records with equal contents hash equal.
           The CPU reads the buffer back after queue retirement, compares
           against the mapped upload records, and reduces to per-pass record,
           distinct-index and mismatch counts so a failure names a shader.
           WHICH STAGE WRITES WHAT IS THE LOAD-BEARING PART. Both vertex shaders
           write the object words, because the vertex stage is where
           objectBuffer is consumed; basic_ps writes the material words, beside
           the `mat` it shades with, because the pixel stage is where
           materialBuffer is consumed. Hashing the material record from the
           vertex shader - which reads it for no other purpose - witnesses the
           probe's own load and stays green with basic_ps hardcoded to
           materialBuffer[0]. All four mutations are watched failing: object
           indexing in each vertex shader, material indexing in the pixel
           shader, and the deferred-release fence tagging.
           BOTH GPU PROBES - the draw-record probe and the SHADOW-MAP probe -
           run on the LAST RASTER FRAME of the run, not the final frame, and
           that frame is decoupled from the CAPTURE frame. The final frame is
           path-traced in the default mode, which runs none of the three raster
           shaders and skips the shadow pass entirely, so probing there gave the
           default run zero probe coverage - assertions that existed and were
           never reached. In the default mode the probe frame is now frame 14,
           ahead of the RT switch and inside the frame 8..16 growth churn, so
           the draw probe also reads a buffer that has already been grown and
           deferred-released mid-run. The capture stays on the end frame and
           still asserts the default mode's final path-traced image - the frame
           an image is taken on and the frame correctness is verified on are
           separate concerns, and separating them is what closed the hole.
           The shadow probe was the second half of this fix and lagged the
           first: after the draw probe moved, every shadow and cascade assertion
           was still armed on the capture frame, with the harness's
           `if ($RasterOnly)` gate documenting the hole rather than closing it.
           MEASURED before the fix, the default run emitted ZERO
           shadow_map_written and ZERO shadow_cascade_* markers. Now both modes
           emit all sixteen and the gate is gone. Watched failing IN THE DEFAULT
           MODE: pinning BeginShadowCascade's CBPerPass to m_lightViewProj[0]
           flips shadow_cascade_depths_distinct to "no" and fails the default
           run, where the identical mutation passed green before. probeShadow is
           gated on !renderedPathTracing - the same predicate that selects the
           render branch - not on !m_usePathTracing, so it runs exactly when
           there is a shadow pass to observe.
           A third b3 constant gates the writes to that frame, so ordinary
           frames pay no per-invocation witness cost. The UAV DECLARATION in
           basic_ps is GATED TOO, behind DAWNING_DRAW_PROBE: declaring a UAV in
           a pixel shader defeats early-Z for the whole PSO, and a runtime flag
           cannot buy it back, because it is the declaration and not the write
           that marks the shader side-effecting. So Renderer::CreatePSO compiles
           basic_ps twice - m_pso without the UAV for every real frame, and
           m_psoDrawProbe with it, bound only on the probe frame. The main
           opaque pass keeps early-Z in Release and the probe is still compiled
           into shipping builds; the cost is one extra PSO. The vertex shaders
           are not permuted - early-Z is a pixel-stage property
           IMAGE-BASED LIGHTING IS AT STAGE 4 OF SIX
           (docs/research/IBL_DESIGN.md section 11). Stages 1-3 are done in the
           RASTER path and Stage 4 has landed in the DXR STABLE PREVIEW.
           STAGE 4 IS WHERE F1 STOPPED CHANGING THE LIGHTING MODEL. The DXR
           stable preview ran an ad-hoc environment with a magic 2.5 diffuse
           multiplier, a magic 0.25 bounce damper, a mirror reflection that
           ignored roughness entirely, and a gloss ramp with no physical basis -
           so toggling F1 changed how surfaces respond to the environment, not
           just which renderer drew them. It now calls the SAME
           shaders/ibl_common.hlsli basic_ps.hlsl calls, against the same cube
           and the same nine SH coefficients from the same EnvironmentIBL. All
           four constants are gone. The bounce damper survives deliberately
           (IBL_DESIGN.md 8.2): the preview traces no secondary bounce, and
           removing it is an appearance change that would make any luminance
           shift unattributable in the same stage.
           THE FULL PATH TRACER IS UNTOUCHED, deliberately. It samples
           DawningSkyRadiance on every miss - it evaluates the very integral the
           split-sum approximates - so substituting the prefiltered cube would
           replace the reference with the approximation, and
           ASSET_PIPELINE_SPEC makes DXR the reference. Terminating the last
           bounce into the cube is rejected on the same ground plus a second
           one: it biases the estimator, and this project names its two biases
           (throughput clamp 4.0, firefly clamp) rather than accumulating
           unlisted ones. PROOF THAT THE BLAST RADIUS IS WHAT IT CLAIMS: the
           raster and DXR-FULL captures are BYTE-IDENTICAL across this change.
           Only DXR stable's image moved.
           CONVERGENCE, MEASURED. Whole-frame mean luminance |DXR-stable - X|
           before -> after: vs raster 5.499 -> 1.163, vs DXR-full 7.000 ->
           0.339. On the corridor region: vs raster 2.944 -> 1.057. DXR stable
           moved decisively toward raster on both, and toward the DXR-full
           reference on the whole frame.
           IT MOVED AWAY FROM DXR-FULL ON THE CORRIDOR, 22.608 -> 24.496, and
           that is not a regression - it is an INHERITED, already-named gap. The
           corridor now agrees with raster to 1.057, so it inherits raster's own
           overshoot against the reference, which CLAUDE.md already records
           (15.57 -> 16.07) with the same cause: the split-sum has no visibility
           term, the Meshy asset ships no occlusion map, and the corridor is
           concave. Specular occlusion is IBL_DESIGN.md 9.3's deliberately
           deferred item. Converging DXR stable ONTO raster necessarily
           transports raster's known limitation with it.
           COSTS, COUNTED FROM THE TREE rather than taken from the design.
           RTPerFrameConstants 212 -> 384 with a MANDATORY pad0[3] at 212..223:
           HLSL rounds the float4 SH array to offset 224 and C++ would place it
           at 212, a 12-byte shear with no compile error on either side that
           would produce a smooth, plausible, wrong ambient. Three static_asserts
           pin sizeof==384, offsetof(iblSH)==224 and offsetof(iblParams)==368;
           deleting pad0[3] fails all three at BUILD time, watched.
           RT descriptor heap 258 -> 259 per frame slot. DXR root DWORDs 16 ->
           18, and the split matters: the env cube costs ZERO, because it is a
           fifth range inside the table root parameter 8 already binds, and both
           DWORDs go to the consumption probe's root UAV at u0/space4. Exactly
           the trade the raster path made at 16 -> 18 for the same probe.
           TWO DESIGN CLAIMS WERE WRONG AND ARE CORRECTED IN PLACE. The design
           gave the cube OffsetInDescriptorsFromTableStart = 258; the table is
           bound at the ALBEDO base (heap index 2), not the heap start, so the
           table-relative offset is 256 - the design's value indexes one past the
           end of a 259-entry heap. A static_assert now pins the relation. The
           design also attributed the +2 DWORDs to a Stage 5 numeric-agreement
           probe at u0/space9; they were needed at Stage 4, for a consumption
           probe, at u0/space4. The 16-DWORD baseline and the 258 -> 259 heap
           figures were both verified CORRECT.
           DXR NOW HAS CONSUMPTION EVIDENCE, which it had none of before. Same
           pattern as the raster probe and reduced by the SAME SHIPPED
           ReduceIBLConsumeProbe on the same 64-byte block, so the two paths'
           markers are the same quantities computed the same way.
           path_trace.hlsl writes what it ACTUALLY LOADED and ACTUALLY ADDED into
           a root UAV, AT BOUNCE 0 ONLY - where throughput and the damper are
           both exactly 1, so the numbers are directly comparable to raster's and
           the cubeSamples == shadedPixels equality holds by construction.
           MEASURED live: spec 0.322357, diffuse 0.287125, in-final 0.322540,
           sky_rel_err 0.001007 over 10.19M primary path vertices.
           IT RUNS A NEGATIVE CONTROL DISPATCH, one frame ahead of the live one,
           with iblParams.z forced to 0: every word asserted zero while 10.19M
           vertices are still shaded. Both verdicts are demanded.
           THE DIFFUSE MAXIMA AGREE EXACTLY - raster 0.287125, DXR 0.287125,
           delta 0 - and the harness asserts it. That is the cross-path
           assertion: the diffuse term rides the constant buffer, so it is what
           the pad0[3] shear would corrupt, and no probe verdict would notice.
           WATCHED FAILING, each broken -> watched failing -> restored -> green:
             (a) stable preview stops sampling the environment ->
                 rt_ibl_consume reached/consumption/identity ALL fail,
                 0.322357/0.287125/0.322540 -> 0. Every startup marker AND the
                 entire raster consumption probe STAY GREEN, which is the gap
                 measured rather than argued
             (b) bind the wrong descriptor as the DXR env cube ->
                 rt_ibl_consume_identity fails, sky_rel_err 0.001007 -> 1.000000,
                 spec -> 0, while DIFFUSE STAYS AT 0.287125 because it touches no
                 descriptor. Raster stays green
             (c) neuter RTEnvironmentInputs::disabled -> the CONTROL fails and
                 the live verdict stays green, the pair doing its job
             (d) delete pad0[3] -> three static_asserts fail at build time
             (e) feed the DXR path different SH (x0.9) -> the raster/DXR
                 agreement assertion fails at delta 0.005051 while EVERY probe
                 verdict stays ok, which is the gap that assertion exists for
           The probe is armed ONLY in the default stable mode. Under -RasterOnly
           there is no dispatch and under -FullQuality the branch does not
           execute, so the harness asserts the markers are ABSENT in both -
           a probe armed on a mode it cannot observe would read an empty block
           as a pass.
           A HARNESS ASSERTION WAS WRITTEN DEAD AND WAS CAUGHT. The raster/DXR
           agreement check was first wrapped in a bare `{ ... }`, which in
           PowerShell is a script-block LITERAL, not a scope: constructed,
           discarded, never executed. It was noticed because its Write-Host line
           never appeared in the output. Same never-reached-assertion failure
           this file documents for the shadow and draw probes, reproduced in the
           harness itself.
           STAGE 3 IS WHERE THE IMAGE CHANGED. basic_ps.hlsl's hemisphere ambient
           (ambientDiffuse / ambientSpecular) is DELETED - not scaled down, which
           would have been a straight double count - and replaced by L2 spherical
           -harmonic diffuse plus split-sum specular. The near-black Meshy
           corridor is FIXED: its region mean luminance goes 66.96 -> 98.59 in
           the raster capture. Forcing the iblParams.z kill switch to 0 sends the
           same region to 0.148, i.e. black, which is both the proof that
           basic_ps really reaches the new code and a demonstration of the
           original defect - a glTF metallicFactor of 1.0 has no diffuse lobe, so
           with no environment term there is literally nothing to shade it with.
           AGAINST THE DXR REFERENCE, per region, |raster - DXR-full| before ->
           after: convex metal sphere 28.25 -> 3.63, near ground 13.82 -> 2.75,
           far ground 5.01 -> 1.83. Raster moved decisively toward the reference
           wherever environment VISIBILITY is not the dominant term.
           It did NOT on the corridor: 15.57 -> 16.07, undershoot turned into
           overshoot of almost the same size. That is not a mystery and it is not
           a regression - the split-sum has NO visibility term, the Meshy asset
           ships no occlusion map (base_color/metallic/roughness/normal only), and
           the corridor is concave, so raster gives it full unoccluded
           environment specular where the path tracer traces the actual geometry.
           Specular occlusion is IBL_DESIGN.md 9.3's named, deliberately deferred
           item and this is what deferring it costs.
           Diffuse costs ZERO descriptors - nine RGB coefficients in CBPerFrame,
           projected on the CPU by core::ProjectSkyRadiance. Specular costs ONE
           root DWORD, and the evidence that it is USED costs two more: raster
           root signature 15 -> 16 -> 18, root parameter 9 being the consumption
           probe's UAV. That is the correct trade - without it every IBL
           assertion passes with the cube unsampled and with the wrong descriptor
           bound. Root parameter 8 binds
           the cube at t0/space6 with a third static sampler s2 (trilinear,
           CLAMP). Both hand-written root-signature branches changed identically.
           The design attributed that DWORD to Stage 1; it belongs here, because a
           descriptor costs a root DWORD only when something BINDS it.
           CBPerFrame 416 -> 576, appended only, so sky_ps.hlsl's frozen prefix is
           untouched. Ring peak 1792 -> 2048 against a budget that reads
           cb_per_frame_bytes and is now 2304. The budget stays FLAT in entity
           count, which is the property it exists to protect.
           FOUR STARTUP GPU ASSERTIONS, in every mode, gated on nothing, all
           watched failing: SH basis agreement, mirror specular, env-BRDF
           physical bounds, roughness->mip monotonicity.
           THE ONE THAT MATTERED MOST is ibl_sh_agreement. core::SHBasisL2 and
           DawningSHBasisL2 are nine expressions written twice, in two languages.
           WATCHED: negate y[1] in the HLSL only and all 161 CPU tests pass while
           the two genuinely disagree; only this probe fails. Note that negating
           the C++ basis instead changes NOTHING, because it is used by both the
           projector and the evaluator and cancels - which is the property
           IBL_DESIGN.md section 4 relies on, measured rather than assumed.
           WHAT THOSE SIX DO NOT COVER, AND WHAT NOW DOES. They witness
           ibl_common.hlsli - the same header basic_ps includes - but NOT
           basic_ps's CALL SITE. MEASURED: deleting the IBL block from basic_ps
           leaves every one of them green, and so does BINDING THE WRONG
           DESCRIPTOR at t0/space6. That is this repo's recurring failure, hit
           for the fourth time: the shadow probe, the draw probe, Stage 1's
           direction probe, and now these.
           A SEVENTH PROBE CLOSES IT, and it is a different kind of evidence:
           the IBL CONSUMPTION probe (src/render/ibl_consume_probe.h,
           shaders/ibl_consume_probe.hlsli). basic_ps writes what it ACTUALLY
           LOADED and what it ACTUALLY ADDED into a UAV at u1/space4 - the same
           pattern the draw-record probe uses, on the same probe frame, through
           the same PSO permutation and root-constant gate, so there is no
           second frame schedule to get wrong. Three independent claims:
             CONSUMPTION  envSpecular, envDiffuse, and the environment as it
                          landed in finalColor (recovered as
                          finalColor - direct - emission, so DELETING the terms
                          from the combine is caught, not just zeroing them)
             IDENTITY     the cube's own mip-0 fetch along the SHADING
                          reflection vector against the closed-form sky. Worst
                          relative error MEASURED 0.000977 against a 2% bound
             LIVENESS     cubeSamples == shadedPixels == 1.49M, not > 0
           AND IT RUNS A NEGATIVE CONTROL FRAME. This is the part that answers
           "every assertion passes with the feature absent": the frame before
           the verify frame renders with CBPerFrame::iblParams.z forced to 0 and
           asserts every word reads ZERO while pixels are still being shaded.
           The harness demands BOTH verdicts. An assertion shown only to pass
           with the feature present is what all six above already were.
           WATCHED FAILING, each broken -> failing -> restored -> green:
             (a) zero envSpecular/envDiffuse at the point of use ->
                 ibl_consume_consumption=failed, spec/diffuse/in-final 0.343292/
                 0.287125/0.343384 -> 0. Every startup marker stays green
             (b) bind heap slot 1 (the shadow map) as the environment cube ->
                 ibl_consume_identity=failed, sky_rel_err 0.000977 -> 1.000000,
                 radiance and mirror maxima -> 0. ibl_sh_agreement,
                 ibl_spec_mirror and ibl_env_brdf ALL STILL PASS, which is the
                 gap measured rather than argued
             (c) sample the cube on only part of the screen -> reached=failed
                 on cubeSamples 65750 != shadedPixels 1491649, while consumption
                 AND identity both stay ok. A `cubeSamples > 0` check passes here
             (d) neuter SetIBLDisabledForFrame -> the CONTROL fails and the live
                 assertion stays green, which is the pair doing its job
           Plus nine CPU cases over the GPU-free reduction, including one that
           asserts the two claim sets reject each other's blocks (171 tests, up
           from 161).
           A COMMITTED CLAIM WAS FALSE AND IS CORRECTED. tools/smoke_test.ps1
           recorded "WATCHED NOT FAILING: adding 1.0 to the mip leaves both this
           and the mirror assertion green". RE-MEASURED by performing the
           mutation: ibl_spec_mip_monotonic FAILS, worst backward step
           -0.00098392 against -1e-5, deterministically, identical in both smoke
           modes. Half the claim was true - ibl_spec_mirror does stay green at
           0.001768 - and it was wrongly extended to the other assertion. The
           mechanism: kEnvMipSweepLastSlot excludes the 2x2/1x1 faces' luminance
           reversal under the CORRECT mapping, and +1 drags that reversal into
           the asserted window, so the exclusion range and the detection are the
           same fact. Corrected in smoke_test.ps1 and environment_ibl.h.
           TWO DESIGN CLAIMS REMAIN MEASURED FALSE AND CORRECTED IN PLACE:
           assertion 3.1's furnace bound is unsatisfiable (it contradicts section
           9.4's accepted 55% single-scatter loss at roughness 1); and 3.3 does
           not catch a WRAP sampler, because cube sampling never consults the 2-D
           address modes. 3.3 also does not catch the mip off-by-one (0.0010 ->
           0.001768, inside 2%) - only 3.2 does.
           THE ENV-BRDF ENERGY GUARD IS WIDER THAN IT LOOKED. It bounded A + B,
           which is the F0 = 1 case, and was blind to the dielectric: MEASURED,
           0.04*A + B reaches 1.042432 at roughness 0 and grazing incidence, i.e.
           a smooth dielectric reflects 4.2% MORE environment energy than
           arrives. That is the documented accuracy of the Lazarov fit at the
           worst corner of its domain, not a defect, so the new
           ibl_env_brdf_max_dielectric assertion bounds how far the FIT may stray
           (1.06) rather than claiming physics it does not obey. An A/B swap
           sends it to 1.29.
           A FAILED IBL VALIDATION IS NOW FATAL. It was not, justified by "a
           resource that nothing samples yet" - a clause Stage 3 deleted. There
           is no hemisphere-ambient fallback to degrade to any more, so booting
           means shading every surface from an environment the engine just proved
           wrong, silently, because that reads as "the tone mapping needs
           tuning". It also removes a split where the harness treated the same
           evidence as fatal and the engine did not.
           core::EvaluateIrradiance NOW CLAMPS, like DawningIrradianceSH always
           did. The two were compared to 1e-4 by the SH agreement probe while
           being different functions; it passed only because THIS sky never
           drives the reconstruction negative, which made a property of the
           environment look like a property of the code.
           EvaluateIrradianceRaw exposes the unclamped reconstruction for the
           closed-form identity cases, and one CPU case pins the difference.
           Built: a 128x128x6 R16G16B16A16_FLOAT cube, 8 mips, roughness =
           mip/7, prefiltered from the PROCEDURAL sky. The source is closed-form,
           so the prefilter pass takes NO SRV INPUT - every mip is an independent
           integral of ground truth rather than of a downsampled copy of the one
           above it, which removes the ping-pong and one descriptor. Baked at
           startup behind a REVISION COUNTER, not a bool, so a day/night cycle is
           an edit rather than a redesign.
           Cost: ONE descriptor. The cube's SRV takes raster heap slot 2 and the
           material allocator's firstIndex moved 2 -> 3, so usable material slots
           are 126 -> 125. The prefilter has its own separate root signature.
           VERIFICATION RUNS ON EVERY LAUNCH IN EVERY MODE, gated on nothing.
           The prefilter happens once at startup, before the raster/RT branch
           exists, so there is no frame to arm it on and no mode that can skip
           it - which is the cheapest possible answer to the failure the shadow
           and draw probes were both fixed for. Five GPU assertions, each
           watched failing: the descriptor reservation (reported from the
           allocator's LIVE firstIndex, since nothing samples the cube yet and a
           slipped reservation would otherwise surface two stages later); a
           direction ROUND TRIP that catches any permutation, sign flip or
           v-flip in the cube face table; a SKY AGREEMENT probe comparing the
           shipped HLSL against core::SkyRadiance; per-mip mean luminance; and
           per-mip variance falling with roughness.
           The sky-agreement probe is the one that mattered most. It closes the
           gap src/core/sky_radiance.h documents in its own header - that the
           hash tripwire "pins agreement in TIME, not in VALUE". WATCHED: edit
           sky_common.hlsli, re-pin kPinnedHash to match, leave the twin alone,
           and all 156 CPU tests pass while the HLSL and the twin genuinely
           disagree. Only this probe fails.
           Two vacuity guards, because a comparison neither side reached is not a
           comparison: the probe target is cleared to a -1 poison and the shaders
           write w=+1, so the written-slot count is asserted before the values
           are; and the 64 query directions are asserted to cover all six
           (dominant axis, sign) buckets, because a direction set degenerated to
           64 copies of one direction round-trips PERFECTLY while testing one
           sixth of the face table. Both were watched failing.
  Not done: SM 6.6 bindless (raster still compiles vs_5_1/ps_5_1 through FXC)
           and any real mesh file loading. Emissive surfaces shade
           themselves but are NOT light sources - nothing samples them, so a
           bright panel does not illuminate anything around it.
           `assets/textures/` ships only a README, so a clean clone always takes
           the procedural fallback path
Layer 5: World Foundation — terrain, atmosphere shader, sky dome, camera-relative

## RT UPGRADE PATH (future)

Phase 1 (current): Path tracing with NEE, VNDF specular sampling, and a
         progressive running-mean accumulator that resets on camera motion.
         Not yet done in this phase: reprojection, so accumulation restarts
         whenever the view changes rather than reusing history. Accumulation
         also does not reset on OBJECT motion, only camera motion, so animated
         geometry ghosts across the history buffer. That is deliberate - the
         demo spins continuously, and resetting per object would disable
         accumulation outright - but it is why moving objects look smeared in
         path tracing. Reprojection is the actual fix
Phase 2: NVIDIA RTXDI (ReSTIR DI) for efficient direct light sampling
Phase 3: ReSTIR GI for indirect illumination resampling
Phase 4: SER (Shader Execution Reordering) via NvHitObject/NvReorderThread
Phase 5: DLSS 3.5 Ray Reconstruction (replaces manual denoising + upscaling)
Phase 6: DLSS Frame Generation for 2x effective FPS
Phase 7: Opacity Micro-Maps for alpha-tested geometry (foliage, fences)

## RUNTIME DEPENDENCIES

- dxcompiler.dll + dxil.dll must be in the executable directory for RT shaders.
  Get from: https://github.com/microsoft/DirectXShaderCompiler/releases
  Or from the Microsoft.Direct3D.DXC NuGet package.

## RULES

1. ENFORCED — double precision (`Vec3d`) for world positions, `Vec3f` only after
   camera subtraction. `ecs::Transform::position` and `Camera::m_position` are
   `Vec3d`; `Scene` produces camera-relative float matrices for both raster and
   DXR. GPU camera position is local zero, while path-tracing history compares
   the full double-precision position. New world-space quantities must use
   `Vec3d`; never narrow an absolute world position before camera subtraction.
   ONE SANCTIONED EXCEPTION: `core::BuildShadowCascadeMatrix` takes the absolute
   `Vec3d` camera position. It uses it only to quantise the shadow texel lattice
   onto a grid fixed to the world origin, entirely in double, and narrows only a
   sub-texel RESIDUAL - never a position. The exception is necessary because
   quantising a camera-relative coordinate quantises nothing: the lattice would
   move with the thing being quantised, giving a snap that compiles, does not
   crash, and shimmers exactly as much as no snap at all. Two unit cases pin
   this, `Cascades_MatricesAreCameraPositionInvariant` (probed at 1e7) and
   `Cascades_SnapIsStableUnderSubTexelCameraMotion`.
2. Proper .h/.cpp split. No header-only implementations except tiny inlines/templates.
3. No copyrighted terms. Rename map: Starfleet→Vanguard, LCARS→HELIX, Phaser→Arc Lance,
   Photon Torpedo→Fusion Torpedo, Dilithium→Helion, Federation→Commonwealth, etc.
4. Always include <cstdint> when using uint32_t/uint64_t.
5. Compile after every change.
6. Fixed timestep for physics (accumulator pattern, default 60Hz).
7. LEFT-HANDED coordinate system everywhere. +Z is forward (into screen).
   LookAt is LH. PerspectiveFovLH maps Z to [0,1]. Back-face culling is CW.
8. Logger is NOT thread-safe. Call only from main thread until job system exists.

## PARALLEL AGENT RULES

Read `AGENT_COORDINATION.md` before editing. Codex and Claude Code must use
separate Git worktrees and named branches, inspect live repository state before
each pass, report branch/commit/files/tests at handoff, and check file overlap
before integration.
