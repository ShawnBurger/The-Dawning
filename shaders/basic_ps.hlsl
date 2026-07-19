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
    float4x4 lightViewProj;
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
    uint   useOrmTexture;
    uint   ormTextureIndex;
    float3 emissive;
    float  emissiveStrength;
    uint   useEmissiveTexture;
    uint   emissiveTextureIndex;
    uint2  cbMaterialPad;
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
Texture2D<float>          shadowMap     : register(t0, space1);
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
    // World units per shadow texel: the frustum is kShadowExtent*2 wide across
    // kShadowMapSize texels. Kept in sync with renderer.h by the numbers below
    // being the only place either appears in this shader.
    const float shadowExtent  = 24.0f;
    const float shadowMapSize = 2048.0f;
    const float texelWorld    = (shadowExtent * 2.0f) / shadowMapSize;

    const float slope  = saturate(1.0f - NdotL);
    const float offset = texelWorld * (1.0f + 3.0f * slope);
    float3 offsetPos   = positionWS + N * offset;

    float4 lightClip = mul(lightViewProj, float4(offsetPos, 1.0f));

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
        if (lightClip.z >= 0.0f && lightClip.z <= 1.0f)
        {
            // Clip space to texture space. Y flips because clip space is +Y up
            // and texture space is +Y down.
            float2 shadowUV = float2(lightClip.x * 0.5f + 0.5f,
                                     -lightClip.y * 0.5f + 0.5f);

            // 3x3 grid of hardware-PCF taps: 9 taps, each already 2x2 filtered,
            // so the effective kernel is 4x4 texels. Enough to hide the texel
            // grid at this resolution without a separate blur pass.
            const float texel = 1.0f / shadowMapSize;
            float sum = 0.0f;
            [unroll]
            for (int y = -1; y <= 1; ++y)
            {
                [unroll]
                for (int x = -1; x <= 1; ++x)
                {
                    sum += shadowMap.SampleCmpLevelZero(
                        shadowSampler, shadowUV + float2(x, y) * texel, lightClip.z);
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

    // Packed occlusion / roughness / metallic (glTF: AO=R, rough=G, metal=B).
    // MODULATES the material scalars rather than replacing them, so those stay
    // usable as per-instance tints - which is what glTF specifies.
    float materialRoughness = roughness;
    float materialMetallic  = metallic;
    float ambientOcclusion  = 1.0;
    if (useOrmTexture != 0)
    {
        float3 orm = materialTextures[ormTextureIndex].Sample(linearSampler, input.uv).rgb;
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
    float3 emission = emissive * emissiveStrength;
    if (useEmissiveTexture != 0)
        emission *= materialTextures[emissiveTextureIndex].Sample(linearSampler, input.uv).rgb;

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

    return float4(finalColor, albedo.a * input.color.a);
}
