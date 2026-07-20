// =============================================================================
// path_trace.hlsl — Full Path Tracing Shader
// =============================================================================
// Megakernel path tracer with Next Event Estimation (NEE).
// Ray generation → iterative bounces → closest hit (material eval) → shadow rays.
//
// Architecture designed for future SER + ReSTIR upgrade:
//   - Single DispatchRays, iterative loop (no recursion beyond shadow test)
//   - Material data via global StructuredBuffer indexed by InstanceID()
//   - Disney/GGX BSDF evaluation in closest hit
//
// Target: SM 6.3 (lib_6_3), DXR 1.1
// =============================================================================

#include "display_common.hlsli"
#include "sky_common.hlsli"

// =============================================================================
// Global root signature bindings
// =============================================================================
RaytracingAccelerationStructure g_Scene : register(t0, space0);
RWTexture2D<float4>             g_Output : register(u0, space0);
RWTexture2D<float4>             g_DisplayOutput : register(u1, space0);

cbuffer PerFrameConstants : register(b0, space0)
{
    float4x4 g_ViewProj;        // Reserved for future inverse-VP/reprojection work
    float4   g_CameraPos;
    float4   g_CameraRight;
    float4   g_CameraUp;
    float4   g_CameraForward;
    float4   g_LightDir;
    float4   g_LightColor;
    // VESTIGIAL as of IBL Stage 4, and deliberately NOT removed. The stable
    // preview was its last reader; it now evaluates the same physically-derived
    // environment the raster path does. The field stays because this cbuffer is
    // mirrored byte-for-byte by RTPerFrameConstants and removing a float4 from
    // the middle would shift every field after it - a layout change with no
    // benefit. CBPerFrame's ambientColor is vestigial for the same reason on the
    // raster side. Keep uploading it.
    float4   g_AmbientColor;
    uint     g_FrameIndex;
    uint     g_MaxBounces;
    uint     g_RenderWidth;
    uint     g_RenderHeight;
    uint     g_SamplesPerPixel;
    uint     g_StablePreview;
    uint     g_SeedIndex;       // Wall-clock dispatch counter — RNG decorrelation only
    float    g_TanHalfFovY;     // tan(fovY/2) from the camera; see RTPerFrameConstants
    // Primary ray-cone spread angle in radians per pixel of output height,
    // computed CPU-side by render::PrimaryRayConeSpreadAngle. See rt_texture_lod.h.
    float    g_PrimaryConeSpread;
    // 212..223. The C++ twin carries an explicit pad0[3] here because HLSL
    // rounds the float4 array below up to offset 224 and C++ would not. See the
    // static_asserts on RTPerFrameConstants in src/render/rt_pipeline.h - they
    // are what turn that 12-byte shear into a build error.
    float3   g_Pad0;
    // Image-based lighting. Read by the STABLE PREVIEW only; the full path
    // tracer samples DawningSkyRadiance on every miss instead, which is the
    // integral these coefficients approximate. See the stable-preview block.
    float4   g_IblSH[9];        // 224..367
    float4   g_IblParams;       // 368..383  x=mipCount y=intensity z=enable w=probe
};

struct MaterialData
{
    float4 albedo;
    float  roughness;
    float  metallic;
    uint   albedoTextureIndex;
    uint   useAlbedoTexture;
    uint   normalTextureIndex;
    uint   useNormalTexture;
    uint   ormTextureIndex;
    uint   useOrmTexture;
    float3 emissive;
    float  emissiveStrength;
    uint   emissiveTextureIndex;
    uint   useEmissiveTexture;
    uint2  materialPad;
};
StructuredBuffer<MaterialData> g_Materials : register(t1, space0);

struct TriangleNormalData
{
    float4 n0;
    float4 n1;
    float4 n2;
};
StructuredBuffer<TriangleNormalData> g_TriangleNormals : register(t2, space0);

struct InstanceData
{
    uint   triangleNormalOffset;
    uint   triangleUVOffset;
    uint   trianglePositionOffset;
    uint   pad;
    // Transposed object-to-world normal matrix; see RTInstanceData in mesh.h.
    float4 normalMatrix0;
    float4 normalMatrix1;
    float4 normalMatrix2;
};
StructuredBuffer<InstanceData> g_InstanceData : register(t3, space0);

struct TriangleUVData
{
    float4 uv0;
    float4 uv1;
    float4 uv2;
};
StructuredBuffer<TriangleUVData> g_TriangleUVs : register(t4, space0);

struct TrianglePositionData
{
    float4 p0;
    float4 p1;
    float4 p2;
};
StructuredBuffer<TrianglePositionData> g_TrianglePositions : register(t5, space0);

Texture2D<float4> g_AlbedoTextures[64] : register(t0, space4);
Texture2D<float4> g_NormalTextures[64] : register(t0, space5);
Texture2D<float4> g_OrmTextures[64]    : register(t0, space6);
Texture2D<float4> g_EmissiveTextures[64] : register(t0, space7);
// Texture LOD here comes from RAY CONES, not from screen-space derivatives - a
// ray has none. See the "Ray cone texture LOD" block below for the formulation
// and RayConeTextureLod for the per-sample-site term. Every SampleLevel call in
// this file passes a computed mip; none of them force mip 0 any more.
//
// This sampler stays MIN_MAG_MIP_LINEAR on purpose. Making it ANISOTROPIC would
// be inert without a mip chain to select across, and that exact mistake has
// already been made once on the raster side (MaxAnisotropy=16 set while the
// filter was MIN_MAG_MIP_LINEAR). Anisotropic DXR filtering would additionally
// need a per-hit anisotropy axis, which ray cones do not carry - a cone is
// isotropic by construction. The grazing-angle term below picks the MAJOR axis
// of the footprint, so a surface seen edge-on blurs rather than aliases. That is
// the isotropic-filtering trade, and it is a known residual difference from the
// raster path's anisotropic filtering.
SamplerState g_TextureSampler : register(s0, space0);

