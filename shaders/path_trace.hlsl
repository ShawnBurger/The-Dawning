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
    uint2    g_Pad;
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
    uint  triangleNormalOffset;
    uint  triangleUVOffset;
    uint  trianglePositionOffset;
    uint  pad;
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
    return PCGHash(pixel.x + pixel.y * 16384 + frameIndex * 16384 * 16384);
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
    return a2 / (PI * d * d + 0.0001f);
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

float3 ClampFireflySample(float3 sampleRadiance, float3 previousRadiance, uint frameIndex)
{
    float sampleLum = Luminance(sampleRadiance);
    if (sampleLum <= 0.0001f)
        return sampleRadiance;

    if (frameIndex == 0)
    {
        const float firstFrameLimit = 8.0f;
        if (sampleLum > firstFrameLimit)
            sampleRadiance *= firstFrameLimit / sampleLum;
        return sampleRadiance;
    }

    float previousLum = Luminance(previousRadiance);
    float maxLum = max(1.5f, previousLum * 3.0f + 0.25f);
    if (sampleLum > maxLum)
        sampleRadiance *= maxLum / sampleLum;

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

    // Initialize RNG
    uint rngState = InitRNG(launchIndex, g_FrameIndex);

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
        else
        {
            float3 ambient = albedo * g_AmbientColor.rgb * (1.0f - mat.metallic);
            radiance += throughput * ambient * (bounce == 0 ? 0.3f : 0.1f);
        }

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
            // Specular bounce (reflect with roughness perturbation)
            float3 reflected = reflect(-V, N);
            newDir = SampleCosineHemisphere(u, reflected);

            // Lerp toward perfect reflection based on roughness
            newDir = normalize(lerp(reflected, newDir, mat.roughness * mat.roughness));

            float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, mat.metallic);
            float3 F = FresnelSchlick(saturate(dot(V, normalize(V + newDir))), F0);
            throughput *= F;
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
    float3 filteredRadiance = ClampFireflySample(radiance, g_Output[launchIndex].rgb, g_FrameIndex);
    float3 accumulatedRadiance = filteredRadiance;

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

    float3x4 objToWorld = ObjectToWorld3x4();
    float3 worldNormal = normalize(float3(
        dot(objToWorld[0].xyz, objectNormal),
        dot(objToWorld[1].xyz, objectNormal),
        dot(objToWorld[2].xyz, objectNormal)
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
