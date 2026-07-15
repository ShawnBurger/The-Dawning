// =============================================================================
// overlay_ps.hlsl - alpha-composite the generated debug overlay texture
// =============================================================================

Texture2D<float4> overlayTexture : register(t0);
SamplerState overlaySampler : register(s0);

cbuffer OverlayConstants : register(b0)
{
    float2 viewportSize;
    float2 overlaySize;
    float2 overlayPos;
    float opacity;
    float pad;
};

float4 main(float4 positionCS : SV_POSITION) : SV_TARGET
{
    float2 local = positionCS.xy - overlayPos;
    if (local.x < 0.0 || local.y < 0.0 ||
        local.x >= overlaySize.x || local.y >= overlaySize.y)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float2 uv = (local + 0.5) / overlaySize;
    float4 color = overlayTexture.SampleLevel(overlaySampler, uv, 0.0);
    color.a *= opacity;
    return color;
}
