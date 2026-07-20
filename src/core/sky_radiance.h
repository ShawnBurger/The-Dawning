#pragma once
// =============================================================================
// core/sky_radiance.h - CPU evaluation of the procedural sky
// =============================================================================
//
//   *** THIS IS A MIRROR OF HLSL. READ THIS BEFORE TRUSTING ANY TEST OVER IT. ***
//
// SkyRadiance() below is a hand-written C++ TWIN of DawningSkyRadiance() in
// shaders/sky_common.hlsli. Nothing in the engine calls it. The shipped renderer
// evaluates the HLSL; this file evaluates a copy of it.
//
// That is exactly the hazard src/render/rt_texture_lod.h:9-23 already records in
// this tree, in its own words:
//
//     "If path_trace.hlsl is edited without editing this file, the tests stay
//      green and the image is wrong. Keep the two in step by hand."
//
// A twin was written here anyway, deliberately, because the invariant that has
// to be tested - that the sky contains no localised high-energy lobe - can only
// be decided by evaluating the sky densely over the whole sphere, and the test
// binary is CPU-only by construction (see the TheDawningTests notes in
// CMakeLists.txt: no D3D12, no device, no GPU). There is no way to run the HLSL.
//
// -----------------------------------------------------------------------------
// WHAT IS DONE ABOUT IT, AND WHAT IS STILL MISSING
// -----------------------------------------------------------------------------
// "Keep the two in step by hand" is not accepted here. tests/test_sky_energy.cpp
// carries Sky_TwinIsInStepWithShaderSource, which reads shaders/sky_common.hlsli
// off disk, strips comments and whitespace, hashes what is left, and compares it
// against a pinned constant. Editing the HLSL without acknowledging this file
// therefore FAILS THE SUITE instead of silently passing it. That converts the
// mirror from an unwatched convention into a watched assertion.
//
// BE CLEAR ABOUT WHAT THAT TRIPWIRE DOES AND DOES NOT BUY:
//
//   It DOES catch the drift that matters - somebody edits the sky in HLSL and
//   does not touch this file. That is the failure mode rt_texture_lod.h names,
//   and it is the one that would void the energy argument in
//   docs/research/IBL_DESIGN.md section 9.2 while every test stayed green.
//
//   It does NOT verify that these two expressions COMPUTE THE SAME NUMBERS. It
//   verifies that the HLSL has not changed since a human last checked them
//   against each other. A twin that was wrong the day it was written stays wrong
//   and stays green. The tripwire pins agreement in TIME, not in VALUE.
//
// CLOSING IT COMPLETELY needs a GPU probe: evaluate DawningSkyRadiance in HLSL
// for a fixed direction set, read the results back, and compare against
// SkyRadiance here. That is specified as assertion 1.3 in IBL_DESIGN.md section
// 11 Stage 1, it belongs in tools/smoke_test.ps1 rather than in a CPU-only unit
// test, and it is NOT built yet. Until it is, the guarantee is the one stated
// above and no stronger. Do not describe this file as verified against the
// shader.
//
// This translation unit is GPU-free on purpose (core/types.h and <cmath> only),
// on the same sanctioned footing as core/shadow_cascades.cpp. Do not add a D3D12
// include here.
// =============================================================================

#include "types.h"

namespace core
{

// Twin of DawningSkyRadianceFromBlend(). `t` is saturated internally.
Vec3f SkyRadianceFromBlend(float t);

// Twin of DawningSkyRadiance(). `direction` is expected normalised; the function
// does not normalise it, matching the HLSL, which does not either.
Vec3f SkyRadiance(const Vec3f& direction);

// Rec. 709 relative luminance. Not a twin of anything - the shaders do not
// compute sky luminance - but the energy tests need one scalar per direction and
// the choice of weights must be stated somewhere rather than inlined thrice.
float Luminance(const Vec3f& linearRGB);

} // namespace core
