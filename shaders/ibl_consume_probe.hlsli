#ifndef DAWNING_IBL_CONSUME_PROBE_HLSLI
#define DAWNING_IBL_CONSUME_PROBE_HLSLI

// =============================================================================
// ibl_consume_probe.hlsli - GPU evidence that basic_ps CONSUMES the environment
// =============================================================================
// WHAT THIS EXISTS FOR, AND WHY THE OTHER SIX PROBES ARE NOT IT.
//
// shaders/ibl_eval_probe_ps.hlsl validates the IBL evaluation IN ISOLATION. It
// calls the same shaders/ibl_common.hlsli that basic_ps.hlsl calls, on the same
// cube, and it is good evidence about that header. It is NO evidence at all
// about the raster path, and its own header said so: every assertion behind it
// passes with the feature entirely absent from the rendered image, and passes
// with the WRONG DESCRIPTOR bound as the environment cube. Deleting the whole
// `if (iblParams.z != 0)` block from basic_ps left every marker green.
//
// That is this repository's recurring failure - a probe that passes when the
// feature is absent. It has now happened to the shadow-map probe (deleting the
// entire shadow pass broke nothing observable), to the draw-record probe (armed
// on a frame the default mode path-traces), and to Stage 1's direction probe
// (a perfect round trip while exercising one sixth of the face table).
//
// The draw-record probe solved the analogous problem, and this is the same
// pattern: THE SHADER WRITES WHAT IT ACTUALLY LOADED AND WHAT IT ACTUALLY
// ADDED, from the consumption site, into a UAV that is read back and reduced on
// the CPU. Not what it was handed, not a second evaluation done for the probe's
// benefit - the values `finalColor` is literally computed from.
//
// THREE INDEPENDENT CLAIMS, because no one of them covers the others:
//
//   CONSUMPTION  envSpecularMax / envDiffuseMax are reduced from the exact two
//                variables the combine line adds, and envInFinalMax is reduced
//                from what the environment CONTRIBUTED TO finalColor, recovered
//                as finalColor - direct - emission. Three words rather than two
//                because they catch different edits: zeroing either variable
//                empties its own word, while DELETING the terms from the combine
//                expression - which leaves both variables perfectly nonzero -
//                is caught only by the third. All are written AFTER the combine,
//                because a witness taken before it is a witness of a value that
//                may never have reached the image.
//
//   IDENTITY     cubeSkyRelErrMax compares the cube's OWN mip-0 fetch, along the
//                shading reflection vector, against the closed-form sky it is a
//                prefiltered version of. Bind anything else at t0/space6 - the
//                null SRV, the shadow map, a material texture - and this
//                explodes, because no other resource in the heap is the sky.
//                Consumption cannot cover this: a wrong cube still produces a
//                nonzero specular term.
//
//   LIVENESS     cubeSamples counts the invocations that actually performed the
//                environment fetch, and shadedPixels counts the invocations that
//                reached the combine. A comparison neither side reached is not a
//                comparison; these are what stop every claim above passing
//                vacuously on a frame that shaded nothing.
//
// AND A NEGATIVE CONTROL, which is the part that answers the review directly.
// The probe runs on TWO raster frames: one with CBPerFrame::iblParams.z forced
// to 0 and one live. The control frame asserts every word above reads ZERO. So
// the harness does not merely check that the assertions pass with the feature
// present - it checks, in the same run, that they FAIL to pass with the feature
// absent. See Renderer::SetIBLDisabledForFrame and App::RenderFrame.
//
// COMPILED ONLY INTO THE PROBE PERMUTATION of basic_ps.hlsl, behind
// DAWNING_DRAW_PROBE, for the reason that file already documents at length: a
// UAV DECLARATION in a pixel shader defeats early-Z for the whole PSO and a
// runtime flag cannot buy it back. m_pso keeps early-Z; m_psoDrawProbe carries
// this and is bound only on the two probe frames.
// =============================================================================