// =============================================================================
// The prefiltered environment cubemap — THE SAME RESOURCE THE RASTER PATH USES
// =============================================================================
// basic_ps.hlsl declares this cube at t0/space6 with sampler s2; this file
// declares it at t0/space8 with sampler s1. THE DECLARATIONS DIFFER AND THE
// EVALUATION DOES NOT, which is the entire point of shaders/ibl_common.hlsli
// taking its resources as function PARAMETERS: two root signatures, two
// toolchains (FXC ps_5_1 and DXC lib_6_3), one copy of the split-sum integral.
//
// The register spaces differ because space6 is already the ORM texture table
// here. There is no shared numbering to preserve - the anti-divergence claim is
// about the CODE that reads the resource, not about where it is bound.
//
// Sampler s1 is trilinear/CLAMP, matching the raster s2. The address mode is
// documented as inert for cube sampling (CLAUDE.md records that assertion 3.3
// cannot catch a WRAP sampler because cube lookups never consult the 2-D address
// modes) and is set to match anyway rather than leave the two paths differing on
// a line a reader would have to check.
TextureCube<float4> g_EnvCube     : register(t0, space8);
SamplerState        g_EnvSampler  : register(s1, space0);

// The DXR IBL CONSUMPTION probe's target. See shaders/ibl_consume_probe.hlsli
// for what it claims; this is the DXR twin of basic_ps.hlsl's u1/space4.
//
// A ROOT UAV rather than a descriptor-table entry: it costs 2 root DWORDs and
// zero heap slots, and a raw buffer is exactly what a root UAV supports. It is
// bound on EVERY dispatch - a root descriptor cannot be left unbound - and the
// WRITES are gated on g_IblParams.w, so ordinary frames pay one wave-uniform
// branch and no traffic.
//
// Unlike the raster side there is no PSO permutation here. That gating exists in
// basic_ps because a UAV declaration in a PIXEL shader defeats early-Z for the
// whole PSO; a DXR raygen shader has no early-Z to lose, so the declaration is
// unconditional and the cost is genuinely just the branch.
RWByteAddressBuffer g_IBLProbe : register(u0, space4);

// =============================================================================
// Ray cone texture LOD
// =============================================================================
// Akenine-Möller, Nilsson, Andersson, Barré-Brisebois, Toth, Karras,
// "Texture Level of Detail Strategies for Real-Time Ray Tracing",
// Ray Tracing Gems (2019), chapter 20.
//
// A cone is tracked along each path. Its width at a hit is
//     coneWidth = widthAtOrigin + spreadAngle * hitDistance
// and the mip for a texture of W x H texels is
//     lambda = 0.5*log2(uvArea / worldArea)      <- per-triangle, unitless
//            + log2(coneWidth / |dot(rayDir, geometricNormal)|)
//            + 0.5*log2(W * H)                   <- per-texture
// The first two terms are texture-independent, so the closest-hit shader
// computes them once and hands the sum ("lodBase") back through the payload;
// each sample site adds only its own 0.5*log2(W*H).
//
// The CPU mirror of this arithmetic, and the unit tests that pin its signs and
// log base, live in src/render/rt_texture_lod.{h,cpp} and
// tests/test_rt_texture_lod.cpp. Those tests do NOT compile this file - keep
// the two in step by hand.

// Sentinel for a triangle with no usable UV or world area (zero-area triangle,
// or UVs collapsed to a point). Large and negative so that adding any real
// 0.5*log2(W*H) still clamps to mip 0 - i.e. degenerate triangles fall back to
// exactly the old behaviour rather than to a garbage mip.
static const float kRayConeDegenerateLod = -64.0f;

// Diffuse bounces sample a full cosine-weighted hemisphere, so the outgoing
// "cone" is a fiction - the true footprint is the whole hemisphere. This is a
// deliberately coarse stand-in: diffuse indirect is low-frequency, and a coarser
// mip is both cheaper to sample and less noisy than a sharp one. Roughly a 29
// degree spread per diffuse bounce.
static const float kDiffuseBounceConeSpread = 0.5f;

// Grazing-angle floor. Without it, |dot(rayDir, N)| -> 0 at the horizon sends
// lambda to +inf and the far ground plane collapses to its 1x1 mip. 0.1 caps the
// grazing term at +3.32 mips.
//
// This term is the one place a ray cone cannot imitate the raster path. Cones
// are isotropic, so the footprint gets one number; anisotropic filtering
// resolves the minor axis separately and keeps detail a cone has to throw away.
// The floor is the knob that trades the resulting over-blur against aliasing,
// and 0.1 was MEASURED, not guessed. Mean absolute horizontal pixel-to-pixel
// contrast over the distant-ground band of the smoke capture (rows 420-480),
// against the raster capture of the same frame as the reference:
//
//     mip 0 (before)      4.876      3.1x the reference: aliasing
//     floor 0.02          0.813      over-blurred past the reference
//     floor 0.10          1.694      within 7% of the reference   <- chosen
//     floor 0.25          3.538      still visibly aliasing
//     raster (aniso)      1.582      the reference
static const float kRayConeGrazingFloor = 0.1f;

// The payload carries the cone IN (spread angle, width at ray origin) and the
// resolved lodBase OUT, through one 32-bit field. Two halves rather than two
// floats keeps RayPayload at 32 bytes; half precision costs ~0.001 mip here,
// because everything downstream of the pack is inside a log2.
uint PackRayCone(float spreadAngle, float widthAtOrigin)
{
    return (f32tof16(spreadAngle) & 0xFFFFu) | (f32tof16(widthAtOrigin) << 16);
}

float2 UnpackRayCone(uint packedCone)
{
    return float2(f16tof32(packedCone & 0xFFFFu), f16tof32(packedCone >> 16));
}

// 0.5*log2(uvArea / worldArea). Both areas are twice the true triangle area; the
// factor of 2 cancels in the ratio, so it is not worth paying for.
float RayConeTriangleLodConstant(
    float2 uv0, float2 uv1, float2 uv2,
    float3 p0,  float3 p1,  float3 p2)
{
    float2 duv1 = uv1 - uv0;
    float2 duv2 = uv2 - uv0;
    float uvArea = abs(duv1.x * duv2.y - duv1.y * duv2.x);
    float worldArea = length(cross(p1 - p0, p2 - p0));

    if (uvArea < 1e-12f || worldArea < 1e-12f)
        return kRayConeDegenerateLod;

    return 0.5f * log2(uvArea / worldArea);
}

