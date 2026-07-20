#pragma once
// =============================================================================
// render/environment_ibl.h - environment IBL: the cube, the SH, and the probes
// =============================================================================
//
// WHAT THIS IS, AND WHAT IT IS NOT YET
// -----------------------------------------------------------------------------
// STAGES 1, 2 AND 3 of docs/research/IBL_DESIGN.md section 11. This class owns
// everything that knows what the environment is:
//
//   Stage 1  the prefiltered environment cubemap, its SRV at a reserved raster
//            heap slot, and the four GPU assertions that prove it correct
//   Stage 2  the L2 diffuse irradiance projection (core::ProjectSkyRadiance),
//            handed to Renderer::BeginFrame for CBPerFrame::iblSH, plus the GPU
//            probe that closes the C++/HLSL SH BASIS mirror
//   Stage 3  the GPU probes for the split-sum specular path basic_ps.hlsl now
//            runs: mirror agreement, env-BRDF bounds, roughness -> mip
//
// STAGE 3 IS WHERE THE IMAGE CHANGES. basic_ps.hlsl's hemisphere ambient
// (ambientDiffuse / ambientSpecular) is DELETED and replaced by the SH diffuse
// and split-sum specular terms; that is what stops a glTF asset with
// metallicFactor 1.0 rendering near-black.
//
// STAGE 4 IS DONE, and it is where the two RENDER PATHS stopped disagreeing.
// path_trace.hlsl's STABLE PREVIEW used to run an ad-hoc environment with a
// magic 2.5 diffuse multiplier, a magic 0.25 bounce damper, a mirror reflection
// that ignored roughness entirely and a gloss ramp corresponding to no physical
// quantity - so F1 changed the LIGHTING MODEL, not just the renderer. It now
// calls the same shaders/ibl_common.hlsli basic_ps.hlsl calls, with the same
// arguments, against this same cube and these same SH coefficients. All four
// constants are gone; only the bounce damper survives, deliberately, because the
// preview traces no secondary bounce (IBL_DESIGN.md 8.2).
//
// THE FULL PATH TRACER IS DELIBERATELY UNTOUCHED and must stay so. It collects
// DawningSkyRadiance on every miss - it evaluates the very integral the split-sum
// approximates - so feeding it the prefiltered cube would replace the reference
// with the approximation. Terminating the last bounce into the cube is rejected
// for the same reason plus a second one: it biases the estimator, and CLAUDE.md
// names this project's two biases rather than accumulating unlisted ones.
// MEASURED: the DXR-full and raster captures are BYTE-IDENTICAL across this
// change; only the stable preview's image moved.
//
// The DXR side now carries its own CONSUMPTION evidence, which it had none of
// before - see PathTracer::ReadIBLProbe and the rt_ibl_consume_* markers. It is
// reduced by the SAME ReduceIBLConsumeProbe the raster probe uses, so the two
// paths' numbers are directly comparable; the SH diffuse maxima agree exactly, at
// 0.287125, and the harness asserts that agreement.
//
// WHY THE SOURCE IS THE PROCEDURAL SKY RATHER THAN A LOADED HDR
// -----------------------------------------------------------------------------
// DawningSkyRadiance is a closed-form function of direction, and that changes
// the shape of the whole feature (IBL_DESIGN.md section 2): the prefilter pass
// takes NO SRV INPUT, so there is no ping-pong, no source descriptor, and every
// mip is an independent integral of ground truth rather than of a downsampled
// copy of the mip above it. Agreement with the DXR miss shader is structural -
// one function, one file - rather than something a test has to police.
//
// WHEN IT RUNS: A REVISION COUNTER, NOT A BOOL
// -----------------------------------------------------------------------------
// EnsureBuilt() early-outs when the baked revision equals the requested sky
// revision, so the static case costs one integer compare. It is a COUNTER rather
// than a "built" flag because a day/night cycle is plausible for a space sim,
// and IBL_DESIGN.md section 5 wants that to land as an edit rather than a
// redesign. Bump the revision and the cube rebuilds.
//
// WHAT A DYNAMIC SKY WOULD STILL BREAK (section 5 names these; none are built):
//   - the cube becomes read-and-written in the same frame and must be
//     kFrameCount-instanced, because barriers order GPU work and frames N-1 and
//     N-2 may still be sampling the old contents;
//   - DXR temporal accumulation must reset on sky revision, not just on camera
//     motion, or it averages over stale radiance;
//   - the Stage 2 SH coefficients live on the CPU and would need reprojecting.
//
// =============================================================================
// VERIFICATION RUNS ON EVERY LAUNCH, IN EVERY MODE. THIS IS DELIBERATE.
// =============================================================================
// Two probes in this repo were previously fixed for running only on a
// path-traced final frame, or only under -RasterOnly - assertions that existed
// and were never reached. The prefilter runs once at startup, so the cheapest
// way to guarantee its evidence is reached in the mode people actually execute
// is to gate it on nothing at all: the readback and every GPU assertion below
// runs on every launch of the engine, in both smoke modes and in ordinary play.
// The cost is one ~1.1 MB copy, one pass over 131k texels, and six 64-pixel
// draws, once, at startup.
//
// The assertions, per IBL_DESIGN.md section 11:
//   1.1 ibl_env_slot=2 while shadow_map_slot=1 still holds  (reservation slipping)
//   1.2 direction round trip                                (the face table)
//   1.3 HLSL/C++ sky agreement                              (the sky twin drifting)
//   1.4 per-mip mean luminance                              (unnormalised filter)
//   1.5 per-mip variance decreasing, with a vacuity floor   (mips never rendered)
//   2.x HLSL/C++ SH agreement                               (the BASIS twin drifting)
//   3.1 env-BRDF physical bounds                            (an A/B swap; a unified Smith k)
//   3.2 roughness -> mip monotonicity                       (an inverted mapping)
//   3.3 mirror agreement at roughness 0                     (mip 0; sampler address mode)
//
// WHAT NONE OF THEM COVER, said plainly: they witness shaders/ibl_common.hlsli,
// which is the header basic_ps.hlsl includes, compiled by the same FXC at the
// same profile. They do NOT witness basic_ps's CALL SITE. Deleting the IBL block
// from basic_ps leaves every marker in this file green, and so does binding the
// WRONG DESCRIPTOR at t0/space6 - the probes here get the cube through their own
// SRV heap and would never notice.
//
// THAT GAP IS NOW CLOSED BY A DIFFERENT PROBE, in a different file, because it
// is a different kind of evidence: render/ibl_consume_probe.h. It rides the
// draw-record probe's UAV pattern, writes from basic_ps's own consumption site
// on a raster frame, and runs a NEGATIVE CONTROL frame beside the live one so
// the assertion is demonstrated to fail with the feature absent rather than
// merely to pass with it present. Nothing in THIS file gained teeth; the
// disclosure above is still exactly true of everything below it.
// =============================================================================

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

