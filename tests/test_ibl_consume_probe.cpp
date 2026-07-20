// =============================================================================
// tests/test_ibl_consume_probe.cpp - the IBL consumption probe's verdict logic
// =============================================================================
// The device-free half of the evidence that basic_ps.hlsl consumes the
// environment cube. The other half is the [SMOKE] ibl_consume / ibl_consume_control
// pair, which supplies a real 64-byte block written by a real pixel shader.
// Neither covers the other:
//
//   these cases    prove the REDUCTION cannot report "ok" for a block that says
//                  the feature is absent, and cannot report "ok" for a block
//                  nothing wrote. No GPU required.
//   the GPU pair   proves the block is written from the consumption site by the
//                  shipped shader, which no CPU test can see.
//
// EVERY CASE HERE IS A CLAIM ABOUT THE VERDICT, not a golden number. The one
// thing they exist to stop is the failure this probe was built to fix: a
// verdict function that returns "ok" when the feature is not there.
// =============================================================================

#include "test_framework.h"

#include "render/ibl_consume_probe.h"

namespace
{

// A block shaped like a healthy live frame. Values are the ones MEASURED in the
// raster smoke run, quantised the way the shader quantises them, so the fixture
// is the real operating point rather than a set of round numbers that happen to
// clear every threshold.
render::IBLConsumeProbeBlock HealthyLiveBlock()
{
    auto q = [](float v) {
        return static_cast<uint32_t>(v * render::kIBLProbeQuantScale);
    };
    render::IBLConsumeProbeBlock b;
    b.envSpecularMaxQ     = q(0.343292f);
    b.envDiffuseMaxQ      = q(0.287125f);
    b.envInFinalMaxQ      = q(0.343384f);
    b.radianceMaxQ        = q(0.420258f);
    b.mirrorLuminanceMaxQ = q(0.421326f);
    b.skyRelErrMaxQ       = q(0.000977f);
    b.shadedPixels        = 1491649;
    b.cubeSamples         = 1491649;
    // MEASURED 0, but the fixture uses a nonzero value on purpose: the verdict
    // requires only that SOME pixel received environment light, and pinning the
    // fixture to the scene's current 0 would turn a legitimate future change
    // (one black-albedo surface) into a test failure.
    b.envZeroPixels       = 12000;
    return b;
}

// A block shaped like the negative control: the branch was never taken, so every
// word the branch writes is untouched and every pixel recorded no environment.
render::IBLConsumeProbeBlock HealthyControlBlock()
{
    render::IBLConsumeProbeBlock b;
    b.shadedPixels  = 1491519;
    b.cubeSamples   = 0;
    b.envZeroPixels = 1491519;
    return b;
}

} // namespace

// =============================================================================
// The healthy operating point passes on the side it belongs to
// =============================================================================
// A baseline, and the case that would notice a threshold tightened past the
// measurement. If this fails after a tolerance edit, the tolerance is wrong, not
// the engine.
TEST_CASE(IBLConsume_MeasuredLiveBlockPasses)
{
    const auto v = render::ReduceIBLConsumeProbe(HealthyLiveBlock(), true);
    CHECK(v.reachedOk);
    CHECK(v.consumptionOk);
    CHECK(v.identityOk);
    CHECK(v.ok);
}

TEST_CASE(IBLConsume_MeasuredControlBlockPasses)
{
    const auto v = render::ReduceIBLConsumeProbe(HealthyControlBlock(), false);
    CHECK(v.reachedOk);
    CHECK(v.consumptionOk);
    CHECK(v.identityOk);
    CHECK(v.ok);
}

