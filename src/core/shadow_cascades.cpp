// =============================================================================
// core/shadow_cascades.cpp - Cascaded shadow map geometry
// =============================================================================

#include "shadow_cascades.h"

#include <cmath>

namespace core
{

namespace
{

// Clamp a caller-supplied cascade index. Every entry point takes a uint32_t, so
// the only reachable error is an index past the end; folding it to the last
// cascade keeps the math total rather than undefined.
uint32_t ClampCascade(uint32_t cascade)
{
    return (cascade < kShadowCascadeCount) ? cascade : (kShadowCascadeCount - 1u);
}

} // namespace

float ShadowCascadeSplitRadius(uint32_t cascade)
{
    return kShadowCascadeExtent[ClampCascade(cascade)] / kShadowCascadeMargin;
}

float ShadowCascadeTexelWorld(uint32_t cascade)
{
    return (2.0f * kShadowCascadeExtent[ClampCascade(cascade)]) /
           static_cast<float>(kShadowMapSize);
}

float ShadowCascadeDepthRange(uint32_t cascade)
{
    // Against the legacy constants, not a fresh literal, so cascade 0 lands on
    // exactly 120.0f. See the header for why the ratio being fixed is what lets
    // one shadow PSO serve all four cascades.
    return kShadowCascadeExtent[ClampCascade(cascade)] *
           (kShadowDepthRange / kShadowExtent);
}

float ShadowCascadeFadeLo(uint32_t cascade)
{
    return ShadowCascadeSplitRadius(cascade) * kShadowCascadeFadeFraction;
}

uint32_t SelectShadowCascade(float radialDistance)
{
    // IDENTICAL structure to the three `if` statements in basic_ps.hlsl:
    // descending order, strict `<`, plain assignment, no early exit. The
    // tightest containing cascade wins. Keep these in lockstep - this function
    // exists precisely so the tests constrain the shader's arithmetic.
    uint32_t cascade = kShadowCascadeCount - 1u;
    if (radialDistance < ShadowCascadeSplitRadius(2)) cascade = 2;
    if (radialDistance < ShadowCascadeSplitRadius(1)) cascade = 1;
    if (radialDistance < ShadowCascadeSplitRadius(0)) cascade = 0;
    return cascade;
}

Mat4x4 BuildShadowCascadeMatrix(const Vec3f& lightDir,
                                uint32_t cascade,
                                const Vec3d& cameraPosition)
{
    const uint32_t c = ClampCascade(cascade);

    // The light basis, derived element-for-element the way Mat4x4::LookAt
    // derives it internally, so the snap space and the view matrix agree. If
    // these two ever disagree the snap quantises along an axis the projection
    // does not use, and the result is a shimmer that looks like too little
    // shadow-map resolution.
    const Vec3f up = (std::fabs(lightDir.y) > 0.99f) ? Vec3f(0.0f, 0.0f, 1.0f)
                                                     : Vec3f(0.0f, 1.0f, 0.0f);
    const Vec3f zAxis = (Vec3f(0.0f, 0.0f, 0.0f) - lightDir).Normalized();  // == -lightDir
    const Vec3f xAxis = up.Cross(zAxis).Normalized();
    const Vec3f yAxis = zAxis.Cross(xAxis);

    const Vec3d Xd = Vec3d::FromFloat(xAxis);
    const Vec3d Yd = Vec3d::FromFloat(yAxis);

    // The cascade centre in ABSOLUTE world space is the camera position itself,
    // because the camera-relative centre is the origin. Quantise its light-space
    // XY onto a lattice fixed to the world origin, entirely in double.
    //
    // floor(lx/t) is exact while |lx|/t < 2^53, i.e. out to ~3.8e14 world units
    // at cascade 0's 0.0234-unit texel. Beyond that the snap degrades
    // continuously into partial snapping: mild shimmer returns, nothing breaks.
    const double t  = static_cast<double>(ShadowCascadeTexelWorld(c));
    const double lx = cameraPosition.Dot(Xd);
    const double ly = cameraPosition.Dot(Yd);
    const double dx = std::floor(lx / t) * t - lx;   // in (-t, 0]
    const double dy = std::floor(ly / t) * t - ly;   // in (-t, 0]

    // THE ONLY NARROWING IN THIS FUNCTION, and it narrows a RESIDUAL. Each term
    // is bounded by one texel before it touches float. This is emphatically NOT
    // a reconstruction of an absolute position from a basis - see the header.
    const Vec3f centre = (Xd * dx + Yd * dy).ToFloat();

    const float depthRange = ShadowCascadeDepthRange(c);

    // lightDir points TOWARD the light, so the light sits up that direction and
    // looks back down it at the cascade centre.
    const Vec3f eye = centre + lightDir * (depthRange * 0.5f);

    const Mat4x4 view = Mat4x4::LookAt(eye, centre, up);
    const Mat4x4 proj = Mat4x4::OrthoLH(2.0f * kShadowCascadeExtent[c],
                                        2.0f * kShadowCascadeExtent[c],
                                        0.1f, depthRange);

    // Row-vector order, matching every other matrix product in this codebase.
    return view * proj;
}

} // namespace core