// Texture-independent part of lambda. Returned through the payload.
float RayConeLodBase(float triangleLodConstant, float coneWidth, float normalDotRayDir)
{
    if (triangleLodConstant <= kRayConeDegenerateLod || coneWidth <= 0.0f)
        return kRayConeDegenerateLod;

    float grazing = max(abs(normalDotRayDir), kRayConeGrazingFloor);
    return triangleLodConstant + log2(coneWidth / grazing);
}

// Per-sample-site term. Clamped at 0 because SampleLevel with a negative LOD is
// just mip 0 with extra steps, and because it is what turns the degenerate
// sentinel back into the old mip-0 behaviour.
float RayConeTextureLod(float lodBase, uint texWidth, uint texHeight)
{
    return max(lodBase + 0.5f * log2(float(texWidth) * float(texHeight)), 0.0f);
}

// =============================================================================
// Ray payload structures
// =============================================================================
struct RayPayload
{
    float  hitT;
    float3 normal;
    float2 uv;
    uint   instanceID;
    // Dual-purpose, and the only field written in both directions:
    //   IN  (set by RayGen before TraceRay) - PackRayCone(spreadAngle, widthAtOrigin)
    //   OUT (set by ClosestHit)             - asuint(lodBase)
    // Reusing the former `pad` keeps the payload at 32 bytes. The miss shader
    // does not write it, but a miss breaks out of the bounce loop before anything
    // reads it back.
    uint   conePacked;
};

struct ShadowPayload
{
    float shadowFactor; // 0 = in shadow, 1 = lit
};

// =============================================================================
// Utility functions
// =============================================================================
#include "brdf_common.hlsli"   // PI and the microfacet BRDF, shared with basic_ps.hlsl
// The ONE image-based-lighting evaluation, shared with basic_ps.hlsl. Must come
// after brdf_common.hlsli, which defines PI. IBL_DESIGN.md 8.3 layer 2.
#include "ibl_common.hlsli"
// The consumption probe's writers, shared with basic_ps.hlsl for the same
// reason: the evidence format and the fixed-point scale have one definition, so
// the raster and DXR blocks are reduced by the SAME ReduceIBLConsumeProbe.
#include "ibl_consume_probe.hlsli"

float3 SafeNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return lenSq > 1e-8f ? value * rsqrt(lenSq) : fallback;
}

float3 SpawnRayOrigin(float3 hitPosition, float3 surfaceNormal, float3 outgoingDirection)
{
    // Camera-relative rendering keeps this scale tied to camera distance instead
    // of absolute world position. The proportional term is about two fp32 ULPs;
    // the floor covers local intersection and normal-reconstruction error near
    // the origin without the old scene-wide 1 mm bias. Offset toward the side
    // the spawned ray actually leaves so this also remains valid for future
    // transmission rays.
    const float kMinimumOffset = 1.0e-4f;
    const float kPositionErrorScale = 4.76837158203125e-7f; // 2^-21
    float positionMagnitude = max(abs(hitPosition.x),
                                  max(abs(hitPosition.y), abs(hitPosition.z)));
    float offsetDistance = max(kMinimumOffset, positionMagnitude * kPositionErrorScale);
    float side = dot(surfaceNormal, outgoingDirection) >= 0.0f ? 1.0f : -1.0f;
    return hitPosition + surfaceNormal * (side * offsetDistance);
}

// PCG hash for random number generation
uint PCGHash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Random(inout uint seed)
{
    seed = PCGHash(seed);
    return float(seed) / 4294967295.0f;
}

uint InitRNG(uint2 pixel, uint frameIndex)
{
    // Hash each dimension in sequence. The previous form packed the three values into
    // one uint via multiplies (pixel.y * 16384 + frameIndex * 2^28), which wrapped mod
    // 2^32 at frameIndex 16 — only sixteen distinct frame seeds existed — and aliased
    // pixel/frame contributions against each other. Each PCGHash fully avalanches, so
    // chaining them decorrelates all three dimensions with no packing constraints.
    uint seed = PCGHash(pixel.x);
    seed = PCGHash(seed ^ pixel.y);
    seed = PCGHash(seed ^ frameIndex);
    return seed;
}

// Cosine-weighted hemisphere sampling (for diffuse)
float3 SampleCosineHemisphere(float2 u, float3 N)
{
    // Generate direction in local space
    float phi = 2.0f * PI * u.x;
    float cosTheta = sqrt(u.y);
    float sinTheta = sqrt(1.0f - u.y);

    float3 localDir = float3(
        sinTheta * cos(phi),
        cosTheta,
        sinTheta * sin(phi)
    );

    // Build tangent frame from normal
    float3 up = abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);

    return normalize(T * localDir.x + N * localDir.y + B * localDir.z);
}

float3 ClampFireflySample(float3 sampleRadiance)
{
    // Scrub NaN/Inf first. The old code computed maxLum/sampleLum with an infinite
    // sampleLum, evaluating Inf * 0 = NaN, and wrote that straight to g_Output
    // where it would poison the accumulation buffer permanently.
    if (any(isnan(sampleRadiance)) || any(isinf(sampleRadiance)))
        return float3(0.0f, 0.0f, 0.0f);

    float sampleLum = DawningLuminance(sampleRadiance);
    if (sampleLum <= 0.0001f)
        return sampleRadiance;

    // Fixed energy ceiling per sample. Still biased - any firefly clamp is - but
    // biased by a constant rather than by a history-dependent ratchet.
    const float kMaxSampleLuminance = 24.0f;
    if (sampleLum > kMaxSampleLuminance)
        sampleRadiance *= kMaxSampleLuminance / sampleLum;

    return sampleRadiance;
}

float3 ResolveAlbedo(MaterialData mat, float2 uv, float lodBase)
{
    float3 albedo = mat.albedo.rgb;
    if (mat.useAlbedoTexture != 0 && mat.albedoTextureIndex < 64)
    {
        uint textureIndex = NonUniformResourceIndex(mat.albedoTextureIndex);
        uint texWidth, texHeight;
        g_AlbedoTextures[textureIndex].GetDimensions(texWidth, texHeight);
        float lod = RayConeTextureLod(lodBase, texWidth, texHeight);
        albedo *= g_AlbedoTextures[textureIndex].SampleLevel(g_TextureSampler, uv, lod).rgb;
    }
    return saturate(albedo);
}