#include "../core/sh_irradiance.h"
#include "../core/types.h"

namespace render
{

class D3D12Device;

// -----------------------------------------------------------------------------
// Cubemap parameters (IBL_DESIGN.md section 3).
// -----------------------------------------------------------------------------
// 128 is generous for a smooth gradient; 8 mips give roughness = mip / 7, so
// mip 0 is a mirror and mip 7 is fully rough. R16G16B16A16_FLOAT matches
// Renderer::kHDRFormat, so the chain stays linear HDR with no encode/decode.
// Memory: 6 * sum(128^2 .. 1^2) * 8 B = ~1.05 MB, against 64 MiB of shadow map.
static constexpr uint32_t   kEnvCubeSize   = 128;
static constexpr uint32_t   kEnvCubeMips   = 8;
static constexpr DXGI_FORMAT kEnvCubeFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Hammersley samples per texel for mips 1..7. Mip 0 is an exact evaluation.
static constexpr uint32_t kEnvPrefilterSamples = 128;

// Direction-cubemap probe (assertion 1.2).
//
// IBL_DESIGN.md specifies 4x4. MEASURED at 4x4, the worst round-trip dot was
// 0.998897 against the design's own 0.999 threshold - a fail. That is NOT the
// face table being wrong: a permuted, sign-flipped or v-flipped table sends the
// dot to roughly zero or negative, not to 0.9989. It is cross-seam bilinear
// filtering. Within one face the direction is affine in (u, v), so the fetch
// reconstructs the generator exactly; across a face seam the hardware blends two
// differently-parameterised faces, and the chord-vs-arc error there scales with
// the square of the texel's angular size. At 4x4 a texel spans 22.5 degrees and
// the residual is 2.7 degrees of direction error.
//
// Raising the size to 16 was chosen over widening the tolerance because it
// removes the artefact instead of accommodating it, and it costs nothing the
// assertion cares about: a permuted table is caught just as loudly at any size.
// MEASURED at 16x16 the worst dot is 0.999951, so the design's 0.999 threshold
// is kept unchanged with room to spare. The error fell by a factor of 22 for a
// 4x linear increase, which is the quadratic scaling the seam explanation
// predicts - that agreement is the evidence the diagnosis is right, rather than
// a number that merely went green.
static constexpr uint32_t kEnvDirectionCubeSize = 16;

// Probe slots per pass. One 64x1 target, drawn once per pass.
static constexpr uint32_t kEnvProbeCount = 64;

// -----------------------------------------------------------------------------
// The probe passes, in readback row order.
// -----------------------------------------------------------------------------
// Rows 0-1 are Stage 1's; rows 2-5 are Stages 2 and 3. All six run at startup,
// in every mode, gated on nothing - the same answer this file already gives to
// the failure two other probes in this repo were fixed for.
enum EnvProbePass : uint32_t
{
    kEnvPassSkyAgreement   = 0,   // 1.3
    kEnvPassDirectionRT    = 1,   // 1.2
    kEnvPassSHIrradiance   = 2,   // Stage 2: the C++/HLSL basis mirror
    kEnvPassMirrorSpecular = 3,   // 3.3
    kEnvPassEnvBRDF        = 4,   // 3.1
    kEnvPassMipSweep       = 5,   // 3.2
    kEnvPassCount          = 6,
};

// -----------------------------------------------------------------------------
// Stage 2 / Stage 3 tolerances.
// -----------------------------------------------------------------------------
// SH irradiance: HLSL against core::EvaluateIrradiance. Both evaluate the same
// nine-term dot product on the same float coefficients; the only slack is
// fp32 reassociation between two compilers, which is well under 1e-5 on
// irradiances of order 1.
static constexpr float kEnvSHAgreementTolerance = 1e-4f;

// Mirror specular against core::SkyRadiance. Wider than the sky-agreement
// tolerance because THE CUBE IS R16G16B16A16_FLOAT: a half carries ~11 bits of
// mantissa, so a value near 0.3 quantises at ~1.5e-4 relative, and the trilinear
// fetch blends four of them. 2% is the design's figure and it holds with room to
// spare - MEASURED worst relative error 0.0010.
//
// WHAT IT CATCHES, MEASURED RATHER THAN CLAIMED:
//   reflect(V, N) instead of reflect(-V, N)   -> relative error 1.03, fails hard
//   roughness -> mip inverted                 -> fails
//
// WHAT IT DOES NOT CATCH, AND THE DESIGN SAYS IT DOES:
//   * "Set the cube sampler to WRAP -> fails at face edges." IT DOES NOT.
//     MEASURED: switching this file's static sampler from CLAMP to WRAP leaves
//     both this assertion AND the direction round trip passing. Cube sampling
//     does not use the 2-D address modes - the hardware resolves a direction to
//     a face and filters across face seams itself - so the address mode is inert
//     here. CLAMP is kept because it is the honest declaration of intent, but no
//     assertion in this repo depends on it and none can.
//   * An OFF-BY-ONE in roughness -> mip. MEASURED: adding 1.0 to the mip moves
//     the worst relative error from 0.0010 to 0.001768, far inside 2%, so THIS
//     assertion does not see it. Adjacent mips of a smooth linear gradient
//     differ by ~0.1%, which is at the half-precision noise floor.
//
//     THE MONOTONICITY ASSERTION DOES SEE IT, and an earlier version of this
//     comment claimed otherwise. RE-MEASURED by performing the mutation: 3.2
//     fails with worst backward step -0.00098392 against a -1e-5 tolerance,
//     deterministically, with identical numbers in both smoke modes. The
//     mechanism is kEnvMipSweepLastSlot: the sweep excludes the 2x2 and 1x1
//     faces' luminance reversal under the CORRECT mapping, and an off-by-one
//     shifts that reversal down into the asserted window. The exclusion range
//     and the detection are the same fact.
//
//     So the design's "inverted or off by one" claim for 3.2 holds. What does
//     not hold is any claim that 3.3 covers the off-by-one; that is what this
//     paragraph is for.
static constexpr float kEnvMirrorTolerance = 0.02f;

// Env-BRDF fit bounds.
//
// THESE TWO APPLY AT ROUGHNESS 0 ONLY, and that restriction was MEASURED into
// existence rather than assumed. Applied across the whole 8x8 grid they failed:
// at roughness 1 and grazing incidence the fit returns 0.0157 for a dielectric,
// not something near unity.
//
// Diagnosed: that is correct physics, and the assertion was the thing that was
// wrong. "Grazing reflectance approaches 1" and "normal-incidence reflectance
// equals F0" are properties of a SMOOTH surface. On a rough one the visible
// microfacets are widely distributed, so a grazing view still meets many facets
// near their own normal incidence and the Fresnel boost washes out; and the
// single-scattering split-sum additionally loses energy with roughness, which
// IBL_DESIGN.md section 9.4 names and deliberately does not correct.
//
//   MEASURED at roughness 0: normal-incidence error 0.0056, grazing 1.038.
//   MEASURED at roughness 1: normal-incidence error 0.0243, grazing 0.0157.
//
// Restricting to the smooth row costs nothing that matters, because roughness 0
// is exactly where the A/B swap is most visible. The whole-grid claim is carried
// instead by the energy bounds below, which DO hold everywhere.
static constexpr float kEnvBRDFNormalIncidenceTolerance = 0.02f;
static constexpr float kEnvBRDFGrazingFloor             = 0.80f;
// F0 = 1 is a perfect mirror: single-scattering split-sum may LOSE energy at
// high roughness (IBL_DESIGN.md 9.4, accepted) but must never create any.
static constexpr float kEnvBRDFEnergyCeiling            = 1.02f;

// THE SAME CLAIM FOR A DIELECTRIC, and it is a separate constant because the two
// are separate claims with genuinely different answers.
//
// The F0 = 1 ceiling above cannot see the dielectric case at all: it reads
// A + B, and at F0 = 0.04 the quantity that matters is 0.04*A + B. MEASURED over
// the 8x8 grid, that reaches 1.042432 at (roughness 0, NdotV 0) - a smooth
// dielectric at grazing incidence returning 4.2% MORE environment energy than
// arrives at it. The old guard reported "no energy creation" while that number
// sat unexamined in the same table.
//
// IT IS NOT A BUG AND IT IS NOT BEING HIDDEN. DawningEnvBRDFApprox is Lazarov's
// analytic fit to the split-sum integral, accurate to a few percent and worst
// exactly where this is - low roughness, grazing angles - and the reference
// integral it fits does obey the bound. A 4.2% overshoot at one corner of the
// domain is the documented cost of spending ~10 ALU instead of a heap slot on a
// LUT (see the note above DawningEnvBRDFApprox in shaders/ibl_common.hlsli).
//
// So the honest guard is a bound on how far the FIT may stray, not a claim that
// it never strays. 1.06 is the measured 1.0417 with margin, set once from the
// measurement. WHAT IT STILL CATCHES: an A/B swap sends this to 1.29, and
// re-expressing the fit with brdf_common.hlsli's direct-lighting Smith k moves
// it too. What it deliberately does NOT do is fail for the 4.2%, which would be
// asserting against a trade the design made on purpose.
//
// If a LUT ever replaces the fit, tighten this to kEnvBRDFEnergyCeiling and the
// two claims become one.
static constexpr float kEnvBRDFDielectricEnergyCeiling  = 1.06f;
// ...and must not collapse. MEASURED minimum over the 8x8 grid: 0.452, at
// roughness 1. That number is the single-scatter energy loss the design names,
// stated here so the next reader can tell it from a regression.
static constexpr float kEnvBRDFEnergyFloor              = 0.35f;

// -----------------------------------------------------------------------------
// Mip sweep (assertion 3.2). The prefiltered radiance along +Y must not DECREASE
// with roughness, and must rise appreciably. An inverted or off-by-one
// roughness -> mip mapping reverses the sequence and fails both halves.
// -----------------------------------------------------------------------------
// THE SWEEP IS ASSERTED OVER SLOTS 0..54, NOT 0..63. That excludes roughness
// above 6/7, the neighbourhood of mips 6 and 7 - the 2x2 and 1x1 faces. The
// exclusion is measured, and it is the SAME pair of mips kEnvVarianceLastMip
// already excludes, for a related reason.
//
// MEASURED, the sequence rises monotonically from 0.243156 at roughness 0 to
// 0.280733 at roughness 6/7, then FALLS to 0.272305 at roughness 1.
//
// Diagnosed, and it is not a mapping error. At mip 6 a cube face is 2x2, so a
// fetch along exactly +Y lands on the meeting point of all four texels and
// bilinearly averages four integrals taken about directions ~35 degrees off +Y.
// Those lean toward the bright horizon, so their average exceeds mip 7's single
// texel, which is the integral about +Y itself. The reversal is a property of a
// 128-cube's last two mips, worth 0.008 in luminance; "fixing" it would mean a
// larger cube for no visible gain, and asserting through it would be asserting
// on the artefact.
//
// Over the asserted range every step is >= 0 with exact ties, so the tolerance
// below is half-ulp slack rather than room for a wrong answer, and the rise
// floor sits at about half the measured 0.0376. An inverted mapping does not
// merely miss that floor - it produces a rise of about -0.0376.
//
// THE EXCLUSION IS ALSO WHAT CATCHES AN OFF-BY-ONE, which is worth stating
// because it reads like pure accommodation. Under the correct mapping slot 54 is
// roughness 6/7 and lands on mip 6, just below the reversal. `mip + 1` sends that
// same slot to mip 7 and drags the reversal INTO the window: MEASURED worst step
// -0.00098392, a hard fail. Narrowing the range further to "be safe" would give
// that mutation somewhere to hide.
static constexpr uint32_t kEnvMipSweepLastSlot      = 54;
static constexpr float    kEnvMipSweepStepTolerance = 1e-5f;
static constexpr float    kEnvMipSweepMinRise       = 0.02f;

// -----------------------------------------------------------------------------
// The mip range over which variance is required to STRICTLY decrease (1.5).
// -----------------------------------------------------------------------------
// IBL_DESIGN.md asks for mips 0..5, excluding 6 and 7 because a 2x2 and a 1x1
// face have variance legitimately at the floor and asserting on it would be
// asserting on noise. The design's lower bound is raised here by one, and the
// reason is measured rather than assumed:
//
//   mip 0 is roughness 0 and mip 1 is roughness 1/7, whose GGX lobe is so tight
//   that the true variance drop between them is a fraction of a percent - while
//   the two mips are 128^2 and 64^2, so they sample the sphere at different
//   densities, and that discretisation difference is LARGER than the signal.
//   Requiring mip1 < mip0 would therefore be asserting on which of two errors
//   happened to win, not on whether the prefilter blurs.
//
// So the strict-decrease chain runs 1..5, and the claim that the chain really is
// progressively blurring is carried instead by kEnvVarianceFalloff below, which
// compares across the full range where the signal is unambiguous. The teeth are
// unchanged: mips that were never rendered are zero or garbage, and zero fails
// strict decrease immediately, while the variance[0] floor catches an all-zero
// cube that would satisfy "decreasing" vacuously.
static constexpr uint32_t kEnvVarianceFirstMip = 1;
static constexpr uint32_t kEnvVarianceLastMip  = 5;

// variance(last) must be at most this fraction of variance(first).
//
// MEASURED: variance[1] = 0.00263044, variance[5] = 0.00153879, ratio 0.585.
// The threshold is set ONCE from that measurement with margin, not tightened
// until it went green - 0.5 was the initial guess and it failed, so the number
// below is the calibrated one and the measurement is recorded here so the next
// reader can tell a real regression from a re-tuning.
static constexpr float kEnvVarianceFalloff = 0.75f;

// Assertion 1.4: per-mip mean luminance must stay within this fraction of mip 0.
static constexpr float kEnvMipEnergyTolerance = 0.05f;

// Assertion 1.2 / 1.3 tolerances.
static constexpr float kEnvDirectionDotTolerance = 0.999f;
static constexpr float kEnvSkyAgreementTolerance = 1e-4f;

// -----------------------------------------------------------------------------
// Readback statistics, exposed so the caller can log markers and so the unit
// tests can reason about the reduction without a GPU.
// -----------------------------------------------------------------------------
struct EnvironmentIBLValidation
{
    bool  built                = false;