// =============================================================================
// THE CASE THIS FILE EXISTS FOR: the two claim sets are not interchangeable
// =============================================================================
// The whole point of running a control frame is that the live claim set REJECTS
// what the control frame produces. If it did not - if a reduction that saw an
// all-zero block still said "ok" - the live assertion would be exactly the kind
// of probe that passes with the feature absent, which is what this probe was
// built to replace.
//
// Both directions, because a reduction that ignored `iblExpectedActive` and
// always applied one claim set would pass one of them.
TEST_CASE(IBLConsume_TheTwoClaimSetsRejectEachOthersBlocks)
{
    // The control frame's block, judged as if it were live: the feature is
    // absent and the live claim set must say so.
    const auto controlAsLive = render::ReduceIBLConsumeProbe(HealthyControlBlock(), true);
    CHECK(!controlAsLive.ok);
    CHECK(!controlAsLive.consumptionOk);
    CHECK(!controlAsLive.identityOk);
    CHECK(!controlAsLive.reachedOk);   // cubeSamples 0 against 1.49M shaded pixels

    // The live frame's block, judged as if it were the control: the feature is
    // present and the control claim set must say so.
    const auto liveAsControl = render::ReduceIBLConsumeProbe(HealthyLiveBlock(), false);
    CHECK(!liveAsControl.ok);
    CHECK(!liveAsControl.consumptionOk);
    CHECK(!liveAsControl.identityOk);
    CHECK(!liveAsControl.reachedOk);
}

// =============================================================================
// A block nobody wrote is not a pass, on EITHER side
// =============================================================================
// The vacuity guard, and it is the half that is easy to get wrong on the control
// side: every "is zero" claim the control makes is satisfied perfectly by a
// frame that shaded nothing at all. Without the shadedPixels floor, a control
// frame that failed to render would report success and the live assertion beside
// it would inherit a credibility it had not earned.
TEST_CASE(IBLConsume_AnUnwrittenBlockFailsBothClaimSets)
{
    const render::IBLConsumeProbeBlock empty;

    const auto asLive = render::ReduceIBLConsumeProbe(empty, true);
    CHECK(!asLive.ok);
    CHECK(!asLive.reachedOk);

    const auto asControl = render::ReduceIBLConsumeProbe(empty, false);
    CHECK(!asControl.ok);
    CHECK(!asControl.reachedOk);
}

// =============================================================================
// The three consumption words are independent claims
// =============================================================================
// Zeroing any ONE of them must fail, because each catches a different edit to
// basic_ps.hlsl:
//   envSpecular  the split-sum term, which is the half that rides the cube
//   envDiffuse   the SH term, which rides CBPerFrame and no descriptor at all
//   envInFinal   the environment as it landed in finalColor, which is the only
//                one that sees the terms dropped from the combine expression
//                while the variables keep their values
//
// A reduction that summed them, or that checked only the total, would pass with
// two of the three at zero - and a null descriptor at t0/space6 is exactly the
// case where the specular half is zero and the diffuse half is not.
TEST_CASE(IBLConsume_EachEnvironmentTermIsCheckedSeparately)
{
    {
        auto b = HealthyLiveBlock();
        b.envSpecularMaxQ = 0;
        CHECK(!render::ReduceIBLConsumeProbe(b, true).consumptionOk);
    }
    {
        auto b = HealthyLiveBlock();
        b.envDiffuseMaxQ = 0;
        CHECK(!render::ReduceIBLConsumeProbe(b, true).consumptionOk);
    }
    {
        auto b = HealthyLiveBlock();
        b.envInFinalMaxQ = 0;
        CHECK(!render::ReduceIBLConsumeProbe(b, true).consumptionOk);
    }
}

// =============================================================================
// The identity claim cannot be satisfied by a resource that reads zero
// =============================================================================
// |mirror - sky| / |sky| is small when both are the value they should be, and it
// is ALSO small when the comparison never happened. The two luminance floors
// beside it are what separate those, and this case pins that they are load
// bearing rather than decoration: a block with a perfect relative error but a
// zero mirror luminance is a block whose fetch returned nothing.
TEST_CASE(IBLConsume_IdentityNeedsANonZeroFetchNotJustASmallError)
{
    auto b = HealthyLiveBlock();
    b.skyRelErrMaxQ       = 0;   // "perfect" agreement
    b.mirrorLuminanceMaxQ = 0;   // ...between two quantities that are both zero
    CHECK(!render::ReduceIBLConsumeProbe(b, true).identityOk);

    auto c = HealthyLiveBlock();
    c.radianceMaxQ = 0;          // the SHADING fetch returned nothing
    CHECK(!render::ReduceIBLConsumeProbe(c, true).identityOk);
}

