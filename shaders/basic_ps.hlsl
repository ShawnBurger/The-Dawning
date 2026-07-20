// =============================================================================
// basic_ps.hlsl — The Dawning V3 Pixel Shader
// =============================================================================
// Raster material shader: Cook-Torrance/GGX direct lighting with texture-driven
// albedo and normal maps.
// =============================================================================

#include "display_common.hlsli"
#include "gpu_draw_records.hlsli"

// Supplied by Renderer::CreatePSO from core::kShadowCascadeCount and
// Renderer::kShadowMapSize. The fallbacks exist only for compiling this file
// standalone with fxc when checking syntax; the engine always defines both.
#ifndef SHADOW_CASCADE_COUNT
#define SHADOW_CASCADE_COUNT 4
#endif
#if SHADOW_CASCADE_COUNT != 4
#error "SHADOW_CASCADE_COUNT must be 4: the cascade tables below are float4."
#endif
#ifndef SHADOW_MAP_SIZE
#define SHADOW_MAP_SIZE 2048.0
#endif

// Mirrors struct CBPerFrame in src/render/renderer.h, which is static_assert'd
// at 416 bytes with four offsetof checks. sky_ps.hlsl declares ONLY the first
// 112 bytes of this layout and reads them by offset, so fields may only ever be
// APPENDED below lightViewProj.
//
// WHY packoffset IS ON EVERY MEMBER, AND WHY IT IS THE MOST VALUABLE LINE HERE.
// HLSL places every element of a cbuffer ARRAY on its own 16-byte register, so
// `float cascadeSplitRadius[4]` would be 64 bytes in HLSL against 16 in C++. That
// mismatch compiles, the member names still match, the C++ static_asserts still
// pass, and the shader silently reads 48 bytes of neighbouring data as splits
// 1..3 - meaning selection is correct only for cascade 0, the one cascade that
// already worked before cascades existed. Nothing in the build or the smoke
// harness would notice.
//
// packoffset converts that into a compile error: an overlapping register
// assignment is rejected with X4019. VERIFIED, not assumed - fxc /T ps_5_1 /WX
// /Od rejects both `float cascadeSplitRadius[4] : packoffset(c23)` (it would
// span c23..c26 and collide with cascadeTexelWorld at c24) and any field
// inserted into the frozen prefix. Zero bytes, zero runtime cost.
//
// Do NOT add `row_major`. Correctness depends on the CPU's row-major upload
// being reinterpreted by HLSL's default column-major packing, with mul(M, v)
// cancelling both transposes. `row_major` would silently transpose all four
// cascade matrices with no compile error.
cbuffer CBPerFrame : register(b1)
{
    float3   lightDir           : packoffset(c0);    // Normalized direction TO the light
    float    pad0               : packoffset(c0.w);
    float3   lightColor         : packoffset(c1);
    float    pad1               : packoffset(c1.w);
    float3   ambientColor       : packoffset(c2);
    float    pad2               : packoffset(c2.w);
    float3   eyePos             : packoffset(c3);    // Camera world position
    float    pad3               : packoffset(c3.w);
    // Camera basis; used by sky_ps.hlsl, which binds the same b1.
    float3   camRight           : packoffset(c4);
    float    tanHalfFovY        : packoffset(c4.w);
    float3   camUp              : packoffset(c5);
    float    aspect             : packoffset(c5.w);
    float3   camForward         : packoffset(c6);
    float    pad4               : packoffset(c6.w);
    // ---- frozen prefix ends at c7 / byte 112 -------------------------------
    float4x4 lightViewProj[SHADOW_CASCADE_COUNT] : packoffset(c7);   // 112..367
    float4   cascadeSplitRadius : packoffset(c23);                   // 368..383
    float4   cascadeTexelWorld  : packoffset(c24);                   // 384..399
    float4   cascadeFadeLo      : packoffset(c25);                   // 400..415 (reserved)
};

StructuredBuffer<MaterialData> materialBuffer : register(t0, space3);