    // 1.2 - worst dot(normalize(sampled), query) over the probe set. 1.0 is exact.
    float worstDirectionDot    = 0.0f;
    uint32_t directionSlots    = 0;

    // 1.3 - worst absolute component difference between HLSL and core::SkyRadiance.
    float worstSkyDelta        = 0.0f;
    uint32_t skySlots          = 0;

    // How many of the six (dominant axis, sign) buckets the probe direction set
    // actually reaches. THIS IS A VACUITY GUARD FOR 1.2, and it is not covered
    // by the written-slot count: a direction set that degenerated to 64 copies
    // of one direction would write all 64 slots and round-trip perfectly while
    // testing exactly one cube face. The design's wording for 1.2 is "covering
    // all six faces and both signs of every axis"; this is that clause, asserted
    // rather than assumed.
    uint32_t probeFacesCovered = 0;

    // 1.4 / 1.5 - per-mip reduction over the whole cube.
    float mipMeanLuminance[kEnvCubeMips]  = {};
    float mipVariance[kEnvCubeMips]       = {};
    // Worst |mean(m) - mean(0)| / mean(0) over every mip.
    float worstMipEnergyDrift  = 0.0f;
    // True when variance is strictly decreasing across the asserted mip range.
    bool  varianceDecreasing   = false;