float3 TransformObjectToWorldPoint(float3 objectPosition)
{
    float3x4 objToWorld = ObjectToWorld3x4();
    float4 p = float4(objectPosition, 1.0f);
    return float3(
        dot(objToWorld[0], p),
        dot(objToWorld[1], p),
        dot(objToWorld[2], p));
}

float3 ApplyNormalMap(
    MaterialData mat,
    float3 baseNormal,
    float3 worldP0,
    float3 worldP1,
    float3 worldP2,
    float2 uv0,
    float2 uv1,
    float2 uv2,
    float2 surfaceUV,
    float  lodBase)
{
    float3 N = normalize(baseNormal);
    if (mat.useNormalTexture == 0 || mat.normalTextureIndex >= 64)
        return N;

    float3 up = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T = SafeNormalize(cross(up, N), float3(1.0f, 0.0f, 0.0f));
    float3 B = SafeNormalize(cross(N, T), float3(0.0f, 0.0f, 1.0f));

    float3 edge1 = worldP1 - worldP0;
    float3 edge2 = worldP2 - worldP0;
    float2 duv1 = uv1 - uv0;
    float2 duv2 = uv2 - uv0;
    float det = duv1.x * duv2.y - duv1.y * duv2.x;

    if (abs(det) >= 1e-6f)
    {
        float invDet = 1.0f / det;
        float3 derivedT = (edge1 * duv2.y - edge2 * duv1.y) * invDet;
        float3 derivedB = (-edge1 * duv2.x + edge2 * duv1.x) * invDet;
        derivedT = derivedT - N * dot(N, derivedT);
        if (dot(derivedT, derivedT) > 1e-8f)
        {
            T = normalize(derivedT);
            B = SafeNormalize(cross(N, T), B);
            if (dot(B, derivedB) < 0.0f)
                B = -B;
        }
    }

    // The normal map dominates both halves of the ray-cone result, measured on
    // the smoke capture's distant-ground band (rows 420-480) against the raster
    // capture. Mipping albedo/ORM/emissive but leaving THIS at mip 0 takes the
    // band's pixel-to-pixel contrast from 4.876 to 3.666; mipping the normal map
    // as well takes it to 1.694, against a raster reference of 1.582. Most of
    // the aliasing in a path-traced frame was shading noise from point-sampling
    // a minified normal map, not colour noise from the albedo.
    //
    // KNOWN RESIDUAL, and it is a real cost, not a rounding error. Filtering a
    // normal map averages tangent-space normals toward (0,0,1), which discards
    // the sub-pixel normal variance that was acting as roughness. The same band
    // brightens from 130.8 to 143.0 out of 255 as a result. The raster path has
    // the identical problem and takes a smaller hit only because anisotropic
    // filtering flattens less than an isotropic cone does. The principled fix is
    // to fold the lost variance back in as roughness - Toksvig, or LEAN/CLEAN
    // mapping - which is a separate piece of work and is NOT implemented here.
    uint textureIndex = NonUniformResourceIndex(mat.normalTextureIndex);
    uint texWidth, texHeight;
    g_NormalTextures[textureIndex].GetDimensions(texWidth, texHeight);
    float lod = RayConeTextureLod(lodBase, texWidth, texHeight);
    float3 tangentNormal = g_NormalTextures[textureIndex].SampleLevel(g_TextureSampler, surfaceUV, lod).xyz * 2.0f - 1.0f;
    tangentNormal.z = max(tangentNormal.z, 0.0f);

    float3 mappedNormal = tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N;
    return SafeNormalize(mappedNormal, N);
}

