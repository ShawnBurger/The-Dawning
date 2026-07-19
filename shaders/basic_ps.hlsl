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

Texture2D<float4> materialTextures[128] : register(t0);
SamplerState linearSampler : register(s0);

static const float PI = 3.14159265358979323846;

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
        float3 derivedT = dpdx * duvdy.y - dpdy * duvdx.y;
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

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float DistributionGGX(float NdotH, float materialRoughness)
{
    float a = materialRoughness * materialRoughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    // Multiplicative floor, NOT an additive epsilon: at the lobe peak d == a2, so the
    // true denominator is PI*a2*a2, which for roughness < ~0.35 is smaller than 1e-4.
    // Adding an epsilon there lets it dominate and flattens the specular peak away.
    return a2 / max(PI * d * d, 1e-7);
}

float GeometrySmithG1(float NdotV, float materialRoughness)
{
    float r = materialRoughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float materialRoughness)
{
    return GeometrySmithG1(NdotV, materialRoughness) *
           GeometrySmithG1(NdotL, materialRoughness);
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
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);
    float3 F = FresnelSchlick(VdotH, F0);
    float D = DistributionGGX(NdotH, materialRoughness);
    float G = GeometrySmith(NdotV, NdotL, materialRoughness);

    float3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kD * baseColor / PI;
    float3 direct = (diffuse + specular) * lightColor * NdotL;

    // Ambient (hemisphere approximation — ground color darker)
    float hemisphereBlend = N.y * 0.5 + 0.5;
    float3 groundColor = ambientColor * 0.3;
    float3 ambientDiffuse = baseColor * lerp(groundColor, ambientColor, hemisphereBlend) * (1.0 - metallic);
    float3 ambientSpecular = FresnelSchlick(NdotV, F0) * (ambientColor + 0.04) *
                             lerp(0.2, 0.8, 1.0 - materialRoughness);

    // Combine
    float3 finalColor = direct + ambientDiffuse + ambientSpecular;

    return float4(DawningToneMapForDisplay(finalColor), albedo.a * input.color.a);
}