    // ---- Stage 2 -----------------------------------------------------------
    // Worst absolute component difference between DawningIrradianceSH on the GPU
    // and core::EvaluateIrradiance on the CPU, over the probe direction set.
    float worstSHDelta         = 0.0f;
    uint32_t shSlots           = 0;
    // Smallest irradiance luminance seen. A VACUITY GUARD: a basis that returned
    // zero everywhere would agree with a CPU side that did the same, and the
    // corridor would stay black with every assertion green.
    float minSHLuminance       = 0.0f;

    // ---- Stage 3 -----------------------------------------------------------
    // 3.3 - worst RELATIVE error between the shipped mirror fetch and
    // core::SkyRadiance, after dividing out the env-BRDF scalar the shader wrote.
    float worstMirrorRelError  = 0.0f;
    uint32_t mirrorSlots       = 0;
    float minMirrorLuminance   = 0.0f;

    // 3.1 - the env-BRDF fit's physical bounds over the 8x8 (NdotV, roughness) grid.
    uint32_t envBRDFSlots      = 0;
    // How many (roughness 0, NdotV 0 or 1) grid points the two smooth-row claims
    // actually evaluated. Must be 2. A vacuity guard: both are single-point
    // claims and a grid that no longer reaches those corners would satisfy them
    // by never running.
    uint32_t envBRDFSmoothRowSamples = 0;
    float worstNormalIncidenceError = 0.0f;   // |F0*A+B - F0| at NdotV = 1, F0 = 0.04
    float worstGrazingReflectance   = 1.0f;   // min F0*A+B at NdotV = 0, F0 = 0.04
    float worstEnergyExcess         = 0.0f;   // max (A+B)   over the grid
    float worstEnergyShortfall      = 1.0f;   // min (A+B)   over the grid
    // max (0.04*A + B) over the grid. A SEPARATE claim from worstEnergyExcess,
    // not a restatement: F0*A + B is symmetric in A and B at F0 = 1, so the
    // metal reading is blind to things the dielectric reading is not - which is
    // the same asymmetry that makes the A/B swap invisible at F0 = 1.
    float worstDielectricExcess     = 0.0f;

