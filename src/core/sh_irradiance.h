#pragma once
// =============================================================================
// core/sh_irradiance.h - L2 spherical-harmonic diffuse irradiance
// =============================================================================
// IBL_DESIGN.md section 4. Diffuse image-based lighting is stored as nine RGB
// coefficients in CBPerFrame rather than as an irradiance cubemap, because that
// costs ZERO descriptors against a 128-slot heap the asset pipeline already
// names as the binding constraint, and because this sky is a linear function of
// direction.y and therefore lives ENTIRELY in the L0 and L1 bands - the L2
// truncation is not an approximation here, it is exact.
//
// -----------------------------------------------------------------------------
// WHAT IS STORED, AND WHY IT IS NOT WHAT "SH COEFFICIENTS" USUALLY MEANS
// -----------------------------------------------------------------------------
// ProjectRadiance returns the RADIANCE coefficients L_i = integral L(w) Y_i(w) dw.
// PackIrradianceCoefficients then folds in the Lambertian convolution constants
//
//     A0 = PI, A1 = 2 PI / 3, A2 = PI / 4
//
// so what actually reaches the GPU is E_i = A_l * L_i, and the shader's whole
// evaluation is a nine-term dot product against the basis:
//
//     E(N) = sum_i E_i * Y_i(N)
//
// The convolution constants therefore exist in exactly ONE place, on the CPU.
// The alternative - shipping raw L_i and applying A_l in HLSL - would put three
// magic constants on both sides of a C++/HLSL boundary, and a mismatch there
// produces a plausible image at the wrong brightness, which is precisely the
// class of bug section 11 assertion 2.1 exists to catch. One copy cannot
// disagree with itself.
//
// -----------------------------------------------------------------------------
// THE BASIS IS A MIRROR OF HLSL. SAY SO OUT LOUD.
// -----------------------------------------------------------------------------
// DawningSHBasisL2 in shaders/ibl_common.hlsli is a hand-written twin of
// SHBasisL2 below. This is the same hazard src/render/rt_texture_lod.h:9-23 and
// src/core/sky_radiance.h document, and it is a REAL one here: projection and
// evaluation must share a basis, and IBL_DESIGN.md section 4 warns that if the
// two are written independently from two sources the basis WILL be wrong. A
// permuted or sign-flipped basis function still produces a smooth, plausible
// image - lit from the wrong direction.
//
// WHAT CLOSES IT: render::EnvironmentIBL runs a GPU probe on every launch, in
// every mode, that evaluates DawningIrradianceSH in the SHIPPED HLSL for 64
// directions against the SHIPPED coefficients, reads the result back, and
// compares it to EvaluateIrradiance() below. The verdict is the [SMOKE]
// ibl_sh_agreement marker. That converts the mirror from a convention into a
// watched assertion, exactly as the sky-agreement probe did for sky_radiance.h.
//
// STILL NOT COVERED: the probe compares 64 directions, not the whole sphere, and
// it runs only where a D3D12 device exists. The CPU cases in
// tests/test_sh_irradiance.cpp are the device-free evidence.
//
// This translation unit is GPU-FREE on purpose (core/types.h and <cmath> only),
// on the same sanctioned footing as core/shadow_cascades.cpp. Do not add a
// D3D12 include here.
// =============================================================================

#include <cstdint>

#include "types.h"

