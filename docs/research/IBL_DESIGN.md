# IMAGE-BASED LIGHTING — DESIGN

Status: **design only.** Nothing here is implemented. This document exists so the
implementation can be reviewed before it is written, and so the parts that are
guesses are labelled as guesses.

Every claim below is tagged. **VERIFIED** means I read it in the tree at the
commit this was written against and quote the file and line. **ASSUMED** means I
reasoned it out and did not confirm it against a compiler, a driver, or a run.
Anything untagged in a numeric table is VERIFIED.

---

## 0. Correction to the brief

The brief states `CBPerFrame` is 176 bytes. **It is 416.**

    src/render/renderer.h:139   static_assert(sizeof(CBPerFrame) == 416, ...)
    src/render/renderer.h:141   offsetof(lightViewProj)      == 112
    src/render/renderer.h:144   offsetof(cascadeSplitRadius) == 368
    src/render/renderer.h:145   offsetof(cascadeTexelWorld)  == 384
    src/render/renderer.h:146   offsetof(cascadeFadeLo)      == 400

176 was the size before four-cascade shadows landed; `docs/CLAUDE_SESSION_LOG.md`
§3 records a stale comment claiming 112 when it was 176, so this is the same
number drifting a second time. The *constraint* the brief states is exactly
right and is the one that matters: `shaders/sky_ps.hlsl:9-41` declares only bytes
0..111 and reads them by offset, so **fields may only ever be appended**. All
layout arithmetic below is against 416.

---

## 1. The problem, stated against the code

`shaders/basic_ps.hlsl:359-365` is the entire environment response of the raster
path:

```hlsl
float hemisphereBlend = N.y * 0.5 + 0.5;
float3 groundColor = ambientColor * 0.3;
float3 ambientDiffuse = baseColor * lerp(groundColor, ambientColor, hemisphereBlend)
                      * (1.0 - materialMetallic) * ambientOcclusion;
float3 ambientSpecular = DawningFresnelSchlick(NdotV, F0) * (ambientColor + 0.04) *
                         lerp(0.2, 0.8, 1.0 - materialRoughness) * ambientOcclusion;
```

What that actually is, term by term:

- `ambientDiffuse` is a two-colour hemisphere lerp on `N.y`, gated by
  `(1 - metallic)`. **At `metallic == 1.0` it is identically zero.**
- `ambientSpecular` is Fresnel at `NdotV` times a flat colour times a scalar
  ramp in roughness. It has **no directional dependence at all** — it does not
  know where the surface is looking. It cannot reflect anything, because nothing
  is being sampled. `ambientColor` is `{0.12, 0.14, 0.22}` (`src/app.cpp:176`),
  so the whole term is bounded by roughly `F · 0.26 · 0.8`.
- `ambientColor` is *not* the sky. `shaders/sky_common.hlsli` returns
  `lerp(float3(0.8,0.85,0.9), float3(0.3,0.5,0.9), t) * 0.5`, i.e. luminance
  ~0.40 at the horizon and ~0.24 at zenith. The ambient constant is roughly a
  sixth of that and a different hue. The surfaces and the background they sit
  against are lit by two unrelated constants.

So for the imported Meshy corridor, `metallicFactor = 1.0`:

    diffuse direct   = kD · albedo/PI · ...   with kD = (1-F)(1-metallic) = 0
    ambient diffuse  = ... · (1 - metallic)   = 0
    specular direct  = one directional light, one lobe
    ambient specular = F · 0.26 · lerp(0.2,0.8,1-r)   — a dim flat wash

Everything except one specular highlight is gone. The asset is near-black and
the renderer is behaving exactly as written. This is the concrete demonstration
`docs/CLAUDE_SESSION_LOG.md` §6 records, and IBL is what fills those two zeros.

**What IBL replaces:** `ambientDiffuse` and `ambientSpecular`, entirely. Both
lines are deleted. Nothing else in `basic_ps.hlsl` changes except the roughness
plumbing described in §9.

---

## 2. Choice 1 — environment source

### Recommendation: **prefilter the existing procedural sky.** Do not load an HDR.

The decisive argument is not "avoid shipping a file". It is that
`DawningSkyRadiance` is a *closed-form function of direction*, which changes the
shape of the whole feature:

| | Procedural sky | Loaded HDR/EXR |
|---|---|---|
| Source resource | **none** | equirect or cube SRV, 1+ descriptor slot |
| Conversion pass | none | equirect → cube, or a cube DDS loader |
| Mip *m* is computed from | the exact function | a downsampled copy of mip 0 |
| Prefilter reads | `DawningSkyRadiance(dir)` | `TextureCube.SampleLevel` while writing the same resource |
| Agreement with the DXR miss shader | **by construction** — one function, one file | requires the miss shader to sample the same texture |
| New loader code | none | EXR decode (absent) or DDS cube (partial) |
| Repo weight | 0 | 5–30 MB of binary in a public repo |

The middle rows are the ones that matter. With a source texture you must either
ping-pong (read mip *m*, write mip *m+1*) or bind the cube as both SRV and RTV,
and every mip inherits the filtering error of the one above it. With an analytic
source, **every mip is an independent integral of ground truth** and the prefilter
pass has no SRV input whatsoever — it is a pure function of the texel's direction.
That removes an entire class of bug and one descriptor.

The agreement row is the project's own rule doing the work.
`ASSET_PIPELINE_SPEC.md` says raster/DXR divergence is a defect, and
`shaders/brdf_common.hlsli:8-13` records that sky evaluation is one of the three
things that already diverged. Prefiltering the function both paths already call
means the environment cannot diverge; it can only be *approximated differently*,
which is the sanctioned relationship (§8).

### What this costs

The sky is a two-colour vertical gradient with no sun disc, no clouds, and no
horizon detail. Prefiltering it is, visually, close to prefiltering a constant.
Specular IBL will make the corridor legible and metallic rather than black, but
it will not make it *interesting* — there is nothing in this environment to
reflect. That is a real limitation and the honest framing is: this change fixes
the physics and leaves the art for later.

The upgrade path is deliberately cheap. Everything downstream of §3 consumes a
`TextureCube` + 9 SH coefficients and does not care where they came from. Loading
an HDR later means replacing the *contents* of the prefilter pixel shader and
nothing else — no root signature change, no cbuffer change, no shader-side
change. **Design the prefilter so `DawningSkyRadiance(dir)` appears in exactly one
line of one shader.**

### The one thing that must not be forgotten

`DawningSkyRadiance` has **no sun disc.** That is load-bearing for §8's
double-counting argument. If a sun is ever added to it, the directional light and
the environment become two representations of the same energy and one of them
must be removed. Write that in `sky_common.hlsli` when this lands, not here.

---

## 3. Choice 2 — specular IBL

### Recommendation: split-sum (Karis), with the **analytic env-BRDF approximation**, not a LUT.

Split-sum factors the specular environment integral into

    ∫ L(l) f(l,v) (n·l) dl  ≈  [ prefiltered radiance at (R, roughness) ] × [ F0·A + B ]

The first factor is a cubemap with roughness in the mip chain. The second is a
2-D function of `(NdotV, roughness)` — either a precomputed 2-channel LUT or a
polynomial fit.

**LUT vs analytic:**

| | Analytic fit (Lazarov) | Precomputed LUT |
|---|---|---|
| Descriptor slots | **0** | 1 (of 126 usable) |
| ALU | ~10 instructions | ~0 (one bilinear fetch) |
| New resource + upload + generation pass | no | yes |
| Accuracy | a few % over most of the domain; worst at low roughness + grazing (ASSUMED — published figure, not measured here) | reference |
| Extra thing that can be wrong | a magic-constant table | a generation pass, a format, a clamp-to-edge sampler, a slot |

Take the analytic version. `ASSET_PIPELINE_SPEC.md` "Known scaling limits" names
the 128-slot table as the binding constraint and
`src/render/renderer.cpp:784-786` states the policy in as many words: *"That heap
is the SCARCE budget… Spend DWORDs, not heap slots."* Ten ALU in a pixel shader
that already runs nine `SampleCmpLevelZero` is not a cost worth a descriptor.

The fit (Lazarov 2013, as used by Karis):