    // 3.2 - roughness -> mip.
    uint32_t mipSweepSlots     = 0;
    float worstMipSweepStep    = 0.0f;   // most negative luminance step, 0 if monotone
    float mipSweepRise         = 0.0f;   // luminance(roughness 1) - luminance(roughness 0)

    // Per-assertion verdicts, so a marker names which claim broke.
    bool  shAgreementOk        = false;
    bool  mirrorOk             = false;
    bool  envBRDFOk            = false;
    bool  mipSweepOk           = false;
};

// =============================================================================
// EnvironmentIBL
// =============================================================================
class EnvironmentIBL
{
public:
    EnvironmentIBL() = default;
    ~EnvironmentIBL() = default;

    EnvironmentIBL(const EnvironmentIBL&)            = delete;
    EnvironmentIBL& operator=(const EnvironmentIBL&) = delete;

    // Creates the cube, its 48 RTVs, the prefilter root signature and PSOs, and
    // the verification resources. Records no GPU work.
    bool Init(ID3D12Device* device);

    // Bakes the cube if `skyRevision` differs from the revision already baked,
    // then reads back and asserts. Opens, closes and flushes its own command
    // list in the App::InitializeScene style, so it must not be called between
    // an existing ResetCommandList and its ExecuteCommandLists.
    bool EnsureBuilt(D3D12Device& device, uint32_t skyRevision);

