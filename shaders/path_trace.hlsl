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
    float4   g_AmbientColor;
    uint     g_FrameIndex;
    uint     g_MaxBounces;
    uint     g_RenderWidth;
    uint     g_RenderHeight;
    uint     g_SamplesPerPixel;
    uint     g_StablePreview;
    uint     g_SeedIndex;       // Wall-clock dispatch counter — RNG decorrelation only
    uint     g_Pad;
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
SamplerState g_TextureSampler : register(s0, space0);

// =============================================================================
// Ray payload structures
// =============================================================================
struct RayPayload
{
    float  hitT;
    float3 normal;
    float2 uv;
    uint   instanceID;
    uint   pad;
};

struct ShadowPayload
{
    float shadowFactor; // 0 = in shadow, 1 = lit
};

// =============================================================================
// Utility functions
// =============================================================================
static const float PI = 3.14159265358979323846f;

float3 SafeNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return lenSq > 1e-8f ? value * rsqrt(lenSq) : fallback;
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

// Build an orthonormal basis around N (Duff et al. 2017, branchless and stable).
void BuildOrthonormalBasis(float3 N, out float3 T, out float3 B)
{
    float sign = N.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + N.z);
    float b = N.x * N.y * a;
    T = float3(1.0f + sign * N.x * N.x * a, sign * b, -sign * N.x);
    B = float3(b, sign + N.y * N.y * a, -N.y);
}

// Sample the GGX distribution of VISIBLE normals (Heitz 2018, "Sampling the GGX
// Distribution of Visible Normals"). Ve is the view direction in tangent space
// with the shading normal along +Z; the returned half-vector is in the same space.
//
// Importance-sampling the visible normals rather than perturbing a mirror
// direction is what makes the Monte Carlo weight tractable: with a separable
// Smith G, f*cos/pdf collapses to exactly F * G1(NdotL), with no explicit PDF
// division and no D or G evaluation needed at the sample point.
float3 SampleGGXVNDF(float3 Ve, float alpha, float2 u)
{
    // Warp to the hemisphere configuration.
    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));

    // Orthonormal basis around Vh.
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0f ? float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lensq)
                             : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(Vh, T1);

    // Uniform point on the projected disk, warped for the hemisphere.
    float r   = sqrt(u.x);
    float phi = 2.0f * PI * u.y;
    float t1  = r * cos(phi);
    float t2  = r * sin(phi);
    float s   = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * sqrt(saturate(1.0f - t1 * t1)) + s * t2;

    // Reproject onto the hemisphere.
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(saturate(1.0f - t1 * t1 - t2 * t2)) * Vh;

    // Unwarp back to the ellipsoid configuration.
    return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0f, Nh.z)));
}

// Schlick Fresnel
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// GGX Normal Distribution Function
float DistributionGGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    // Multiplicative floor, NOT an additive epsilon: at the lobe peak d == a2, so the
    // true denominator is PI*a2*a2, which for roughness < ~0.35 is smaller than 1e-4.
    // Adding an epsilon there lets it dominate and flattens the specular peak away.
    return a2 / max(PI * d * d, 1e-7f);
}