// The merged draw-record probe, MATERIAL half. See gpu_draw_records.hlsli.
//
// This lives in the pixel shader and nowhere else, because this is the only
// stage that consumes materialBuffer. The earlier arrangement hashed the
// material record from basic_vs, which reads it for no other purpose - that
// witnesses the probe's own load, and stays green with the line below changed
// to materialBuffer[0]. Moving it here is what closes that gap.
//
// COMPILED OUT UNLESS DAWNING_DRAW_PROBE IS DEFINED, and that is not tidiness.
// Declaring a UAV in a pixel shader defeats early-Z for the whole PSO. A
// runtime gate cannot buy it back: `drawProbeEnabled` gates the WRITE, but it
// is the DECLARATION that tells the hardware this shader may have side effects,
// so a b3-gated probe still costs early-Z on every pixel of every frame. That
// is a per-frame cost on the main opaque pass, paid in Release, to serve an
// assertion that runs on one frame of a smoke run.
//
// So Renderer::CreatePSO compiles this file TWICE: once plain for m_pso, the
// PSO every real frame uses, which now declares no UAV and keeps early-Z; and
// once with DAWNING_DRAW_PROBE=1 for m_psoDrawProbe, bound only on the frame the
// probe runs. The probe therefore still works in EVERY configuration, Debug and
// Release alike - it is not compiled away in shipping builds, it is moved off
// the hot PSO. The runtime `drawProbeEnabled` gate is kept inside the probe
// permutation so the two PSOs agree on the root-constant layout.
#if DAWNING_DRAW_PROBE
RWByteAddressBuffer drawRecordProbe : register(u0, space4);
#endif

// Which record this draw owns. Declared identically in basic_vs.hlsl and
// shadow_vs.hlsl; the vertex stage reads objectIndex, this stage reads
// materialIndex. Both arrive in one root parameter, set by a single
// SetGraphicsRoot32BitConstants per draw.
//
// The index is a root constant and therefore wave-uniform BY CONSTRUCTION,
// which matters below: FXC at SM 5.1 has no NonUniformResourceIndex, so
// materialTextures[mat.albedoTextureIndex] must be uniform across the wave.
// One draw call per entity keeps it that way for free.
cbuffer CBDrawIndex : register(b3)
{
    uint objectIndex;
    uint materialIndex;
    uint drawProbeEnabled;
};

// Size comes from Renderer::kMaxRasterTextures, passed as a define at compile
// time, so the C++ heap, the root-signature range and this array cannot drift
// apart. The fallback exists only for standalone compilation (compiling this
// file directly with fxc, e.g. when checking shader syntax); the engine always
// supplies the define.
#ifndef MAX_RASTER_TEXTURES
#define MAX_RASTER_TEXTURES 128
#endif

Texture2D<float4> materialTextures[MAX_RASTER_TEXTURES] : register(t0);
SamplerState linearSampler : register(s0);

// Shadow map in its own register space so it cannot collide with the material
// table, however large that grows. s1 is a COMPARISON sampler: the hardware
// compares each of four texels against the reference depth and bilinearly
// filters the four boolean results, so one SampleCmpLevelZero is already 2x2
// percentage-closer filtering.
// A Texture2DArray with one slice per cascade. This is the SM 5.1-compatible
// cascade indexing form: the slice is a texture COORDINATE, not an addressed
// dimension, so s1's OPAQUE_WHITE border still applies per-slice and an
// out-of-footprint tap still reads as lit with no branch.
Texture2DArray<float>     shadowMap     : register(t0, space1);
SamplerComparisonState    shadowSampler : register(s1);

#include "brdf_common.hlsli"   // PI and the microfacet BRDF, shared with path_trace.hlsl

