// =============================================================================
// render/ibl_consume_probe.cpp - the IBL consumption probe's reduction
// =============================================================================
// See ibl_consume_probe.h for what the probe claims and why the startup probes
// do not claim it.
// =============================================================================

#include "ibl_consume_probe.h"

namespace render
{

IBLConsumeValidation ReduceIBLConsumeProbe(const IBLConsumeProbeBlock& block,
                                           bool iblExpectedActive)
{
    IBLConsumeValidation v;
    v.iblExpectedActive = iblExpectedActive;

    v.shadedPixels  = block.shadedPixels;
    v.cubeSamples   = block.cubeSamples;
    v.envZeroPixels = block.envZeroPixels;

    v.envSpecularMax     = IBLProbeDequantise(block.envSpecularMaxQ);
    v.envDiffuseMax      = IBLProbeDequantise(block.envDiffuseMaxQ);
    v.envInFinalMax      = IBLProbeDequantise(block.envInFinalMaxQ);
    v.radianceMax        = IBLProbeDequantise(block.radianceMaxQ);
    v.mirrorLuminanceMax = IBLProbeDequantise(block.mirrorLuminanceMaxQ);
    v.skyRelError        = IBLProbeDequantise(block.skyRelErrMaxQ);

    if (iblExpectedActive)
    {
        // THE VACUITY GUARD, and it is asserted FIRST because everything below
        // it is a maximum over a set that may be empty. A frame that shaded no
        // pixels satisfies "no pixel disagreed with the sky" perfectly.
        //
        // cubeSamples == shadedPixels rather than merely > 0. The IBL branch is
        // predicated on iblParams.z, which is wave-uniform and frame-constant, so
        // on a live frame EVERY invocation that reaches the combine must also
        // have fetched the cube. Any inequality means some pixels are shading
        // without the environment, which is the defect this probe exists for,
        // caught even when the remaining pixels keep the maxima above their
        // floors.
        v.reachedOk = (block.shadedPixels > 0) &&
                      (block.cubeSamples == block.shadedPixels) &&
                      (block.envZeroPixels < block.shadedPixels);

        // The three consumption words. Separate rather than summed, and all
        // three MEASURED to be independently necessary:
        //
        //   zero envSpecular/envDiffuse at the point of use -> spec 0.343292 and
        //     diffuse 0.287125 both go to 0, in-final follows
        //   bind heap slot 1 as the environment cube        -> spec goes to 0 and
        //     DIFFUSE STAYS AT 0.287125, because the diffuse term rides
        //     CBPerFrame and touches no descriptor. A sum would have hidden this
        //   delete the terms from the combine line          -> spec AND diffuse
        //     stay at their exact healthy values, 0.343292 and 0.287125, and ONLY
        //     in-final goes to 0. This is the edit the third word exists for and
        //     the reason it is recovered from finalColor rather than added up
        //     from the variables
        v.consumptionOk = (v.envSpecularMax > kIBLConsumeEnvFloor) &&
                          (v.envDiffuseMax  > kIBLConsumeEnvFloor) &&
                          (v.envInFinalMax  > kIBLConsumeEnvFloor);

        // The identity words. The relative-error bound is the claim; the two
        // luminance floors beside it are its vacuity guard, because a resource
        // that read zero everywhere against a sky that also read zero would
        // agree to within any tolerance.
        v.identityOk = (v.skyRelError < kIBLConsumeSkyTolerance) &&
                       (v.mirrorLuminanceMax > kIBLConsumeEnvFloor) &&
                       (v.radianceMax        > kIBLConsumeEnvFloor);
    }
    else
    {
        // THE NEGATIVE CONTROL. Same words, opposite claim.
        //
        // shadedPixels > 0 is what stops this passing on a frame that rendered
        // nothing - which would otherwise satisfy every "is zero" below and
        // report a control that never ran as a control that succeeded. It is the
        // same guard as on the live side and it matters more here, because every
        // other claim on this branch is satisfied by an empty frame.
        v.reachedOk = (block.shadedPixels > 0) &&
                      (block.cubeSamples == 0) &&
                      (block.envZeroPixels == block.shadedPixels);

        v.consumptionOk = (v.envSpecularMax < kIBLConsumeZeroCeiling) &&
                          (v.envDiffuseMax  < kIBLConsumeZeroCeiling) &&
                          (v.envInFinalMax  < kIBLConsumeZeroCeiling);

        // Not merely "the fetch is small" - the identity words are written only
        // inside the branch that fetches, so on the control frame they must be
        // untouched. Asserting that is what proves the branch really was skipped
        // rather than taken and multiplied by zero somewhere downstream.
        v.identityOk = (block.skyRelErrMaxQ == 0) &&
                       (block.mirrorLuminanceMaxQ == 0) &&
                       (block.radianceMaxQ == 0);
    }

    v.ok = v.reachedOk && v.consumptionOk && v.identityOk;
    return v;
}

} // namespace render