// One flat block, not one slot per record. The claims are scene-wide - "some
// pixel consumed a nonzero environment term", "no pixel's cube fetch disagreed
// with the sky" - and a per-record array would invite the reader to believe
// per-record claims that are not being made.
//
// Byte offsets, mirrored by IBLConsumeProbeBlock in src/render/ibl_consume_probe.h.
#define DAWNING_IBL_PROBE_ENV_SPECULAR_MAX   0u
#define DAWNING_IBL_PROBE_ENV_DIFFUSE_MAX    4u
#define DAWNING_IBL_PROBE_SKY_REL_ERR_MAX    8u
#define DAWNING_IBL_PROBE_CUBE_SAMPLES      12u
#define DAWNING_IBL_PROBE_SHADED_PIXELS     16u
#define DAWNING_IBL_PROBE_RADIANCE_MAX      20u
#define DAWNING_IBL_PROBE_MIRROR_LUM_MAX    24u
#define DAWNING_IBL_PROBE_ENV_ZERO_PIXELS   28u
#define DAWNING_IBL_PROBE_ENV_IN_FINAL_MAX  32u
// The specular-fidelity words, claimed by the two fixes in this same stage. Each
// is a MAX over the live-frame pixels of a quantity that is EXACTLY ZERO when its
// fix is absent, so the negative control asserts them zero and a "simplification"
// that reverts either fix sends its word to zero on the live frame too.
//
//   SPEC_OCC_ABOVE_AO  max(specularOcclusion - ambientOcclusion). The old code
//                      multiplied envSpecular by the diffuse AO; the remap
//                      departs from it upward wherever the specular lobe is
//                      narrower than a hemisphere. Reverting to `* ao` makes the
//                      applied occlusion equal AO, so this difference is 0.
//   TOKSVIG_ROUGH_INC  max(shadingRoughness - preToksvigRoughness). The widening
//                      the filtered-normal length drives into the roughness the
//                      BRDF and the IBL mip both read. Deleting the widening makes
//                      the shading roughness equal its pre-Toksvig value, so this
//                      difference is 0.
//
// Both are written ONLY inside the cube-sampled branch, so they are untouched on
// the control frame exactly like the identity words - see DawningWriteIBLSpecFidelity.
#define DAWNING_IBL_PROBE_SPEC_OCC_ABOVE_AO 36u
#define DAWNING_IBL_PROBE_TOKSVIG_ROUGH_INC 40u
// 44..63 reserved. The block is a fixed 64 bytes so a word appended later does
// not change the resource size, the zero-fill, or the readback footprint.
#define DAWNING_IBL_PROBE_BYTES             64u

// Fixed-point, because a UAV reduction has to be an INTEGER atomic to be
// order-independent, and an order-dependent float reduction over a million
// pixels is not reproducible. 1/65536 resolution on quantities of order 1 is
// four decimal digits, far finer than any threshold below cares about.
//
// The ceiling is 2^32 - 2^16 rather than 2^32: converting a float at or above
// UINT_MAX to uint is undefined in HLSL, and half-precision radiance really can
// spike at grazing angles. Saturating there is correct for a MAXIMUM - it stays
// above every threshold - and it is the only place this file rounds toward
// "pass", which is why the value is picked to be unreachable by any quantity
// these assertions read rather than merely large.
#define DAWNING_IBL_PROBE_SCALE 65536.0f

uint DawningIBLProbeQuantise(float v)
{
    return (uint)clamp(v * DAWNING_IBL_PROBE_SCALE, 0.0f, 4294901760.0f);
}

// Rec. 709 luminance. Same weights as core::Luminance, so the CPU reads the
// markers in the units it prints them in.
float DawningIBLProbeLuminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// The consumption half. `envDiffuse` and `envSpecular` MUST be the variables the
// caller's combine line adds, and `envInFinal` MUST be recovered from the
// caller's finalColor - passing copies taken earlier reintroduces exactly the
// blind spot this probe exists to remove.
void DawningWriteIBLConsumption(RWByteAddressBuffer probe,
                                float3 envDiffuse, float3 envSpecular,
                                float3 envInFinal,
                                bool sampledCube)
{
    uint ignored;
    const float specLum = DawningIBLProbeLuminance(envSpecular);
    const float diffLum = DawningIBLProbeLuminance(envDiffuse);

    probe.InterlockedMax(DAWNING_IBL_PROBE_ENV_SPECULAR_MAX,
                         DawningIBLProbeQuantise(specLum), ignored);
    probe.InterlockedMax(DAWNING_IBL_PROBE_ENV_DIFFUSE_MAX,
                         DawningIBLProbeQuantise(diffLum), ignored);
    probe.InterlockedMax(DAWNING_IBL_PROBE_ENV_IN_FINAL_MAX,
                         DawningIBLProbeQuantise(DawningIBLProbeLuminance(envInFinal)), ignored);
    probe.InterlockedAdd(DAWNING_IBL_PROBE_SHADED_PIXELS, 1u, ignored);
    if (sampledCube)
        probe.InterlockedAdd(DAWNING_IBL_PROBE_CUBE_SAMPLES, 1u, ignored);
    if (specLum + diffLum <= 0.0f)
        probe.InterlockedAdd(DAWNING_IBL_PROBE_ENV_ZERO_PIXELS, 1u, ignored);
}

