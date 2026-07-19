# Shadow Cascade Design (agreed plan)
Produced by an adversarial design pass: four independent designs judged
through three lenses, then synthesised. Recorded verbatim because the
reasoning is what makes it correct, not the conclusion.

## recommendation
Build **Design 1 (origin-centred radial CSM on one Texture2DArray, zero root-signature change)** as the skeleton, and graft onto it the one thing Lens 2 correctly called fatal: **Design 2's world-anchored, residual-only texel snap**.

Why Design 1 is the skeleton. I verified its two load-bearing claims against the real files rather than trusting them.

1. The containment theorem is real. `Mat4x4::LookAt` (types.h:453) builds `zAxis = (target-eye).Normalized()`, `xAxis = up.Cross(zAxis).Normalized()`, `yAxis = zAxis.Cross(xAxis)`. With `eye = lightDir*d` and `target = origin`, `zAxis = -lightDir`, so `xAxis` and `yAxis` are both perpendicular to `lightDir` and `xAxis.Dot(eye) = yAxis.Dot(eye) = 0`. A point `p` therefore has light-space `x = xAxis.Dot(p)`, giving `|x| <= |p|` **exactly**. Selecting by radius with `split[c] = extent[c]/1.05` makes "the selected point is inside the selected cascade's XY footprint" a theorem. The cascade-boundary-fallthrough bug class is structurally impossible, so the shader needs no XY guard and no trial-project-and-retry.
2. Zero binding change is real. A Texture2DArray SRV is one descriptor, so `shadowRange.NumDescriptors` stays 1 in both hand-written root-signature branches (renderer.cpp:800 and :865), `kShadowDescriptorIndex` stays 1, `m_textureAllocator.Init(kMaxRasterTextures, kShadowDescriptorIndex + 1)` (renderer.cpp:1153) still starts at 2, and smoke_test.ps1:143's literal `shadow_map_slot=1` stays honest. `CreateRootSignature` is not opened at all — which also means the dead-store bug at renderer.cpp:773-775 (staticSamplers[] is built before staticSampler.RegisterSpace/ShaderVisibility are assigned, so s0 ships with ShaderVisibility=ALL) cannot be tripped over as a drive-by.

The graft. Design 1 ships no stabilization and admits it; Lens 2 ranked it last for exactly that. But Lens 2's claim that this design "structurally cannot be stabilized" is wrong, and I checked the algebra. Because the cascade centre is the camera-relative origin, the snap collapses to a pure residual:

    lx = cameraPos.Dot(Xd);  sx = floor(lx/t)*t          // both double, cameraPos is Vec3d
    centreCR = Xd*(sx-lx) + Yd*(sy-ly)                   // narrowed; |.| <= sqrt(2)*texelWorld

No absolute position is ever narrowed — the two parenthesised terms are each bounded by one texel before they touch float. This is Design 2's residual-only form (the reason Design 2 is immune to the basis-orthonormality error that Lens 2 found fatal in Design 4), and it is simpler here because the pre-snap centre is exactly zero. I re-verified the theorem survives it: with `|centreCR| <= sqrt(2)*texelWorld`, worst-case `|x|/E = 1/1.05 + (sqrt(2)*2 + 4*2 + 1.5*2)/2048 = 0.9591`, leaving ~42 texels of slack against a ~7-texel requirement. Light-space z lands in [1.546E, 3.454E] inside [0.1, 5E], i.e. ndc [0.309, 0.691] — the shader's existing `0 <= z <= 1` guard provably never trips for a selected point.

Also grafted:
- **From Design 4**: the idea that the C++/HLSL packing trap needs a real detector. I reject its runtime canary (Lens 3 proved the debug-hue PSO that reads it has no teeth in this scene) and replace it with **HLSL `packoffset` on every CBPerFrame member in basic_ps.hlsl**. `float cascadeSplitRadius[4] : packoffset(c23)` would occupy c23..c26 and collide with `cascadeTexelWorld : packoffset(c24)`, which FXC rejects. That converts the highest-severity silent failure in the whole change into a compile error, at zero bytes and zero runtime cost. Nobody proposed this.
- **From Design 2**: the radial partition-of-unity blend, staged as the last optional step, consuming a `cascadeFadeLo` float4 that is reserved and uploaded from the start so the byte layout never churns.
- **From all four**: the CB-ring ordering prerequisite. Verified live — `BeginFrame` (renderer.cpp:1259-1260) is the only thing that sets `m_currentFrame` and zeroes `m_cbOffset`, and app.cpp:1131 runs it *after* the shadow pass at app.cpp:1104-1109.

Dropped as fatal: Design 3 entirely (`Vec3d(lightDir)` and `Vec3f(up)` do not compile — types.h:130-131 gives Vec3d only a default and a 3-double ctor, with conversion only via the static `FromFloat` and member `ToFloat`; plus it ships DepthBias=2000 against a 5.33x-widened depth range, and regresses cascade 0 from 23.4mm to 32.7mm/texel). Design 4's zero-margin `OrthoLH(2*radius, 2*radius, ...)` against a snapped centre. Design 2's simultaneous four-field PSO rewrite (DepthBias 0 / SlopeScaled 0 / DepthClipEnable FALSE / CullMode NONE), which stakes the one region that renders correctly today on an unmeasured receiver-plane bias.

## cascadeCount
4

## storage
ONE Texture2DArray. `m_shadowMap` keeps R32_TYPELESS / MipLevels=1 / ALLOW_DEPTH_STENCIL (no DENY_SHADER_RESOURCE) / 2048x2048 / created in PIXEL_SHADER_RESOURCE with a D32_FLOAT clear of 1.0, and changes exactly one field at renderer.cpp:1470: `DepthOrArraySize = 1` -> `core::kShadowCascadeCount`. 4 * 2048 * 2048 * 4 = 64 MiB, up from 16 MiB. State the number in the commit message.

SRV (renderer.cpp:1519-1527), still written to `m_textureHeap` CPU start + `kShadowDescriptorIndex * m_textureDescSize`, still ONE descriptor:
    Format                             = DXGI_FORMAT_R32_FLOAT          (unchanged)
    ViewDimension                      = D3D12_SRV_DIMENSION_TEXTURE2DARRAY
    Shader4ComponentMapping            = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
    Texture2DArray.MostDetailedMip     = 0
    Texture2DArray.MipLevels           = 1
    Texture2DArray.FirstArraySlice     = 0
    Texture2DArray.ArraySize           = core::kShadowCascadeCount
    Texture2DArray.PlaneSlice          = 0
    Texture2DArray.ResourceMinLODClamp = 0.0f

DSV heap (renderer.cpp:1497-1513): `NumDescriptors = core::kShadowCascadeCount`, TYPE_DSV, non-shader-visible so it never competes with the material heap. Loop c in [0,N):
    Format                          = DXGI_FORMAT_D32_FLOAT
    ViewDimension                   = D3D12_DSV_DIMENSION_TEXTURE2DARRAY
    Texture2DArray.MipSlice         = 0
    Texture2DArray.FirstArraySlice  = c
    Texture2DArray.ArraySize        = 1
    Flags                           = D3D12_DSV_FLAG_NONE
written at `heapStart.ptr + c * m_shadowDsvDescSize`, where `m_shadowDsvDescSize` is a new cached `GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)`.

Why not the alternatives. N separate Texture2Ds cost N-1 material slots and force `shadowRange.NumDescriptors` and the allocator's firstIndex to move in lockstep across two hand-written root-signature branches, breaking `shadow_map_slot=1`. An atlas destroys the property basic_ps.hlsl:108-109 documents and renderer.cpp:760-766 implements: the s1 OPAQUE_WHITE border makes out-of-footprint taps read as lit with no branch. In an atlas, off-tile is still inside the texture, so a 3x3 kernel at a tile edge silently reads a neighbouring cascade's depth and returns a plausible wrong answer.

Init order at renderer.cpp:25/31 is unchanged and stays load-bearing: `CreateTextureHeap` null-fills all 128 slots before `CreateShadowResources` writes slot 1.

Readback buffer (renderer.cpp:1670-1695): grows to `kShadowCascadeCount * rowPitch * kShadowProbeSize` = 4 * 1024 * 256 = 1 MiB. Slice stride 262144 is a multiple of D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512). Per slice: `dst.PlacedFootprint.Offset = c * 262144`, `src.SubresourceIndex = c` (one mip, so subresource index == array slice). RECREATE whenever the required size differs, not only when the pointer is null — the current lazy-once-never-resized shape would Map successfully on an undersized buffer and read past the copied region.