```hlsl
// Returns (A, B) for  specular = prefiltered * (F0 * A + B).
float2 DawningEnvBRDFApprox(float NdotV, float roughness)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f,  0.022f);
    const float4 c1 = float4( 1.0f,  0.0425f,  1.04f,  -0.04f);
    float4 r = roughness * c0 + c1;
    float  a004 = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
}
```

Single expression, no branches, so FXC's X4000 "potentially uninitialized"
analysis (`basic_ps.hlsl:192-196`) has nothing to complain about.

### The trap: this term uses a different Smith k

`shaders/brdf_common.hlsli:55-60` uses the **direct-lighting** remapping
`k = (r+1)²/8`. The split-sum BRDF integral — LUT or fit — is derived with the
**IBL** remapping `k = α/2 = r²/2`. Both are correct; they integrate different
things.

Consequence, and it must be a comment in the code: **`DawningEnvBRDFApprox` must
not be re-expressed in terms of `DawningGeometrySmithG1`.** They look
interchangeable and are not. A later "cleanup" that unifies them silently
darkens every rough surface's environment specular. `brdf_common.hlsli:50-54`
already carries a warning of exactly this shape about the VNDF weight; this is
the second instance of the same hazard.

### Cubemap parameters

| Property | Value | Why |
|---|---|---|
| Face size | 128 × 128 | The sky is a smooth gradient; 128 is generous. One constant, `kEnvCubeSize`. |
| Mips | 8 (128→1) | `roughness = mip / (mips-1)`, so mip 0 = mirror, mip 7 = fully rough. |
| Format | `R16G16B16A16_FLOAT` | Matches `Renderer::kHDRFormat` (`renderer.h:580`). Linear HDR, no encode/decode. |
| Memory | 6 · Σ(128²…1²) · 8 B ≈ **1.05 MB** | Against 64 MiB of shadow map, noise. |
| Flags | `ALLOW_RENDER_TARGET` | Generated by raster passes (§6). |
| Final state | `PIXEL_SHADER_RESOURCE \| NON_PIXEL_SHADER_RESOURCE` | **Both.** DXR shaders are non-pixel; PIXEL alone is a debug-layer error the moment the path tracer samples it. |

Roughness↔mip is **linear**, and the prefilter must generate mip *m* at exactly
`roughness = m/(mips-1)`. A mismatch between generation and lookup is invisible —
it reads as "reflections are a bit too blurry" — which is why §11 gates it with a
monotonicity assertion rather than an eyeball.

At high roughness the `N = V = R` assumption in split-sum drops the elongated
grazing lobe. Known, accepted, not fixed here.

---

## 4. Choice 3 — diffuse IBL

### Recommendation: **9-coefficient spherical harmonics in `CBPerFrame`.** No irradiance cubemap.

| | SH (L2, 9 coeff) | Irradiance cubemap (16²×6) |
|---|---|---|
| Descriptor slots | **0** | 1 |
| Constant bytes | 144 (+16 params) | 0 |
| Shader cost | ~20 madd | 1 cube fetch |
| Accuracy for a smooth sky | **exact to within L2 truncation, which for a linear-in-y sky is exact** | exact |
| Where it is computed | CPU, at startup | GPU, one more pass |
| New failure mode | C++/HLSL sky twin can drift | none |

Irradiance is a low-pass of the environment — L2 SH captures ~99% of it for any
smooth sky, and this particular sky (`c₀ + c₁·y`) lives entirely in the L0 and
L1 bands, so the representation is *exact* rather than approximate. Paying a
descriptor slot for an exact thing you can hold in 144 bytes of a cbuffer you are
already uploading is the wrong trade against a 126-slot ceiling.

### The cost of SH, stated plainly

The coefficients must exist on the **CPU**, because they live in a constant
buffer. That means either a GPU→CPU readback at startup, or a C++ twin of
`DawningSkyRadiance`. Readback for a 9-value reduction with no compute shaders
in the project is disproportionate. So: **a C++ twin**, `core::SkyRadiance`, in a
GPU-free translation unit alongside `core/shadow_cascades.h`.

That reintroduces the hazard `src/render/rt_texture_lod.h:9-23` names out loud:
*"If path_trace.hlsl is edited without editing this file, the tests stay green
and the image is wrong. Keep the two in step by hand."*

**Do not accept "by hand" here.** §11 Stage 1 specifies a GPU probe that
evaluates `DawningSkyRadiance` in HLSL for a fixed direction set and compares it
against `core::SkyRadiance` on the CPU. That converts the mirror from an
unwatched convention into a watched assertion, with a one-character negative
test. This is the single mitigation that makes the SH recommendation defensible;
if it is dropped, switch to the irradiance cubemap and pay the slot.

### Evaluation

Ramamoorthi & Hanrahan's quadratic irradiance form, with

    c1 = 0.429043  c2 = 0.511664  c3 = 0.743125  c4 = 0.886227  c5 = 0.247708

giving `E(N) = ∫ L(ω)·max(N·ω, 0) dω`, so the Lambert term is

    diffuse = kD · albedo · E(N) / PI

**Basis convention:** define the basis functions directly on the raw components
of the world-space direction `(x, y, z)` in the engine's left-handed, +Y-up,
+Z-forward frame, and use the *same* expressions in the C++ projector and the
HLSL evaluator. Do not import a formula written for a Z-up convention and
"adapt" it. If projection and evaluation share a basis, the basis cannot be
wrong; if they are written independently from two sources, it will be.

For constant radiance `L`, `E = π·L` and `diffuse = kD · albedo · L` — the white
furnace. That identity is the unit test (§11).

---

## 5. Choice 4 — when the prefilter runs

### Recommendation: **at startup, and on an explicit sky-revision bump. Not per frame.**

Mechanism:

```cpp
// Renderer
uint32_t m_skyRevision = 0;      // bumped by SetSkyParameters()
uint32_t m_iblRevision = UINT32_MAX;  // last revision baked into the cubemap
bool EnsureEnvironmentIBL(D3D12Device& device);   // early-outs when equal
```

`EnsureEnvironmentIBL` is called from `Renderer::Init` (Stage 1) and later from
`BeginFrameResources` (a follow-up stage), so the static case costs one call and
one comparison per frame.

Placement in `Renderer::Init` follows the pattern App already uses twice —
`WaitForCurrentFrame` → `ResetCommandList` → record → `Close` →
`ExecuteCommandLists` → `WaitForGpu` (`src/app.cpp:203`/`476-485` for texture
uploads, `622`/`636` for BLAS builds). By the time frame 0 records, the cube is
in `PIXEL|NON_PIXEL_SHADER_RESOURCE` and the SH coefficients are in `CBPerFrame`.

**Caveat, ASSUMED:** `Renderer::Init` runs at `app.cpp:166`, *before*
`SetDirectionalLight` at `app.cpp:173`. Today the sky does not depend on the
light, so prefiltering before the light is set is correct. The moment the sky
gains a sun this ordering silently bakes the default light direction. The
revision mechanism is what saves it — `SetDirectionalLight` must bump
`m_skyRevision` from the day the sky reads the light, not the day someone
notices.

### Cost, ASSUMED (not measured — no run was performed)

Texels: 6 · Σ(128²…1²) = 131,070. Mip 0 is a direct evaluation (roughness 0 is a
delta lobe — no integration). Mips 1..7 at 128 Hammersley samples each:
6 · Σ(64²…1²) · 128 ≈ 2.1 M sky evaluations, each a `lerp` and a `saturate`.
Sub-millisecond on any DXR-capable GPU. 48 draw calls (6 faces × 8 mips), each a
fullscreen triangle. Startup cost is noise; the reason not to do it per frame is
that it is *pointless* work at 60 Hz, not that it is expensive.

### What breaks if the sky becomes dynamic

A day/night cycle is plausible for a space sim, so name the breakages now:

1. **The SH coefficients are on the CPU.** Recomputing them per frame means
   evaluating `core::SkyRadiance` over a sphere on the main thread — at 6·32²
   samples that is ~6k evaluations per frame, tolerable, but it is real CPU work
   in the frame path and the projection loop is not currently written to be
   incremental. *Fix:* recompute only when the sun moves past an angular
   threshold, or project analytically if the sky stays a low-order function of
   direction.