namespace core
{

// Number of L2 real spherical-harmonic basis functions.
static constexpr uint32_t kSHCoefficientCount = 9;

// Nine RGB coefficients. Used for both the raw radiance projection and the
// Lambert-convolved irradiance form; which one an instance holds is a property
// of the function that produced it, stated at each call site.
struct SHColor9
{
    Vec3f c[kSHCoefficientCount] = {};
};

// -----------------------------------------------------------------------------
// The basis. ONE definition, used by the projector and the evaluator alike.
// -----------------------------------------------------------------------------
// Written directly on the raw components of a world-space direction in the
// engine's left-handed, +Y-up, +Z-forward frame (RULE 7). These are the standard
// real SH functions with the polar axis along the THIRD component, so a sky that
// varies only with direction.y - which this one does - lands in Y[0] and Y[1]
// and nowhere else. That is asserted, not assumed: see
// Sh_LinearSkyOccupiesOnlyTwoBands.
//
// Do NOT "adapt" a formula written for a Z-up convention. If projection and
// evaluation share these expressions the basis cannot be wrong; the only way to
// get it wrong is to write the two independently.
//
// Order: [0] Y00, [1] Y1-1(y), [2] Y10(z), [3] Y11(x),
//        [4] Y2-2(xy), [5] Y2-1(yz), [6] Y20(3z^2-1), [7] Y21(xz), [8] Y22(x^2-y^2)
void SHBasisL2(const Vec3f& direction, float out[kSHCoefficientCount]);

// -----------------------------------------------------------------------------
// Projection
// -----------------------------------------------------------------------------
// A radiance function of direction. Deliberately a plain function pointer plus a
// void* rather than std::function: this TU is meant to stay dependency-free.
using RadianceFn = Vec3f (*)(const Vec3f& direction, void* user);

// Monte-Carlo projection over a deterministic spherical Fibonacci point set.
// `sampleCount` points, each weighted by the uniform solid angle 4*PI/N.
//
// Fibonacci rather than a random set because the result feeds a constant buffer
// that must be identical from run to run - a stochastic projection would make
// every downstream assertion a tolerance question.
SHColor9 ProjectRadiance(RadianceFn fn, void* user, uint32_t sampleCount);

// Projects core::SkyRadiance, i.e. the CPU twin of the shipped sky. This is the
// function the renderer actually calls.
SHColor9 ProjectSkyRadiance(uint32_t sampleCount);

// Sample count for the startup projection, and for every unit case, so the two
// cannot disagree about what was measured.
//
// 65536, AND THE NUMBER IS MEASURED RATHER THAN GUESSED. The design's Stage 2
// assertion 2.2 requires the band-2 coefficients of a y-linear sky to vanish
// below 1e-6. At 16384 samples they did not - the worst was 2.0e-6, in the two
// MIXED coefficients Y2-2 (xy) and Y2-1 (yz), the ones that couple the lattice's
// stratified axis to its azimuthal one.
//
// DIAGNOSED RATHER THAN ACCOMMODATED, per the precedent this repo set when the
// direction round trip missed 0.999 by 0.0001 and the cube was resized instead
// of the threshold. The residual is spherical-Fibonacci QUADRATURE error, not a
// basis or normalisation defect, and the evidence is that it converges:
//
//     N =   4096   worst mixed band-2 coefficient  2.6e-6
//     N =  16384                                   2.0e-6
//     N =  65536                                   8.6e-8
//     N = 262144                                   3.3e-8
//
// A basis error does not shrink by 20x when you add samples; it is a fixed
// fraction of the DC term, which here is ~0.9. So the fix is the sample count,
// and the 1e-6 bound stands unmodified with an order of magnitude to spare.
//
// The cost is a startup-only loop of 65536 * 9 multiply-adds. It runs once.
static constexpr uint32_t kSHProjectionSamples = 65536;

// -----------------------------------------------------------------------------
// Convolution and evaluation
// -----------------------------------------------------------------------------
// Folds the Lambertian transfer constants into the radiance coefficients. The
// result is what CBPerFrame::iblSH holds and what DawningIrradianceSH consumes.
SHColor9 PackIrradianceCoefficients(const SHColor9& radiance);

// E(N) = integral L(w) max(N.w, 0) dw, from PACKED coefficients.
//
// THE TWIN OF DawningIrradianceSH in shaders/ibl_common.hlsli, and now actually
// its twin. These two were NOT the same function: the HLSL ended with
// `max(e, 0)` and this did not, while the GPU agreement probe compared them
// against each other to 1e-4. The comparison was sound only because this sky
// never drives the reconstruction negative - i.e. it was a fact about the
// environment, not about the code, and a steeper environment would have failed
// the probe for a reason that was not a defect.
//
// So the clamp is here too, and the RAW reconstruction is available separately
// below for the callers that want the unclamped identity. The two entry points
// name the difference instead of leaving it for the next reader to find in a
// diff of two languages.
//
// For constant radiance L this returns PI*L exactly - the white furnace
// identity, which is the unit case in tests/test_sh_irradiance.cpp.
Vec3f EvaluateIrradiance(const SHColor9& packed, const Vec3f& normal);

// The same dot product WITHOUT the clamp.
//
// The L2 projection is exact only for the bands it keeps, so a steep environment
// can push the reconstruction slightly negative at grazing normals - and a
// negative irradiance multiplied into albedo is a black rim that reads as a
// normal-map bug, which is why the shipped path clamps. The closed-form identity
// cases in tests/test_sh_irradiance.cpp assert against the UNCLAMPED
// reconstruction, because clamping is a rendering decision and the identity they
// check is a statement about the maths.
//
// Nothing in the engine calls this. If you are reaching for it in render code,
// you want EvaluateIrradiance.
Vec3f EvaluateIrradianceRaw(const SHColor9& packed, const Vec3f& normal);

} // namespace core