Pre-existing aliasing, widened but not created: root param 3 binds the material table at heap index 0, so `materialTextures[1]` formally declares a Texture2D<float4> over the shadow descriptor. Already true today (R32_FLOAT Texture2D under a float4 declaration) and inert because the allocator never hands out index 1 and no shader indexes it. Worth a comment at renderer.cpp:1318; not worth fixing here.

## splitScheme
Fixed half-extent table, NOT a camera-frustum fit. This is the single biggest simplification and it is deliberate.

    core::kShadowCascadeExtent[4] = { 24.0f, 65.0f, 175.0f, 470.0f }   // half-extents, world units
    core::kShadowCascadeMargin    = 1.05f
    ShadowCascadeSplitRadius(c)   = extent[c] / kShadowCascadeMargin
    ShadowCascadeTexelWorld(c)    = 2*extent[c] / kShadowMapSize
    ShadowCascadeDepthRange(c)    = extent[c] * (kShadowDepthRange / kShadowExtent)   // == extent[c]*5

Computed table (verified numerically, not by hand):

    c  extent    split      texelWorld   depthRange   texel ratio
    0   24.00   22.8571     0.0234375       120.0        -
    1   65.00   61.9048     0.0634766       325.0      2.708
    2  175.00  166.6667     0.1708984       875.0      2.692
    3  470.00  447.6190     0.4589844      2350.0      2.686

Texel ratios sit in (1, 8] as required and are near-uniform. Shadow reach goes 24 -> ~448 world units (18.7x). Cascade 0's extent (24) and depth range (120) are bit-identical to today, so the near field is a provable zero-pixel diff and any visual change is at range.

Why fixed rather than a practical/PSSM split of the camera frustum:
1. Radial selection makes the footprints rotation-invariant, so there is nothing about camera orientation left to fit.
2. The camera's far plane is 10000 (camera.h:63). Fitting to it produces absurd cascades, so a shadow-specific reach has to be chosen by hand regardless — the table IS the tunable.
3. A frustum fit needs near/far/fov/aspect plumbed into the shadow path, which is the exact edit that historically becomes "let me also centre the cascades on camera.Position()", narrowing a Vec3d into a matrix while every world matrix stays camera-relative. Keeping the fit argument-free means the only camera datum that ever enters is the Vec3d used for the snap residual.

`depthRange[c] = extent[c] * (kShadowDepthRange / kShadowExtent)` is expressed against the two existing constants rather than a new magic 5.0, so cascade 0 lands on exactly 120.0 with no clamp. It also fixes the ratio between depth range and texel size, which is what makes ONE shared shadow PSO sufficient: for a FLOAT depth format D3D12 scales `RasterizerState.DepthBias` per primitive by the exponent of the max z, so the constant term is roughly a fixed NDC offset — i.e. a world bias proportional to depthRange, hence proportional to extent, hence proportional to texelWorld. Bias-per-texel stays constant across all four cascades and the existing DepthBias=2000 / SlopeScaledDepthBias=2.5 carry over unchanged. I am moderately, not fully, confident in the exact float-format scaling rule (see openQuestions); cascade 0 is immune either way because its extent, depth range and bias are all unchanged.

Margin accounting, every part of the 5% justified. The theorem needs slack for: snap displacement (<= sqrt(2) texels), the normal offset (<= 4 texels at grazing), and the 3x3 PCF half-width (1.5 texels) — about 7 texels. Worst-case projected |x|/E = 1/1.05 + (sqrt(2)*2 + 4*2 + 1.5*2)/2048 = 0.9591, leaving ~42 texels of slack. An order of magnitude of headroom.

## selection
Radial distance from the camera-relative origin, evaluated in the pixel shader.

    const float viewDist = length(positionWS);

`positionWS` comes from basic_vs.hlsl:37 (`mul(world, pos)` where `world` is `Transform::ToCameraRelativeMatrix`), so the camera IS the origin of that space and `length()` is the exact radial view distance.

Two formulations deliberately NOT used:
- `distance(positionWS, eyePos)` — correct only by accident. renderer.cpp:1278 hardwires `m_eyePos = {}` and basic_ps.hlsl:189 depends on that zero for its V vector. It would break silently the moment anyone writes a real camera position there.
- `input.positionCS.w` — `PerspectiveFovLH` sets `m[2][3] = 1` (types.h:485) so clip.w is view z, but the rasteriser has already reciprocated it by the time a pixel shader sees SV_POSITION.w.

Radial rather than `dot(positionWS, camForward)` because `|lightSpaceX| <= |p|` and `|lightSpaceY| <= |p|` hold *exactly* for a radius. A view-depth split would need an inflation of sqrt(1 + tan^2(fov/2)(1+aspect^2)), i.e. two more values kept in sync for no benefit — and radial is what makes the footprints rotation-invariant.

The selector is written as three literal-indexed `if` statements, NOT as an [unroll] loop subscripting a float4 with the loop counter. Design 1 flagged the loop form as "asserted from the language rules, not verified by compiling"; the literal form removes the only unverified line in the shader at a cost of three lines. Descending order with `<` and plain assignment (never `break`) means the tightest containing cascade wins, and the single exit FXC's X4000 analysis requires is preserved:

    float4x4 cascadeVP  = lightViewProj[SHADOW_CASCADE_COUNT - 1];
    float    texelWorld = cascadeTexelWorld.w;
    float    slice      = 3.0f;
    if (viewDist < cascadeSplitRadius.z) { cascadeVP = lightViewProj[2]; texelWorld = cascadeTexelWorld.z; slice = 2.0f; }
    if (viewDist < cascadeSplitRadius.y) { cascadeVP = lightViewProj[1]; texelWorld = cascadeTexelWorld.y; slice = 1.0f; }
    if (viewDist < cascadeSplitRadius.x) { cascadeVP = lightViewProj[0]; texelWorld = cascadeTexelWorld.x; slice = 0.0f; }

Every cbuffer index is a literal; every float4 access is a static swizzle. Selecting the MATRIX into a local `float4x4` (resolved to movc chains) rather than carrying an index into the PCF block means exactly 9 `SampleCmpLevelZero` instructions are emitted, not 36 — this is the specific trap that `[unroll] for(i) if (cascade==i) { 9 taps }` falls into, which FXC will very plausibly flatten to 4x the cost with no warning.

The CPU twin `core::SelectShadowCascade(float radialDistance)` in shadow_cascades.cpp is written with the identical descending-`<` structure so the unit tests constrain the shader's arithmetic rather than a paraphrase of it.

Beyond `split[3]` the selector still picks cascade 3 and the border sampler returns lit. No `viewDist > split[N-1]` early-out: it would introduce a second, spherical cutoff alongside the square footprint boundary, and the two disagreeing is exactly the ring artifact that reads as a bug. The 9 wasted taps on very distant pixels are the cheaper price.

## stabilization
IMPLEMENTED — world-anchored texel snap, residual-only, in double precision. This is the primary graft from Design 2 and it is what makes the design survive a moving camera.

What the origin-centred construction already gives for free:
- Camera ROTATION produces zero shimmer and needs zero refit. The footprints are centred on the camera-relative origin and oriented purely by lightDir, and the radial split is rotation-invariant, so turning the camera does not move a single shadow texel. Frustum-fitted cascades must be stabilised against rotation; these need not be. This is also why no sphere fit is required, and why there is no scale shimmer to eliminate — the extent is a compile-time constant per cascade.

What must be added: camera TRANSLATION. Under camera-relative rendering the camera is pinned at the origin and the world slides beneath it, so quantising a camera-relative coordinate quantises nothing — the grid moves with the thing being quantised, and the snap is a no-op that looks like it works. The lattice must be anchored to the world origin.