2. **The cubemap becomes read-and-written in the same frame.** Today it is
   immutable after `Init` and needs no per-frame-slot instancing. A per-frame
   regeneration must either be `kFrameCount`-instanced (3 × 1.05 MB, and three
   SRV slots or a per-frame descriptor rewrite) or fully barriered
   `SRV → RENDER_TARGET → SRV` with the understanding that frames N-1 and N-2 may
   still be sampling it. `src/render/renderer.h:441-458` is explicit that
   barriers order GPU work and do not protect CPU writes; this is the GPU-side
   twin of that hazard and the same reasoning applies.
   **Instancing is the correct answer.** 3 MB is not worth a subtle bug.
3. **DXR temporal accumulation ghosts.** `CLAUDE.md` records that accumulation
   resets on camera motion only. A changing sky changes every pixel's radiance
   while the accumulator keeps averaging over stale history. The accumulation
   reset must also key on sky revision.
4. **Amortisation is the cheap out.** Regenerate one face per frame (6-frame
   latency) or one mip per frame. For a sun that crosses the sky in minutes,
   nobody can see a 6-frame lag. This is the recommended shape *if* dynamic sky
   lands, and it is why the revision counter exists rather than a bool.

None of this is built now. What is built now is a counter and an early-out, so
that when it does land it is an edit rather than a redesign.

---

## 6. Exact struct, cbuffer, and shader layout

### 6.1 `CBPerFrame` — 416 → 576 bytes

Appended only. Bytes 0..415 are byte-identical. `sky_ps.hlsl` needs no edit,
which is the property being protected.

```cpp
// src/render/renderer.h, appended after cascadeFadeLo
    // 416..559 — L2 spherical-harmonic irradiance for the environment.
    //
    // [9][4], NOT [9][3]. HLSL places every element of a cbuffer ARRAY on its own
    // 16-byte register, so `float3 iblSH[9]` is 144 bytes in HLSL and 108 in C++.
    // That mismatch compiles on both sides, keeps the member names identical, and
    // makes the shader read coefficient 1 out of the middle of coefficient 0.
    // This is the identical trap basic_ps.hlsl:29-42 documents for the cascade
    // tables, and the fourth component is dead padding on purpose.
    float iblSH[9][4];
    // 560..575 — x: specular mip count (float, so no int/float cbuffer split)
    //            y: environment intensity multiplier, 1.0 today
    //            z: IBL enable, 0 or 1 — the runtime kill switch the negative
    //               tests in section 11 toggle. NOT a debug leftover.
    //            w: reserved, must be written zero
    float iblParams[4];
```

```cpp
static_assert(sizeof(CBPerFrame) == 576,
              "CBPerFrame must match cbuffer CBPerFrame (b1) in the raster shaders");
static_assert(offsetof(CBPerFrame, iblSH)     == 416, "");
static_assert(offsetof(CBPerFrame, iblParams) == 560, "");
// THE ONE THAT CATCHES THE REAL BUG: [9][3] would be 108 and would still
// compile, still match member-for-member, and still pass every offset check
// above, because iblSH is the last-but-one field.
static_assert(sizeof(CBPerFrame::iblSH) == 144,
              "HLSL puts each cbuffer array element on its own 16-byte register; "
              "iblSH must be float[9][4]");
// Unchanged, restated so a future edit that moves the frozen prefix fails here.
static_assert(offsetof(CBPerFrame, lightViewProj) == 112, "");
```

HLSL side, appended to `basic_ps.hlsl`'s cbuffer with `packoffset` on every
member as the file already requires:

```hlsl
    float4 iblSH[9]         : packoffset(c26);   // 416..559
    float4 iblParams        : packoffset(c35);   // 560..575
```

416/16 = 26 and 560/16 = 35. An accidental overlap is an FXC **X4019**, which is
the property `basic_ps.hlsl:29-42` explains is worth more than it looks.

**Ring impact — this is the one place this design touches an existing gate.**
`AlignCBSize(416) = 512` today; `AlignCBSize(576) = 768`. `CBPerFrame` is
uploaded once per frame, so the measured ring peak moves **1792 → 2048 bytes.**

`tools/smoke_test.ps1:230` computes `$flatBudget = 512 + (($cascades+1)*256) + 256`
= 2048 and fails on `$peak -gt $flatBudget`. 2048 > 2048 is false, so **it passes
— with exactly zero margin**, consuming the entire slack that line's comment
describes as "one spare slot for a future per-pass constant".

That is not acceptable as-is. The harness's `512` is a hardcoded mirror of
`AlignCBSize(sizeof(CBPerFrame))`. The correct fix is to emit the aligned size as
a marker and have the harness read it:

    [SMOKE] cb_per_frame_bytes=768
    $flatBudget = [int]$markers["cb_per_frame_bytes"] + (($cascades + 1) * 256) + 256

This keeps the gate measuring what it is for — *per-draw traffic leaking back
into the ring* — instead of also measuring `CBPerFrame`'s size, which is pinned
by a `static_assert` and does not need a second guard. **`tools/smoke_test.ps1` is
owned by another agent right now; this is a coordination point, not a change to
be made unilaterally.**

### 6.2 `RTPerFrameConstants` — 212 → 384 bytes

There is a padding trap here that C++ will not catch.

`primaryConeSpread` sits at 208; the struct ends at **212**. HLSL will place a
`float4` array at the next 16-byte boundary, **224**. C++ places it at 212. That
is a 12-byte silent shear across every coefficient.

```cpp
// src/render/rt_pipeline.h, appended after primaryConeSpread
    // 212..223 — MANDATORY. HLSL rounds the array below up to the next 16-byte
    // row; C++ does not. Without this the two layouts differ by 12 bytes from
    // iblSH onward, with no compile error on either side.
    float pad0[3];
    float iblSH[9][4];    // 224..367   — identical basis to CBPerFrame::iblSH
    float iblParams[4];   // 368..383   — identical meaning to CBPerFrame::iblParams

static_assert(sizeof(RTPerFrameConstants) == 384,
              "RTPerFrameConstants layout changed — update path_trace.hlsl to match");
static_assert(offsetof(RTPerFrameConstants, iblSH)     == 224, "");
static_assert(offsetof(RTPerFrameConstants, iblParams) == 368, "");
static_assert(sizeof(RTPerFrameConstants::iblSH) == 144, "");
```

### 6.3 New shared header — `shaders/ibl_common.hlsli`

Same contract as `brdf_common.hlsli`: must compile under **FXC ps_5_1 /WX** and
**DXC lib_6_3**. No SM 6.x intrinsics, no wave ops, no ray-tracing types, and
single-exit functions.

```hlsl
float2 DawningEnvBRDFApprox(float NdotV, float roughness);
float3 DawningFresnelSchlickRoughness(float cosTheta, float3 F0, float roughness);
float3 DawningIrradianceSH(float4 sh[9], float3 N);          // returns E(N)
float3 DawningSpecularIBL(TextureCube<float4> envCube, SamplerState envSampler,
                          float3 N, float3 V, float roughness, float3 F0,
                          float mipCount);
```

**UNRESOLVED, and it must be settled before writing a line of this:** passing
`TextureCube` and `SamplerState` as function parameters. It is legal in SM 5.1
(resources as function arguments resolve statically and inline) and I am
reasonably confident FXC accepts it — but I did not compile it, and the raster
path builds with `/WX`, so "reasonably confident" is not good enough. **Verify
with a throwaway `fxc /T ps_5_1 /WX` stub before committing to this signature.**

Fallback if it does not compile: the header refers to macro-named globals
(`DAWNING_ENV_CUBE`, `DAWNING_ENV_SAMPLER`) that each including shader defines
before the `#include`. Uglier, but it is the same mechanism `basic_ps.hlsl`
already uses for `MAX_RASTER_TEXTURES`, so it is not a new idea in this tree.

The parameter form is preferred because it is what makes the raster and DXR
declarations able to differ (different registers, different spaces, different
samplers) while the *evaluation* stays one copy of one function. That is the
anti-divergence mechanism, and it is the whole reason this is a header.

### 6.4 New prefilter shaders

- `shaders/ibl_prefilter_ps.hlsl` — one fullscreen triangle per (face, mip).
  Root constants: `{ uint faceIndex; float roughness; uint sampleCount; float invFaceSize; }`.
  Reconstructs the direction for the texel, GGX-importance-samples with
  Hammersley, evaluates `DawningSkyRadiance` per sample, accumulates
  `Σ L·NdotL / Σ NdotL`.
  **`DawningSkyRadiance` must appear exactly once in this file.** That line is
  the entire "swap in a real HDR later" seam.