// Returns 1 for fully lit, 0 for fully shadowed.
//
// NdotL is used for a normal-offset: the shading point is pushed along its
// normal before projection, by an amount that grows as the surface turns away
// from the light. That handles the acne the rasteriser depth bias cannot,
// because depth bias acts along the light direction while the sampling error is
// across the surface. The two together are what keeps flat ground clean without
// the shadows visibly detaching from their casters.
float ComputeShadow(float3 positionWS, float3 N, float NdotL)
{
    // Radial view distance. positionWS is camera-relative (RULE 1), so the
    // camera IS the origin of this space and length() is the exact radial
    // distance. Deliberately NOT distance(positionWS, eyePos): eyePos is
    // hardwired to zero on the C++ side, so that form is correct only by
    // accident and would break silently the moment a real camera position were
    // written there. Also not input.positionCS.w, which the rasteriser has
    // already reciprocated by the time a pixel shader sees it.
    const float viewDist = length(positionWS);

    // Select the tightest cascade containing this point, defaulting to the
    // outermost. Written as three literal-indexed `if` statements rather than an
    // [unroll] loop for two reasons: every cbuffer access stays a literal index
    // and every table access a static swizzle (no dynamic subscript for FXC to
    // reject), and selecting the MATRIX before the PCF kernel rather than
    // duplicating the kernel per cascade keeps this at exactly 9
    // SampleCmpLevelZero instructions. `[unroll] for(i) if (cascade==i) { 9 taps }`
    // flattens to 36 with no warning. VERIFIED: fxc /Fc reports 9 sample_c_lz.
    //
    // Descending order with strict `<` and plain assignment (never `break`)
    // means the tightest containing cascade wins, and the single exit FXC's
    // X4000 analysis requires is preserved. core::SelectShadowCascade has the
    // identical structure so the unit tests constrain this arithmetic.
    float4x4 cascadeVP  = lightViewProj[SHADOW_CASCADE_COUNT - 1];
    float    texelWorld = cascadeTexelWorld.w;
    float    slice      = 3.0f;
    if (viewDist < cascadeSplitRadius.z) { cascadeVP = lightViewProj[2]; texelWorld = cascadeTexelWorld.z; slice = 2.0f; }
    if (viewDist < cascadeSplitRadius.y) { cascadeVP = lightViewProj[1]; texelWorld = cascadeTexelWorld.y; slice = 1.0f; }
    if (viewDist < cascadeSplitRadius.x) { cascadeVP = lightViewProj[0]; texelWorld = cascadeTexelWorld.x; slice = 0.0f; }

    // Beyond the last split this keeps cascade 3 and the border sampler returns
    // lit. No early-out on viewDist: that would add a second, SPHERICAL cutoff
    // alongside the square footprint boundary, and the two disagreeing is
    // exactly the ring artifact that reads as a bug.

    const float slope  = saturate(1.0f - NdotL);
    const float offset = texelWorld * (1.0f + 3.0f * slope);
    float3 offsetPos   = positionWS + N * offset;

    float4 lightClip = mul(cascadeVP, float4(offsetPos, 1.0f));

    // Single exit rather than early returns. FXC's flow analysis reports X4000
    // "potentially uninitialized" for a function whose returns sit inside
    // branches, and the raster path compiles with /WX - so this shape is load
    // bearing, not style.
    float result = 1.0f;

    // w is 1 for the orthographic light projection, so this guard is defensive
    // against a future perspective (spot/point) light rather than live today.
    if (lightClip.w > 0.0f)
    {
        lightClip.xyz /= lightClip.w;

        // Outside the frustum along Z there is no depth information; treat as
        // lit rather than shadowed. The XY case is handled by the sampler's
        // white border, so it needs no branch here.
        //
        // Because radial selection never hands a point to a cascade that does
        // not contain it, this z-guard provably never trips for a selected
        // point: light-space z lands in [1.546E, 3.454E] inside the [0.1, 5E]
        // slab, i.e. ndc [0.309, 0.691]. It is retained purely as defence
        // against a future perspective (spot/point) light.
        if (lightClip.z >= 0.0f && lightClip.z <= 1.0f)
        {
            // Clip space to texture space. Y flips because clip space is +Y up
            // and texture space is +Y down.
            float2 shadowUV = float2(lightClip.x * 0.5f + 0.5f,
                                     -lightClip.y * 0.5f + 0.5f);

            // 3x3 grid of hardware-PCF taps: 9 taps, each already 2x2 filtered,
            // so the effective kernel is 4x4 texels. Enough to hide the texel
            // grid at this resolution without a separate blur pass.
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

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD0;
    float3 normalWS   : TEXCOORD1;
    float4 color      : COLOR;
    float2 uv         : TEXCOORD2;
};

float3 ApplyNormalMap(float3 normalWS, float3 positionWS, float2 uv, uint textureIndex)
{
    float3 N = normalize(normalWS);
    float3 dpdx = ddx(positionWS);
    float3 dpdy = ddy(positionWS);
    float2 duvdx = ddx(uv);
    float2 duvdy = ddy(uv);

    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 T = normalize(cross(up, N));
    float3 B = normalize(cross(N, T));

    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(det) >= 1e-6)
    {
        // Divide by det, do not merely test it. The tangent is
        // (dpdx*duvdy.y - dpdy*duvdx.y) / det; using the numerator alone discards
        // sign(det), and normalize() below cannot recover it - so T came out
        // negated wherever the UV parameterisation is mirrored. Both normal-mapped
        // objects in the demo scene have det < 0, and the DXR path computes this
        // correctly, so the two render paths disagreed on the same geometry.
        float3 derivedT = (dpdx * duvdy.y - dpdy * duvdx.y) / det;
        if (dot(derivedT, derivedT) > 1e-8)
        {
            T = normalize(derivedT - N * dot(N, derivedT));
            B = normalize(cross(N, T));
        }
    }

    float3 tangentNormal = materialTextures[textureIndex].Sample(linearSampler, uv).xyz * 2.0 - 1.0;
    tangentNormal.z = max(tangentNormal.z, 0.0);
    float3 mappedNormal = tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N;
    return dot(mappedNormal, mappedNormal) > 1e-8 ? normalize(mappedNormal) : N;
}

float4 main(PSInput input) : SV_TARGET
{
    MaterialData mat = materialBuffer[materialIndex];

    // Witness the record THIS SHADER SHADES WITH, not a second load of it. `mat`
    // is the variable every line below reads; hashing anything else here would
    // reintroduce exactly the blind spot this probe exists to remove.
#if DAWNING_DRAW_PROBE
    if (drawProbeEnabled != 0)
    {
        DawningWriteMaterialProbe(drawRecordProbe, objectIndex, mat);
    }
#endif

    float3 N = normalize(input.normalWS);
    if (mat.useNormalTexture != 0)
        N = ApplyNormalMap(N, input.positionWS, input.uv, mat.normalTextureIndex);

    float3 V = normalize(eyePos - input.positionWS);
    float3 L = normalize(lightDir);
    float3 H = normalize(L + V);

    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float NdotV = saturate(dot(N, V));
    float VdotH = saturate(dot(V, H));

    // Base color from material albedo * vertex color * optional albedo texture
    float3 baseColor = mat.albedo.rgb * input.color.rgb;
    if (mat.useAlbedoTexture != 0)
        baseColor *= materialTextures[mat.albedoTextureIndex].Sample(linearSampler, input.uv).rgb;

    // Packed occlusion / roughness / metallic (glTF: AO=R, rough=G, metal=B).
    // MODULATES the material scalars rather than replacing them, so those stay
    // usable as per-instance tints - which is what glTF specifies.
    float materialRoughness = mat.roughness;
    float materialMetallic  = mat.metallic;
    float ambientOcclusion  = 1.0;
    if (mat.useOrmTexture != 0)
    {
        float3 orm = materialTextures[mat.ormTextureIndex].Sample(linearSampler, input.uv).rgb;
        ambientOcclusion  = orm.r;
        materialRoughness *= orm.g;
        materialMetallic  *= orm.b;
    }

    // Clamped AFTER modulation: an ORM map with near-zero green would otherwise
    // drive roughness below the value the GGX denominator is conditioned for.
    materialRoughness = clamp(materialRoughness, 0.04, 1.0);
    materialMetallic  = saturate(materialMetallic);

    // Cook-Torrance direct lighting
    float3 F0 = DawningF0(baseColor, materialMetallic);
    
    float3 F = DawningFresnelSchlick(VdotH, F0);
    float3 specular = DawningCookTorranceSpecular(NdotV, NdotL, NdotH, VdotH,
                                                  materialRoughness, F0);
    float3 kD = DawningDiffuseWeight(F, materialMetallic);
    float3 diffuse = kD * baseColor / PI;

    // Shadowing multiplies DIRECT light only. Ambient and emission are
    // deliberately untouched: ambient is a crude stand-in for everything the
    // single directional light does not carry, so occluding it too would leave
    // shadowed regions pure black, and emission is produced by the surface
    // rather than received. This matches what the path tracer does, where the
    // shadow ray gates the NEE term and nothing else.
    float shadow = ComputeShadow(input.positionWS, N, NdotL);

    float3 direct = (diffuse + specular) * lightColor * NdotL * shadow;

    // Ambient (hemisphere approximation — ground color darker)
    float hemisphereBlend = N.y * 0.5 + 0.5;
    float3 groundColor = ambientColor * 0.3;
    float3 ambientDiffuse = baseColor * lerp(groundColor, ambientColor, hemisphereBlend)
                          * (1.0 - materialMetallic) * ambientOcclusion;
    float3 ambientSpecular = DawningFresnelSchlick(NdotV, F0) * (ambientColor + 0.04) *
                             lerp(0.2, 0.8, 1.0 - materialRoughness) * ambientOcclusion;

    // Emission is added last and is unaffected by lighting, occlusion or the
    // Fresnel split: it is radiance the surface produces, not radiance it
    // reflects. It is also NOT a light source - nothing else in the scene is
    // brightened by it.
    float3 emission = mat.emissive * mat.emissiveStrength;
    if (mat.useEmissiveTexture != 0)
        emission *= materialTextures[mat.emissiveTextureIndex].Sample(linearSampler, input.uv).rgb;

    // Combine
    float3 finalColor = direct + ambientDiffuse + ambientSpecular + emission;

    // Linear HDR out. Tone mapping happens once, in tonemap_ps.hlsl, so that
    // the scene exists as linear radiance in a buffer that bloom/exposure/TAA
    // can consume.
    //
    // Clamped to the largest finite half. The target is R16G16B16A16_FLOAT, and
    // the specular term can legitimately spike past 65504 at grazing angles,
    // where the Cook-Torrance denominator sits on its floor while the GGX peak
    // is large. Storing that as +Inf makes the Reinhard operator in the resolve
    // evaluate Inf/(Inf+1) = NaN, which reads back as near-black speckle. It did
    // not surface before because tone mapping happened in the pixel shader, in
    // fp32, before anything was stored - so the overflow never existed. This is
    // a storage-range clamp, not an artistic one: Reinhard has already saturated
    // to 255 by ~1000, so nothing visible is lost.
    finalColor = min(finalColor, 65504.0);

    return float4(finalColor, mat.albedo.a * input.color.a);
}
