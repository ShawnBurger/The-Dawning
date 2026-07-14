// =============================================================================
// basic_ps.hlsl — The Dawning V3 Pixel Shader
// =============================================================================
// PBR-lite: Lambert diffuse + Blinn-Phong specular with roughness/metallic.
// Will be upgraded to full Cook-Torrance GGX in Layer 4.
// =============================================================================

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
    float2 matPad;
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD0;
    float3 normalWS   : TEXCOORD1;
    float4 color      : COLOR;
    float2 uv         : TEXCOORD2;
};

float4 main(PSInput input) : SV_TARGET
{
    float3 N = normalize(input.normalWS);
    float3 V = normalize(eyePos - input.positionWS);
    float3 L = normalize(lightDir);
    float3 H = normalize(L + V);

    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float NdotV = saturate(dot(N, V));

    // Base color from material albedo * vertex color
    float3 baseColor = albedo.rgb * input.color.rgb;

    // Fresnel-Schlick approximation
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);
    float3 fresnel = F0 + (1.0 - F0) * pow(1.0 - saturate(dot(H, V)), 5.0);

    // Diffuse (Lambert, energy-conserving: reduced by metallic)
    float3 diffuse = baseColor * (1.0 - metallic) * NdotL * lightColor;

    // Specular (Blinn-Phong, roughness-controlled sharpness)
    // Will be replaced with GGX NDF in Layer 4
    float specPower = lerp(256.0, 4.0, roughness * roughness);
    float specIntensity = pow(NdotH, specPower) * NdotL;
    float3 specular = fresnel * lightColor * specIntensity;

    // Ambient (hemisphere approximation — ground color darker)
    float hemisphereBlend = N.y * 0.5 + 0.5;
    float3 groundColor = ambientColor * 0.3;
    float3 ambient = baseColor * lerp(groundColor, ambientColor, hemisphereBlend);

    // Combine
    float3 finalColor = diffuse + specular + ambient;

    return float4(finalColor, albedo.a * input.color.a);
}