// =============================================================================
// Ray Generation Shader
// =============================================================================
[shader("raygeneration")]
void RayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim   = DispatchRaysDimensions().xy;

    // Initialize RNG. Seed from the wall-clock dispatch counter, NOT g_FrameIndex:
    // g_FrameIndex is the accumulation index and resets to 0 on any camera motion,
    // which pinned every moving frame to an identical random stream.
    uint rngState = InitRNG(launchIndex, g_SeedIndex);

    uint samplesPerPixel = max(g_SamplesPerPixel, 1u);
    float3 frameRadiance = float3(0, 0, 0);

    for (uint sampleIndex = 0; sampleIndex < samplesPerPixel; sampleIndex++)
    {

    // Compute ray direction from pixel coordinates
    // NDC: [-1, 1] with jitter for anti-aliasing
    float2 jitter = float2(Random(rngState), Random(rngState));
    float2 uv = (float2(launchIndex) + jitter) / float2(launchDim);
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y; // Flip Y for screen space

    // Reconstruct world-space ray from camera
    // Use inverse view-proj to unproject near and far points
    // Since we're passing viewProj (not inverse), we reconstruct differently:
    // Simple camera ray construction from position + screen UV
    // From the camera via the constant buffer, NOT a literal. This was
    // 70 degrees hardcoded with a comment saying it must match the camera; it
    // did match, so any future divergence would have been silent - the DXR path
    // tracing one frustum while raster rasterises another.
    float aspect = float(g_RenderWidth) / float(g_RenderHeight);
    float tanHalfFov = g_TanHalfFovY;

    // We need the view direction in world space. Since we don't have the
    // inverse VP easily, reconstruct from camera basis vectors encoded
    // in the view-projection matrix rows.
    // For now: use a simplified approach — shoot rays through a virtual film plane
    float3 rayOrigin = g_CameraPos.xyz;

    float3 forward = normalize(g_CameraForward.xyz);
    float3 right   = normalize(g_CameraRight.xyz);
    float3 up      = normalize(g_CameraUp.xyz);

    float3 rayDir = normalize(
        forward + right * ndc.x * tanHalfFov * aspect + up * ndc.y * tanHalfFov
    );

    // =================================================================
    // Path tracing loop (iterative bounces with NEE)
    // =================================================================
    float3 radiance   = float3(0, 0, 0);
    float3 throughput  = float3(1, 1, 1);

    float3 currentOrigin = rayOrigin;
    float3 currentDir    = rayDir;

    // Ray cone state, carried across bounces. The camera is a pinhole, so the
    // primary cone starts with zero width at the aperture and opens at one pixel
    // of output height per unit distance.
    float coneSpread = g_PrimaryConeSpread;
    float coneWidth  = 0.0f;

    for (uint bounce = 0; bounce < g_MaxBounces; bounce++)
    {
        // Trace primary/bounce ray
        RayDesc ray;
        ray.Origin    = currentOrigin;
        ray.Direction = currentDir;
        ray.TMin      = 0.0f;
        ray.TMax      = 10000.0f;

        RayPayload payload;
        payload.hitT       = -1.0f;
        payload.normal     = float3(0, 0, 0);
        payload.uv         = float2(0, 0);
        payload.instanceID = 0;
        payload.conePacked = PackRayCone(coneSpread, coneWidth);

        TraceRay(g_Scene,
            RAY_FLAG_NONE,
            0xFF,                    // Instance mask
            0,                       // Ray contribution to hit group (primary = 0)
            2,                       // Multiplier for geometry (2 ray types: primary + shadow)
            0,                       // Miss shader index (primary miss = 0)
            ray, payload);

        // Miss — sky
        if (payload.hitT < 0.0f)
        {
            radiance += throughput * DawningSkyRadiance(currentDir);
            break;
        }

        // Texture-independent mip term for this hit, resolved by ClosestHit from
        // the cone we packed in above. Read before anything else overwrites it.
        float lodBase = asfloat(payload.conePacked);

        // Advance the cone to this hit. Must match RayConeLodBase's own width
        // computation in ClosestHit, or the LOD used for the normal map and the
        // LOD used for albedo/ORM/emissive would disagree on the same surface.
        coneWidth += coneSpread * payload.hitT;

        // Get material for this instance
        MaterialData mat = g_Materials[payload.instanceID];
        float3 albedo = ResolveAlbedo(mat, payload.uv, lodBase);

        // Packed occlusion / roughness / metallic (glTF: AO=R, rough=G, metal=B).
        // Modulating the local copy here means every downstream use - NEE, the
        // env approximation, the lobe choice, VNDF alpha - picks it up without
        // each site having to remember. Keep this identical in spirit to
        // basic_ps.hlsl; the two paths diverging on material interpretation is
        // exactly the class of bug this codebase has already paid for once.
        float ambientOcclusion = 1.0f;
        if (mat.useOrmTexture != 0 && mat.ormTextureIndex < 64)
        {
            uint ormIndex = NonUniformResourceIndex(mat.ormTextureIndex);
            uint ormWidth, ormHeight;
            g_OrmTextures[ormIndex].GetDimensions(ormWidth, ormHeight);
            float ormLod = RayConeTextureLod(lodBase, ormWidth, ormHeight);
            float3 orm = g_OrmTextures[ormIndex].SampleLevel(g_TextureSampler, payload.uv, ormLod).rgb;
            ambientOcclusion = orm.r;
            mat.roughness *= orm.g;
            mat.metallic  *= orm.b;
        }
        // Clamped after modulation, matching the raster path: a near-zero green
        // channel would otherwise drive roughness below what the GGX denominator
        // and the VNDF alpha floor are conditioned for.
        mat.roughness = clamp(mat.roughness, 0.04f, 1.0f);
        mat.metallic  = saturate(mat.metallic);

        // Toksvig normal-variance widening, at the SINGLE POINT OF TRUTH for this
        // path's roughness (IBL_DESIGN 9.5): mat.roughness below feeds NEE, the
        // VNDF sample, and the stable-preview IBL mip. The filtered normal length
        // is re-read from the SAME resource, uv and ray-cone LOD that ClosestHit's
        // ApplyNormalMap already filtered through, so it is the exact filtered
        // normal the shading normal came from - one extra tap rather than a second
        // schedule, and it keeps the 32-byte payload untouched. length < 1 is the
        // sub-pixel variance the filtering discarded; DawningToksvigRoughness folds
        // it back and is the identity at length 1.
        //
        // THIS APPLIES TO THE FULL PATH TRACER TOO, deliberately: it runs this
        // same material setup and filters the normal map through the same ray
        // cone, so it has the identical roughness-understatement defect, and
        // correcting it here keeps all three paths agreeing on the roughness a
        // minified normal-mapped surface actually has rather than diverging. It
        // does NOT touch the full tracer's ENVIRONMENT sampling - the miss-shader
        // sky integral is untouched; only the BRDF roughness moves. The reference's
        // movement from this is reported rather than hidden.
        float preToksvigRoughness = mat.roughness;
        if (mat.useNormalTexture != 0 && mat.normalTextureIndex < 64)
        {
            uint tvIndex = NonUniformResourceIndex(mat.normalTextureIndex);
            uint tvWidth, tvHeight;
            g_NormalTextures[tvIndex].GetDimensions(tvWidth, tvHeight);
            float tvLod = RayConeTextureLod(lodBase, tvWidth, tvHeight);
            float3 tvNormal = g_NormalTextures[tvIndex]
                                  .SampleLevel(g_TextureSampler, payload.uv, tvLod).xyz * 2.0f - 1.0f;
            mat.roughness = DawningToksvigRoughness(mat.roughness, length(tvNormal));
        }

        // Emission is added along the current throughput and terminates nothing:
        // a ray that lands on an emitter still continues, it just deposits this
        // first. Matches basic_ps.hlsl. Emitters are NOT sampled by NEE, so they
        // light only themselves - see ecs::Material.
        float3 emission = mat.emissive * mat.emissiveStrength;
        if (mat.useEmissiveTexture != 0 && mat.emissiveTextureIndex < 64)
        {
            uint emissiveIndex = NonUniformResourceIndex(mat.emissiveTextureIndex);
            uint emissiveWidth, emissiveHeight;
            g_EmissiveTextures[emissiveIndex].GetDimensions(emissiveWidth, emissiveHeight);
            float emissiveLod = RayConeTextureLod(lodBase, emissiveWidth, emissiveHeight);
            emission *= g_EmissiveTextures[emissiveIndex]
                            .SampleLevel(g_TextureSampler, payload.uv, emissiveLod).rgb;
        }
        radiance += throughput * emission;
        float3 hitPos = currentOrigin + currentDir * payload.hitT;
        float3 N = normalize(payload.normal);
        float3 V = -currentDir;

        // =============================================================
        // Next Event Estimation — direct light sampling
        // =============================================================
        float3 L = normalize(g_LightDir.xyz);
        float NdotL = saturate(dot(N, L));

        if (NdotL > 0.001f)
        {
            // Trace shadow ray
            RayDesc shadowRay;
            shadowRay.Origin    = SpawnRayOrigin(hitPos, N, L);
            shadowRay.Direction = L;
            shadowRay.TMin      = 0.0f;
            shadowRay.TMax      = 10000.0f;

            ShadowPayload shadowPayload;
            shadowPayload.shadowFactor = 0.0f;

            TraceRay(g_Scene,
                RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                0xFF,
                1,    // Shadow ray type
                2,    // Multiplier
                1,    // Shadow miss index
                shadowRay, shadowPayload);

            if (shadowPayload.shadowFactor > 0.0f)
            {
                // Evaluate Cook-Torrance BRDF for direct lighting
                float3 H = normalize(V + L);
                float NdotH = saturate(dot(N, H));
                float NdotV = saturate(dot(N, V));
                float VdotH = saturate(dot(V, H));

                float3 F0 = DawningF0(albedo, mat.metallic);
                float3 F  = DawningFresnelSchlick(VdotH, F0);

                float3 specular = DawningCookTorranceSpecular(NdotV, NdotL, NdotH, VdotH,
                                                              mat.roughness, F0);
                float3 kD = DawningDiffuseWeight(F, mat.metallic);
                float3 diffuse = kD * albedo / PI;

                float3 directLight = (diffuse + specular) * g_LightColor.rgb * NdotL;
                radiance += throughput * directLight * shadowPayload.shadowFactor;
            }
        }

        if (g_StablePreview != 0)
        {
            // =========================================================
            // Stable preview environment — THE SAME EVALUATION AS RASTER
            // =========================================================
            // IBL_DESIGN.md 8.2. What used to sit here was an ad-hoc fill with a
            // magic 2.5 diffuse multiplier, a mirror reflection that ignored
            // roughness entirely, and a gloss ramp corresponding to no physical
            // quantity. It shared nothing with the raster environment but the sky
            // function, so F1 changed the LIGHTING MODEL and not just the
            // renderer - and this project treats raster/DXR divergence as a
            // defect. All four constants are gone.
            //
            // WHAT REPLACES THEM is line-for-line basic_ps.hlsl's block: the
            // roughness-aware environment Fresnel, the kD split, L2 SH
            // irradiance, and the split-sum specular, through the same
            // shaders/ibl_common.hlsli functions with the same arguments.
            //
            // THE FULL PATH TRACER IS DELIBERATELY UNTOUCHED and must stay so.
            // It collects DawningSkyRadiance on every miss - it evaluates the
            // very integral the split-sum approximates - so substituting the
            // prefiltered cube would replace the reference with the
            // approximation. ASSET_PIPELINE_SPEC.md makes DXR the reference.
            // Terminating the last bounce into the cube is rejected for the same
            // reason plus a second one: it biases the estimator, and CLAUDE.md
            // tracks this project's two biases by name rather than accumulating
            // unlisted ones.
            //
            // Single `if`, both outputs initialised above it, one exit - the same
            // shape basic_ps keeps for FXC's X4000 analysis. DXC does not require
            // it; matching the raster source line for line does.
            float3 envDiffuse  = (float3)0.0f;
            float3 envSpecular = (float3)0.0f;
            float3 envRadiance = (float3)0.0f;
            float3 envR        = float3(0.0f, 1.0f, 0.0f);
            bool   sampledCube = false;

            float  envNdotV = saturate(dot(N, V));
            float3 envF0    = DawningF0(albedo, mat.metallic);

            if (g_IblParams.z != 0.0f)
            {
                float3 F_env  = DawningFresnelSchlickRoughness(envNdotV, envF0, mat.roughness);
                float3 kD_env = (1.0f - F_env) * (1.0f - mat.metallic);

                float3 irradiance = DawningIrradianceSH(g_IblSH, N);
                envDiffuse = kD_env * albedo * irradiance / PI;

                envSpecular = DawningSpecularIBLWitnessed(g_EnvCube, g_EnvSampler, N, V,
                                                          mat.roughness, envF0, g_IblParams.x,
                                                          envRadiance, envR);
                sampledCube = true;
            }

            // ORM occlusion multiplies the environment only, never direct light -
            // the same rule basic_ps.hlsl states, inherited unchanged. Diffuse
            // takes the raw hemispherical AO; SPECULAR takes the Lagarde remap of
            // the same AO by NdotV and roughness, identical to the raster path, so
            // the two renderers apply the same visibility to the same lobe. It is
            // an identity (== ao) at roughness 1 and where no AO map ships.
            float specularOcclusion = DawningSpecularOcclusion(envNdotV, mat.roughness, ambientOcclusion);
            envDiffuse  *= ambientOcclusion   * g_IblParams.y;
            envSpecular *= specularOcclusion  * g_IblParams.y;

            // The (bounce == 0 ? 1 : 0.25) damper is KEPT, on purpose. It is a
            // preview heuristic rather than a magic constant standing in for a
            // physical quantity: the stable preview traces no secondary bounce,
            // so it damps the environment fill at later vertices. IBL_DESIGN.md
            // 8.2 says to keep it for now - removing it is an appearance change,
            // not a correctness one, and folding both into one stage would make
            // any luminance shift unattributable.
            float  envDamp = (bounce == 0 ? 1.0f : 0.25f);
            float3 radianceBeforeEnv = radiance;
            radiance += throughput * (envDiffuse + envSpecular) * envDamp;

            // -----------------------------------------------------------------
            // The DXR IBL CONSUMPTION probe. AFTER the combine, deliberately.
            // -----------------------------------------------------------------
            // This is the only evidence in the tree that the DXR STABLE PREVIEW
            // consumes the environment cube, as opposed to ibl_common.hlsli
            // computing it correctly when asked. Every startup assertion and the
            // whole raster consumption probe stay green with this entire block
            // deleted and with the wrong descriptor bound at t0/space8.
            //
            // The in-final witness is recovered from `radiance` BY SUBTRACTION,
            // exactly as basic_ps recovers it from finalColor: written as
            // envDiffuse + envSpecular it would stay green with the terms deleted
            // from the line above while both variables kept their values. That is
            // the specific edit the third word exists to catch.
            //
            // WRITTEN ONLY AT bounce == 0, and that is what makes the numbers
            // mean anything. There, throughput is exactly 1 and envDamp is
            // exactly 1, so the subtraction recovers envDiffuse + envSpecular
            // with NO division and the values are directly comparable to the
            // raster probe's - same quantity, same fixed-point scale, same
            // reduction, so the two paths' markers can be read side by side.
            // Writing at every vertex would scale the witness by an
            // accumulated throughput and turn the floors into a claim about
            // path weights rather than about the environment.
            //
            // It also keeps the LIVENESS claim exact: every invocation that
            // reaches this combine at bounce 0 also fetched the cube, so the
            // reduction's cubeSamples == shadedPixels equality holds by
            // construction rather than by luck.
            if (g_IblParams.w != 0.0f && bounce == 0)
            {
                DawningWriteIBLConsumption(g_IBLProbe, envDiffuse, envSpecular,
                                           radiance - radianceBeforeEnv, sampledCube);
                if (sampledCube)
                {
                    DawningWriteIBLIdentity(g_IBLProbe, g_EnvCube, g_EnvSampler,
                                            envR, envRadiance);
                    // The specular-fidelity witnesses, from the EXACT scalars this
                    // path applied: specularOcclusion multiplied envSpecular and
                    // mat.roughness (post-Toksvig) fed the env mip, with
                    // preToksvigRoughness its value before the widening. Same
                    // writer, same reduction, same floors as the raster probe.
                    DawningWriteIBLSpecFidelity(g_IBLProbe,
                                                specularOcclusion, ambientOcclusion,
                                                mat.roughness, preToksvigRoughness);
                }
            }
        }
        // Full path tracing adds no ad-hoc ambient. BSDF rays that escape the scene
        // already collect environment radiance from the sky miss shader, so adding
        // an ambient term at every path vertex double-counted it - with magic
        // constants corresponding to no physical quantity. Worse, it defeated the
        // bounce loop: the error was added per bounce rather than reduced per
        // bounce, so no bounce count converged to ground truth.
        //
        // The stable-preview branch above keeps a fill deliberately, because it
        // traces no secondary bounce and needs an approximation to stand in for
        // one - but as of IBL Stage 4 that approximation is the SAME split-sum
        // plus SH the raster path evaluates, not an invented one.
        //
        // THIS branch stays empty, and that is a decision rather than an
        // omission: the prefiltered cube approximates the integral this path
        // evaluates by sampling, so binding it here would substitute the
        // approximation for the reference. Terminating the last bounce into it is
        // rejected on the same ground plus a second one - it biases the
        // estimator, and CLAUDE.md names this project's two biases rather than
        // accumulating unlisted ones.

        if (bounce + 1 >= g_MaxBounces)
            break;

        // =============================================================
        // Sample BSDF for next bounce direction
        // =============================================================
        float2 u = float2(Random(rngState), Random(rngState));

        // Probabilistically choose diffuse vs specular based on metallic
        float specProb = 0.5f + 0.5f * mat.metallic;
        float3 newDir;
        bool choseSpecular = Random(rngState) <= specProb;
        float branchPdf = choseSpecular ? specProb : (1.0f - specProb);

        // How much the ray cone opens up over this bounce. Set in both branches
        // below and applied once after them.
        float bounceConeSpread = kDiffuseBounceConeSpread;

        if (!choseSpecular)
        {
            // Diffuse bounce (cosine-weighted hemisphere)
            newDir = SampleCosineHemisphere(u, N);

            // Update throughput
            float3 F0 = DawningF0(albedo, mat.metallic);
            float3 F = DawningFresnelSchlick(saturate(dot(V, normalize(V + newDir))), F0);
            float3 kD = (1.0f - F) * (1.0f - mat.metallic);
            throughput *= kD * albedo;
        }
        else
        {
            // Specular bounce, GGX VNDF importance sampling.
            //
            // The previous version sampled a cosine lobe about the mirror direction,
            // warped it by a non-invertible lerp, and multiplied throughput by F
            // alone: no D, no G, no cos factor, and no PDF division - it computed no
            // PDF at all. That is only the correct weight in the roughness == 0
            // delta-mirror limit, so rough metals were systematically over-bright and
            // indirect specular never converged to anything in particular. It also
            // had no lower-hemisphere guard, firing a large share of rays at grazing
            // angles straight into the surface.
            float3 T, B;
            DawningBuildOrthonormalBasis(N, T, B);

            // Clamp alpha away from zero so the VNDF stays well-defined; a true
            // delta mirror would need a separate specular-path branch.
            float alpha = max(mat.roughness * mat.roughness, 1e-3f);

            // A specular lobe stays tight as alpha -> 0, so unlike the diffuse
            // branch the cone can legitimately keep its spread: a mirror
            // reflection is as sharp as the incoming cone was. Widen in
            // proportion to the lobe, capped at the diffuse spread so a fully
            // rough metal never claims a tighter footprint than a diffuse bounce.
            bounceConeSpread = min(2.0f * alpha, kDiffuseBounceConeSpread);

            float3 Ve = float3(dot(V, T), dot(V, B), dot(V, N));
            float3 Hl = DawningSampleGGXVNDF(Ve, alpha, u);
            float3 H  = normalize(Hl.x * T + Hl.y * B + Hl.z * N);

            newDir = reflect(-V, H);

            float NdotL = dot(N, newDir);
            if (NdotL <= 0.0f)
                break;   // Sampled below the horizon; this path carries no energy.

            float3 F0 = DawningF0(albedo, mat.metallic);
            float3 F  = DawningFresnelSchlick(saturate(dot(V, H)), F0);

            // f * cos / pdf for VNDF sampling with separable Smith G reduces to
            // F * G1(NdotL). See SampleGGXVNDF.
            throughput *= F * DawningGeometrySmithG1(saturate(NdotL), mat.roughness);
        }

        // Correct for sampling probability
        throughput /= max(branchPdf, 0.001f);
        throughput = min(throughput, float3(4.0f, 4.0f, 4.0f));

        // Russian Roulette after first bounce
        if (bounce > 0)
        {
            float pContinue = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95f);
            if (Random(rngState) > pContinue)
                break;
            throughput /= pContinue;
        }

        // Set up next bounce. The new ray inherits the footprint the path has
        // accumulated so far (coneWidth, already advanced to this hit above) and
        // an angular spread widened by the lobe it was sampled from. The width
        // term is why a secondary bounce never drops back to a sharp mip even
        // when the bounce itself is a mirror.
        coneSpread   += bounceConeSpread;
        currentOrigin = SpawnRayOrigin(hitPos, N, newDir);
        currentDir    = newDir;
    }

    frameRadiance += radiance;
    }

    float3 radiance = frameRadiance / float(samplesPerPixel);

    // Accumulate linear HDR radiance; tone mapping happens only for display.
    float3 filteredRadiance = ClampFireflySample(radiance);

    // Progressive running mean over g_FrameIndex, which the CPU resets to 0 on any
    // camera or quality change (path_tracer.cpp). Weighting by 1/(n+1) gives every
    // frame's estimate equal weight, so variance falls as 1/n while the view is
    // still - as opposed to an exponential blend, which would converge to a moving
    // average and never actually resolve.
    //
    // This previously did not exist: `accumulatedRadiance` was a pass-through alias
    // for the current frame, and the HDR history texture was read only as the
    // firefly clamp's reference. The whole CPU-side apparatus - accumulation index,
    // camera-change detection, the dedicated R16G16B16A16_FLOAT history texture -
    // drove nothing.
    float3 accumulatedRadiance;
    if (g_FrameIndex == 0)
    {
        accumulatedRadiance = filteredRadiance;
    }
    else
    {
        float3 history = g_Output[launchIndex].rgb;
        // Guard against a poisoned history buffer resurrecting itself forever.
        if (any(isnan(history)) || any(isinf(history)))
            history = filteredRadiance;
        accumulatedRadiance = lerp(history, filteredRadiance, 1.0f / float(g_FrameIndex + 1));
    }

    g_Output[launchIndex] = float4(accumulatedRadiance, 1.0f);
    g_DisplayOutput[launchIndex] = float4(DawningToneMapForDisplay(accumulatedRadiance), 1.0f);
}

