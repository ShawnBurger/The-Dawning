#pragma once
// =============================================================================
// render/ibl_consume_probe.h - the IBL CONSUMPTION probe's layout and reduction
// =============================================================================
// The CPU mirror of shaders/ibl_consume_probe.hlsli, plus the pure reduction
// that turns one 64-byte GPU block into a verdict.
//
// DELIBERATELY FREE OF D3D12, on the same sanctioned footing as
// gpu_draw_records.h and rt_texture_lod.h: the layout, the fixed-point scale and
// every threshold below are testable on a machine with no device, and
// tests/test_ibl_consume_probe.cpp exercises the SHIPPED reduction rather than a
// copy of it. Do NOT add a d3d12.h include here.
//
// =============================================================================
// WHAT THIS PROBE IS FOR
// =============================================================================
// The six startup probes in environment_ibl.h witness shaders/ibl_common.hlsli:
// the SH basis, the mirror fetch, the env-BRDF fit, the roughness -> mip
// mapping. They are good evidence about that header and NO evidence about the
// raster path, which their own headers said in as many words - every one of them
// passes with the IBL block deleted from basic_ps.hlsl, and passes with the
// WRONG DESCRIPTOR bound at t0/space6.
//
// That gap is this repository's recurring failure: a probe that passes when the
// feature is absent. The shadow-map probe had it (deleting the shadow pass broke
// nothing observable), the draw-record probe had it (armed on a frame the
// default mode path-traces), and Stage 1's direction probe had it (a perfect
// round trip over one sixth of the face table).
//
// The fix follows the draw-record probe's proven pattern: the shader writes WHAT
// IT ACTUALLY LOADED AND WHAT IT ACTUALLY ADDED, from the consumption site, into
// a UAV that is read back and reduced here.
//
// =============================================================================
// THE NEGATIVE CONTROL, WHICH IS THE PART THAT ANSWERS THE REVIEW
// =============================================================================
// An assertion that passes with the feature present proves nothing on its own;
// every assertion this probe replaces did exactly that. So the probe runs on TWO
// consecutive raster frames of every smoke run:
//
//   the CONTROL frame  Renderer::SetIBLDisabledForFrame(true) forces
//                      CBPerFrame::iblParams.z to 0, basic_ps takes the zero
//                      branch, and EVERY word below is asserted to read zero
//                      while shadedPixels stays positive.
//   the LIVE frame     the ordinary configuration, where every word is asserted
//                      to be nonzero and the cube fetch is asserted to agree
//                      with the sky.
//
// The two together are the claim: not "the assertion passes", but "the assertion
// passes when the feature is present and fails to pass when it is absent",
// demonstrated inside the run rather than in a commit message. It is also what
// makes iblParams.z a REACHABLE state rather than a kill switch no code path can
// enter - see the note on Renderer::SetIBLDisabledForFrame.
// =============================================================================

#include <cstdint>

