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

// =============================================================================
// Global root signature bindings
// =============================================================================
RaytracingAccelerationStructure g_Scene : register(t0, space0);
RWTexture2D<float4>             g_Output : register(u0, space0);

cbuffer PerFrameConstants : register(b0, space0)
{
    float4x4 g_ViewProj;        // View-projection (we reconstruct rays from this)
    float4   g_CameraPos;
    float4   g_LightDir;
    float4   g_LightColor;
    float4   g_AmbientColor;
    uint     g_FrameIndex;
    uint     g_MaxBounces;
    uint     g_RenderWidth;
    uint     g_RenderHeight;
};

struct MaterialData
{
    float4 albedo;
    float  roughness;
    float  metallic;
    float2 pad;
};
StructuredBuffer<MaterialData> g_Materials : register(t1, space0);

// =============================================================================
// Ray payload structures
// =============================================================================
struct RayPayload
{
    float3 color;
    float  hitT;
    float3 normal;
    uint   instanceID;
};

struct ShadowPayload
{
    float shadowFactor; // 0 = in shadow, 1 = lit
};

// =============================================================================
// Utility functions
// =============================================================================
static const float PI = 3.14159265358979323846f;

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

    // Extract right/up/forward from the first 3 columns of viewProj
    // This is an approximation; proper inverse VP would be better
    // TODO: Pass actual inverse VP or camera basis vectors
    float3 forward = normalize(float3(g_ViewProj[0][2], g_ViewProj[1][2], g_ViewProj[2][2]));
    float3 right   = normalize(float3(g_ViewProj[0][0], g_ViewProj[1][0], g_ViewProj[2][0]));
    float3 up      = normalize(float3(g_ViewProj[0][1], g_ViewProj[1][1], g_ViewProj[2][1]));

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
        payload.color      = float3(0, 0, 0);
        payload.hitT       = -1.0f;
        payload.normal     = float3(0, 0, 0);
        payload.instanceID = 0;

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
            // Simple sky gradient
            float t = 0.5f * (currentDir.y + 1.0f);
            float3 skyColor = lerp(float3(0.8, 0.85, 0.9), float3(0.3, 0.5, 0.9), t);
            radiance += throughput * skyColor * 0.5f;
            break;
        }

        // Get material for this instance
        MaterialData mat = g_Materials[payload.instanceID];
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

                float3 F0 = lerp(float3(0.04, 0.04, 0.04), mat.albedo.rgb, mat.metallic);
                float3 F  = FresnelSchlick(VdotH, F0);
                float  D  = DistributionGGX(NdotH, mat.roughness);
                float  G  = GeometrySmith(NdotV, NdotL, mat.roughness);

                float3 specular = (D * G * F) / (4.0f * NdotV * NdotL + 0.0001f);
                float3 kD = (1.0f - F) * (1.0f - mat.metallic);
                float3 diffuse = kD * mat.albedo.rgb / PI;

                float3 directLight = (diffuse + specular) * g_LightColor.rgb * NdotL;
                radiance += throughput * directLight * shadowPayload.shadowFactor;
            }
        }

        // Add ambient term (rough approximation of multi-bounce GI)
        float3 ambient = mat.albedo.rgb * g_AmbientColor.rgb * (1.0f - mat.metallic);
        radiance += throughput * ambient * (bounce == 0 ? 0.3f : 0.1f);

        // =============================================================
        // Sample BSDF for next bounce direction
        // =============================================================
        float2 u = float2(Random(rngState), Random(rngState));

        // Probabilistically choose diffuse vs specular based on metallic
        float specProb = 0.5f + 0.5f * mat.metallic;
        float3 newDir;
        float pdf;

        if (Random(rngState) > specProb)
        {
            // Diffuse bounce (cosine-weighted hemisphere)
            newDir = SampleCosineHemisphere(u, N);
            pdf = saturate(dot(N, newDir)) / PI;

            // Update throughput
            float3 F0 = lerp(float3(0.04, 0.04, 0.04), mat.albedo.rgb, mat.metallic);
            float3 F = FresnelSchlick(saturate(dot(V, normalize(V + newDir))), F0);
            float3 kD = (1.0f - F) * (1.0f - mat.metallic);
            throughput *= kD * mat.albedo.rgb;
        }
        else
        {
            // Specular bounce (reflect with roughness perturbation)
            float3 reflected = reflect(-V, N);
            newDir = SampleCosineHemisphere(u, reflected);

            // Lerp toward perfect reflection based on roughness
            newDir = normalize(lerp(reflected, newDir, mat.roughness * mat.roughness));
            pdf = 1.0f; // Approximate

            float3 F0 = lerp(float3(0.04, 0.04, 0.04), mat.albedo.rgb, mat.metallic);
            float3 F = FresnelSchlick(saturate(dot(V, normalize(V + newDir))), F0);
            throughput *= F;
        }

        // Correct for sampling probability
        throughput /= (Random(rngState) > specProb) ? (1.0f - specProb) : specProb;

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

    // Clamp to prevent fireflies
    radiance = min(radiance, float3(10, 10, 10));

    // Simple temporal accumulation (future: proper reprojection with motion vectors)
    float4 prevColor = g_Output[launchIndex];
    float blend = (g_FrameIndex == 0) ? 1.0f : 0.1f; // 10% new, 90% history
    float4 finalColor = lerp(prevColor, float4(radiance, 1.0f), blend);

    g_Output[launchIndex] = finalColor;
}

// =============================================================================
// Closest Hit Shader — evaluates surface properties at hit point
// =============================================================================
[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.hitT       = RayTCurrent();
    payload.instanceID = InstanceID();

    // Compute interpolated normal from barycentrics
    // For now, use the geometry normal (flat shading)
    // TODO: access vertex normals via bindless vertex buffer
    float3 bary = float3(
        1.0f - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y
    );

    // Compute geometric normal from triangle edges
    // Using ObjectRayDirection and the hit transforms
    float3 worldNormal = float3(0, 1, 0); // Fallback — will be replaced by vertex normal lookup

    // For triangle primitives, we can compute the face normal from the
    // object-to-world transform and the hit position
    // Simple approximation: use the instance transform's up vector
    float3x4 objToWorld = ObjectToWorld3x4();

    // Extract normal from the world-space hit
    // This is approximate for non-planar geometry
    // A proper implementation reads vertex normals from a ByteAddressBuffer
    float3 edge1 = float3(objToWorld[0][0], objToWorld[1][0], objToWorld[2][0]);
    float3 edge2 = float3(objToWorld[0][1], objToWorld[1][1], objToWorld[2][1]);
    worldNormal = normalize(cross(edge1, edge2));

    // If the normal faces away from the ray, flip it
    if (dot(worldNormal, WorldRayDirection()) > 0)
        worldNormal = -worldNormal;

    payload.normal = worldNormal;
    payload.color  = float3(1, 1, 1); // Color comes from material lookup in ray gen
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
    payload.color = float3(0, 0, 0);
}

// =============================================================================
// Shadow Miss — ray reached the light (no occlusion)
// =============================================================================
[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.shadowFactor = 1.0f; // Not occluded
}