    // Writes the cube's SRV into `destination`, which must be a CPU handle in
    // the caller's shader-visible heap.
    void WriteCubeSRV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE destination) const;

    // Emits the [SMOKE] markers the harness asserts on, and returns false when
    // any Stage 1 assertion failed.
    //
    // `descriptorSlot` is the raster heap slot the caller published the cube's
    // SRV into, so assertion 1.1 reports the slot that was actually written
    // rather than the constant that was intended.
    //
    // `firstMaterialSlot` is the descriptor allocator's live firstIndex, and it
    // is the half of 1.1 with teeth in Stage 1. Nothing samples the cube yet, so
    // if the allocator's reservation slipped back to 2 a material texture would
    // quietly overwrite the cube's SRV and NO runtime check would notice - the
    // damage would surface in Stage 3 as "reflections are wrong". Reporting the
    // allocator's own value, rather than the constant it was derived from,
    // is what makes that regression fail here instead of two stages later.
    bool LogMarkers(uint32_t descriptorSlot, uint32_t firstMaterialSlot) const;

    void Shutdown();

    bool IsBuilt() const { return m_bakedRevision != kUnbakedRevision; }
    ID3D12Resource* Cube() const { return m_cube.Get(); }
    const EnvironmentIBLValidation& Validation() const { return m_validation; }

