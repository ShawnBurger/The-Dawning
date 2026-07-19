// =============================================================================
// basic_ps.hlsl — The Dawning V3 Pixel Shader
// =============================================================================
// Raster material shader: Cook-Torrance/GGX direct lighting with texture-driven
// albedo and normal maps.
// =============================================================================

#include "display_common.hlsli"

cbuffer CBPerFrame : register(b1)
{
    float3 lightDir;       // Normalized direction TO the light
    float  pad0;
    float3 lightColor;
    float  pad1;
    float3 ambientColor;
    float  pad2;
    float3 eyePos;         // Camera world position
    float  pad3;
    // Camera basis; used by sky_ps.hlsl. Declared here too because both shaders
    // bind the same b1 and the layouts must agree. Keep in sync with
    // struct CBPerFrame in src/render/renderer.h (static_assert'd at 112 bytes).
    float3 camRight;
    float  tanHalfFovY;
    float3 camUp;
    float  aspect;
    float3 camForward;
    float  pad4;
};

cbuffer CBMaterial : register(b2)
{
    float4 albedo;         // Base color (RGB) + alpha
    float  roughness;      // 0 = mirror, 1 = matte
    float  metallic;       // 0 = dielectric, 1 = metal
    uint   useAlbedoTexture;
    uint   useNormalTexture;
    uint   albedoTextureIndex;
    uint   normalTextureIndex;
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

#include "brdf_common.hlsli"   // PI and the microfacet BRDF, shared with path_trace.hlsl

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
    float3 N = normalize(input.normalWS);
    if (useNormalTexture != 0)
        N = ApplyNormalMap(N, input.positionWS, input.uv, normalTextureIndex);

    float3 V = normalize(eyePos - input.positionWS);
    float3 L = normalize(lightDir);
    float3 H = normalize(L + V);

    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float NdotV = saturate(dot(N, V));
    float VdotH = saturate(dot(V, H));

    // Base color from material albedo * vertex color * optional albedo texture
    float3 baseColor = albedo.rgb * input.color.rgb;
    if (useAlbedoTexture != 0)
        baseColor *= materialTextures[albedoTextureIndex].Sample(linearSampler, input.uv).rgb;

    float materialRoughness = clamp(roughness, 0.04, 1.0);

    // Cook-Torrance direct lighting
    float3 F0 = DawningF0(baseColor, metallic);
    
    float3 F = DawningFresnelSchlick(VdotH, F0);
    float3 specular = DawningCookTorranceSpecular(NdotV, NdotL, NdotH, VdotH,
                                                  materialRoughness, F0);
    float3 kD = DawningDiffuseWeight(F, metallic);
    float3 diffuse = kD * baseColor / PI;
    float3 direct = (diffuse + specular) * lightColor * NdotL;

    // Ambient (hemisphere approximation — ground color darker)
    float hemisphereBlend = N.y * 0.5 + 0.5;
    float3 groundColor = ambientColor * 0.3;
    float3 ambientDiffuse = baseColor * lerp(groundColor, ambientColor, hemisphereBlend) * (1.0 - metallic);
    float3 ambientSpecular = DawningFresnelSchlick(NdotV, F0) * (ambientColor + 0.04) *
                             lerp(0.2, 0.8, 1.0 - materialRoughness);

    // Combine
    float3 finalColor = direct + ambientDiffuse + ambientSpecular;

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

    return float4(finalColor, albedo.a * input.color.a);
}