Exact construction, in `core::BuildShadowCascadeMatrix(const core::Vec3f& lightDir, uint32_t cascade, const core::Vec3d& cameraPosition)`:

    // Light basis, identical element-for-element to what Mat4x4::LookAt derives
    // internally (types.h:453-467), so the snap space and the view matrix agree.
    const core::Vec3f up    = (std::fabs(lightDir.y) > 0.99f) ? core::Vec3f(0,0,1) : core::Vec3f(0,1,0);
    const core::Vec3f zAxis = (core::Vec3f(0,0,0) - lightDir).Normalized();   // == -lightDir
    const core::Vec3f xAxis = up.Cross(zAxis).Normalized();
    const core::Vec3f yAxis = zAxis.Cross(xAxis);

    const core::Vec3d Xd = core::Vec3d::FromFloat(xAxis);   // types.h:179; there is NO Vec3d(Vec3f) ctor
    const core::Vec3d Yd = core::Vec3d::FromFloat(yAxis);

    // The cascade centre in ABSOLUTE world space is the camera position itself,
    // because the camera-relative centre is the origin. Quantise its light-space
    // XY on a lattice fixed to the world origin, entirely in double.
    const double t  = static_cast<double>(core::ShadowCascadeTexelWorld(cascade));
    const double lx = cameraPosition.Dot(Xd);
    const double ly = cameraPosition.Dot(Yd);
    const double dx = std::floor(lx / t) * t - lx;          // in (-t, 0]
    const double dy = std::floor(ly / t) * t - ly;          // in (-t, 0]

    // THE ONLY NARROWING, and it narrows a RESIDUAL, never a position. Each term
    // is bounded by one texel before it touches float, so RULE 1 holds by
    // construction rather than by the subtraction being remembered.
    const core::Vec3f centre = (Xd * dx + Yd * dy).ToFloat();

    const float depthRange = core::ShadowCascadeDepthRange(cascade);
    const core::Vec3f eye  = centre + lightDir * (depthRange * 0.5f);
    const core::Mat4x4 view = core::Mat4x4::LookAt(eye, centre, up);
    const core::Mat4x4 proj = core::Mat4x4::OrthoLH(2.0f * core::kShadowCascadeExtent[cascade],
                                                    2.0f * core::kShadowCascadeExtent[cascade],
                                                    0.1f, depthRange);
    return view * proj;                                     // row-vector order, as today

Note the form: `Xd*dx + Yd*dy` is a sum of two one-texel residuals, never a reconstruction of an absolute position from a basis. That is precisely the distinction Lens 2 found fatal in Design 4, which built `snappedWorld = xd*lx + yd*ly + zd*lz` from a float-derived basis at ~1e7 magnitude and incurred ~1 world unit (~55 texels) of orthonormality error. Here the basis error multiplies a quantity bounded by 0.46, not by 1e7.

Why it eliminates crawl rather than reducing it. `xAxis` is orthogonal to `lightDir`, so `xAxis.Dot(eye) == xAxis.Dot(centre)`, and the centred `OrthoLH` maps `[centre_x - E, centre_x + E]` across `mapSize` texels. Because `2E == mapSize * texelWorld`, `centre_x - E` is an integer multiple of `texelWorld` whenever `centre_x` is. Texel boundaries therefore lie on a lattice fixed to the world origin, and a shading point's light-space X relative to the cascade centre is a difference of absolute positions with the camera algebraically cancelled. The same world point lands in the same texel every frame regardless of camera translation, so the compared depth is bit-stable and the edge cannot crawl.

Depth is deliberately NOT snapped. The same matrix produces both the stored depth and the reference depth, so a continuous shift of the depth mapping cancels exactly in the comparison; snapping it would only spend precision.

Precision limit, stated rather than glossed: `floor(lx/t)` is exact while `|lx|/t < 2^53`. At cascade 0's 0.0234-unit texel that is ~3.8e14 units. Beyond that the snap degrades continuously into partial snapping — mild shimmer returns, nothing breaks.

NOT addressed, and it is the one thing Design 2 got right that this does not: the `|lightDir.y| > 0.99` up-vector switch is a hard basis FLIP sitting exactly at zenith, where a sun passes at noon. When it fires, `xAxis` rotates ~90 degrees and all four texel lattices re-orient in one frame. Today that costs one frame of shimmer against an already-shimmering map; after this change it pops against otherwise-perfect stability, so the snap makes this artifact MORE visible, not less. Design 2's Frisvad-in-a-permuted-frame construction moves the singularity to nadir and is the correct fix. It is deferred deliberately: it changes the light basis for the existing single-cascade path too, deserves its own bisect point, and its own continuity test — and note that Design 2's own test 14 for it (continuity of every matrix element through zenith) is unsound once a snap exists, because the snap is deliberately discontinuous at texel boundaries. Assert continuity of the up-vector only.

## exactCBufferLayout
CBPerFrame grows 176 -> 416 bytes, purely by APPENDING at offset 112. Bytes 0..111 — the prefix sky_ps.hlsl:9-36 declares and reads by offset — are byte-for-byte untouched, and cascade 0's matrix lands at exactly offset 112 where the old single `lightViewProj` sat. sky_ps.hlsl needs no edit and must not receive one.

I verified this layout by compiling it, not by hand. `g++ -std=c++20` reports: sizeof=416, lightViewProj=112, cascadeSplitRadius=368, cascadeTexelWorld=384, cascadeFadeLo=400. All five static_asserts below pass.

C++ (src/render/renderer.h), replacing only the trailing `float lightViewProj[16];`:

    struct CBPerFrame
    {
        float lightDir[3];      float pad0;            //   0 ..  15
        float lightColor[3];    float pad1;            //  16 ..  31
        float ambientColor[3];  float pad2;            //  32 ..  47
        float eyePos[3];        float pad3;            //  48 ..  63
        float camRight[3];      float tanHalfFovY;     //  64 ..  79
        float camUp[3];         float aspect;          //  80 ..  95
        float camForward[3];    float pad4;            //  96 .. 111
        // ---- FROZEN PREFIX ENDS HERE. sky_ps.hlsl declares exactly bytes
        // ---- 0..111 and reads by offset. Nothing may be inserted above.

        // 112..367 (256 B): one camera-relative light view-projection per
        // cascade. Cascade 0 occupies exactly the bytes the old single matrix
        // did, which is the property sky_ps.hlsl cannot notice.
        float lightViewProj[core::kShadowCascadeCount][16];
        // 368..383: outer radius of each cascade, camera-relative world units.
        float cascadeSplitRadius[core::kShadowCascadeCount];
        // 384..399: world units per shadow texel, per cascade. Replaces the
        // hardcoded 24.0f/2048.0f pair at basic_ps.hlsl:85-86 that nothing enforced.
        float cascadeTexelWorld[core::kShadowCascadeCount];
        // 400..415: inner edge of each cascade's outer blend band. RESERVED AND
        // UPLOADED from step 5; consumed only by the optional blend in step 9.
        // Present from the start so the byte layout never churns mid-plan.
        float cascadeFadeLo[core::kShadowCascadeCount];
    };
    static_assert(sizeof(CBPerFrame) == 416,
                  "CBPerFrame must match cbuffer CBPerFrame (b1) in the raster shaders");
    static_assert(offsetof(CBPerFrame, lightViewProj)      == 112,
                  "sky_ps.hlsl declares bytes 0..111 as a prefix; lightViewProj must stay at 112");
    static_assert(offsetof(CBPerFrame, cascadeSplitRadius) == 368, "");
    static_assert(offsetof(CBPerFrame, cascadeTexelWorld)  == 384, "");
    static_assert(offsetof(CBPerFrame, cascadeFadeLo)      == 400, "");
    static_assert(core::kShadowCascadeCount == 4,
                  "the split/texel/fade arrays are declared float4 in basic_ps.hlsl; "
                  "changing the count means repacking as float4[(N+3)/4] and indexing [i/4][i%4]");

416 is a multiple of 16, which HLSL requires of a cbuffer size, and there is no trailing padding (alignof is 4). `AlignCBSize(416)` = 512.

HLSL (shaders/basic_ps.hlsl) — THIS EXACT TEXT, with packoffset on EVERY member:

    #ifndef SHADOW_CASCADE_COUNT
    #define SHADOW_CASCADE_COUNT 4
    #endif
    #if SHADOW_CASCADE_COUNT != 4
    #error "SHADOW_CASCADE_COUNT must be 4: the cascade tables are packed as float4."
    #endif
    #ifndef SHADOW_MAP_SIZE
    #define SHADOW_MAP_SIZE 2048.0
    #endif

    cbuffer CBPerFrame : register(b1)
    {
        float3   lightDir           : packoffset(c0);
        float    pad0               : packoffset(c0.w);
        float3   lightColor         : packoffset(c1);
        float    pad1               : packoffset(c1.w);
        float3   ambientColor       : packoffset(c2);
        float    pad2               : packoffset(c2.w);
        float3   eyePos             : packoffset(c3);
        float    pad3               : packoffset(c3.w);
        float3   camRight           : packoffset(c4);
        float    tanHalfFovY        : packoffset(c4.w);
        float3   camUp              : packoffset(c5);
        float    aspect             : packoffset(c5.w);
        float3   camForward         : packoffset(c6);
        float    pad4               : packoffset(c6.w);
        float4x4 lightViewProj[SHADOW_CASCADE_COUNT] : packoffset(c7);    // 112..367
        float4   cascadeSplitRadius : packoffset(c23);                    // 368..383
        float4   cascadeTexelWorld  : packoffset(c24);                    // 384..399
        float4   cascadeFadeLo      : packoffset(c25);                    // 400..415
    };