    // The L2 diffuse irradiance coefficients, ALREADY Lambert-convolved
    // (core::PackIrradianceCoefficients). This is what Renderer::BeginFrame
    // copies into CBPerFrame::iblSH.
    //
    // It lives here rather than in Renderer because this class is the one thing
    // that knows what the environment is: the same projection feeds the constant
    // buffer AND the startup agreement probe, so there is no second projection to
    // drift from the one the shader sees. Zeroed until EnsureBuilt succeeds, and
    // BeginFrame keys the IBL enable flag on IsBuilt() for the same reason.
    const core::SHColor9& IrradianceCoefficients() const { return m_irradiance; }

    // The (NdotV, roughness) grid the env-BRDF probe pass sweeps.
    //
    // DISCLOSED MIRROR: DawningProbeGrid in shaders/ibl_eval_probe_ps.hlsl is the
    // HLSL twin of this. It is two lines of integer arithmetic with no floating
    // point subtlety, and the CPU's expectations of it are PHYSICAL BOUNDS rather
    // than a golden table - so a desynchronised grid moves the tuples out of the
    // region where those bounds hold and the assertion fails rather than passing
    // on the wrong data.
    static void EnvBRDFProbeTuple(uint32_t slot, float& NdotV, float& roughness);

private:
    static constexpr uint32_t kUnbakedRevision = 0xFFFFFFFFu;