// =============================================================================
// Closest Hit Shader — evaluates surface properties at hit point
// =============================================================================
[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.hitT       = RayTCurrent();
    payload.instanceID = InstanceID();

    float3 bary = float3(
        1.0f - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y
    );

    InstanceData instanceData = g_InstanceData[payload.instanceID];
    TriangleNormalData tri = g_TriangleNormals[instanceData.triangleNormalOffset + PrimitiveIndex()];
    TriangleUVData triUV = g_TriangleUVs[instanceData.triangleUVOffset + PrimitiveIndex()];
    TrianglePositionData triPos = g_TrianglePositions[instanceData.trianglePositionOffset + PrimitiveIndex()];
    float3 objectNormal = normalize(tri.n0.xyz * bary.x + tri.n1.xyz * bary.y + tri.n2.xyz * bary.z);
    float2 surfaceUV = triUV.uv0.xy * bary.x + triUV.uv1.xy * bary.y + triUV.uv2.xy * bary.z;

    // Normals are covectors: transform by the inverse transpose, not by the world
    // matrix. Using ObjectToWorld3x4() here skewed them under non-uniform scale
    // (a 10:1 anisotropy tilts normals by tens of degrees) and diverged from the
    // raster path, which has always done this correctly. The matrix arrives
    // pre-transposed in the instance metadata so this stays three dot products.
    float3 worldNormal = normalize(float3(
        dot(instanceData.normalMatrix0.xyz, objectNormal),
        dot(instanceData.normalMatrix1.xyz, objectNormal),
        dot(instanceData.normalMatrix2.xyz, objectNormal)
    ));

    // If the normal faces away from the ray, flip it
    if (dot(worldNormal, WorldRayDirection()) > 0)
        worldNormal = -worldNormal;

    float3 worldP0 = TransformObjectToWorldPoint(triPos.p0.xyz);
    float3 worldP1 = TransformObjectToWorldPoint(triPos.p1.xyz);
    float3 worldP2 = TransformObjectToWorldPoint(triPos.p2.xyz);

    // ---- Ray cone texture LOD -----------------------------------------------
    // Read the incoming cone BEFORE overwriting the field with the result. The
    // grazing term uses the GEOMETRIC normal, not the interpolated or
    // normal-mapped one: the triangle LOD constant is a flat-triangle quantity,
    // so the footprint has to be projected onto the same flat triangle. Using a
    // normal-mapped normal here would make the mip depend on the bump detail it
    // is supposed to be filtering.
    float2 incomingCone = UnpackRayCone(payload.conePacked);
    float  coneWidth    = incomingCone.y + incomingCone.x * RayTCurrent();

    float3 geometricNormal = SafeNormalize(cross(worldP1 - worldP0, worldP2 - worldP0), worldNormal);
    float  triangleLod = RayConeTriangleLodConstant(
        triUV.uv0.xy, triUV.uv1.xy, triUV.uv2.xy,
        worldP0, worldP1, worldP2);
    float  lodBase = RayConeLodBase(
        triangleLod, coneWidth, dot(normalize(WorldRayDirection()), geometricNormal));

    MaterialData mat = g_Materials[payload.instanceID];
    worldNormal = ApplyNormalMap(mat, worldNormal,
                                 worldP0, worldP1, worldP2,
                                 triUV.uv0.xy, triUV.uv1.xy, triUV.uv2.xy,
                                 surfaceUV, lodBase);

    if (dot(worldNormal, WorldRayDirection()) > 0)
        worldNormal = -worldNormal;

    payload.normal     = worldNormal;
    payload.uv         = surfaceUV;
    payload.conePacked = asuint(lodBase);
}

// =============================================================================
// Shadow Closest Hit — empty, shadow test uses SKIP_CLOSEST_HIT_SHADER flag
// =============================================================================
[shader("closesthit")]
void ShadowClosestHit(inout ShadowPayload payload,
                       in BuiltInTriangleIntersectionAttributes attribs)
{
    // Hit something = in shadow
    payload.shadowFactor = 0.0f;
}

// =============================================================================
// Miss Shader — sky/environment for primary rays
// =============================================================================
[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.hitT  = -1.0f; // Signal miss
}

// =============================================================================
// Shadow Miss — ray reached the light (no occlusion)
// =============================================================================
[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.shadowFactor = 1.0f; // Not occluded
}