- `shaders/ibl_probe_ps.hlsl` — smoke-only, §11.
- Reuse the existing fullscreen-triangle vertex shader pattern
  (`shaders/bloom_vs.hlsl` / `shaders/tonemap_vs.hlsl`). ASSUMED their output
  signature suits; if not, one more four-line VS.

### 6.5 Cube face convention — RULE 7

**The risk here is not left- vs right-handedness.** D3D12's cube face ordering
and the direction→(face, u, v) mapping are fixed by the API and are the same
regardless of what handedness the engine's world uses. `reflect()` is
handedness-independent. RULE 7's real bite is elsewhere: the direction fed to
`SampleLevel` comes from the engine's LH +Z-forward camera basis, so as long as
the prefilter generates each texel's direction in that same basis, the round trip
is consistent.

So the only thing that can be wrong is the **face-basis table in the prefilter**,
and a naive reading of RULE 7 that sends someone flipping Z "for handedness" is
the most likely way to break it.

The D3D convention, with `s,t ∈ [0,1]` across the face image, `u = 2s-1`,
`v = 1-2t` (so `v` points up in the image):

| Face | Direction |
|---|---|
| 0 (+X) | `( 1,  v, -u)` |
| 1 (−X) | `(-1,  v,  u)` |
| 2 (+Y) | `( u,  1, -v)` |
| 3 (−Y) | `( u, -1,  v)` |
| 4 (+Z) | `( u,  v,  1)` |
| 5 (−Z) | `(-u,  v, -1)` |

**Do not trust this table.** I wrote it from memory of the D3D spec and did not
verify it against a run. §11 Stage 1 specifies a round-trip probe that renders a
*direction cubemap* — each texel stores the direction the generator computed —
then samples it along known query directions and requires `sample ≈ query`. If
the table is wrong in any way (permutation, sign, v-flip), the round trip fails.
If it round-trips, the convention is self-consistent, which is the only property
the renderer actually needs.

This matters because RULE 7's own warning applies: a face swap makes the corridor
look like a broken asset, not a broken renderer.

### 6.6 RULE 1 — why there is no `Vec3d` here

The cubemap is indexed by **direction only**. Directions are invariant under the
camera-relative translation `basic_ps.hlsl` works in, so `reflect(-V, N)` computed
from camera-relative positions is the correct world direction with no conversion
and no precision concern. `positionWS` never enters the environment lookup.

This holds only because the environment is at infinity. **Parallax-corrected
local probes would break it** — they need the world position of the shading point
against the world position of the probe volume, which is a `Vec3d` subtraction and
a RULE 1 question. Not in scope; named so nobody assumes the precedent.

---

## 7. Exact root signature and descriptor heap changes

### 7.1 Raster descriptor heap

Current (`renderer.cpp:1358-1394`, `renderer.h:633`, `671-677`):

| Slot | Contents |
|---|---|
| 0 | null SRV (permanent fallback) |
| 1 | shadow map `Texture2DArray` (`kShadowDescriptorIndex = 1`) |
| 2..127 | material textures — `m_textureAllocator.Init(128, 2)` |

New:

| Slot | Contents |
|---|---|
| 0 | null SRV — unchanged |
| 1 | shadow map — **unchanged**, `shadow_map_slot=1` still holds |
| **2** | **prefiltered environment `TextureCube`** — `kEnvCubeDescriptorIndex = 2` |
| 3..127 | material textures — `m_textureAllocator.Init(128, kEnvCubeDescriptorIndex + 1)` |

**Cost: exactly one descriptor. Usable material slots 126 → 125.**

Per the brief's requirement to say so plainly: this *does* make the 128-slot
ceiling marginally worse. One slot is ~0.25 of a four-map PBR material, taking
the practical ceiling from ~31.5 materials to ~31.25. It is the smallest
possible bite and it buys the single largest missing piece of the lighting model.
It does not change the ceiling's character or bring the bindless work forward.

This follows the shadow map's precedent exactly: a fixed reserved slot below the
allocator's `firstIndex`, with its own single-descriptor table, so it is valid
before any material exists and can never be recycled.

**Known wart, inherited rather than introduced:** `basic_ps.hlsl:126` declares
`Texture2D<float4> materialTextures[128] : register(t0)` over a range whose base
is heap slot 0, so slots 0..2 are nominally inside a `Texture2D` array while
holding a null SRV, a `Texture2DArray` and a `TextureCube`. This is already true
of the shadow map today and is safe only because the allocator cannot hand out
those indices and the range is `DESCRIPTORS_VOLATILE`. **ASSUMED:** GPU-based
validation does not flag an unindexed type mismatch in a volatile range; I did
not run `-GPUValidation` to confirm. If it does, the alternative is to place the
cube at slot 127 and drop the material range to 127 descriptors — more invasive,
because `MAX_RASTER_TEXTURES` is a define consumed by the shader
(`renderer.cpp:1132-1143`), so the array bound and the range size would stop
being the same number.

### 7.2 Raster root signature

Both hand-written branches in `Renderer::CreateRootSignature` change identically.
`renderer.cpp:1027-1033` states why in the code itself: the v1.0 branch runs only
when `CheckFeatureSupport` reports no 1.1, so a divergence surfaces on someone
else's driver and no local run — including both smoke modes — would catch it.

Current, from `renderer.cpp:759-769` (**VERIFIED by summing the parameters**):

| Slot | Type | Register | DWORDs |
|---|---|---|---|
| 0 | root SRV | t0/space2 | 2 |
| 1 | root CBV | b1 | 2 |
| 2 | root SRV | t0/space3 | 2 |
| 3 | table | t0-t127/space0 | 1 |
| 4 | table | t0/space1 | 1 |
| 5 | 32-bit constants ×3 | b3 | 3 |
| 6 | root CBV | b4 | 2 |
| 7 | root UAV | u0/space4 | 2 |
| | | **total** | **15** |

New:

| Slot | Type | Register | DWORDs |
|---|---|---|---|
| **8** | **table, 1 descriptor** | **t0/space2 → conflict, use t0/space6** | **1** |
| | | **new total** | **16 of 64** |

**Register space check.** Occupied SRV spaces on the raster side: space0
(material table), space1 (shadow), space2 (object structured buffer), space3
(material structured buffer); space4 is the probe UAV. The environment cube takes
**`t0, space6`** — deliberately skipping space5 so a reader cannot mistake it for
"the next one after the probe". `basic_ps.hlsl:135-137` already states the policy:
a separate register space so it cannot collide with the material table however
large that grows.

```cpp
// v1.1 branch
D3D12_DESCRIPTOR_RANGE1 envRange = {};
envRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
envRange.NumDescriptors                    = 1;
envRange.BaseShaderRegister                = 0;
envRange.RegisterSpace                     = 6;
envRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
envRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

rootParams[8].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
rootParams[8].DescriptorTable.pDescriptorRanges   = &envRange;
rootParams[8].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
```

The v1.0 branch is the same minus `Flags` (v1.0 has no per-descriptor flags;
volatile is its implicit default, which is what the v1.1 branch asks for
explicitly — `renderer.cpp:1030-1033`).