// =============================================================================
// A wrong descriptor at t0/space6 fails the identity claim
// =============================================================================
// The numbers here are what the shader would actually write for the two mistakes
// that are physically available in this engine's one bindable heap. Slot 0 is a
// null SRV, which reads (0,0,0) and gives a relative error of exactly 1 against
// a sky that is nowhere zero. Slot 1 is the shadow map, whose depths sit near 1
// against a sky luminance near 0.4, which is off by more than a factor of two.
//
// Both are orders of magnitude outside kIBLConsumeSkyTolerance, which is the
// point: there is no resource in that heap that could sit inside 2% of the
// prefiltered sky by accident, so this bound does not need to be tight to have
// teeth.
TEST_CASE(IBLConsume_AWrongResourceFailsTheIdentityClaim)
{
    auto q = [](float v) {
        return static_cast<uint32_t>(v * render::kIBLProbeQuantScale);
    };

    // The null SRV: every fetch reads zero.
    auto nullSrv = HealthyLiveBlock();
    nullSrv.skyRelErrMaxQ       = q(1.0f);
    nullSrv.mirrorLuminanceMaxQ = 0;
    nullSrv.radianceMaxQ        = 0;
    nullSrv.envSpecularMaxQ     = 0;   // no radiance, no split-sum specular
    const auto vNull = render::ReduceIBLConsumeProbe(nullSrv, true);
    CHECK(!vNull.identityOk);
    CHECK(!vNull.consumptionOk);
    CHECK(!vNull.ok);

    // Something else entirely, reading plausible but wrong values.
    auto wrong = HealthyLiveBlock();
    wrong.skyRelErrMaxQ = q(1.4f);
    const auto vWrong = render::ReduceIBLConsumeProbe(wrong, true);
    CHECK(!vWrong.identityOk);
    CHECK(!vWrong.ok);
    // ...and the consumption half stays perfectly green, which is exactly why
    // identity is a separate claim: a wrong cube still produces a nonzero
    // specular term and nothing about the energy reaching the image looks off.
    CHECK(vWrong.consumptionOk);
}

// =============================================================================
// Every shaded pixel must have taken the branch, not merely some of them
// =============================================================================
// cubeSamples == shadedPixels rather than cubeSamples > 0. The IBL branch is
// predicated on a frame-constant, so on a live frame the two counts are equal by
// construction; any inequality means some pixels shaded without the environment.
// A "> 0" form would pass with a single pixel sampling the cube and the rest of
// the image unlit by it, while the maxima - which are maxima - stayed high.
TEST_CASE(IBLConsume_PartialBranchCoverageFails)
{
    auto b = HealthyLiveBlock();
    b.cubeSamples = b.shadedPixels - 1;
    CHECK(!render::ReduceIBLConsumeProbe(b, true).reachedOk);
    CHECK(!render::ReduceIBLConsumeProbe(b, true).ok);
}

// =============================================================================
// A live frame where NO pixel received environment light fails
// =============================================================================
// envZeroPixels < shadedPixels. The counts can legitimately disagree - a fully
// metallic surface has no diffuse lobe and a black-albedo one contributes almost
// nothing - so the claim is "some pixel got environment light", not "every pixel
// did. Equality means the branch ran and produced nothing anywhere, which is the
// corridor going black with every other marker green.
TEST_CASE(IBLConsume_ALiveFrameWhereNoPixelGotEnvironmentLightFails)
{
    auto b = HealthyLiveBlock();
    b.envZeroPixels = b.shadedPixels;
    CHECK(!render::ReduceIBLConsumeProbe(b, true).reachedOk);
}