WHY packoffset, and why it is the most valuable line in the change. All four designs independently identified the same highest-severity risk: HLSL places every cbuffer ARRAY element on its own 16-byte register, so `float cascadeSplitRadius[4]` is 64 bytes in HLSL against 16 in C++. It compiles, the member names match, the C++ static_assert still passes, and the shader reads 48 bytes of neighbouring data as splits 1..3 — so selection is correct only for cascade 0, the one cascade that already worked, with nothing in the build or the harness to notice. Design 4's answer was a runtime canary read by a debug PSO that Lens 3 proved has no teeth in this scene. packoffset is strictly better: `float cascadeSplitRadius[4] : packoffset(c23)` would occupy c23..c26 and overlap `cascadeTexelWorld : packoffset(c24)`, which FXC rejects at compile time. Zero bytes, zero runtime cost, and it also pins the frozen prefix so an inserted field is a compile error rather than a silently shifted horizon.

Packing rules this relies on, each checked:
- A column-major float4x4 (HLSL's default) is exactly 4 registers = 64 bytes, so an array of them has no inter-element padding and matches C++ `float[N][16]` byte for byte.
- Do NOT write `row_major`. Correctness today depends on the CPU's row-major upload being reinterpreted by HLSL's default column-major packing, with `mul(M, v)` cancelling both transposes. Adding `row_major` silently transposes all four cascade matrices with no compile error.

Upload (renderer.cpp BeginFrame, replacing the single memcpy at :1313) — an explicit loop, not one 256-byte memcpy, because it must not assume `core::Mat4x4` (types.h:346, `float m[4][4]`) is padding-free in an array:

    for (uint32_t c = 0; c < core::kShadowCascadeCount; ++c)
    {
        memcpy(perFrame.lightViewProj[c], m_lightViewProj[c].Data(), sizeof(float) * 16);
        perFrame.cascadeSplitRadius[c] = core::ShadowCascadeSplitRadius(c);
        perFrame.cascadeTexelWorld[c]  = core::ShadowCascadeTexelWorld(c);
        perFrame.cascadeFadeLo[c]      = core::ShadowCascadeFadeLo(c);
    }

Define plumbing (renderer.cpp CreatePSO :942-949, extending the existing MAX_RASTER_TEXTURES mechanism — the precedent the codebase already set for exactly this class of drift):

    char cascadeCountText[16];  snprintf(cascadeCountText,  sizeof(cascadeCountText),  "%u",   core::kShadowCascadeCount);
    char shadowMapSizeText[24]; snprintf(shadowMapSizeText, sizeof(shadowMapSizeText), "%u.0", kShadowMapSize);
    const D3D_SHADER_MACRO psDefines[] = {
        { "MAX_RASTER_TEXTURES",  maxRasterTexturesText },
        { "SHADOW_CASCADE_COUNT", cascadeCountText },
        { "SHADOW_MAP_SIZE",      shadowMapSizeText },
        { nullptr, nullptr }
    };

Only basic_ps takes these; shadow_vs and sky_ps keep compiling with none. Safe here because basic_ps goes through FXC — note shader_utils.cpp:223 SILENTLY DROPS the defines argument on the DXC path, so the same trick is a no-op for anything on SM 6.x.

Also fix while in the file: basic_ps.hlsl:22 says CBPerFrame is "static_assert'd at 112 bytes". It is 176 today and 416 after. That comment sits directly above the block being edited and names the very boundary the append-only rule depends on, so a reader trusting it concludes the whole struct is the frozen prefix. sky_ps.hlsl:27 correctly says 176; update it to 416.

Ring budget after the ordering fix (kCBRingSize = 256 KB/frame, everything 256-byte aligned): 512 (CBPerFrame 416 -> 512) + E*512 (main pass: CBPerObject 192->256 plus CBMaterial 80->256) + E*256*N (shadow). At the demo's worst case E=85, N=4: 512 + 43,520 + 87,040 = 131,072 bytes, exactly half the ring.

## exactRootSigChange
NONE. `CreateRootSignature` (renderer.cpp:725-927) is not edited at all — neither the v1.1 branch (:806-841) nor the v1.0 fallback (:870-895). This matters beyond convenience: those are separate hand-written literal blocks with no shared source, and a change applied to one and not the other produces a machine that behaves differently on hardware without root-signature 1.1.

Specifically unchanged, all verified by reading the function:
- 5 root parameters, 8 DWORDs (3 root CBVs x 2 + 2 tables x 1). 56 of 64 DWORDs remain free; the root signature was never a constraint on this design.
- `shadowRange.NumDescriptors` stays 1 at renderer.cpp:800 (v1.1) and :865 (v1.0). A Texture2DArray SRV is ONE descriptor no matter how many slices it has — this is the whole reason the design costs nothing here.
- `shadowRange` BaseShaderRegister 0 / RegisterSpace 1, PIXEL-visible. Root param 4 still bound at `heapStart + kShadowDescriptorIndex * m_textureDescSize` (renderer.cpp:1323-1325).
- `textureRange.NumDescriptors` stays kMaxRasterTextures = 128.
- Both static samplers untouched, including s1's COMPARISON_MIN_MAG_LINEAR_MIP_POINT / ADDRESS_MODE_BORDER / OPAQUE_WHITE / LESS_EQUAL. Border addressing applies per-slice on an array — the array index is a texture coordinate, not an addressed dimension — so the "out of frustum reads as lit with no branch" guarantee that basic_ps.hlsl:108-109 documents survives intact and no third sampler is needed.

Descriptor heap: also untouched. `kShadowDescriptorIndex` stays 1 (slot 0 remains the null SRV), `m_textureAllocator.Init(kMaxRasterTextures, kShadowDescriptorIndex + 1)` still starts at 2 (renderer.cpp:1153), 126 material slots remain, and the CreateTextureHeap null-fill is unchanged. smoke_test.ps1:143's literal `Assert-Marker "shadow_map_slot" "1"` therefore keeps asserting exactly what it claims to.

DELIBERATELY LEFT ALONE, and this is an argument FOR not opening the function. renderer.cpp:773-775 constructs `staticSamplers[] = { staticSampler, shadowSampler }` BEFORE assigning `staticSampler.RegisterSpace` and `.ShaderVisibility`, so those two lines are dead stores and s0 ships with the zero-initialised ShaderVisibility (= D3D12_SHADER_VISIBILITY_ALL) rather than the intended PIXEL. It is benign today (ALL is a superset, and DENY_PIXEL is not among the flags), but anyone editing this block to add a cascade sampler would very likely "fix" the ordering and change root-signature validation behaviour as an invisible side effect of a cascade change. Not touching the function avoids the trap entirely. Likewise `MaxAnisotropy = 16` on s0 is inert because the filter is MIN_MAG_MIP_LINEAR, not ANISOTROPIC. File both separately if they bother you.

Two stale numbers sitting in the same function, worth correcting in a separate cosmetic commit rather than adding a third: renderer.cpp:723 comments "Total: 7 DWORDs" (stale from before the shadow table existed) and renderer.cpp:924 logs "9 DWORDs". The real count is 8. Two contradictory wrong numbers in one function is a good sign nobody recomputes this by hand.

## hlslNotes
Compiled with FXC ps_5_1 and /WX (shader_utils.cpp:30 sets D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS), and in Debug ALSO with D3DCOMPILE_SKIP_OPTIMIZATION (shader_utils.cpp:31-32) — a fact none of the four designs noticed, and the reason the shader must not rely on the optimizer to fold anything.

Hazards and the shapes that avoid them:

X4000 "potentially uninitialized" — fires for returns inside branches. AVOIDED: `float result = 1.0f;` before all flow control, exactly one `return` as the last statement of the function, outside every branch. This is the existing shape and it is load-bearing, not style.

X3550 dynamic index into a shader-local array — AVOIDED entirely. There is no local array. The selected matrix is a local `float4x4`, which FXC resolves to movc chains, not to indexed storage.

Dynamic index into a float4 with a loop counter — AVOIDED by writing the selector as three literal-indexed `if` statements rather than an `[unroll]` loop subscripting `cascadeSplitRadius[c]`. Design 1 explicitly flagged that form as reasoned-from-the-language-rules and never compiled; the literal form costs three lines and removes the only unverified construct in the shader. Every cbuffer access is `lightViewProj[0]`/`[1]`/`[2]`/`[3]` with a literal, and every table access is a static swizzle `.x/.y/.z/.w`.

X3557 un-attributed small loop — AVOIDED: both PCF loops carry `[unroll]`, exactly as the shipping code already proves works.

Implicit-gradient sample under divergent flow — AVOIDED: every tap is `SampleCmpLevelZero`, which takes an explicit LOD. The design never reaches for `.Sample()` inside conditional code.

Texture2DArray under SM 5.1 — `Texture2DArray<float> shadowMap : register(t0, space1);` sampled as `SampleCmpLevelZero(shadowSampler, float3(uv, sliceAsFloat), z)`. This is the SM 5.1-compatible cascade indexing form. No SM 6.6, no ResourceDescriptorHeap. The declaration change is one line; the sampler declaration is untouched.

Instruction count: exactly 9 SampleCmpLevelZero are emitted, not 36, because the MATRIX is selected before the kernel rather than the kernel being duplicated per cascade. The trap this avoids is `[unroll] for(i) if (cascade==i) { 9 taps }`, which FXC will very plausibly flatten into 36 taps for every pixel — a 4x pixel-shader shadow cost with no warning.

THE FULL FUNCTION, replacing basic_ps.hlsl:78-137:

    float ComputeShadow(float3 positionWS, float3 N, float NdotL)
    {
        // Radial view distance. positionWS is camera-relative (RULE 1), so the
        // camera is the origin of this space and length() is exact. Not
        // eyePos-relative (hardwired to zero at renderer.cpp:1278, correct only
        // by accident) and not positionCS.w (already reciprocated by the
        // rasteriser before a pixel shader sees it).
        const float viewDist = length(positionWS);

        // Select the tightest cascade containing this point, defaulting to the
        // outermost. Literal indices only: no dynamic cbuffer subscript, no
        // dynamic vector component index, no loop for FXC to reject.
        float4x4 cascadeVP  = lightViewProj[SHADOW_CASCADE_COUNT - 1];
        float    texelWorld = cascadeTexelWorld.w;
        float    slice      = 3.0f;
        if (viewDist < cascadeSplitRadius.z) { cascadeVP = lightViewProj[2]; texelWorld = cascadeTexelWorld.z; slice = 2.0f; }
        if (viewDist < cascadeSplitRadius.y) { cascadeVP = lightViewProj[1]; texelWorld = cascadeTexelWorld.y; slice = 1.0f; }
        if (viewDist < cascadeSplitRadius.x) { cascadeVP = lightViewProj[0]; texelWorld = cascadeTexelWorld.x; slice = 0.0f; }

        const float slope  = saturate(1.0f - NdotL);
        const float offset = texelWorld * (1.0f + 3.0f * slope);
        float3 offsetPos   = positionWS + N * offset;

        float4 lightClip = mul(cascadeVP, float4(offsetPos, 1.0f));

        // Single exit. FXC's X4000 flow analysis reports "potentially
        // uninitialized" for a function whose returns sit inside branches, and
        // the raster path compiles with /WX.
        float result = 1.0f;

        if (lightClip.w > 0.0f)
        {
            lightClip.xyz /= lightClip.w;
            if (lightClip.z >= 0.0f && lightClip.z <= 1.0f)
            {
                float2 shadowUV = float2(lightClip.x * 0.5f + 0.5f,
                                         -lightClip.y * 0.5f + 0.5f);
                const float texel = 1.0f / SHADOW_MAP_SIZE;
                float sum = 0.0f;
                [unroll]
                for (int y = -1; y <= 1; ++y)
                {
                    [unroll]
                    for (int x = -1; x <= 1; ++x)
                    {
                        sum += shadowMap.SampleCmpLevelZero(
                            shadowSampler,
                            float3(shadowUV + float2(x, y) * texel, slice),
                            lightClip.z);
                    }
                }
                result = sum / 9.0f;
            }
        }
        return result;
    }

Semantics of the two guards, now correct at ALL boundaries rather than only the last. Because selection never hands a point to a cascade that does not contain it (the theorem in `splitScheme`), the z-guard provably never trips for a selected point — I computed light-space z lands in [1.546E, 3.454E] inside the [0.1, 5E] slab, i.e. ndc [0.309, 0.691]. It is retained purely as defence against a future perspective (spot/point) light. The OPAQUE_WHITE border keeps its "treat as lit" meaning for the sub-texel overshoot the normal offset can produce.

STEP 2 OF THE PLAN IS TO COMPILE THIS STANDALONE BEFORE ANY C++ IS WRITTEN:

    fxc /T ps_5_1 /WX /Zi /Od /E main ^
        /D MAX_RASTER_TEXTURES=128 /D SHADOW_CASCADE_COUNT=4 /D SHADOW_MAP_SIZE=2048.0 ^
        /I shaders shaders\basic_ps.hlsl /Fo nul

`/Od` matches the Debug D3DCOMPILE_SKIP_OPTIMIZATION path, which is the stricter of the two and the one nobody analysed.

## implementationSteps
- STEP 1 — CB-ring ordering prerequisite. Separate commit, no cascades. Verified live: BeginFrame (renderer.cpp:1259-1260) is the ONLY thing that sets m_currentFrame and zeroes m_cbOffset, and app.cpp:1131 calls it AFTER the shadow pass at app.cpp:1104-1109. Every DrawMeshShadow constant therefore lands in the previous frame's ring. On frame 0 the stale m_currentFrame and FrameIndex() are both 0, so the main pass overwrites the exact bytes the shadow draws point at before the command list is ever submitted — frame 0's shadow map is rasterised with camera matrices. The same aliasing recurs after ResizeSwapChain, which reassigns m_frameIndex with no matching BeginFrame. Cascades multiply this traffic by 4. FIX: add `void Renderer::BeginFrameRing(D3D12Device& device) { m_currentFrame = device.FrameIndex(); m_cbOffset = 0; }`, delete those two lines from BeginFrame, and call BeginFrameRing once at the top of App::RenderFrame immediately after ResetCommandList (~app.cpp:1060) and BEFORE the raster/RT branch so both paths get it. Do NOT move BeginFrame itself — SetGraphicsRootSignature invalidates all root arguments even when the signature is unchanged, so BeginShadowPass would then blow away the b1 CBV and both descriptor tables. Net: one new 4-line method, two lines deleted, one call added. GREEN; behaviour improves, nothing else changes.
- STEP 2 — Standalone FXC probe. No commit. Write the new basic_ps.hlsl cbuffer tail (with packoffset) and ComputeShadow into a scratch copy and compile it by hand with `fxc /T ps_5_1 /WX /Od /D MAX_RASTER_TEXTURES=128 /D SHADOW_CASCADE_COUNT=4 /D SHADOW_MAP_SIZE=2048.0 /I shaders`. /Od matches the Debug SKIP_OPTIMIZATION path. This resolves the packoffset all-or-nothing question and the Texture2DArray/selector legality BEFORE a line of C++ is written, which is the cheapest possible place to discover a surprise. If packoffset is rejected, drop it, keep the plain float4 declarations, and record in the commit that failure mode F9 has no automated detector. Build is untouched, so trivially GREEN.
- STEP 3 — New src/core/shadow_cascades.{h,cpp} plus tests, engine untouched. Pure addition. The header includes ONLY ../core/types.h and <cstdint> — no d3d12, no renderer.h. Contents: `inline constexpr uint32_t kShadowCascadeCount = 4;`, `kShadowCascadeMargin = 1.05f`, `kShadowCascadeExtent[4] = {24,65,175,470}`, plus kShadowMapSize/kShadowExtent/kShadowDepthRange MOVED here from class Renderer to namespace core scope, and the functions ShadowCascadeSplitRadius / ShadowCascadeTexelWorld / ShadowCascadeDepthRange / ShadowCascadeFadeLo / SelectShadowCascade / BuildShadowCascadeMatrix (the last in the .cpp, per RULE 2). CMakeLists: add shadow_cascades.cpp to CORE_SOURCES, the .h to CORE_HEADERS, tests/test_shadow_cascades.cpp to TEST_SOURCES (line 105 — an explicit list, not a glob; a file not added there compiles nowhere and looks identical to a passing test in the output), and shadow_cascades.cpp to the add_executable(TheDawningTests ...) list at line 215. Amend the CMakeLists:207 comment ('Do NOT add src/render/*') to name this as a sanctioned src/core exception with its rationale, or the next author either breaks the rule silently or duplicates the math into the test. Write ALL of the CPU unit tests in this step. GREEN, and the math now has coverage BEFORE anything consumes it.
- STEP 4 — Texture2DArray plumbing only, still one cascade. renderer.cpp:1470 DepthOrArraySize 1 -> kShadowCascadeCount; DSV heap NumDescriptors -> 4 with four TEXTURE2DARRAY DSVs and the new cached m_shadowDsvDescSize; SRV -> D3D12_SRV_DIMENSION_TEXTURE2DARRAY with ArraySize 4. basic_ps.hlsl: `Texture2D<float>` -> `Texture2DArray<float>`, and the single tap becomes `float3(shadowUV + ..., 0.0f)` with a hardcoded slice 0. App still calls BeginShadowPass/RenderShadowCasters/EndShadowPass once. GREEN and the frame is BIT-IDENTICAL to today — which is the point: this isolates every piece of D3D array plumbing from every piece of cascade math, giving a clean bisect boundary. Add `shadow_cascades=%u` to the existing [SMOKE] shadow_map marker line (%u, never %.1f — Assert-Marker is a PowerShell string compare, so '4.0' will not match '4').
- STEP 5 — CBPerFrame 176 -> 416, all four matrices uploaded, shader still reads cascade 0 only. renderer.h gets the struct above with its five static_asserts; basic_ps.hlsl gets the packoffset cbuffer; sky_ps.hlsl gets a COMMENT-ONLY update (176 -> 416) and nothing else. m_lightViewProj becomes core::Mat4x4 m_lightViewProj[kShadowCascadeCount]; UpdateLightMatrix becomes UpdateShadowCascades(const core::Vec3d& cameraPosition) calling core::BuildShadowCascadeMatrix per cascade; BeginFrame's single memcpy becomes the explicit four-iteration loop. ComputeShadow still uses lightViewProj[0] and slice 0. GREEN and still visually identical, which is the guard: if the packing is wrong, cascade 0 is wrong and it is immediately visible rather than hiding in cascades 1..3. Fix the stale basic_ps.hlsl:22 comment here.
- STEP 6 — Render all four slices; shader still selects cascade 0. Split the pass: BeginShadowPass(device, cameraPosition) does UpdateShadowCascades ONCE, the PSR->DEPTH_WRITE ALL_SUBRESOURCES barrier still guarded by !m_shadowIsDepthTarget, viewport+scissor (identical every cascade, so set once), SetGraphicsRootSignature + SetPipelineState + IASetPrimitiveTopology, m_activeCascade = 0. New BeginShadowCascade(device, c) sets m_activeCascade, binds dsvHeapStart + c*m_shadowDsvDescSize via OMSetRenderTargets, and UNCONDITIONALLY ClearDepthStencilView — D3D12 does not zero-initialise a committed ALLOW_DEPTH_STENCIL resource, so a slice whose clear was skipped can hold arbitrary values below 1.0 and report written=yes while never having been rendered into. EndShadowPass unchanged. ONE Begin/End pair per frame with ALL_SUBRESOURCES barriers, so the whole-resource m_shadowIsDepthTarget bool stays coherent; per-slice barriers or an early-out mid-loop would desync it and produce a validation error far from its cause. DrawMeshShadow: one character, m_lightViewProj[m_activeCascade]. app.cpp:1104-1109 becomes the four-iteration loop, still entirely before BeginScenePass and before the viewport/scissor restore at :1116-1129. Generalise RecordShadowMapReadback and ReadShadowMapCoverage per slice, grow and RESIZE-ON-CHANGE the readback buffer. Emit and assert every per-cascade smoke marker HERE, including the fraction_0 > fraction_3 ordering relation. GREEN; the harness now has teeth before the selector exists.
- STEP 7 — Enable radial selection in the shader. Replace the hardcoded lightViewProj[0]/slice 0 with the three-if selector, and delete the hardcoded shadowExtent=24.0f / shadowMapSize=2048.0f at basic_ps.hlsl:85-86 in favour of cascadeTexelWorld and the SHADOW_MAP_SIZE define. This is the first step with a visible change: shadow reach goes 24 -> ~448 units. GREEN. Verify shadow_cascade_fraction_0 is still ~1.0 and the near field is unchanged.
- STEP 8 — Texel snap. BuildShadowCascadeMatrix gains the const core::Vec3d& cameraPosition parameter and the residual-only floor() quantisation; BeginShadowPass already threads it from app.cpp's m_camera.Position(). SetDirectionalLight stops calling the matrix builder (it has no camera position) and merely stores; CreateShadowResources seeds with UpdateShadowCascades(core::Vec3d{}) so the first BeginFrame has valid matrices. REWRITE, do not delete, the comment at renderer.cpp:1437-1443 and renderer.h:186-188 that says 'no camera position enters this calculation, and none should': the new text must say the camera position enters ONLY in double and ONLY to quantise, and that narrowing anything but the residual is the bug it guards against. Add a one-line note to CLAUDE.md RULE 1 pointing at this as the sanctioned exception. GREEN; distant shadow edges stop crawling.
- STEP 9 — OPTIONAL, recommended for a larger world, useless for the demo. Radial partition-of-unity blend consuming cascadeFadeLo, which steps 5-8 have already been uploading. Keep the single exit and literal indices: four explicit blocks calling a `float SampleCascade(float4x4 vp, float texelWorld, float slice, float3 positionWS, float3 N, float slope)` helper (itself single-exit), accumulating `sum`/`wsum` pre-initialised before all flow control, and returning `sum + (1.0f - saturate(wsum))` so missing weight reads as LIT and the last cascade fades out instead of ending on a line. fadeIn[c] must be a BYTE-IDENTICAL copy of fadeOut[c-1] so the same smoothstep argument pair yields (1-S)+S = 1 exactly. Staged last precisely so that if FXC surprises here, steps 1-8 are already landed and working. BE HONEST IN THE COMMIT: the demo's 10x10 ground plane and objects within ~4.5 units sit entirely inside cascade 0's 22.86-unit split radius, so the demo scene cannot exhibit the seam this step removes and cannot verify the fix.

## verificationPlan
- TESTABILITY PREREQUISITE, not a test. The cascade math lives in src/core/shadow_cascades.{h,cpp}, GPU-free, compiled into BOTH the app and TheDawningTests, so the unit tests call the SHIPPED function. This is not cosmetic. tests/test_math.cpp:667 and :703 read as shadow-matrix tests but rebuild LookAt*OrthoLH inline with their own hardcoded 24.0f/120.0f — they cover core::Mat4x4, not Renderer, and would keep passing if UpdateLightMatrix were deleted outright. The shipped light-matrix code has ZERO coverage today. A fourth mirror would pass while cascades were broken, which is worse than no test. Leave those two cases alone (they still describe cascade 0 correctly and are cheap); test F10 below is the real thing.
- F1 — A CASCADE NEVER RASTERISES. ASSERTION: per-slice smoke markers `shadow_cascade_written_0..3=yes`, with the INDEX IN THE KEY. This shape is mandatory: smoke_test.ps1:114 does `$markers[$kv[0]] = $kv[1]`, so a duplicate key silently OVERWRITES and a looped `cascade_written=yes` would collapse to the last cascade, passing while cascades 0..N-2 were empty. Keep `shadow_map_written` as cascade 0's value so smoke_test.ps1:150 needs no edit and stays true. NEGATIVE TEST: change the app loop bound to `c < N-1`; `shadow_cascade_written_3` flips to `no`. This also proves the readback reaches slices other than 0, which the current SubresourceIndex=0 probe does not.
- F2 — CASCADE N's MATRIX PAIRED WITH CASCADE N+1's SLICE. ASSERTION: `[double]$markers['shadow_cascade_fraction_0'] -gt [double]$markers['shadow_cascade_fraction_3']`, inside the existing `if ($RasterOnly)` block. This is the ONLY GPU-side assertion in any of the four designs with real discriminating power on pairing, and I verified the margin against the actual scene rather than trusting it: the centred 256/2048 probe covers extent[c]/4 world units, so cascade 0 sees a 6x6 window against a 10x10 ground plane (app.cpp:209, GeneratePlane(10,10,...)) whose camera-relative extent straddles the origin — fraction ~1.0 — while cascade 3 sees a 117x117 window over the same 10-unit plane, fraction ~0.007. Roughly two orders of magnitude. It is CATEGORICAL (an ordering relation), never numeric and never a reference image, which is required because README.md:105-127 documents raster captures differing by up to 109/255 per channel between consecutive runs of the same build (SystemRotation animates on wall-clock dt) and README.md:129-143 documents mean luminance moving 127.5 -> 124.4 across checkouts purely from leftover files in build/<Config>/assets/textures/. NEGATIVE TEST: swap the matrices uploaded for slices 0 and 3 (a permutation) and confirm the ordering inverts and the assertion FAILS. NOTE — Design 1's stated negative test for this assertion was misattributed: binding DSV slice 0 for every cascade leaves slices 1-3 at their cleared value, so fraction_3 = 0 and the ordering assertion still PASSES; what catches THAT failure is shadow_cascade_written_3=no (F1). Getting this right matters, because a demonstration that itself manufactures false confidence is exactly what the exercise is trying to prevent.
- F3 — CASCADE SELECTION STUCK AT 0. ASSERTION (CPU, tests/test_shadow_cascades.cpp): Cascades_SelectionIsNotStuckAtZero. Construct the probe in LIGHT SPACE, not view space — this is where Designs 3 and 4 both went wrong. Take p = xAxis * (split[1] * 0.99) = 61.3 along the light-space X axis, so |lightSpaceX| = 61.3 > extent[0] = 24 by construction, independent of camera or light orientation. Assert the CONJUNCTION: CHECK(InsideClip(matrix(1), p)) AND CHECK_FALSE(InsideClip(matrix(0), p)) AND CHECK_EQ(SelectShadowCascade(length(p)), 1u). Inclusion alone is satisfied by a selector stuck at 0; the CHECK_FALSE clause is what fires. Repeat for cascades 2 and 3 and for three light directions including a near-straight-down one that trips the up-vector switch. NEGATIVE TEST: hardwire SelectShadowCascade to return 0. This test fails while EVERY GPU marker stays green — run it and observe that, because demonstrating the gap is what stops a reviewer over-trusting four green cascade markers.
- F4 — SPLIT DISTANCES OFF BY ONE / BOUNDARY DRIFT. ASSERTION: Cascades_PartitionHasNoGapOrOverlap. Sweep r over 4096 steps from 0 to split[3]*1.1. Assert SelectShadowCascade is non-decreasing, starts at 0, reaches 3, and that the returned c satisfies r < split[c] for every c < 3 and (c == 0 || r >= split[c-1]). This catches the `<` vs `<=` drift that renders each cascade correctly and leaves a one-sample-wide unshadowed ring at a fixed distance — orders of magnitude below every threshold at smoke_test.ps1:255. NEGATIVE TEST: make the selector compare against cascadeSplitRadius[c+1] (a plausible off-by-one); the sweep fails on the first transition.
- F5 — THE CONTAINMENT THEOREM BREAKS (margin too small, or the snap displacement is unaccounted). ASSERTION: Cascades_SplitFitsInsideItsOwnFootprint. For each cascade, for ~200 directions sampled over the sphere, p = dir * split[c] * 0.999, CHECK(InsideClip(BuildShadowCascadeMatrix(lightDir, c, cameraPos), p)). Run it at cameraPos = 0 AND at a cameraPos chosen to produce a worst-case snap residual, so the assertion covers the snapped matrix and not just the unsnapped one — this is the gap that made Design 4's own containment test self-contradictory. NEGATIVE TEST: set kShadowCascadeMargin = 0.95; fails immediately. Second negative: inflate the snap by multiplying the residual by 100; fails, proving the test sees the snap.
- F6 — ADJACENT CASCADES DO NOT OVERLAP (a seam becomes a gap). ASSERTION: Cascades_BoundaryPointIsInsideBothNeighbours. At r == split[c] exactly, for 32 directions, the point must project inside BOTH cascade c and cascade c+1. Non-overlapping frusta render fine individually and produce a ring of missing shadow; this is the only assertion that catches it. NEGATIVE TEST: set extent[1] = split[0] * 0.9; fails.
- F7 — RULE 1 REGRESSION. The single highest-value test, because the most probable future edit is someone threading the absolute Vec3d camera position in to 'centre the cascades on the camera' — correct at the origin, catastrophic at planetary distance, invisible in a demo that sits near it. ASSERTION: Cascades_MatricesAreCameraPositionInvariant. Build the set at cameraPos (0,0,0) and at (1e7, 1e7, 1e7). Rows 0..2 (the rotation block) cannot depend on position at all: CHECK_APPROX_EPS to 1e-6. Row 3 may differ by at most one snapped texel in light-space XY: a world shift of t along xAxis moves clip x by t/extent, and t <= texelWorld = 2*extent/mapSize, so the bound is 2.0f/kShadowMapSize + 1e-5 (~0.00108) for columns 0..1 and 1e-6 for columns 2..3. CHECK no element is NaN or Inf. NEGATIVE TEST: replace `(Xd*dx + Yd*dy).ToFloat()` with a form that reconstructs the absolute snapped position and narrows it (`(Xd*sx + Yd*sy + Zd*lz).ToFloat()`) — the classic violation, and the exact shape Design 4 specified. The translation row jumps by ~1e7 and the assertion fails by ten orders of magnitude, while the cameraPos=0 case still passes, which is precisely why the far-camera case must be in the test. Follows the existing template at tests/test_math.cpp:613 (Vec3d_CameraRelativeSurvivesPlanetaryDistance).
- F8 — THE SNAP DOES NOT ACTUALLY SNAP (shimmer). Nothing else in the project can see this, and getting it wrong does not crash — it produces edge crawl that reads as 'the shadow map needs more resolution'. ASSERTION: Cascades_SnapIsStableUnderSubTexelCameraMotion. Fix an absolute world point W = Vec3d(3, 1, 5). Sweep the camera 200 times along xAxis in steps of texelWorld[0]/64 (a ~3-texel total sweep). At each step build the set and evaluate `viewProj[0].TransformPoint(((W - camPos).ToFloat())).x`. Count distinct values at 1e-6 tolerance; assert <= 8. NEGATIVE TEST 1: delete the two std::floor() calls — the count jumps to 200. NEGATIVE TEST 2 (the important one): perform the snap on the camera-RELATIVE centre instead of the absolute one, i.e. quantise a value that is identically zero. This is the plausible-looking wrong implementation that produces no compile error, no crash, and full shimmer; it must also drive the count to 200.
- F9 — C++/HLSL CBUFFER PACKING DRIFT. This is the highest-severity silent failure all four designs independently identified: `float cascadeSplitRadius[4]` in HLSL is 64 bytes (each scalar alone in the .x of its own register) against 16 in C++. It compiles, the member names match, the C++ static_assert still passes, and the shader reads 48 bytes of neighbouring data as splits 1..3 — so selection is correct only for cascade 0, the one cascade that already worked. ASSERTION, C++ side: sizeof(CBPerFrame) == 416 plus four offsetof asserts (112/368/384/400) plus static_assert(kShadowCascadeCount == 4). ASSERTION, HLSL side: packoffset on every member. NEGATIVE TEST: change the declaration to `float cascadeSplitRadius[4] : packoffset(c23);` — it then occupies c23..c26 and overlaps `cascadeTexelWorld : packoffset(c24)`, which FXC rejects, so the build fails instead of shipping wrong cascades. IF packoffset turns out to be unusable (see openQuestions), THIS FAILURE MODE HAS NO AUTOMATED DETECTOR AT ALL and that must be recorded in the commit message rather than papered over — the mitigations then reduce to the #error guard on the count, a comment, and the fact that the F5/F6 CPU tests cannot see GPU-side packing.
- F10 — CASCADE 0 REGRESSES FROM TODAY. ASSERTION: Cascades_CascadeZeroMatchesLegacySingleCascade. CHECK_APPROX(kShadowCascadeExtent[0], 24.0f), CHECK_APPROX(ShadowCascadeDepthRange(0), 120.0f), and BuildShadowCascadeMatrix(dir, 0, Vec3d{}) equal ELEMENTWISE to LookAt(dir*60, {0,0,0}, up) * OrthoLH(48, 48, 0.1f, 120.0f). This pins the property that makes the near-field diff exactly zero at step 5-6, and it is the anchor for bisecting any visual regression. NEGATIVE TEST: change extent[0] to 25.0f; fails.
- F11 — A SLICE IS NEVER CLEARED. D3D12 does not zero-initialise a committed ALLOW_DEPTH_STENCIL resource, so a slice whose ClearDepthStencilView was skipped can hold arbitrary values below 1.0 and report written=yes without ever having been rendered into. The coverage probe CANNOT distinguish 'cleared and drawn' from 'never cleared'. MITIGATION IS STRUCTURAL, not an assertion: the clear lives inside BeginShadowCascade, unconditionally, ahead of any per-cascade decision, so the only way to skip it is to skip the whole cascade — which F1 does catch. Rated PARTIALLY COVERED. Do not let a future per-cascade culling change introduce a path that skips the clear.
- F12 — A RUNTIME REGRESSION IN THE LIVE UPLOADED TABLE. ASSERTION: `[SMOKE] shadow_cascade_texel_monotonic=yes|no`, computed in C++ from the CascadeData that was ACTUALLY uploaded this frame (strictly increasing texelWorld, consecutive ratios in (1,8]), emitted categorically so PowerShell does no float arithmetic. Grafted from Design 2. It is a runtime check of the real fit rather than a mirror of the table, and it catches the whole family of 'all cascades ended up the same size' and 'the table got reversed'. NEGATIVE TEST: set kShadowCascadeExtent to {24,24,24,24}; the marker flips to no.
- WHAT NONE OF THIS COVERS — state this in the commit, not just here. (a) THE GPU NEVER PROVES WHICH CASCADE A PIXEL SELECTED. Every cascade frustum is centred on the camera-relative origin, so the centred probe window is written by all four; four `yes` markers are consistent with a selector hardwired to 0. I considered and REJECT Design 4's --smoke-cascade-debug hue PSO: I checked the scene and it has no teeth here (see openQuestions). The honest position is that the CPU selector (F3, F4) is written with the identical descending-`<` structure as the HLSL and the shader is verified against it by inspection. (b) ReadShadowMapCoverage performs NO GPU synchronisation of its own; it reads valid data only because WriteBackBufferCapture calls WaitForGpu and runs first at app.cpp:1176-1181, gated on --smoke-capture. Run with -NoCapture and the coverage numbers are undefined, with a failure mode of a marker that reads plausibly rather than an error. Four markers on the same machinery make a wrong reading four times more convincing. (c) `tools\smoke_test.cmd` with NO arguments runs the RT path, where app.cpp:1160 suppresses the probe entirely, so the DEFAULT smoke run exercises no shadow assertion today and will exercise no cascade assertion either. Every new assert must sit inside the existing `if ($RasterOnly)` block because Assert-Marker hard-fails on an absent key. Fixing that gate is out of scope but must be recorded. (d) Every cascade failure mode degrades toward BRIGHTER, never toward a black artifact, because s1's OPAQUE_WHITE border makes out-of-frustum reads fully lit — and smoke_test.ps1:255-256's mean-luminance ceiling of 245 is the direction the harness is least sensitive to.

## deferred
- Per-cascade frustum culling of casters. RenderShadowCasters is walked once per cascade, so the demo's 85 entities become 340 draws. This is pure waste, not an artifact. It is also not implementable today: I read src/render/mesh.h — struct Mesh carries vertexBuffer, indexBuffer, views, counts, indexFormat and the RT triangle vectors, and NO bounding volume. Adding bounds and populating them in CreateMesh/CreateMesh32 is real scope of its own. When it lands, the base visibility predicate in Scene::RenderShadowCasters must stay byte-identical to RenderEntities (scene.h:61-64 requires this); per-cascade rejection is the correct place to diverge and belongs AFTER the shared predicate.
- The shadow pass uploads 128 bytes of `world` and `worldInvTranspose` per caster per cascade that shadow_vs.hlsl:30 never reads, and recomputes Mat4x4::InverseTranspose3x3 for each (renderer.cpp:1795-1797). Shrinking CBPerObject for the shadow path deletes 340 matrix inversions per frame. Deferred because it touches the shared CBPerObject struct that both passes deliberately share, and the current comment explains that sharing as a drift-prevention measure.
- The zenith light-basis flip. The `|lightDir.y| > 0.99` up-vector switch is a hard ~90-degree basis rotation sitting exactly where a sun passes at noon, and the texel snap makes it MORE visible by popping against otherwise-perfect stability. Design 2's Frisvad-tangent-in-a-permuted-frame fix (moving the singularity to nadir, where there is no direct light) is correct and should be its own commit with its own test — asserting continuity of the UP VECTOR only, never of the snapped matrices, which are deliberately discontinuous at texel boundaries.
- Removing rasterizer depth bias in favour of receiver-plane depth bias. Design 2's RPDB Jacobian is correct and its per-cascade units are the principled answer to varying texel size, but landing it alongside DepthClipEnable=FALSE and CullMode=NONE stakes the one region that renders correctly today on an unmeasured bias, with no fallback specified. If far cascades show acne after this change, raise SlopeScaledDepthBias first; per-cascade PSOs are the purely-additive last resort. Cascade 0 is immune either way.
- Moving SetDescriptorHeaps out of BeginFrame into BeginFrameRing. BeginShadowPass never calls SetDescriptorHeaps (renderer.cpp:1594-1631), which is legal ONLY because the shadow PSO has no pixel shader and references no descriptor table. That legality is load-bearing and fragile: alpha-tested casters, a very plausible companion feature, would need both SetDescriptorHeaps and the material table bound here, and the failure mode is device removal, not a compile error. Deferred to keep step 1's blast radius minimal.
- Reference-image comparison for shadow appearance. Blocked until the texture set is pinned as tracked input or the procedural fallback is made mandatory under --smoke — README.md:129-143 measures mean luminance moving 127.5 -> 124.4 across checkouts purely from leftover PNGs in build/<Config>/assets/textures/, which is build output, not tracked source.
- Fixing the smoke gate so `tools\smoke_test.cmd` with no arguments exercises a shadow assertion. app.cpp:1160 gates the probe on !m_usePathTracing and the default run is the RT path. Out of scope, recorded so it is not assumed.

## openQuestions
- PACKOFFSET ALL-OR-NOTHING, UNVERIFIED. I did not run FXC. I am confident packoffset works on cbuffer members in ps_5_1 and that FXC rejects overlapping packoffset registers (which is the entire mechanism protecting failure mode F9), but I am NOT certain whether FXC requires that if ANY member carries packoffset then ALL must (I believe it does, hence specifying it on all 17 members), nor am I fully certain it accepts packoffset on the START of a float4x4 ARRAY (`lightViewProj[4] : packoffset(c7)`). Step 2 of the plan is a standalone fxc invocation precisely to settle both before any C++ is written. If either is rejected, drop packoffset entirely, keep the plain float4 declarations, and RECORD IN THE COMMIT that failure mode F9 then has no automated detector at all — do not silently proceed.
- THE DEMO SCENE NEVER SELECTS ANY CASCADE BUT ZERO, so the shipped selector is effectively unexercised at runtime by the harness. I checked: app.cpp:180 puts the camera at (0,2,-5), app.cpp:209 creates a 10x10 ground plane, and the other objects sit within ~4.5 units of the origin. Maximum radial distance from the camera to any visible geometry is roughly 11-12 units, against cascade 0's split radius of 22.857. Cascades 1-3 will render (F1 confirms that) and their matrices will be correct (F2 confirms that), but no pixel in the smoke capture ever samples them. This is ALSO why I rejected Design 4's --smoke-cascade-debug hue-class-count assertion, which it called 'the ONLY check on the cascade selector': every visible pixel would classify as cascade 0's hue, so its stated negative test (hardwire cascade=0, watch the class count drop to 1) would show no change whatsoever. Closing this gap needs a scene change — moving the smoke camera far back, or extending the ground plane past 450 units — which perturbs capture statistics that README.md already documents as run-to-run unstable. I did not resolve it and I do not recommend guessing a threshold.
- DEPTH-BIAS SCALING FOR A FLOAT DEPTH FORMAT. The claim that D3D12 scales RasterizerState.DepthBias by the exponent of the maximum z in the primitive — making the constant term a roughly fixed NDC offset, and therefore a world-space bias proportional to depthRange and hence to texelWorld — is the reason ONE shared shadow PSO suffices across four cascades whose texel sizes differ by 19.6x. I am moderately, not fully, confident in the exact rule and did not verify it empirically. Cascade 0 is immune (same extent, same depth range, same bias as today). If far cascades show acne, raise SlopeScaledDepthBias first; per-cascade PSOs are the purely-additive last resort.
- CASCADE 3's ORTHO DEPTH RANGE IS 2350 UNITS. Ortho depth is linear and D32_FLOAT gives ~6e-8 relative precision, so quantisation is ~1.4e-4 units — fine. But any future move to a reversed-Z or perspective shadow projection would not survive this range unexamined, and nothing in the plan would catch it.
- THE RADIAL FIT IS GENEROUS AT RANGE. Cascade 3 covers a 470-unit half-extent to serve a ~448-unit radial reach, which is inherent to the origin-centred formulation and is the price of the containment theorem and of exact rotation invariance. A frustum-fitted design (Designs 2/3/4) gets more resolution per texel at range for the same memory. I chose the theorem over the resolution deliberately, but it is a real trade and a reviewer may reasonably disagree.
- 64 MiB of shadow memory, up from 16 MiB. Array slices must share dimensions, so per-cascade resolution reduction is not available without abandoning the single-resource design and with it the zero-binding-change property. I did not evaluate whether any target device makes 64 MiB a problem.
- THE STEP-9 BLEND CANNOT BE VERIFIED BY THIS PROJECT. The demo scene sits entirely inside cascade 0, so the resolution seam that step 9 removes never appears and its removal cannot be observed. If step 9 is implemented, it ships on reasoning alone. I would rather say that plainly than attach a green assertion to it that proves nothing.