namespace render
{

// -----------------------------------------------------------------------------
// The GPU block. Byte-for-byte the offsets in shaders/ibl_consume_probe.hlsli.
// -----------------------------------------------------------------------------
// ONE flat block rather than one slot per draw record. The claims are scene-wide
// - "some pixel consumed a nonzero environment term", "no pixel's cube fetch
// disagreed with the sky" - and a per-record array would invite a reader to
// believe per-record claims that are not being made.
struct IBLConsumeProbeBlock
{
    // Maxima, fixed-point. An order-independent reduction over a million pixels
    // has to be an INTEGER atomic; a float sum would not be reproducible.
    uint32_t envSpecularMaxQ     = 0;   //  0
    uint32_t envDiffuseMaxQ      = 0;   //  4
    uint32_t skyRelErrMaxQ       = 0;   //  8
    uint32_t cubeSamples         = 0;   // 12  invocations that fetched the cube
    uint32_t shadedPixels        = 0;   // 16  invocations that reached the combine
    uint32_t radianceMaxQ        = 0;   // 20  the SHADING fetch's prefiltered radiance
    uint32_t mirrorLuminanceMaxQ = 0;   // 24  the mip-0 fetch, identity witness
    uint32_t envZeroPixels       = 0;   // 28  invocations that got no environment light
    uint32_t envInFinalMaxQ      = 0;   // 32  finalColor - direct - emission
    uint32_t reserved[7]         = {};  // 36..63
};
static_assert(sizeof(IBLConsumeProbeBlock) == 64,
              "IBLConsumeProbeBlock must match DAWNING_IBL_PROBE_BYTES in "
              "shaders/ibl_consume_probe.hlsli");

// Must equal DAWNING_IBL_PROBE_SCALE. 1/65536 on quantities of order 1 is four
// decimal digits, far finer than any threshold here reads.
constexpr float kIBLProbeQuantScale = 65536.0f;

inline float IBLProbeDequantise(uint32_t q)
{
    return static_cast<float>(q) / kIBLProbeQuantScale;
}

// -----------------------------------------------------------------------------
// Thresholds. Every one of these was MEASURED and is recorded with its
// measurement, so the next reader can tell a regression from a re-tuning.
// -----------------------------------------------------------------------------

// The cube's mip-0 fetch against core::SkyRadiance / DawningSkyRadiance, along
// the SHADING reflection vector, worst case over every shaded pixel.
//
// MEASURED on the live frame: 0.000977 in both smoke modes. Mip 0 is an exact
// per-texel evaluation of the sky (EnvironmentIBL::RecordBake), so the residual
// here is half-precision storage plus the trilinear blend across a 128-texel
// face - the same two effects the startup mirror probe measures at 0.0010 over
// its 64 canned directions, and this lands at the same magnitude over a million
// arbitrary shading reflection vectors. That agreement is worth more than the
// number: two independent samplings of the same resource, one canned and one
// taken from real geometry, produce the same residual.
//
// The threshold is 2%, an order of magnitude above the measurement and the same
// figure kEnvMirrorTolerance already carries for the same comparison. WHAT IT
// CATCHES: any other descriptor bound at t0/space6. The null SRV reads 0 and
// gives 1.0; the shadow map and any material texture are not this sky and give
// numbers far larger still. There is no resource in m_textureHeap that could sit
// inside 2% of the prefiltered sky by accident.
constexpr float kIBLConsumeSkyTolerance = 0.02f;

// Floors on the environment terms that reached the image. These are LIVENESS
// bounds, not accuracy bounds: they exist so "the feature is present" cannot be
// satisfied by a term that rounds to nothing.
//
// MEASURED on the live frame, -RasterOnly / default: envSpecularMax
// 0.343292 / 0.346802, envDiffuseMax 0.287125 / 0.287125, envInFinalMax
// 0.343384 / 0.346893, radianceMax 0.420258, mirrorLuminanceMax 0.421326. The
// floor is set two orders below the smallest of those. Set once from the
// measurement; NOT tightened until it went green.
//
// Also MEASURED, and worth recording because it is the strongest single number
// here: envZeroPixels is 0 against 1.49 million shaded pixels. Every pixel the
// raster path shades receives environment light. The verdict below only requires
// SOME pixel to, because a black-albedo or fully-occluded surface legitimately
// receives none, but nothing in this scene is such a surface today.
constexpr float kIBLConsumeEnvFloor = 0.001f;

// The control frame's ceiling. Zero, plus room for one floating-point residue.
//
// envInFinal is recovered as finalColor - direct - emission, and with the
// environment terms identically zero that is (direct + emission) - direct -
// emission, which is zero only up to rounding. The residue is of order
// ulp(direct + emission), about 1e-6 on the brightest pixel in this scene, so it
// quantises to 0 anyway - but asserting an exact zero on a value that is a
// difference of floats would be asserting on the rounding mode. Three orders
// below kIBLConsumeEnvFloor, so the control and the live claim cannot both hold.
constexpr float kIBLConsumeZeroCeiling = 0.001f;

// -----------------------------------------------------------------------------
// The reduction's output. One struct per probed frame.
// -----------------------------------------------------------------------------
struct IBLConsumeValidation
{
    // Which of the two frames this is. Not a formatting detail: it selects the
    // entire claim set, and getting it backwards would assert the control's
    // bounds against the live frame and pass on a run with no IBL at all.
    bool iblExpectedActive = false;

    uint32_t shadedPixels  = 0;
    uint32_t cubeSamples   = 0;
    uint32_t envZeroPixels = 0;

    float envSpecularMax     = 0.0f;
    float envDiffuseMax      = 0.0f;
    float envInFinalMax      = 0.0f;
    float radianceMax        = 0.0f;
    float mirrorLuminanceMax = 0.0f;
    float skyRelError        = 0.0f;

    // Per-claim verdicts, so a marker names which one broke rather than making
    // the reader diff eight numbers.
    bool reachedOk    = false;   // the pixel counts - the vacuity guard
    bool consumptionOk = false;  // the three environment terms
    bool identityOk   = false;   // the cube really is the environment cube
    bool ok           = false;
};

// Pure. `iblExpectedActive` says which frame's claim set to apply.
IBLConsumeValidation ReduceIBLConsumeProbe(const IBLConsumeProbeBlock& block,
                                           bool iblExpectedActive);

} // namespace render