// Smith geometry function (single direction)
float GeometrySmithG1(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / (NdotV * (1.0f - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySmithG1(NdotV, roughness) * GeometrySmithG1(NdotL, roughness);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// Bound outlier samples before they enter the running mean.
//
// This deliberately does NOT reference the previous frame's radiance. The old
// version clamped against `previousLum * 3 + 0.25`, i.e. against its own already
// clamped output, which is a multiplicative ratchet rather than a filter: a bright
// region could only grow 3x per frame from whatever it had previously been bounded
// to, biasing the estimator by an amount determined by frame history instead of
// sample statistics. Because the accumulation index resets on any camera motion,
// the hard branch also ran on every frame while the user was moving.
float3 ClampFireflySample(float3 sampleRadiance)
{
    // Scrub NaN/Inf first. The old code computed maxLum/sampleLum with an infinite
    // sampleLum, evaluating Inf * 0 = NaN, and wrote that straight to g_Output
    // where it would poison the accumulation buffer permanently.
    if (any(isnan(sampleRadiance)) || any(isinf(sampleRadiance)))
        return float3(0.0f, 0.0f, 0.0f);

    float sampleLum = Luminance(sampleRadiance);
    if (sampleLum <= 0.0001f)
        return sampleRadiance;

    // Fixed energy ceiling per sample. Still biased - any firefly clamp is - but
    // biased by a constant rather than by a history-dependent ratchet.
    const float kMaxSampleLuminance = 24.0f;
    if (sampleLum > kMaxSampleLuminance)
        sampleRadiance *= kMaxSampleLuminance / sampleLum;

    return sampleRadiance;
}

float3 ResolveAlbedo(MaterialData mat, float2 uv)
{
    float3 albedo = mat.albedo.rgb;
    if (mat.useAlbedoTexture != 0 && mat.albedoTextureIndex < 64)
    {
        uint textureIndex = NonUniformResourceIndex(mat.albedoTextureIndex);
        albedo *= g_AlbedoTextures[textureIndex].SampleLevel(g_TextureSampler, uv, 0.0f).rgb;
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
    float2 surfaceUV)
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

    uint textureIndex = NonUniformResourceIndex(mat.normalTextureIndex);
    float3 tangentNormal = g_NormalTextures[textureIndex].SampleLevel(g_TextureSampler, surfaceUV, 0.0f).xyz * 2.0f - 1.0f;
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
    float fovRad = 70.0f * PI / 180.0f; // Must match camera FOV
    float aspect = float(g_RenderWidth) / float(g_RenderHeight);
    float tanHalfFov = tan(fovRad * 0.5f);

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

    for (uint bounce = 0; bounce < g_MaxBounces; bounce++)
    {
        // Trace primary/bounce ray
        RayDesc ray;
        ray.Origin    = currentOrigin;
        ray.Direction = currentDir;
        ray.TMin      = 0.001f;
        ray.TMax      = 10000.0f;

        RayPayload payload;
        payload.hitT       = -1.0f;
        payload.normal     = float3(0, 0, 0);
        payload.uv         = float2(0, 0);
        payload.instanceID = 0;
        payload.pad        = 0;

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

        // Get material for this instance
        MaterialData mat = g_Materials[payload.instanceID];
        float3 albedo = ResolveAlbedo(mat, payload.uv);
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
            shadowRay.Origin    = hitPos + N * 0.001f;
            shadowRay.Direction = L;
            shadowRay.TMin      = 0.001f;
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

                float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, mat.metallic);
                float3 F  = FresnelSchlick(VdotH, F0);
                float  D  = DistributionGGX(NdotH, mat.roughness);
                float  G  = GeometrySmith(NdotV, NdotL, mat.roughness);

                float3 specular = (D * G * F) / (4.0f * NdotV * NdotL + 0.0001f);
                float3 kD = (1.0f - F) * (1.0f - mat.metallic);
                float3 diffuse = kD * albedo / PI;

                float3 directLight = (diffuse + specular) * g_LightColor.rgb * NdotL;
                radiance += throughput * directLight * shadowPayload.shadowFactor;
            }
        }

        if (g_StablePreview != 0)
        {
            // Stable preview fill: approximate missing diffuse GI and sky reflection
            // without tracing another noisy secondary bounce.
            float3 envDiffuse = albedo * g_AmbientColor.rgb * (1.0f - mat.metallic) * 2.5f;
            float3 envReflection = DawningSkyRadiance(reflect(-V, N));
            float3 envF0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, mat.metallic);
            float3 envF = FresnelSchlick(saturate(dot(N, V)), envF0);
            float envGloss = lerp(0.25f, 1.0f, saturate(1.0f - mat.roughness));
            float3 envSpecular = envReflection * envF * envGloss;
            radiance += throughput * (envDiffuse + envSpecular) * (bounce == 0 ? 1.0f : 0.25f);
        }
        // Full path tracing adds no ad-hoc ambient. BSDF rays that escape the scene
        // already collect environment radiance from the sky miss shader, so adding
        // an ambient term at every path vertex double-counted it - with magic
        // constants corresponding to no physical quantity. Worse, it defeated the
        // bounce loop: the error was added per bounce rather than reduced per
        // bounce, so no bounce count converged to ground truth. The stable-preview
        // branch above keeps its fill deliberately, because it traces no secondary
        // bounce and needs an approximation to stand in for one.

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

        if (!choseSpecular)
        {
            // Diffuse bounce (cosine-weighted hemisphere)
            newDir = SampleCosineHemisphere(u, N);

            // Update throughput
            float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, mat.metallic);
            float3 F = FresnelSchlick(saturate(dot(V, normalize(V + newDir))), F0);
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
            BuildOrthonormalBasis(N, T, B);

            // Clamp alpha away from zero so the VNDF stays well-defined; a true
            // delta mirror would need a separate specular-path branch.
            float alpha = max(mat.roughness * mat.roughness, 1e-3f);

            float3 Ve = float3(dot(V, T), dot(V, B), dot(V, N));
            float3 Hl = SampleGGXVNDF(Ve, alpha, u);
            float3 H  = normalize(Hl.x * T + Hl.y * B + Hl.z * N);

            newDir = reflect(-V, H);

            float NdotL = dot(N, newDir);
            if (NdotL <= 0.0f)
                break;   // Sampled below the horizon; this path carries no energy.

            float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, mat.metallic);
            float3 F  = FresnelSchlick(saturate(dot(V, H)), F0);

            // f * cos / pdf for VNDF sampling with separable Smith G reduces to
            // F * G1(NdotL). See SampleGGXVNDF.
            throughput *= F * GeometrySmithG1(saturate(NdotL), mat.roughness);
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

        // Set up next bounce
        currentOrigin = hitPos + N * 0.001f;
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
    MaterialData mat = g_Materials[payload.instanceID];
    worldNormal = ApplyNormalMap(mat, worldNormal,
                                 worldP0, worldP1, worldP2,
                                 triUV.uv0.xy, triUV.uv1.xy, triUV.uv2.xy,
                                 surfaceUV);

    if (dot(worldNormal, WorldRayDirection()) > 0)
        worldNormal = -worldNormal;

    payload.normal = worldNormal;
    payload.uv     = surfaceUV;
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