// The identity half. `radiance` is the prefiltered radiance the SHADING fetch
// loaded and `R` is the direction it loaded it along, both reported out of
// DawningSpecularIBLWitnessed rather than recomputed here.
//
// The mip-0 fetch is a second tap on the SAME resource object, and that is
// deliberate rather than sloppy: the claim being made is about WHICH RESOURCE IS
// BOUND, and there is exactly one `envCube` declaration in basic_ps.hlsl, so a
// mis-bound descriptor corrupts the shading fetch and this fetch identically.
// It is taken at mip 0 because mip 0 is the one mip whose expected value is
// known in closed form - EnvironmentIBL::RecordBake evaluates the sky exactly
// there, and the startup mirror probe measures the residual at 0.0010. A
// comparison at the shading mip would need a tolerance calibrated against the
// GGX lobe width and would have teeth only as sharp as that calibration.
void DawningWriteIBLIdentity(RWByteAddressBuffer probe,
                             TextureCube<float4> envCube, SamplerState envSampler,
                             float3 R, float3 radiance)
{
    uint ignored;
    const float3 mirror = envCube.SampleLevel(envSampler, R, 0.0f).rgb;
    const float3 sky    = DawningSkyRadiance(R);

    // The sky is an affine gradient with no zero anywhere on the sphere, so this
    // denominator is bounded well away from zero by construction; the max() is
    // defence against a future sky, not slack for this one.
    const float denom  = max(max(sky.r, max(sky.g, sky.b)), 1e-4f);
    const float3 delta = abs(mirror - sky);
    const float relErr = max(delta.r, max(delta.g, delta.b)) / denom;

    probe.InterlockedMax(DAWNING_IBL_PROBE_SKY_REL_ERR_MAX,
                         DawningIBLProbeQuantise(relErr), ignored);
    probe.InterlockedMax(DAWNING_IBL_PROBE_MIRROR_LUM_MAX,
                         DawningIBLProbeQuantise(DawningIBLProbeLuminance(mirror)), ignored);
    probe.InterlockedMax(DAWNING_IBL_PROBE_RADIANCE_MAX,
                         DawningIBLProbeQuantise(DawningIBLProbeLuminance(radiance)), ignored);
}

// The specular-fidelity half. `specOcclusion` MUST be the exact scalar the caller
// multiplied envSpecular by, and `ambientOcclusion` the scalar it multiplied
// envDiffuse by, so their difference witnesses the remap the shipped shading
// applied - not a second evaluation done for the probe. `shadingRoughness` MUST
// be the roughness the direct BRDF and the IBL mip both read, and
// `preToksvigRoughness` the value it held before the widening, so their
// difference witnesses the roughness the shading consumed.
//
// Called only where the cube was sampled, so the control frame - which does not
// sample - leaves both words at their zero-fill, and the reduction asserts that.
void DawningWriteIBLSpecFidelity(RWByteAddressBuffer probe,
                                 float specOcclusion, float ambientOcclusion,
                                 float shadingRoughness, float preToksvigRoughness)
{
    uint ignored;
    probe.InterlockedMax(DAWNING_IBL_PROBE_SPEC_OCC_ABOVE_AO,
                         DawningIBLProbeQuantise(max(specOcclusion - ambientOcclusion, 0.0f)),
                         ignored);
    probe.InterlockedMax(DAWNING_IBL_PROBE_TOKSVIG_ROUGH_INC,
                         DawningIBLProbeQuantise(max(shadingRoughness - preToksvigRoughness, 0.0f)),
                         ignored);
}

#endif // DAWNING_IBL_CONSUME_PROBE_HLSLI
