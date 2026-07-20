#ifndef THE_DAWNING_SKY_COMMON_HLSLI
#define THE_DAWNING_SKY_COMMON_HLSLI

// =============================================================================
// DO NOT ADD A SUN DISC TO THIS FUNCTION.
// =============================================================================
// This sky is a function of direction.y alone: an affine, azimuthally symmetric
// gradient with no localised bright region anywhere on the sphere. The renderer
// depends on that, not as a style choice but as an energy-conservation
// precondition.
//
// The engine represents the sun ONCE, as the analytic directional light. If a
// sun disc is added here as well, the directional light and the environment
// become two copies of the brightest energy in the scene and every lit surface
// is silently over-bright by a factor that varies with roughness and view angle.
// It will not crash. It will read as "the tone mapping needs tuning".
// docs/research/IBL_DESIGN.md section 9.2 and section 12 state the argument in
// full; section 12 names this as the single biggest risk in that design.
//
// If you need a sun in the sky, exactly one of these two must also happen:
//   1. exclude the disc from the environment integral that feeds image-based
//      lighting, so the environment still carries no sun energy; or
//   2. delete the analytic directional light, so the sky becomes the only
//      representation of it.
// Keeping both is a straight double count.
//
// THIS COMMENT IS NOT THE MITIGATION. tests/test_sky_energy.cpp asserts the
// no-localised-lobe property directly and fails the suite if a sun disc appears
// here - and it hashes this file, so editing the sky at all is a loud failure
// rather than a silent one. Read that file's header before changing anything
// below; it explains what the tests cover and what they do not.
// =============================================================================

float3 DawningSkyRadianceFromBlend(float t)
{
    t = saturate(t);
    return lerp(float3(0.8f, 0.85f, 0.9f), float3(0.3f, 0.5f, 0.9f), t) * 0.5f;
}

float3 DawningSkyRadiance(float3 direction)
{
    return DawningSkyRadianceFromBlend(0.5f * (direction.y + 1.0f));
}

#endif