    bool CreateCube(ID3D12Device* device);
    bool CreateRootSignature(ID3D12Device* device);
    bool CreatePipelines(ID3D12Device* device);
    bool CreateVerificationResources(ID3D12Device* device);

    void RecordBake(D3D12Device& device);
    void RecordVerification(D3D12Device& device);
    bool UploadProbeConstants();
    bool ReadbackAndValidate();

    Microsoft::WRL::ComPtr<ID3D12Resource>       m_cube;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_cubeRtvHeap;      // 6 faces * 8 mips
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_cubeReadback;

    Microsoft::WRL::ComPtr<ID3D12Resource>       m_directionCube;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_directionRtvHeap; // 6 faces
    // One shader-visible descriptor, bound ONLY during the startup probe pass.
    // Binding a second CBV_SRV_UAV heap is legal because this happens before any
    // frame is recorded; the engine's one-bindable-heap rule is about a single
    // point in time, and no scene pass is open here.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_probeSrvHeap;

    Microsoft::WRL::ComPtr<ID3D12Resource>       m_probeTarget;      // 64x1 RGBA32F
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_probeRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_probeReadback;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_probeDirectionCB;

    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoPrefilter;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoDirection;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoSkyProbe;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoDirectionProbe;
    // Stages 2 and 3. Each of these compiles shaders/ibl_eval_probe_ps.hlsl,
    // which includes shaders/ibl_common.hlsli - so creating them is the standing
    // proof that the shared IBL header compiles under fxc ps_5_1 /WX, checked on
    // every launch rather than asserted in a document.
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoSHProbe;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoMirrorProbe;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoEnvBRDFProbe;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoMipSweepProbe;

    uint32_t m_rtvDescSize    = 0;
    uint32_t m_bakedRevision  = kUnbakedRevision;

    // The probe direction set. Generated once on the CPU, uploaded verbatim, and
    // compared against verbatim - there is no second generator to drift.
    core::Vec3f m_probeDirections[kEnvProbeCount] = {};

    // The diffuse projection. Produced on the CPU in EnsureBuilt, uploaded into
    // the probe constant buffer AND handed to Renderer::BeginFrame - one array,
    // two consumers, no second projection.
    core::SHColor9 m_irradiance;

    // Copyable footprints for the cube readback, one per (face, mip).
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_cubeFootprints[kEnvCubeMips * 6] = {};
    uint64_t m_cubeReadbackBytes = 0;

    EnvironmentIBLValidation m_validation;
};

// Fills `out` with kEnvProbeCount directions covering all six cube faces and
// both signs of every axis. GPU-free and deterministic: the same function
// supplies the GPU's constant buffer and the CPU's expected values, so
// assertions 1.2 and 1.3 compare against the identical direction set.
void BuildEnvironmentProbeDirections(core::Vec3f* out, uint32_t count);

} // namespace render