`D3D12_ROOT_PARAMETER1 rootParams[8]` → `[9]` in both branches, and the log line
at `renderer.cpp:1109` says **16 DWORDs**, not 15. That literal has been wrong in
this function before (`CLAUDE_SESSION_LOG.md` §3: "Two contradictory wrong DWORD
counts in one function"), so it is worth updating deliberately rather than
leaving it to a later reader.

Binding: `Renderer::BeginFrame` sets root parameter 8 to
`m_textureHeap` GPU handle + `kEnvCubeDescriptorIndex * m_textureDescSize`, once
per frame, next to the existing shadow-table bind. The shadow pass does not need
it (depth only, no material shader), so nothing changes there — which is the
property `renderer.cpp:777-786` protects.

**Sampler.** The existing `s0` is `ANISOTROPIC / WRAP` — wrong for a cubemap
(anisotropy across cube faces is meaningless here and WRAP is not the cube
addressing mode). Add a third static sampler `s2`: `MIN_MAG_MIP_LINEAR`,
`ADDRESS_*_CLAMP`, `MaxAnisotropy = 1`, `ShaderVisibility = PIXEL`. Static
samplers are free in root DWORD terms. **Trilinear, not point-mip** — the mip is
a continuous function of roughness and point-mip selection produces visible
banding across a roughness gradient.

And per `renderer.cpp:851-859`: assign every field *before* the sampler goes into
the `staticSamplers[]` array. That exact dead-store bug already shipped once here.

### 7.3 DXR side

**Heap (`path_tracer.cpp:18-22`, `159-168`):** current
`NumDescriptors = kRTEmissiveDescriptorBase + 64 = 258` per frame slot, ×3 slots.

Add the environment cube as a fifth SRV range:

```cpp
static constexpr uint32_t kRTEnvCubeDescriptorBase = kRTEmissiveDescriptorBase + kMaxRTEmissiveTextures; // 258
// heapDesc.NumDescriptors = kRTEnvCubeDescriptorBase + 1;   // 259
```

**Cost: +1 descriptor per frame slot, +3 total. Zero root DWORDs** — it is a new
range appended to the existing root parameter 8 table (`rt_pipeline.cpp:116-149`),
at `t0, space8`.

> **CORRECTED AS BUILT.** This paragraph said
> `OffsetInDescriptorsFromTableStart = 258`. **That is wrong**, and wrong in the
> direction that indexes one descriptor past the end of a 259-entry heap.
> `PathTracer::Dispatch` binds root parameter 8 at the **albedo base**
> (`kRTAlbedoDescriptorBase = 2`), not at the heap start — which is why the four
> existing ranges carry 0 / 64 / 128 / 192 rather than 2 / 66 / 130 / 194. The
> offset is **table-relative**, so the cube's is `258 - 2 = 256`. Shipped as
> `kMaxRTAlbedoTextures + kMaxRTNormalTextures + kMaxRTOrmTextures +
> kMaxRTEmissiveTextures`, with a `static_assert` in `path_tracer.cpp` pinning
> that expression to `kRTEnvCubeDescriptorBase - kRTAlbedoDescriptorBase` so the
> two arithmetics cannot drift apart silently.
>
> The heap numbers in the block above (258 → 259) were verified correct against
> the tree.

**Root signature (`rt_pipeline.cpp:58`, `172-177`):** the global RT root signature
currently costs 16 DWORDs (1 root SRV ×1 = 2, UAV table = 1, CBV = 2, root SRVs
×5 = 10, SRV table = 1). Adding the range costs nothing. The smoke-only IBL
divergence probe (§11 Stage 5) adds one **root UAV** at `u0, space9` for +2, so
**DXR total 16 → 18 of 64.** Following the raster draw probe's precedent
(`renderer.cpp:980-998`), a root UAV rather than a table keeps it out of the heap
entirely.

> **VERIFIED AND AMENDED AS BUILT.** The 16-DWORD baseline was **counted from
> `RTPipeline::CreateGlobalRootSignature` and is correct**, as is "adding the
> range costs nothing" — the cube is genuinely free.
>
> The +2 arrived at Stage 4 rather than Stage 5, and for a different probe. What
> Stage 4 needed was not a raster/DXR *numeric agreement* probe but a DXR
> **consumption** probe, for the same reason the raster path needed one: every
> startup assertion and the entire raster consumption block stay green with the
> DXR stable preview sampling no environment at all, and stay green with the
> wrong descriptor bound at `t0/space8`. Both were measured, not argued.
>
> Shipped at `u0, space4` (not `space9` — `space4` matches the raster probe's
> convention and nothing else in this shader claims it). **DXR total 16 → 18 of
> 64**, exactly as predicted; only the attribution moved. The same lesson Stage 3
> recorded applies again: *a descriptor costs a root DWORD only when something
> binds it, and evidence costs DWORDs of its own.*

The cube resource is **shared** — one resource, one SRV descriptor written into
each of the three RT heaps and one into the raster heap. It is immutable after
`Init`, so there is no aliasing hazard while the sky is static. It becomes one
the day the sky is dynamic (§5, item 2).

---

## 8. How both paths consume it, and how they are kept from diverging

### 8.1 Raster — `basic_ps.hlsl`

Delete lines 359-365. Replace with:

```hlsl
// Environment (IBL). Replaces the hemisphere ambient approximation: that term
// had no directional dependence, could not reflect anything, and went to zero
// for a fully metallic surface, which is why every glTF asset with
// metallicFactor 1.0 rendered near-black.
float3 envDiffuse  = (float3)0.0;
float3 envSpecular = (float3)0.0;
if (iblParams.z != 0.0)
{
    float3 F_env = DawningFresnelSchlickRoughness(NdotV, F0, materialRoughness);
    float3 kD_env = (1.0 - F_env) * (1.0 - materialMetallic);

    float3 irradiance = DawningIrradianceSH(iblSH, N);
    envDiffuse = kD_env * baseColor * irradiance / PI;

    envSpecular = DawningSpecularIBL(envCube, envSampler, N, V,
                                     materialRoughness, F0, iblParams.x);
}
envDiffuse  *= ambientOcclusion * iblParams.y;
envSpecular *= ambientOcclusion * iblParams.y;

float3 finalColor = direct + envDiffuse + envSpecular + emission;
```

Single `if`, both outputs initialised before it, single exit — FXC's X4000
analysis (`basic_ps.hlsl:192-196`) is satisfied by construction.

### 8.2 DXR — `path_trace.hlsl`

Two modes, and they answer the brief's question differently. **This is a
deliberate, justified difference, not a divergence defect.**

**Full path tracing (`g_StablePreview == 0`) — keeps tracing. Nothing changes.**

`path_trace.hlsl:531-536` already collects `DawningSkyRadiance(currentDir)` on
every miss, and `654-661` records that adding an ad-hoc ambient on top
double-counted it and defeated the bounce loop. The prefiltered cubemap is an
*approximation of exactly the integral this path evaluates by sampling.*
Substituting it would replace the reference with the approximation, and
`ASSET_PIPELINE_SPEC.md` makes the DXR path the reference.

A common optimisation — terminate the last bounce into the prefiltered
environment instead of tracing it — is **explicitly rejected here.** It biases
the estimator, and this project tracks its biases deliberately (`CLAUDE.md`
lines 40-42: throughput clamp, firefly clamp, both named). Adding a third
unlisted bias to save one bounce is a bad trade.

**Stable preview (`g_StablePreview != 0`) — uses the identical IBL.**

`path_trace.hlsl:642-653` today:

```hlsl
float3 envDiffuse = albedo * g_AmbientColor.rgb * (1.0f - mat.metallic) * 2.5f * ambientOcclusion;
float3 envReflection = DawningSkyRadiance(reflect(-V, N));       // mirror, ignores roughness
float3 envF0 = lerp(float3(0.04,0.04,0.04), albedo, mat.metallic);
float3 envF = DawningFresnelSchlick(saturate(dot(N, V)), envF0);
float envGloss = lerp(0.25f, 1.0f, saturate(1.0f - mat.roughness));
float3 envSpecular = envReflection * envF * envGloss * ambientOcclusion;
radiance += throughput * (envDiffuse + envSpecular) * (bounce == 0 ? 1.0f : 0.25f);
```

A magic 2.5, a magic 0.25, a mirror reflection that ignores roughness entirely,
and a gloss ramp corresponding to no physical quantity — the same shape of
approximation the raster path is losing. Replace it with the same two
`ibl_common.hlsli` calls the raster path makes.

**This change reduces divergence rather than creating it.** Today the default
smoke mode (`rt_quality=stable`) evaluates an environment approximation that
shares *nothing* with the raster one but the sky function. After this change,
raster and DXR-stable evaluate byte-identical code on identical inputs, and
DXR-full evaluates the ground truth both are approximating. The relationship is:

    DXR full     : ∫ by sampling                    — reference
    DXR stable   : split-sum + SH                   — same code as raster
    raster       : split-sum + SH                   — same code as DXR stable

The `(bounce == 0 ? 1.0 : 0.25)` damping is a preview heuristic and should be
kept for now — removing it is an appearance change, not a correctness one, and
mixing the two in one stage makes a regression unattributable.

`g_AmbientColor` becomes unused by every shader. It sits in `CBPerFrame`'s frozen
prefix at byte 32, so it **cannot be removed** — `sky_ps.hlsl` reads by offset and
the whole prefix is load-bearing. Keep uploading it, and say in the comment that
it is vestigial. Do not delete it to be tidy.

### 8.3 The anti-divergence mechanism, concretely

Three layers, in increasing strength:

1. **One environment function.** `DawningSkyRadiance` in `sky_common.hlsli`,
   called by the raster sky, the DXR miss shader, and the prefilter. Structural.
2. **One evaluation.** `ibl_common.hlsli`, included by `basic_ps.hlsl` and
   `path_trace.hlsl`, exactly as `brdf_common.hlsli` already is. Structural.
3. **A numeric agreement probe.** The same 64 input tuples, through the same
   header, compiled by FXC `ps_5_1` and DXC `lib_6_3`, compared on the CPU.
   §11 Stage 5. This is the only one of the three that can *fail*, and it is
   what catches a `#ifdef` that makes the two toolchains take different paths —
   the one way layers 1 and 2 can be defeated while looking correct.

---

## 9. Energy conservation

### 9.1 The combine

```
finalColor = direct
           + envDiffuse
           + envSpecular
           + emission

direct      = (kD_direct · albedo/π + spec_CT) · lightColor · NdotL · shadow
envDiffuse  = kD_env · albedo · E(N)/π · AO
envSpecular = prefiltered(R, r) · (F0·A + B) · AO
emission    = mat.emissive · strength · [emissive map]
```

with

```
kD_direct = (1 - F_schlick(VdotH, F0)) · (1 - metallic)      // unchanged
kD_env    = (1 - F_schlick_roughness(NdotV, F0, r)) · (1 - metallic)
```

### 9.2 Why each term does not double-count the others

- **direct vs environment.** `DawningSkyRadiance` contains **no sun disc**
  (`sky_common.hlsli` is a two-colour lerp on elevation, nothing else). The
  directional light is therefore not represented anywhere in the environment and
  the two are disjoint energy. **This is the single assumption the whole
  combine rests on.** If a sun is added to the sky, either exclude it from the
  prefilter integral or delete the analytic directional light — keeping both is
  a straight double count of the brightest thing in the scene.
- **`ambientDiffuse`/`ambientSpecular` must be deleted, not scaled down.**
  Leaving them alongside IBL is the obvious double count. They are gone entirely.
- **`kD_direct` vs `kD_env`** use different Fresnel arguments (`VdotH` for the
  half-vector of an actual light, `NdotV` roughness-aware for a whole hemisphere).
  Different integrals of the same physics, not the same quantity computed twice.
- **Emission** is added last and is untouched by lighting, occlusion, or the
  Fresnel split — unchanged from today (`basic_ps.hlsl:367-373`), and it stays
  not-a-light-source. IBL does not change that.

### 9.3 ORM occlusion

Occlusion continues to multiply **environment terms only, never direct light**.
That is today's rule (`basic_ps.hlsl:350-357`) and it matches the path tracer,
where the shadow ray gates NEE and nothing else. IBL inherits it unchanged.

**Known imprecision, kept deliberately:** applying the diffuse AO map directly to
`envSpecular` is not physically correct — specular occlusion depends on roughness
and view direction, and reusing diffuse AO over-darkens smooth surfaces. The
principled fix is a specular-occlusion term (e.g. horizon occlusion, or
Lagarde's `computeSpecOcclusion`). It is **not** done here, because today's
`ambientSpecular` already multiplies by `ambientOcclusion` and changing that in
the same stage that changes everything else makes any luminance shift
unattributable. Name it as an open item; do not fold it in.

### 9.4 Energy the split-sum loses

Single-scattering GGX loses energy at high roughness — light that would have
bounced a second time within the microsurface is dropped, and a rough metal comes
out visibly darker than it should. Multi-scatter compensation
(Fdez-Agüera's single-term approximation) is a ~5-line addition on top of the
split-sum. **Not in this design.** It is a correction to a term that does not
exist yet, and shipping it in the same change makes the white-furnace test in
§11 measure two things at once. It is the natural follow-up stage.

### 9.5 Roughness plumbing — the Toksvig interface

Compute the shading roughness **once**, in one named local, and feed the direct
BRDF *and* the IBL mip selection from it:

```hlsl
float materialRoughness = ...;   // scalar × ORM.g, then clamp(_, 0.04, 1.0)
// SINGLE POINT OF TRUTH. Toksvig/LEAN normal-variance widening plugs in HERE,
// once, and both the direct lobe and the IBL mip pick it up. See section 10.
```

Both paths already do this (`basic_ps.hlsl:324-338`, `path_trace.hlsl:551-573`)
and the property must be preserved rather than assumed.

---

## 10. Interaction with normal-map filtering (Toksvig / LEAN / CLEAN)

### The finding

`CLAUDE_SESSION_LOG.md` §6: ray-cone LOD moved DXR's mean luminance *away* from
raster, isolated to the normal map — filtering averages tangent-space normals
toward flat and discards sub-pixel variance that was acting as roughness. Raster
has the identical defect (`ApplyNormalMap` at `basic_ps.hlsl:283` samples through
the anisotropic mip chain). Not implemented; the fix is Toksvig or LEAN/CLEAN.

### How IBL interacts

**It makes the defect more visible, in a specific and predictable way.**

Today, roughness reaches the environment response through
`lerp(0.2, 0.8, 1 - roughness)` — a bounded scalar with a 4× dynamic range and no
directional content. Under IBL, roughness selects a **mip**, which is a strong,
high-dynamic-range, spatially-varying lookup. A minified normal-mapped metal
surface will have:

- a normal that has been averaged toward flat, so `R = reflect(-V, N)` is more
  coherent across neighbouring pixels than the true surface warrants, **and**
- a roughness that is unchanged and therefore too low, so the fetch lands on too
  sharp a mip.

Both errors point the same way: **too sharp a reflection of too coherent a
direction.** The symptom is specular aliasing and sparkle on distant metal under
camera motion — the classic reason Toksvig exists. It will be *newly visible*
because before IBL there was no directional environment reflection to alias.

### Should IBL wait on it?

**No. Proceed.**

1. IBL's correctness does not depend on it. The split-sum integral is correct for
   whatever roughness it is given; Toksvig changes the *input*, not the maths.
2. The interface is one variable (§9.5). Toksvig later is an edit at one site per
   path, not a redesign.
3. Waiting means the corridor stays black — which is the concrete, present,
   user-visible defect, against a latent quality issue on content that does not
   yet exist at scale.
4. Toksvig needs a normal-variance channel, which means touching the *texture
   cooking* stage (`ASSET_PIPELINE_SPEC.md` Stage 3, which explicitly defers
   block compression and mip generation). That is asset-pipeline work, not
   renderer work, and coupling them delays both.

### One thing that must be recorded when IBL lands

The ray-cone measurements in `CLAUDE_SESSION_LOG.md` §8 — *"Distant-ground
contrast vs raster: 4.876 → 1.694 (raster 1.582)"* — are pre-IBL. IBL changes the
environment response of **both** paths, so those numbers stop being a valid
baseline the moment this lands. **Re-measure the distant band after IBL rather
than comparing against them.** Treating a pre-IBL number as a post-IBL baseline
would attribute an expected change to a regression.

### Coordination

The ray-cone work is on `claude/rt-texture-lod`, **unmerged**
(`CLAUDE_SESSION_LOG.md` §7). This design edits `path_trace.hlsl` in Stage 4.
Those will conflict. Merge order must be decided before Stage 4 starts; the
conflict is textual and small, but it lands in the middle of the bounce loop
where both changes touch `mat.roughness`.

---

## 11. Staged plan and verification

Each stage leaves the build green and is independently verifiable. For each, the
assertion, the failure it catches, and the **negative test that proves it has
teeth** — because, per `ASSET_PIPELINE_SPEC.md`, *an assertion nobody has watched
fail is not evidence.*

**No golden-value gates on capture statistics.** `smoke_test.ps1:758-788` records
why: capture statistics depend on which assets sit in `build/<Config>`, which is
build output rather than tracked source, and a gate calibrated that way was
deleted rather than recalibrated a third time. Every assertion below is either
CPU-side over GPU-free code, or a GPU-side probe reading back values the shipped
shaders actually computed, in the manner of the draw-record probe. **None depends
on scene content.**

---

### Stage 1 — the cubemap exists and is provably correct

**Builds:** cube resource + 48 RTVs, prefilter root signature and PSO,
`ibl_prefilter_ps.hlsl`, `EnsureEnvironmentIBL`, the SRV at raster heap slot 2,
allocator `firstIndex` 2 → 3. `core::SkyRadiance` C++ twin.
**No shader consumes it. No visual change whatsoever.**

Markers: `ibl_env=ok ibl_env_slot=2 ibl_env_size=128 ibl_env_mips=8`

| # | Assertion | Catches | Negative test |
|---|---|---|---|
| 1.1 | `ibl_env_slot=2`; `shadow_map_slot=1` still passes | The reservation slipping so a material texture lands on the cube, or the cube landing on the shadow map | Set `firstIndex` back to 2; a material texture overwrites the cube and 1.3 fails |
| 1.2 | **Direction round trip.** Render a 4×4×6 *direction* cubemap (each texel stores the direction the generator computed), then a 64×1 pass samples it along 64 known query directions covering all six faces and both signs of every axis; CPU asserts `dot(sample, query) > 0.999` for all 64 | **The §6.5 face table being wrong in any way** — permutation, sign flip, v-flip, or a "handedness fix" that flips Z | Swap faces 2 and 3 (+Y/−Y) in the generation loop → fails. Negate `v` in one face → fails. Both are one-character edits |
| 1.3 | **Sky agreement.** The prefilter PSO, in a probe permutation, writes `DawningSkyRadiance(d_i)` for 64 directions; CPU compares against `core::SkyRadiance(d_i)` within 1e-4 | **C++/HLSL sky twin drift** — the hazard `rt_texture_lod.h:9-23` documents as unwatched | Change `0.85f` to `0.86f` in `sky_common.hlsli` → fails. This is the assertion that makes §4's SH recommendation defensible |
| 1.4 | **Mip energy.** Read back all mips of two faces. Whole-cube mean luminance at each mip within ±5% of mip 0's | An **unnormalised prefilter** — forgetting `/ Σ NdotL` — which makes every rough surface systematically wrong and looks like an art choice | Delete the weight-sum division → fails immediately |
| 1.5 | **Mip variance strictly decreasing** for mips 0..5, and `variance(0) > 1e-6` | A mip that was never rendered (garbage, or a copy of mip 0 — equal variance fails strict decrease). The `variance(0)` floor is the **vacuity guard**: an all-zero cube trivially satisfies "decreasing" | Skip generation for mips ≥ 1 → fails. Delete the `variance(0)` floor and clear the cube to zero → the remaining check passes, which is the point of the floor |

Mips 6 and 7 (2×2, 1×1) are excluded from 1.5 — their variance is legitimately at
the floor and asserting on it would be asserting on noise. Say so in the message
rather than quietly using `0..5`.

**Unit tests, CPU only, no GPU:** `core::SkyRadiance` against hand-computed values
at `y = -1, 0, 1`; monotonicity in `y`; output strictly positive everywhere.

---

### Stage 2 — diffuse IBL, raster only

**Builds:** SH projection in a GPU-free TU, `CBPerFrame` 416 → 576, the
`static_assert`s of §6.1, `DawningIrradianceSH` in `ibl_common.hlsli`,
`basic_ps.hlsl` replaces `ambientDiffuse`. `ambientSpecular` **stays for now** —
one term at a time.

**Coordination:** the ring peak moves 1792 → 2048 against a 2048 budget. See
§6.1; the harness change must be agreed with whoever owns `smoke_test.ps1`.

| # | Assertion | Catches | Negative test |
|---|---|---|---|
| 2.1 | **White furnace (CPU).** Project a constant radiance `L`; assert `E(N) = π·L` within 1e-5 for 32 random `N` | A wrong normalisation constant anywhere in projection or evaluation — the single most likely SH bug, and one that produces a plausible-looking image at the wrong brightness | Drop the `4π/N` solid-angle weight from the projector → fails |
| 2.2 | **Linear sky exactness (CPU).** For `L(ω) = a + b·ω.y`, assert coefficients L1₋₁…L2₂ beyond the analytically non-zero ones are `< 1e-6` | A **basis mismatch between projection and evaluation** — the §4 convention hazard. Energy leaking into bands that must be zero is a direct signature of it | Swap two basis functions in the evaluator only → fails |
| 2.3 | **Rotation invariance (CPU).** Project the sky, evaluate `E(N)`; independently rotate the sample set about Y and re-project; assert `E(R·N)` matches within 1e-4 | A sign error in the L1 band, which a y-only sky's DC term would otherwise hide | Negate the L1₋₁ coefficient → fails |
| 2.4 | `static_assert(sizeof(CBPerFrame::iblSH) == 144)` | The `[9][3]` vs `[9][4]` trap of §6.1 — the one that compiles, matches member-for-member, and shears every coefficient | Change the declaration to `[9][3]` → build fails. Compile-time, so "watched failing" means watching the build break |
| 2.5 | `[SMOKE] cb_per_frame_bytes=768` and the ring gate reads it | The harness's hardcoded `512` silently absorbing a future `CBPerFrame` growth into the entity-count budget | Add a dummy 256-byte field; the gate must still measure only per-draw traffic |

---

### Stage 3 — specular IBL, raster

**Builds:** root parameter 8 in **both** branches, static sampler `s2`,
`DawningEnvBRDFApprox` + `DawningSpecularIBL`, `basic_ps.hlsl` replaces
`ambientSpecular`. The hemisphere approximation is now entirely gone.

**The IBL evaluation probe.** A smoke-only 64×1 `R32G32B32A32_FLOAT` target;
`ibl_probe_ps.hlsl` includes `ibl_common.hlsli` and, for 64 `(N, V, roughness,
metallic, F0)` tuples supplied as constants, evaluates the **shipped** functions
and writes the result. One draw, one readback. This is the direct analogue of the
draw-record probe: it witnesses the value the shipped code computes, not a
re-implementation of it. It also proves `ibl_common.hlsli` compiles under
`ps_5_1 /WX`, which resolves §6.3's open question at build time.

| # | Assertion | Catches | Negative test |
|---|---|---|---|
| 3.1 | **Furnace across roughness.** `F0 = 1`, `N = V`, roughness swept 0→1: every result within ±10% of the cube's mean luminance | An unnormalised prefilter, a wrong env-BRDF, an inverted mip map — anything that fails to conserve energy across the roughness range | Swap `A` and `B` in `DawningEnvBRDFApprox` → fails at grazing angles |
| 3.2 | **Mip monotonicity.** For `N` toward the bright part of the sky, increasing roughness moves the result **monotonically toward** the cube mean | **`roughness → mip` inverted or off by one** — reads as "reflections are slightly too blurry/sharp" and is otherwise invisible | Change the mapping to `(1 - roughness) * (mips - 1)` → fails |
| 3.3 | **Mirror agreement.** At roughness 0, `N = V = d`: assert `specular ≈ core::SkyRadiance(d) · (F0·A + B)` within 2% | Mip 0 not being an exact evaluation of the sky; a sampler configured with the wrong address mode | Set the cube sampler to `WRAP` → fails at face edges |
| 3.4 | **The kill switch has an effect.** With `iblParams.z = 0`, all 64 specular results are exactly zero; with `= 1`, at least 60 exceed 1e-4 | The IBL path being bound but never actually reached — the "assertion that cannot fail" class. Without this, every check above could pass on a code path the real shader never takes | Force `iblParams.z = 0` at upload → the second half fails. This is the vacuity guard for the entire stage |
| 3.5 | Root signature log line reports **16 DWORDs**; both v1.1 and v1.0 branches declare `rootParams[9]` | The v1.0 branch drifting — which by construction no local run can catch (`renderer.cpp:1027-1033`) | Revert only the v1.0 branch to `[8]`; a code review must catch it, since no run can. **State this limitation rather than pretending the assertion covers it** |

3.5 is the weakest assertion here and I am labelling it as such. There is no
runtime evidence available for the v1.0 path on a machine that supports v1.1. The
only real mitigations are the lockstep comment already at `renderer.cpp:1027` and
review discipline.

---

### Stage 4 — DXR stable preview uses the same IBL

**Builds:** `RTPerFrameConstants` 212 → 384 with the mandatory `pad0[3]`, the
env-cube range at `t0/space8`, heap 258 → 259,
`path_trace.hlsl:642-653` replaced with the two `ibl_common.hlsli` calls.

**Coordination:** conflicts with the unmerged `claude/rt-texture-lod` branch (§10).

| # | Assertion | Catches | Negative test |
|---|---|---|---|
| 4.1 | `static_assert(sizeof(RTPerFrameConstants) == 384)` and `offsetof(iblSH) == 224` | The 212→224 HLSL row-rounding shear of §6.2 — a 12-byte offset error with no compile error on either side | Delete `pad0[3]` → the `offsetof` assert fails at build time |
| 4.2 | Default smoke mode still passes every existing marker; `rt_quality=stable` | A malformed cbuffer taking down the whole RT path | Standard |
| 4.3 | Capture is not black and not blown out (existing gates at `smoke_test.ps1:753-756`) | Catastrophic breakage only | Existing |

**Stage 4 ships with materially weaker evidence than Stage 3, and that must be
stated rather than glossed.** Nothing in 4.1–4.3 verifies that the DXR path
*numerically agrees* with the raster path — only that it does not crash and that
the layout is pinned. The assertion that would carry that claim is Stage 5. Do
not describe Stage 4 as verified.

---

### Stage 5 — the divergence probe (the assertion this feature actually needs)

**Builds:** a root UAV at `u0, space9` on the DXR global root signature (+2
DWORDs, 16 → 18, zero descriptors — the `renderer.cpp:980-998` precedent). The
raygen shader, gated on a probe-enable constant and `launchIndex.y == 0 &&
launchIndex.x < 64`, evaluates the **same 64 tuples** through the **same
`ibl_common.hlsli`** and writes them to the UAV. CPU reads both back.

| # | Assertion | Catches | Negative test |
|---|---|---|---|
| 5.1 | **`\|raster[i] − dxr[i]\| / max(\|raster[i]\|, 1e-3) < 1e-3` for all 64 tuples** | The one way the shared-header mechanism can be defeated while looking correct: a `#ifdef`, an intrinsic, or a precision difference that makes FXC `ps_5_1` and DXC `lib_6_3` take different paths through the same source | Wrap one constant in `#if defined(__SHADER_TARGET_MAJOR)` so only DXC sees it → fails. Without this probe, that edit is **invisible** |
| 5.2 | Both probes wrote all 64 slots (a written-count marker, not just a comparison) | The comparison passing vacuously because neither side ran — zero equals zero | Disable the probe-enable constant on one side → the count fails, not the comparison |

5.2 is the lesson from `CLAUDE_SESSION_LOG.md` §4's poison sentinel: a comparison
that both sides trivially satisfy is not a comparison. Assert that the data
exists before asserting it agrees.

**A tolerance note.** `1e-3` relative is ASSUMED, not measured. FXC and DXC may
legitimately differ in the last bits of `exp2` and `pow`. If the real spread is
wider, **widen it once, with the measurement written in the failure message** —
do not widen it iteratively until it goes green. `smoke_test.ps1:336-373` records
what happens when a gate gets recalibrated instead of understood.

---

### Stage 6 (optional, later)

Multi-scatter energy compensation (§9.4); specular occlusion (§9.3); the
precomputed LUT if the analytic fit measures worse than acceptable (§3); the
per-frame regeneration path for a dynamic sky (§5).

---

## 12. Risks, and what I could not resolve

Ordered by how much damage they do if ignored.

### The biggest risk: the sun-disc double count

The entire energy argument in §9 rests on `DawningSkyRadiance` containing no sun.
It does not today. The moment someone adds one — and "add a sun to the sky" is an
obviously desirable, obviously small-looking change — the directional light and
the environment become two copies of the brightest energy in the scene, and every
lit surface is silently over-bright by a factor that depends on roughness and
view angle.

It will not crash. It will not fail any assertion in §11 — every one of those
compares the renderer against the sky function, and both sides would move
together. The capture-luminance gates at `smoke_test.ps1:753-756` are loose
enough (10–245) to absorb it. It will look like "the tone mapping needs
tuning".

**Mitigation:** a comment in `sky_common.hlsli` at the point of definition, stating
that the function must not contain a sun disc while an analytic directional light
exists, and naming the two ways out (exclude the disc from the prefilter integral,
or delete the analytic light). A comment is a weak mitigation. I do not have a
better one — an assertion would have to know what "a sun" is. If someone can
think of a test with teeth here, it is worth more than anything else in §11.

### Second: the constant-ring gate has zero margin

§6.1. Peak 1792 → 2048 against a budget of exactly 2048. It passes, and then the
next person to append anything to `CBPerFrame` gets a failure whose message tells
them per-draw traffic leaked into the ring — which will be false, and which will
send them debugging the wrong thing. Fix the budget formula in the same change
that grows the struct, and coordinate it with the agent holding
`tools/smoke_test.ps1`.

### Third: unresolved from the code

- **Resources as HLSL function parameters under FXC `ps_5_1 /WX`** (§6.3). I did
  not compile it. The whole shared-header design depends on it, and the fallback
  (macro-named globals) is uglier but works. **Settle this with a stub before
  writing anything else.**
- **GPU-based validation and the type mismatch at heap slots 0..2** (§7.1). Already
  true of the shadow map, so probably fine, but I did not run `-GPUValidation`.
- **The §6.5 cube face table.** Written from memory of the D3D spec, not verified.
  This is why assertion 1.2 is a *round trip* rather than a comparison against the
  table — the round trip is correct even if my table is wrong, and it fails if the
  implementation is internally inconsistent.
- **The exact merge order with `claude/rt-texture-lod`** (§10). Both change the
  same region of `path_trace.hlsl`'s bounce loop.
- **`Renderer::Init` opening its own command list before `App::InitializeScene`
  opens one** (§5). Both `WaitForCurrentFrame`/`ResetCommandList`/`Close`/
  `Execute`/`WaitForGpu`, so it should compose — but I traced the pattern rather
  than running it.
- **Prefilter timing figures** (§5) are estimates. Nothing was measured; no build
  was run for this document.

### Fourth: things that will look like regressions and are not

- **The frame gets brighter.** IBL adds energy the hemisphere approximation was
  not carrying, especially on metals. Exposure will likely want retuning
  (`Renderer::SetExposure`, currently 1.25). Retune it in a *separate* commit so
  the IBL change and the exposure change are attributable independently.
- **The distant-band raster/DXR numbers move** (§10). Re-measure; do not compare
  against the pre-IBL figures in `CLAUDE_SESSION_LOG.md` §8.
- **Specular aliasing appears on distant normal-mapped metal.** That is the
  pre-existing Toksvig defect becoming visible, not a new bug (§10).

### Fifth: accepted limitations, stated so they are not rediscovered as bugs

- Split-sum's `N = V = R` assumption drops the grazing lobe at high roughness.
- Single-scatter GGX loses energy at high roughness (§9.4).
- Diffuse AO applied to specular over-darkens smooth surfaces (§9.3).
- The environment is infinitely distant — no parallax, no local probes (§6.6).
- The environment has nothing in it. This fixes the physics; the art is separate.

---

## 13. Summary of costs

| Resource | Before | After | Delta |
|---|---|---|---|
| Raster descriptor heap, usable material slots | 126 | 125 | **−1** |
| Raster root signature | 15 / 64 DWORDs | **16 / 64** | +1 |
| DXR global root signature | 16 / 64 DWORDs | **18 / 64** | +2 (both from the Stage 5 probe UAV) |
| DXR descriptor heap, per frame slot | 258 | 259 | +1 (+3 across slots) |
| `CBPerFrame` | 416 B (512 aligned) | **576 B (768 aligned)** | +160 B |
| `RTPerFrameConstants` | 212 B | **384 B** | +172 B |
| Constant-ring peak | 1792 B | **2048 B** | +256 B — **at the gate's budget** |
| GPU memory | — | ~1.05 MB | cubemap |
| Startup cost | — | 48 draws, ~2.1 M sky evaluations (ASSUMED sub-ms) | one-time |
| Per-frame cost | — | 0 (prefilter), ~1 cube fetch + ~30 ALU per pixel | |
