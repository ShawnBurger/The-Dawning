param(
    [double]$Seconds = 4.0,
    [double]$RTDelaySeconds = 0.25,
    [switch]$RasterOnly,
    [switch]$FullQuality,
    [switch]$ResizeStress,
    [switch]$ForceGrow,
    [switch]$Unlocked,
    [switch]$GPUValidation,
    [int]$TimeoutSeconds = 15,
    [string]$Config = "Debug",
    [switch]$NoCapture
)

$ErrorActionPreference = "Stop"

$root   = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root "build\$Config"
$exe    = Join-Path $outDir "TheDawningV3.exe"
$log    = Join-Path $outDir "TheDawning.log"
$capture = Join-Path $outDir "smoke_capture.ppm"
$shaderInclude = Join-Path $outDir "shaders\display_common.hlsli"
$textureDir = Join-Path $outDir "assets\textures"

if (!(Test-Path -LiteralPath $exe)) {
    throw "Executable not found: $exe. Run .\SETUP_AND_BUILD.bat first (config '$Config')."
}
if (!(Test-Path -LiteralPath $shaderInclude)) {
    throw "Shader include not found in output: $shaderInclude. Rebuild before running smoke tests."
}

# Smoke captures must not depend on stale, untracked texture files left in a
# build directory. Remove only the known test-scene copies so App falls back to
# its deterministic procedural checker and normal textures for every run.
$smokeTextureNames = @(
    "ground_grid.ktx",
    "ground_grid.png",
    "ground_grid.dds",
    "blue_panels.ktx",
    "blue_panels.png",
    "blue_panels.dds",
    "ground_normal.ktx",
    "ground_normal.png",
    "ground_normal.dds",
    "cube_normal.ktx",
    "cube_normal.png",
    "cube_normal.dds"
)
foreach ($name in $smokeTextureNames) {
    $path = Join-Path $textureDir $name
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
    }
}

# Delete both artifacts up front so a stale file from an earlier run can never be
# validated in place of one this run failed to produce.
if (Test-Path -LiteralPath $log)     { Remove-Item -LiteralPath $log -Force }
if (Test-Path -LiteralPath $capture) { Remove-Item -LiteralPath $capture -Force }

$arguments = @("--smoke", "--smoke-seconds=$Seconds")
if (!$RasterOnly)  {
    $arguments += "--smoke-rt"
    $arguments += "--smoke-rt-delay=$RTDelaySeconds"
}
if ($FullQuality)  { $arguments += "--smoke-full" }
if ($ResizeStress) { $arguments += "--smoke-resize" }
if ($ForceGrow)    { $arguments += "--smoke-force-grow" }
if ($Unlocked)     { $arguments += "--smoke-unlocked" }
if ($GPUValidation){ $arguments += "--gpu-validation" }
if (!$NoCapture)   { $arguments += "--smoke-capture" }

$process = Start-Process -FilePath $exe `
    -WorkingDirectory $outDir `
    -ArgumentList $arguments `
    -PassThru

if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-Process -Id $process.Id -Force
    throw "Smoke test timed out after $TimeoutSeconds seconds."
}

if ($process.ExitCode -ne 0) {
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 80 }
    throw "Smoke test failed with exit code $($process.ExitCode)."
}

if (!(Test-Path -LiteralPath $log)) {
    throw "Smoke test did not create a log: $log"
}

$logText = Get-Content -LiteralPath $log -Raw

# core::Log writes a four-character column-aligned prefix "[ERR ]", never "[ERROR]".
$errors = Select-String -LiteralPath $log -Pattern "\[ERR\s*\]"
if ($errors) {
    $errors | ForEach-Object { $_.Line }
    throw "Smoke test log contains errors."
}

$warnings = Select-String -LiteralPath $log -Pattern "\[WARN\]"
if ($warnings) {
    Write-Host "Smoke test completed with warnings:"
    $warnings | ForEach-Object { Write-Host $_.Line }
}

# -----------------------------------------------------------------------------
# Structured markers
# -----------------------------------------------------------------------------
# Assertions match "[SMOKE] key=value" rather than human-readable log prose, so
# rewording a log line cannot silently disarm a check.
$markers = @{}
foreach ($m in (Select-String -LiteralPath $log -Pattern "\[SMOKE\]\s+(.+)$")) {
    foreach ($pair in ($m.Matches[0].Groups[1].Value -split "\s+")) {
        $kv = $pair -split "=", 2
        if ($kv.Count -eq 2) { $markers[$kv[0]] = $kv[1] }
    }
}

function Assert-Marker($key, $expected) {
    if (-not $markers.ContainsKey($key)) {
        throw "Smoke test did not emit the '$key' marker. Markers seen: $($markers.Keys -join ', ')"
    }
    if ($markers[$key] -ne $expected) {
        throw "Smoke marker '$key' was '$($markers[$key])', expected '$expected'."
    }
}

if ($logText -notmatch "Smoke mode complete") {
    throw "Smoke test log did not record completion."
}

Assert-Marker "overlay" "ok"
Assert-Marker "timeline" "fixed"
Assert-Marker "simulation_scheduler" "ok"
Assert-Marker "sim_tick" "1"
Assert-Marker "snapshot_roundtrip" "ok"
Assert-Marker "snapshot_bodies" "1"
Assert-Marker "present" $(if ($Unlocked) { "immediate" } else { "vsync" })
Assert-Marker "descriptor_reuse_before_fence" "blocked"
Assert-Marker "descriptor_reuse_after_fence" "reused"
Assert-Marker "descriptors_in_use_after_scene_shutdown" "0"
Assert-Marker "descriptors_pending_after_renderer_shutdown" "0"
# The shadow map reserves descriptor slot 1, immediately after the null SRV. If
# that reservation ever slips, a material texture lands on top of the shadow map
# and the failure looks like "shadows are wrong" rather than "descriptors are
# wrong" - so assert the slot, not just that shadows exist.
Assert-Marker "shadow_map" "ok"
Assert-Marker "shadow_map_slot" "1"

# Production content must arrive through the cooked format. These exact counts
# prevent a tiny placeholder from satisfying the mere-presence marker, while
# model_source=cooked fails if app.cpp silently falls back to source glTF.
Assert-Marker "model_loaded" "ok"
Assert-Marker "model_source" "cooked"
Assert-Marker "model_primitives" "1"
Assert-Marker "model_vertices" "15562"
Assert-Marker "model_indices" "57579"
Assert-Marker "model_images" "3"

# The shipped scene must be assembled from its runtime-content manifest, not
# from App-owned entity construction. The prepare marker proves that all 21
# typed assembly locators resolved through one immutable cooked-model owner;
# the commit marker proves the WS-025 transaction reached the live registry
# only after startup upload retirement. Exact topology counts keep a partial
# or placeholder assembly from satisfying the lifecycle check.
Assert-Marker "runtime_content_prepared" "ok"
Assert-Marker "scene" "ship.reference.runtime"
Assert-Marker "bindings" "21"
Assert-Marker "models" "1"
Assert-Marker "runtime_assembly_committed" "ok"
Assert-Marker "asset" "ship.reference.fighter"
Assert-Marker "modules" "3"
Assert-Marker "moving_parts" "2"
Assert-Marker "entities" "6"
Assert-Marker "interior_runtime_ready" "ok"
Assert-Marker "interactions" "3"
Assert-Marker "portals" "2"
Assert-Marker "interior_interaction" "ok"
Assert-Marker "interaction" "outer_hatch"
Assert-Marker "state" "open"
Assert-Marker "portal" "outer_entry"
Assert-Marker "traversable" "yes"
Assert-Marker "interior_collision_ready" "ok"
Assert-Marker "packages" "3"
Assert-Marker "boxes" "12"
Assert-Marker "frame" "assembly_local"
# Four cascades on ONE Texture2DArray. The slice count is asserted separately
# from the slot because they fail independently: the array can lose a slice
# without the descriptor moving, and vice versa. %u on the C++ side, never
# %.1f - this is a string compare, so "4.0" would not match "4".
Assert-Marker "shadow_cascades" "4"

# =============================================================================
# Environment IBL - docs/research/IBL_DESIGN.md section 11, Stages 1-3
# =============================================================================
# The prefiltered environment cubemap (Stage 1), the L2 diffuse SH projection
# (Stage 2), and the split-sum specular path (Stage 3). basic_ps.hlsl now
# consumes all of it: its hemisphere ambient is DELETED, not scaled down, so
# the raster capture statistics legitimately differ from the pre-IBL run.
#
# There is deliberately NO golden-value gate on those statistics - see the long
# note by the capture block below for why one was deleted rather than
# recalibrated. Every assertion here is either CPU-side over GPU-free code or a
# GPU-side probe reading back what the shipped shaders computed, and none of it
# depends on which assets happen to sit in build/<Config>.
#
# Every one of these has been watched failing.
#
# BOTH MODES, unconditionally. The prefilter runs once at startup, before the
# raster/path-tracing branch exists, so unlike the shadow and draw probes there
# is no frame to arm it on and no mode that can skip it. That is deliberate: two
# probes in this repo were previously fixed for running only on a path-traced
# final frame or only under -RasterOnly, and the cheapest way not to repeat that
# is to gate this on nothing at all.
#
# Assert-Marker throws on a MISSING key as well as a wrong one, which is the
# point of listing them here rather than relying on the engine logging [ERR ].
# If EnsureEnvironmentIBL stopped being called, the engine would log no error
# and the run would go green; these lines are what make that a failure.
Assert-Marker "ibl_env" "ok"
# 1.1 - the reservation. The cube takes slot 2, immediately after the shadow map,
# and the allocator's firstIndex moved 2 -> 3 to keep it. If that slips, a
# material texture lands on top of the cube (or the cube on top of the shadow
# map) and the failure reads as "reflections are wrong", not "descriptors are
# wrong". shadow_map_slot=1 above is the other half of the same claim: both must
# hold, and asserting only one lets the pair slide together.
Assert-Marker "ibl_env_slot" "2"
Assert-Marker "ibl_env_size" "128"
Assert-Marker "ibl_env_mips" "8"
# The half of 1.1 that actually bites in Stage 1. NOTHING SAMPLES THE CUBE YET,
# so if the allocator's firstIndex slipped back to 2 a material texture would
# overwrite the cube's SRV and no runtime check would see it - the damage would
# surface two stages later as "reflections are wrong". This is the allocator's
# own live FirstIndex(), not the constant it was derived from, so reverting the
# reservation fails here rather than in Stage 3.
Assert-Marker "ibl_env_first_material_slot" "3"
# 1.2 - the direction round trip. Catches the cube face table in
# shaders/ibl_environment.hlsli being wrong in ANY way: a permutation, a sign
# flip, a v-flip, or a "handedness fix" applied for RULE 7 that should not have
# been. The table was written from the D3D spec and never verified against a run,
# so this is a round trip rather than a comparison against the table - it is
# correct even if the table is a permutation of the truth, and it fails if the
# implementation is internally inconsistent.
Assert-Marker "ibl_direction_roundtrip" "pass"
# 1.3 - the sky agreement probe, and the reason Stage 2 is allowed to put
# spherical harmonics on the CPU at all. src/core/sky_radiance.h says of its own
# hash tripwire that it "pins agreement in TIME, not in VALUE" and names this
# probe as the thing that closes the gap. It is built now: the shipped HLSL
# evaluates 64 directions on the GPU and the CPU compares against
# core::SkyRadiance.
Assert-Marker "ibl_sky_agreement" "pass"
# Vacuity guards for 1.2 and 1.3, and they are NOT redundant with the verdicts
# above. The probe target is cleared to a -1 poison and the shaders write w=+1,
# so these counts say the draws actually ran. A comparison neither side reached
# is not a comparison - the poison-sentinel lesson, applied before it was needed
# rather than after.
Assert-Marker "ibl_direction_slots" "64"
Assert-Marker "ibl_sky_slots" "64"
# The other half of 1.2's vacuity guard, and NOT implied by the slot count. A
# direction set that degenerated to 64 copies of one direction would write all 64
# slots and round-trip perfectly while exercising a single cube face - the face
# table could then be wrong on the other five and nothing here would notice.
# "Covering all six faces and both signs of every axis" is the design's own
# wording for this assertion; this is that clause asserted rather than assumed.
Assert-Marker "ibl_probe_faces" "6"
# 1.4 - per-mip mean luminance within 5% of mip 0. Catches an UNNORMALISED
# prefilter: dropping the "/ sum(NdotL)" makes every rough surface in the engine
# systematically wrong in a way that looks like an art choice rather than a bug.
Assert-Marker "ibl_mip_energy" "pass"
# 1.5 - variance falls as roughness rises, with a variance[0] > 1e-6 floor. The
# floor is the load-bearing half: an all-zero cube satisfies "decreasing"
# trivially, so without it this assertion passes on a cube that was never
# rendered at all.
Assert-Marker "ibl_mip_variance_decreasing" "yes"

# --- Stage 2 ---------------------------------------------------------------
# The SH BASIS mirror. core::SHBasisL2 (which produced the coefficients) and
# DawningSHBasisL2 in shaders/ibl_common.hlsli (which evaluates them) are two
# hand-written copies of nine expressions in two files and two languages. A sign
# flip or permutation applied to ONE of them does not cancel and lights every
# surface in the scene from the wrong direction, smoothly and plausibly.
#
# No CPU test can see this - the HLSL is not linked into TheDawningTests. This
# probe evaluates the SHIPPED HLSL on the GPU against the SHIPPED coefficients
# and compares to core::EvaluateIrradiance. It is the SH analogue of
# ibl_sky_agreement, and it exists for the same reason: a mirror nobody watches
# is a convention, not a guarantee.
Assert-Marker "ibl_sh_agreement" "pass"
Assert-Marker "ibl_sh_slots" "64"
# The kill switch is ON and the coefficients are not degenerate. DELIBERATELY
# WEAK, and listed as such: it says what BeginFrame will upload, not that
# basic_ps.hlsl reads it. Nothing in this harness witnesses that call site - see
# the note at the top of shaders/ibl_eval_probe_ps.hlsl.
Assert-Marker "ibl_enabled" "yes"

# --- Stage 3 ---------------------------------------------------------------
# 3.3 - mirror agreement. At roughness 0 with N = V = d the split-sum reduces to
# a mip-0 fetch times the env-BRDF scalar, and the shader writes that scalar in
# the alpha channel so the CPU can divide it out and compare the remaining
# radiance against core::SkyRadiance WITHOUT re-implementing the Lazarov fit.
# WATCHED: reflect(V, N) in place of reflect(-V, N) takes the worst relative
# error from 0.0010 to 1.03. WATCHED NOT FAILING, and disclosed rather than
# quietly dropped: switching the cube sampler to WRAP leaves this green, because
# cube sampling never consults the 2-D address modes - the design claims this
# assertion covers that and it does not.
Assert-Marker "ibl_spec_mirror" "pass"
Assert-Marker "ibl_spec_mirror_slots" "64"

# 3.1 - the env-BRDF fit's physical bounds. Restated from the design's furnace
# formulation, which is not satisfiable: at roughness 1 with F0 = 1 the
# single-scattering split-sum returns 0.45, a 55% energy loss that the design's
# own section 9.4 names and deliberately leaves uncorrected, so "within 10% of
# the cube mean" would have failed for an accepted reason.
#
# What is asserted instead are properties of the physics the fit approximates:
# a smooth dielectric reflects F0 at normal incidence and approaches unity at
# grazing, and a perfect mirror never reflects MORE than it receives at any
# roughness. The first two are what give an A/B swap teeth - at F0 = 1 the
# expression F0*A + B is symmetric in A and B and the swap is invisible, which
# the design's stated negative test does not account for.
Assert-Marker "ibl_env_brdf" "pass"
Assert-Marker "ibl_env_brdf_slots" "64"
# Vacuity guard: the two smooth-surface claims each look at ONE grid point, so a
# grid that stopped reaching (roughness 0, NdotV 0) and (roughness 0, NdotV 1)
# would satisfy them by never evaluating them.
Assert-Marker "ibl_env_brdf_smooth_samples" "2"

# The DIELECTRIC energy bound, and it is a separate assertion because the F0 = 1
# reading beside it is structurally blind to this case: it reads A + B, while a
# dielectric's reflectance is 0.04*A + B. MEASURED 1.042432 - a smooth dielectric
# at grazing incidence really does return 4.2% more environment energy than
# arrives, which is the documented accuracy of the Lazarov fit at the corner of
# the domain where it is worst, not a defect. The old guard reported "no energy
# creation" with that number sitting unexamined in the same table; the bound is
# now stated at the value the fit actually reaches rather than at the value the
# physics would.
if ([double]$markers["ibl_env_brdf_max_dielectric"] -ge 1.06) {
    throw ("ibl_env_brdf_max_dielectric=$($markers['ibl_env_brdf_max_dielectric']) exceeds the " +
           "bound the Lazarov fit is allowed to stray to (1.06, measured 1.042432). An A/B swap " +
           "sends this to 1.29, and so does re-expressing the fit with brdf_common.hlsli's " +
           "direct-lighting Smith k - see kEnvBRDFDielectricEnergyCeiling.")
}

# 3.2 - roughness -> mip. The prefiltered fetch along +Y must rise monotonically
# with roughness. WATCHED: inverting the mapping to (1 - roughness) * (mips - 1)
# fails this and the mirror assertion together.
#
# AN OFF-BY-ONE IS CAUGHT, HERE, AND THIS LINE PREVIOUSLY SAID IT WAS NOT.
#
# The claim that stood here - "WATCHED NOT FAILING: adding 1.0 to the mip leaves
# both this and the mirror assertion green" - is FALSE, and it was committed to
# the tree where the next reader would trust it. RE-MEASURED by performing the
# mutation: `mip = saturate(roughness) * (mipCount - 1) + 1.0` in
# shaders/ibl_common.hlsli fails ibl_spec_mip_monotonic with worst backward step
# -0.00098392 against a tolerance of -1e-5, DETERMINISTICALLY, with identical
# numbers in both smoke modes (it is a startup probe, so the mode cannot matter).
#
# HALF the original claim is true and is kept: ibl_spec_mirror really does stay
# green, moving only from 0.001768 to 0.001768 - the mirror comparison cannot see
# it. What was wrong was extending that to this assertion.
#
# WHY it is caught, so the next reader can tell this from luck. The sweep is
# asserted over slots 0..54 because the correct mapping puts the 2x2 and 1x1
# faces' known luminance reversal above that (see kEnvMipSweepLastSlot). Adding 1
# shifts the whole sweep up a mip, which drags mip 7's reversal down INTO the
# asserted window: slot 54 is roughness 6/7, which the mutation sends to mip 7.0
# where the true mapping sends it to mip 6. The exclusion range and the mutation
# detection are therefore the same mechanism, not two independent facts.
#
# A false claim committed to source is trusted by the next reader, and this tree
# has done it before - a merge deleted a -ForceGrow switch and the follow-up
# commit documented the corpse it had just created as pre-existing rot. Measure
# before writing "watched not failing".
Assert-Marker "ibl_spec_mip_monotonic" "pass"
Assert-Marker "ibl_spec_mip_slots" "64"

# -----------------------------------------------------------------------------
# CONSUMPTION. Everything above this line witnesses shaders/ibl_common.hlsli.
# -----------------------------------------------------------------------------
# Every assertion above passes with the IBL block DELETED from basic_ps.hlsl and
# passes with the WRONG DESCRIPTOR bound at t0/space6. That was disclosed in the
# probe shader's own header and it is the same failure this repo hit with the
# shadow-map probe, the draw-record probe and Stage 1's direction probe: a check
# that exists, reads as coverage, and cannot fail for the thing it names.
#
# The block below is the fix. basic_ps writes what it ACTUALLY LOADED and what it
# ACTUALLY ADDED into a UAV at u1/space4, on the same probe frame and through the
# same PSO permutation as the draw-record probe, and the CPU reduces it. See
# src/render/ibl_consume_probe.h.
#
# AND IT RUNS TWICE, which is the part that makes it worth anything. An assertion
# that passes with the feature present is what every check above already did. So
# the harness demands BOTH sides:
#
#   the control frame  renders with CBPerFrame::iblParams.z forced to 0 and must
#                      report every word ZERO with pixels still being shaded
#   the live frame     must report every word nonzero and the cube agreeing with
#                      the sky
#
# Neither verdict is meaningful alone. Assert both or assert nothing.

if (-not $markers.ContainsKey("ibl_consume_frame")) {
    throw ("Smoke test did not emit 'ibl_consume_frame'. The IBL consumption probe " +
           "was never armed. It must run on the last raster frame in BOTH smoke " +
           "modes - see smokeRasterVerifyFrame in App::Run. A missing marker here " +
           "means the only evidence that basic_ps.hlsl samples the environment cube " +
           "at all has gone away.")
}
if (-not $markers.ContainsKey("ibl_consume_control_frame")) {
    throw ("Smoke test did not emit 'ibl_consume_control_frame'. The IBL consumption " +
           "probe's NEGATIVE CONTROL was never armed, so the live verdict beside it " +
           "proves nothing: it has not been shown to fail with the feature absent.")
}
Write-Host ("IBL consumption frames: control $($markers['ibl_consume_control_frame']), " +
            "live $($markers['ibl_consume_frame'])")
if ([uint64]$markers["ibl_consume_control_frame"] -ge [uint64]$markers["ibl_consume_frame"]) {
    throw ("ibl_consume_control_frame=$($markers['ibl_consume_control_frame']) must come " +
           "BEFORE ibl_consume_frame=$($markers['ibl_consume_frame']). The control has to " +
           "be the frame that ran with the environment switched off, and the pair only " +
           "means anything in that order.")
}
if (-not $RasterOnly -and [uint64]$markers["ibl_consume_frame"] -ge [uint64]$markers["frames"]) {
    throw ("ibl_consume_frame=$($markers['ibl_consume_frame']) is not before the end of " +
           "the run ($($markers['frames']) frames). In the default mode this probe must " +
           "land on a raster frame - basic_ps does not run on a path-traced one.")
}

# --- the live frame --------------------------------------------------------
# WATCHED FAILING, all three, each broken -> failing -> restored -> green:
#   (a) force envSpecular and envDiffuse to zero at the point of use in
#       basic_ps.hlsl  -> ibl_consume_consumption=failed
#   (b) bind the shadow map's descriptor (heap slot 1) as the environment cube
#       -> ibl_consume_identity=failed, sky_rel_err 0.000977 -> 1.000000
#   (c) sample the cube on only part of the screen -> ibl_consume_reached=failed
#       on cube_samples 65750 against shaded_pixels 1491649, while consumption
#       AND identity both stay ok. A `cube_samples > 0` form passes here
# Two more, measured because the comments above claim they are distinct:
#   (d) neuter Renderer::SetIBLDisabledForFrame -> the CONTROL fails and the
#       live verdict stays ok, which is the pair doing exactly its job
#   (e) delete the environment from the combine line while leaving the variables
#       alone -> spec 0.343292 and diffuse 0.287125 stay EXACTLY green and only
#       in_final_max goes to 0. This is why the third word is recovered from
#       finalColor rather than summed from the two variables
Assert-Marker "ibl_consume" "ok"
Assert-Marker "ibl_consume_reached" "ok"
Assert-Marker "ibl_consume_consumption" "ok"
Assert-Marker "ibl_consume_identity" "ok"
# The two specular-fidelity fixes added in this stage (IBL_DESIGN 9.3, 10).
# WATCHED FAILING, each broken -> failing -> restored -> green:
#   (f) neuter specularOcclusion back to `envSpecular *= ambientOcclusion` in
#       basic_ps.hlsl -> spec_occ_above_ao 0.192459 -> 0, ibl_consume_occlusion
#       fails, while consumption/identity STAY ok (the applied specular occlusion
#       equals AO, so the shipped image reverts and the witness collapses)
#   (g) delete the DawningToksvigRoughness fold in basic_ps.hlsl -> toksvig_rough
#       _inc 0.791718 -> 0, ibl_consume_occlusion fails, others stay ok
Assert-Marker "ibl_consume_occlusion" "ok"
# Not redundant with the verdict: these are the numbers it was computed FROM, so a
# reduction that lost a clause reads absurd beside a verdict that reads ok. Both
# are MAX over the live frame of a quantity that is exactly 0 when its fix is
# absent, so a small positive value is the fix being present on some pixel.
if ([double]$markers["ibl_consume_spec_occ_above_ao"] -le 0.0) {
    throw ("ibl_consume_spec_occ_above_ao=$($markers['ibl_consume_spec_occ_above_ao']): the " +
           "specular-occlusion remap never departed upward from the diffuse AO on any pixel, " +
           "so envSpecular is still being multiplied by raw AO. The fix is absent.")
}
if ([double]$markers["ibl_consume_toksvig_rough_inc"] -le 0.0) {
    throw ("ibl_consume_toksvig_rough_inc=$($markers['ibl_consume_toksvig_rough_inc']): the " +
           "shading roughness never rose above its pre-Toksvig value on any pixel, so the " +
           "normal-map variance is still being discarded. The fix is absent.")
}
Write-Host ("IBL specular fidelity: spec-occ-above-ao=$($markers['ibl_consume_spec_occ_above_ao']) " +
            "toksvig-rough-inc=$($markers['ibl_consume_toksvig_rough_inc'])")

# Not redundant with the verdict above: the verdict is computed in C++ and these
# are the numbers it was computed FROM, so a reduction that lost a clause is
# visible here as a marker that reads absurd beside a verdict that reads ok.
# Every shaded pixel must have taken the IBL branch - the branch is predicated on
# a frame constant, so any inequality means pixels shading without the
# environment.
if ([uint64]$markers["ibl_consume_shaded_pixels"] -eq 0) {
    throw "ibl_consume_shaded_pixels=0: the probe frame shaded nothing, so every IBL claim on it is vacuous."
}
if ($markers["ibl_consume_cube_samples"] -ne $markers["ibl_consume_shaded_pixels"]) {
    throw ("ibl_consume_cube_samples=$($markers['ibl_consume_cube_samples']) against " +
           "ibl_consume_shaded_pixels=$($markers['ibl_consume_shaded_pixels']). Every pixel " +
           "basic_ps shades must have fetched the environment cube; the branch is " +
           "predicated on a frame constant, so an inequality means some pixels are being " +
           "shaded with no environment term at all.")
}
Write-Host ("IBL consumption: spec=$($markers['ibl_consume_spec_max']) " +
            "diffuse=$($markers['ibl_consume_diffuse_max']) " +
            "in-final=$($markers['ibl_consume_in_final_max']) " +
            "sky-rel-err=$($markers['ibl_consume_sky_rel_err'])")

# --- the negative control --------------------------------------------------
# The half that makes the half above mean something.
Assert-Marker "ibl_consume_control" "ok"
Assert-Marker "ibl_consume_control_reached" "ok"
Assert-Marker "ibl_consume_control_consumption" "ok"
Assert-Marker "ibl_consume_control_identity" "ok"
# The specular-fidelity words are written only inside the cube-sampled branch,
# which the control frame does not take, so they must read exactly zero here. That
# is what proves the live values above are the fixes running and not a constant.
Assert-Marker "ibl_consume_control_occlusion" "ok"
Assert-Marker "ibl_consume_control_spec_occ_above_ao" "0.000000"
Assert-Marker "ibl_consume_control_toksvig_rough_inc" "0.000000"
Assert-Marker "ibl_consume_control_cube_samples" "0"
if ([uint64]$markers["ibl_consume_control_shaded_pixels"] -eq 0) {
    throw ("ibl_consume_control_shaded_pixels=0: the control frame rendered nothing, so " +
           "'every word reads zero' is satisfied by an empty frame rather than by the " +
           "environment being switched off. The control proves nothing in that state.")
}

# -----------------------------------------------------------------------------
# THE DXR IBL CONSUMPTION PROBE — IBL Stage 4
# -----------------------------------------------------------------------------
# The block above is evidence about basic_ps.hlsl and NO evidence about the DXR
# path. Until Stage 4 the merged Stage 3 work said so in as many words: "DXR
# still has no consumption evidence."
#
# What Stage 4 changed is that the DXR STABLE PREVIEW now evaluates the same
# shaders/ibl_common.hlsli the raster path does, in place of an ad-hoc fill with
# a magic 2.5 diffuse multiplier, a magic 0.25 damper on the bounce, a mirror
# reflection that ignored roughness entirely and a gloss ramp corresponding to no
# physical quantity. F1 no longer changes the lighting model.
#
# This block is the evidence for that claim, and it is the SAME KIND the raster
# side uses, reduced by the SAME ReduceIBLConsumeProbe: path_trace.hlsl writes
# what it ACTUALLY LOADED and ACTUALLY ADDED into a root UAV at u0/space4, at
# bounce 0 where throughput and the damper are both exactly 1, so the numbers are
# directly comparable to the raster probe's above.
#
# THE DEFAULT RT MODE ONLY, and both exclusions are asserted rather than assumed.
#
#   -RasterOnly   no dispatch exists to probe
#   -FullQuality  the stable-preview branch does not execute, so there is no
#                 environment evaluation to witness. The full path tracer
#                 deliberately samples DawningSkyRadiance on every miss instead -
#                 it evaluates the integral the split-sum approximates, and
#                 substituting the approximation for the reference is the change
#                 Stage 4 explicitly does NOT make
#
# In both of those the markers must be ABSENT. Asserting the absence is what stops
# a future edit from arming the probe on a mode it cannot observe and reading the
# resulting empty block as a pass.
if ($RasterOnly -or $FullQuality) {
    foreach ($absent in @("rt_ibl_consume_frame", "rt_ibl_consume_control_frame")) {
        if ($markers.ContainsKey($absent)) {
            throw ("Marker '$absent' is present in a mode that cannot observe the DXR " +
                   "stable preview's environment block. Under -RasterOnly there is no " +
                   "dispatch; under -FullQuality the branch does not execute. A probe " +
                   "armed here would report a verdict about a feature that is correctly " +
                   "absent.")
        }
    }
    Write-Host "DXR IBL consumption probe: correctly not armed in this mode."
} else {
    if (-not $markers.ContainsKey("rt_ibl_consume_frame")) {
        throw ("Smoke test did not emit 'rt_ibl_consume_frame'. The DXR IBL consumption " +
               "probe was never armed. It must run on a PATH-TRACED frame - see the " +
               "smokeRTStartFrame arming block in App::Run. A missing marker here means " +
               "the only evidence that path_trace.hlsl's stable preview samples the " +
               "environment cube at all has gone away.")
    }
    if (-not $markers.ContainsKey("rt_ibl_consume_control_frame")) {
        throw ("Smoke test did not emit 'rt_ibl_consume_control_frame'. The DXR probe's " +
               "NEGATIVE CONTROL was never armed, so the live verdict beside it proves " +
               "nothing: it has not been shown to fail with the feature absent.")
    }
    Write-Host ("DXR IBL consumption frames: control $($markers['rt_ibl_consume_control_frame']), " +
                "live $($markers['rt_ibl_consume_frame'])")
    if ([uint64]$markers["rt_ibl_consume_control_frame"] -ge [uint64]$markers["rt_ibl_consume_frame"]) {
        throw ("rt_ibl_consume_control_frame=$($markers['rt_ibl_consume_control_frame']) must " +
               "come BEFORE rt_ibl_consume_frame=$($markers['rt_ibl_consume_frame']).")
    }

    # --- the live dispatch -------------------------------------------------
    # WATCHED FAILING, each broken -> watched failing -> restored -> green:
    #   (a) make the stable preview stop sampling the environment (force the
    #       `if (g_IblParams.z != 0)` branch off) -> rt_ibl_consume_consumption
    #       AND rt_ibl_consume_identity both fail, spec/diffuse/in-final
    #       0.322357/0.287125/0.322540 -> 0. Every startup marker and the whole
    #       RASTER consumption block stay green, which is the gap measured
    #       rather than argued
    #   (b) bind the wrong descriptor as the DXR environment cube (point the
    #       env-cube SRV at the RT display texture) -> rt_ibl_consume_identity
    #       fails, sky_rel_err 0.001007 -> 1.000000. Consumption stays ok,
    #       because a wrong cube still produces a nonzero specular term
    #   (c) neuter RTEnvironmentInputs::disabled -> the CONTROL fails and the
    #       live verdict stays green, which is the pair doing its job
    Assert-Marker "rt_ibl_consume" "ok"
    Assert-Marker "rt_ibl_consume_reached" "ok"
    Assert-Marker "rt_ibl_consume_consumption" "ok"
    Assert-Marker "rt_ibl_consume_identity" "ok"
    # The same two specular-fidelity fixes, witnessed in the DXR stable preview
    # from the exact scalars path_trace.hlsl applied. WATCHED FAILING, each broken
    # -> failing -> restored -> green:
    #   (h) neuter specularOcclusion back to `envSpecular *= ambientOcclusion` in
    #       the stable-preview block -> rt spec_occ_above_ao -> 0, occlusion fails
    #   (i) delete the DawningToksvigRoughness fold in path_trace's material setup
    #       -> rt toksvig_rough_inc -> 0, occlusion fails
    Assert-Marker "rt_ibl_consume_occlusion" "ok"
    if ([double]$markers["rt_ibl_consume_spec_occ_above_ao"] -le 0.0) {
        throw ("rt_ibl_consume_spec_occ_above_ao=$($markers['rt_ibl_consume_spec_occ_above_ao']): " +
               "the DXR stable preview never lifted specular occlusion above raw AO, so it is " +
               "still multiplying envSpecular by the diffuse AO. The fix is absent in DXR.")
    }
    if ([double]$markers["rt_ibl_consume_toksvig_rough_inc"] -le 0.0) {
        throw ("rt_ibl_consume_toksvig_rough_inc=$($markers['rt_ibl_consume_toksvig_rough_inc']): " +
               "the DXR shading roughness never rose above its pre-Toksvig value, so the normal " +
               "-map variance is still discarded. The fix is absent in DXR.")
    }
    Write-Host ("DXR IBL specular fidelity: spec-occ-above-ao=$($markers['rt_ibl_consume_spec_occ_above_ao']) " +
                "toksvig-rough-inc=$($markers['rt_ibl_consume_toksvig_rough_inc'])")

    if ([uint64]$markers["rt_ibl_consume_shaded_pixels"] -eq 0) {
        throw ("rt_ibl_consume_shaded_pixels=0: the probed dispatch shaded nothing, so " +
               "every DXR IBL claim on it is vacuous.")
    }
    # Same equality as the raster side and it holds for the same reason: the IBL
    # branch is predicated on a frame constant, and the probe writes only at
    # bounce 0, so every path vertex that reached the combine also fetched.
    if ($markers["rt_ibl_consume_cube_samples"] -ne $markers["rt_ibl_consume_shaded_pixels"]) {
        throw ("rt_ibl_consume_cube_samples=$($markers['rt_ibl_consume_cube_samples']) against " +
               "rt_ibl_consume_shaded_pixels=$($markers['rt_ibl_consume_shaded_pixels']). Every " +
               "primary path vertex the stable preview shades must have fetched the " +
               "environment cube.")
    }
    Write-Host ("DXR IBL consumption: spec=$($markers['rt_ibl_consume_spec_max']) " +
                "diffuse=$($markers['rt_ibl_consume_diffuse_max']) " +
                "in-final=$($markers['rt_ibl_consume_in_final_max']) " +
                "sky-rel-err=$($markers['rt_ibl_consume_sky_rel_err'])")

    # THE CONVERGENCE ASSERTION, and it is the one that is actually about Stage 4
    # rather than about the probe. The DIFFUSE term rides the constant buffer and
    # touches no descriptor: both paths evaluate DawningIrradianceSH against the
    # same nine coefficients from the same EnvironmentIBL, so the maxima must
    # agree to the fixed-point resolution. MEASURED: both read exactly 0.287125.
    #
    # This is what would catch the two paths being handed DIFFERENT SH - a second
    # projection, a stale upload, or the 12-byte HLSL row-rounding shear that
    # RTPerFrameConstants' pad0[3] exists to prevent. That shear would produce a
    # smooth, plausible, wrong ambient with no compile error on either side, and
    # nothing else in this harness would notice.
    #
    # This whole block is already stable-mode-only, so there is no second gate.
    #
    # NOT WRAPPED IN BRACES. A bare `{ ... }` in PowerShell is a script-block
    # LITERAL, not a scope: it is constructed, discarded, and never executed. It
    # was written that way here first and the assertion below silently did not run
    # - the same never-reached-assertion failure this file documents at length for
    # the shadow and draw probes, reproduced in the harness itself.
    $rasterDiffuse = [double]$markers["ibl_consume_diffuse_max"]
    $rtDiffuse     = [double]$markers["rt_ibl_consume_diffuse_max"]
    $diffuseDelta  = [math]::Abs($rasterDiffuse - $rtDiffuse)
    Write-Host ("IBL raster/DXR SH diffuse agreement: raster=$rasterDiffuse " +
                "dxr=$rtDiffuse delta=$([math]::Round($diffuseDelta,6))")
    # 1/65536 is the probe's quantisation step. Two paths evaluating the same
    # nine coefficients through the same header cannot differ by more than one.
    if ($diffuseDelta -gt 0.0001) {
        throw ("The raster and DXR paths disagree about SH diffuse irradiance: " +
               "raster=$rasterDiffuse dxr=$rtDiffuse. They evaluate the same " +
               "DawningIrradianceSH against coefficients from the same EnvironmentIBL, " +
               "so any difference means one of them is reading a different constant " +
               "buffer than it thinks - the RTPerFrameConstants row-rounding shear is " +
               "the first thing to check.")
    }

    # --- the negative control ----------------------------------------------
    Assert-Marker "rt_ibl_consume_control" "ok"
    Assert-Marker "rt_ibl_consume_control_reached" "ok"
    Assert-Marker "rt_ibl_consume_control_consumption" "ok"
    Assert-Marker "rt_ibl_consume_control_identity" "ok"
    Assert-Marker "rt_ibl_consume_control_occlusion" "ok"
    Assert-Marker "rt_ibl_consume_control_spec_occ_above_ao" "0.000000"
    Assert-Marker "rt_ibl_consume_control_toksvig_rough_inc" "0.000000"
    Assert-Marker "rt_ibl_consume_control_cube_samples" "0"
    if ([uint64]$markers["rt_ibl_consume_control_shaded_pixels"] -eq 0) {
        throw ("rt_ibl_consume_control_shaded_pixels=0: the control dispatch traced " +
               "nothing, so 'every word reads zero' is satisfied by an empty frame " +
               "rather than by the environment being switched off.")
    }
}
# BOTH MODES. This block used to be gated on -RasterOnly, "because the probe is
# skipped in path-tracing runs, which do not use the shadow map at all" - an
# accurate description of a hole, not a reason for one. The shadow probe rode the
# CAPTURE frame, the default mode path-traces the capture frame, so the default
# run - the one that gets run when nobody passes a switch - emitted not one of
# these markers and every assertion below was skipped. Everything here passed
# review and was never reached, which is the same class of gap as the draw probe's
# and as "a startup grow is not coverage".
#
# App::Run now arms the shadow probe on the last RASTER frame (see
# smokeRasterVerifyFrame), separately from the capture, so both modes carry it and
# the gate is gone. The capture frame is untouched: it still takes the default
# mode's final path-traced image.
#
# These are the assertions with teeth. shadow_map=ok only proves the resource was
# created, and deleting the shadow pass entirely leaves the map at its cleared
# value with every pixel reading fully lit, which nothing else here would notice.
# Verified by deleting the caster draw and watching this flip to "no".
if (-not $markers.ContainsKey("shadow_probe_frame")) {
    throw ("Smoke test did not emit 'shadow_probe_frame'. The shadow-map probe was " +
           "never armed. It must run on the last raster frame in BOTH smoke modes - " +
           "see smokeRasterVerifyFrame in App::Run. A missing marker here means the " +
           "probe went back to riding the capture frame, which is path-traced in the " +
           "default mode and renders no shadow pass at all.")
}
Write-Host "Shadow probe frame: $($markers['shadow_probe_frame'])"
if (-not $RasterOnly -and [uint64]$markers["shadow_probe_frame"] -ge [uint64]$markers["frames"]) {
    throw ("shadow_probe_frame=$($markers['shadow_probe_frame']) is not before the end " +
           "of the run ($($markers['frames']) frames). In the default mode the shadow " +
           "probe must land on a raster frame, which means BEFORE path tracing starts.")
}

Assert-Marker "shadow_map_written" "yes"

# The pass began every cascade. This is asserted SEPARATELY from the
# per-slice coverage below because coverage cannot see a skipped cascade:
# measured, not assumed - changing the app's loop bound to c < N-1 leaves
# shadow_cascade_written_3 reading "yes", since an uncleared, never-rendered
# depth slice holds arbitrary values under the 1.0 clear and is
# indistinguishable from real depth. This counter is the only check here that
# flips when a cascade is skipped.
Assert-Marker "shadow_cascades_rendered" "4"

# EVERY cascade holds depth, not just cascade 0. The index is in the KEY on
# purpose: markers are stored in a hashtable, so a shared "cascade_written"
# key would be overwritten by each loop iteration and collapse to the last
# cascade - passing while cascades 0..2 were empty.
foreach ($c in 0..3) { Assert-Marker "shadow_cascade_written_$c" "yes" }

# Each cascade stored a DIFFERENT depth for the scene. Cascade c's slab is
# 120/325/875/2350 units deep, so the same geometry must normalise to a
# different NDC depth in every slice - four equal values mean the slices did
# not each get their own matrix. Structural, not a tuned threshold.
#
# Verified by pinning DrawMeshShadow to m_lightViewProj[0]: this flips to
# "no" while every shadow_cascade_written_* marker stays "yes". Watched failing in
# the DEFAULT mode, not only under -RasterOnly - proving it in the mode that was
# already covered would prove nothing about the mode that was not.
#
# It does NOT catch a permutation of matrices across slices - all four stay
# distinct. Nothing here does; the CPU cases in tests/test_shadow_cascades.cpp
# are what constrain the matrices themselves.
#
# Deliberately NOT asserted: the coverage-fraction ordering. The probe window
# is 1/8 of each footprint, so even cascade 3's is 117 world units across and
# sits entirely on the 200x200 ground plane - all four fractions are 1.0. An
# assertion on them would pass unconditionally, which is worse than none.
Assert-Marker "shadow_cascade_depths_distinct" "yes"

# The live cascade fit, not a mirror of the constant table: strictly
# increasing texel size with consecutive ratios in (1, 8]. Catches the whole
# family of "all cascades ended up the same size" and "the table got
# reversed". Verified by setting the extents to {24,24,24,24} and watching
# this flip to "no".
Assert-Marker "shadow_cascade_texel_monotonic" "yes"

# -----------------------------------------------------------------------------
# Constant-ring FLATNESS
# -----------------------------------------------------------------------------
# This replaces a "fail at 75% of kCBRingSize" gate, which after the per-object
# structured-buffer change could not fire on the regression it existed to catch.
# The ring no longer scales with entity count at all: per-object and per-material
# data live in structured buffers, and the only ring traffic left is CBPerFrame
# plus one CBPerPass per pass. Measured peak is 1792 bytes in BOTH smoke modes -
# 1% of the 256 KB ring. Restoring the ENTIRE pre-change per-draw traffic would
# reach roughly 24% here, so a 75% gate would have sat there passing forever
# while the very thing it was watching for went unnoticed.
#
# So gate on FLATNESS instead of on a fraction of a fixed capacity. The budget
# below scales with the cascade count - a fifth cascade legitimately costs one
# more 256-byte cbuffer - but NOT with entity count. Any per-draw ring
# allocation reintroduced anywhere immediately blows it: one 256-byte upload per
# caster at this scene's draw count is already an order of magnitude over.
#
# This is a gate that can actually trip, which the previous one was not.
if ($markers.ContainsKey("cb_ring_peak") -and $markers.ContainsKey("cb_ring_capacity")) {
    $peak = [double]$markers["cb_ring_peak"]
    $cap  = [double]$markers["cb_ring_capacity"]
    $pct  = [int](100 * $peak / $cap)
    Write-Host "Constant ring peak: $peak / $cap bytes ($pct%)"

    $cascades = if ($markers.ContainsKey("shadow_cascades")) { [int]$markers["shadow_cascades"] } else { 4 }

    # CBPerFrame's own ring cost comes FROM THE ENGINE, not from a literal here.
    # It used to be a hardcoded 512 - a mirror of AlignCBSize(sizeof(CBPerFrame))
    # that nothing kept in step. Any growth of CBPerFrame was silently paid for
    # out of this budget's slack, so the first append that pushed the aligned
    # size to 768 would have consumed the entire spare slot below and the NEXT
    # person to touch the ring would get a failure message telling them per-draw
    # traffic had leaked in - which would have been false, and would have sent
    # them debugging the wrong thing entirely.
    #
    # The gate exists to measure PER-DRAW traffic leaking into the ring.
    # CBPerFrame's size is pinned independently by static_asserts in renderer.h;
    # it does not need a second guard, and making this budget double as one is
    # what made the message misleading. Read it, do not mirror it.
    #
    # The marker is REQUIRED. Defaulting to 512 when it is absent would restore
    # the literal by the back door on any run where the engine failed to emit it.
    if (-not $markers.ContainsKey("cb_per_frame_bytes")) {
        throw ("Marker cb_per_frame_bytes is missing. The constant-ring budget is computed " +
               "from it and there is deliberately no fallback literal: a default here would " +
               "silently re-create the hardcoded mirror of AlignCBSize(sizeof(CBPerFrame)) " +
               "that this marker exists to remove.")
    }
    $cbPerFrame = [int]$markers["cb_per_frame_bytes"]

    # Per-frame constant (read above), plus one CBPerPass per shadow cascade and
    # one for the main pass, plus one spare slot of slack for a future per-pass
    # constant. Scales with cascade count on purpose. Scales with entity count
    # NEVER - that flatness is the whole property being guarded.
    $flatBudget = $cbPerFrame + (($cascades + 1) * 256) + 256
    Write-Host "Constant ring flat budget: $flatBudget bytes (CBPerFrame $cbPerFrame + $cascades+1 pass CBs + 256 slack)"

    if ($peak -gt $flatBudget) {
        $perDrawCost = 0
        if ($markers.ContainsKey("shadow_records")) {
            $perDrawCost = [int]$markers["shadow_records"] * 256
        }
        throw ("Constant ring peak is $peak bytes, above the flat budget of $flatBudget for " +
               "$cascades cascades. The ring is supposed to hold only CBPerFrame and one " +
               "CBPerPass per pass, so its peak must not depend on entity count. This says " +
               "something now allocates from the ring PER DRAW - at this scene's draw count " +
               "one 256-byte upload per caster would add about $perDrawCost bytes. Find the " +
               "new UploadCB call on a per-draw path and move that data into the object or " +
               "material structured buffer instead. Do NOT raise this budget to make the " +
               "failure go away: it already tracks CBPerFrame's real size ($cbPerFrame bytes, " +
               "from the cb_per_frame_bytes marker) and the cascade count, which are the only " +
               "two things it is allowed to scale with. Adding a constant here re-creates the " +
               "entity ceiling this change removed.")
    }
}
if ($ResizeStress) { Assert-Marker "resize_requests" "3" }

# -----------------------------------------------------------------------------
# Per-draw structured-buffer REALLOCATION
# -----------------------------------------------------------------------------
# EnsureFrameStructuredBuffer's grow branch allocates kFrameCount replacement
# buffers, unmaps and DeferredReleases the outgoing ones, and swaps them in. It
# is the only code in the per-object structured-buffer design that can
# use-after-free: kFrameCount frames may still be reading the buffers it
# releases, and CPU writes to persistently mapped UPLOAD memory are not
# synchronised by resource barriers, so nothing but the deferred-release fence
# stands between it and a live buffer being freed underneath a frame in flight.
#
# It used to NEVER EXECUTE in an ordinary run. The demo scene's draw count sat
# under a 256-element kMinObjectCapacity, so after the first allocation the
# function early-outed on every frame and the whole branch was reachable only
# behind the -ForceGrow switch. This block printed "Structured-buffer
# reallocations: 0" and
# moved on - it stated the gap out loud and asserted nothing. An untaken branch
# behind an unrun flag is not coverage, so the DEFAULT run now takes the branch
# by construction and these are HARD assertions:
#
#   * The capacity floors (kMinObjectCapacity = 4, kMinMaterialCapacity = 2) sit
#     below any real scene, and Renderer::Init allocates AT them, so frame one
#     must grow both buffers.
#   * The +80 growth entities App::ApplySmokeRTMutationStress adds at frame 8
#     then push past the frame-one capacity and force a second grow MID-RUN,
#     while earlier frames are still executing.
#
# The two counts are asserted separately because they are not the same claim.
# structured_buffer_reallocations includes frame zero's grow, which releases a
# buffer no command list has ever bound: the deferred-release queue has nothing
# to protect there, and that grow stays green with the fence guard deleted
# outright - measured, not assumed. Only
# structured_buffer_reallocations_in_flight - grows that ran with at least one
# frame already recorded and never waited upon - covers the case that can
# actually use-after-free.
foreach ($k in @("structured_buffer_reallocations",
                 "structured_buffer_reallocations_in_flight")) {
    if (-not $markers.ContainsKey($k)) { throw "Smoke test did not emit the '$k' marker." }
}
$reallocs        = [int]$markers["structured_buffer_reallocations"]
$reallocsInFlight = [int]$markers["structured_buffer_reallocations_in_flight"]
Write-Host "Structured-buffer reallocations: $reallocs ($reallocsInFlight with frames in flight)"

# EVERY run, including a plain -RasterOnly one. This is the assertion that keeps
# the grow path on the default verification loop rather than behind a switch.
if ($reallocs -lt 1) {
    throw ("No structured-buffer reallocations occurred. The grow branch in " +
           "Renderer::EnsureFrameStructuredBuffer - allocate kFrameCount " +
           "replacements, unmap and DeferredRelease the outgoing ones, swap - is " +
           "the ONLY code in the per-draw structured-buffer design that can " +
           "use-after-free, and this run never executed it. Renderer::Init " +
           "allocates both buffers at kMinObjectCapacity/kMinMaterialCapacity " +
           "precisely so the first real frame has to grow past them; a zero here " +
           "means those floors were raised back above the scene's draw count, or " +
           "the Init allocation was removed, or GrownCapacity started padding the " +
           "first allocation with kCapacityHeadroom again. Do not 'fix' this by " +
           "deleting the assertion - it exists because this path was once " +
           "reachable only under -ForceGrow, which nobody ran.")
}

# THE ASSERTION WITH TEETH, and it applies to EVERY mode including -RasterOnly.
#
# App::ApplySmokeGrowthStress adds its 80 entities at frame 8 in both smoke
# modes, deliberately outside the --smoke-rt gate, because this is a raster-path
# hazard: it is Renderer::BeginFrame's root SRVs that address the buffers being
# released. In the default run frame 8 is still raster anyway - RT starts at
# frame 15 with the 0.25 s delay - so the grow lands with the raster path
# actively binding and reading these buffers.
#
# Measured, not assumed: replacing DeferredRelease with an immediate Reset in
# EnsureFrameStructuredBuffer kills the run at exactly this grow. The same break
# left a -RasterOnly run completely green while the growth churn was still
# RT-gated and the only reallocations it performed were frame one's, which is
# why the churn moved and why this assertion is not conditioned on the mode.
if ($reallocsInFlight -lt 1) {
    throw ("This run reallocated $reallocs time(s) but NONE of them ran with " +
           "frames in flight, so the deferred-release fence guard in " +
           "EnsureFrameStructuredBuffer went untested: a grow with nothing " +
           "outstanding would stay green even if the outgoing buffers were " +
           "freed immediately instead of parked at m_globalFenceValue + 1. " +
           "The +80 entities App::ApplySmokeGrowthStress adds at frame 8 are what " +
           "produce this; check that they are still created in BOTH smoke modes " +
           "and that they still push RequiredObjectCapacity past the frame-one " +
           "capacity of 2*MeshInstanceCount + kCapacityHeadroom.")
}

# -ForceGrow: THE OPT-IN HEAVY CASE. Restored, after being deleted on a premise
# that was wrong.
#
# The deletion was justified - in the commit message and in a comment right here
# - by the claim that -ForceGrow was "doubly dead": that the --smoke-force-grow
# ramp had not survived a merge, and that the switch "was never declared in
# smoke_test.ps1's param block", so $ForceGrow was always $null. Checked against
# the history, BOTH halves are false, and on main the switch was live end to end:
#
#   git show main:tools/smoke_test.ps1  ->  line 7  [switch]$ForceGrow,
#                                           line 68 if ($ForceGrow) { $arguments += "--smoke-force-grow" }
#   git grep main -- src/               ->  app.h:28    bool smokeForceGrow
#                                           app.cpp:58  options.smokeForceGrow = HasOption(args, "--smoke-force-grow")
#                                           app.cpp:1242 if (m_options.smoke && m_options.smokeForceGrow)
#
# What actually happened is narrower and worse. The codex side of the merge
# (5953648) had neither the param declaration nor the src ramp; merge a835f5d
# took that side for both files while keeping the OTHER side's `if ($ForceGrow
# -and ...)` assertion body. That merge is what killed the switch. The follow-up
# commit then observed the corpse it had just created and wrote it up as
# pre-existing rot, which is how a regression introduced by a merge became a
# comment telling the next reader the coverage had never existed.
#
# It is dead code only in the post-merge tree, and the fix for that is to restore
# it, not to certify it. The default ramp now runs in BOTH smoke modes, so
# -ForceGrow is no longer the only way to reach the grow branch; it steepens the
# ramp (16 draws/frame against the default 4) and remains the heavy case.
if ($ForceGrow -and $reallocs -lt 20) {
    throw ("-ForceGrow produced only $reallocs structured-buffer reallocations. " +
           "The switch steepens App::RenderFrame's sizing-hint ramp to 16 draws " +
           "per frame against the default 4, so it must comfortably exceed what " +
           "a default run already achieves - and a default run of this length " +
           "performs about 15. A count in the default range means the " +
           "--smoke-force-grow option stopped reaching m_options.smokeForceGrow, " +
           "or the ramp no longer crosses capacity boundaries. Check " +
           "AppOptions::smokeForceGrow in src/app.h and the rampPerFrame " +
           "selection in App::RenderFrame.")
}

# KEPT FROM THE OTHER SIDE OF THIS MERGE. The counts say replacement happened
# with frames outstanding; this says WHEN the first such replacement was, and
# that is a different claim. A run that only ever replaced during start-up would
# satisfy a count while never testing a steady-state grow.
#
# Note this tracks the first IN-FLIGHT replacement, not the first replacement.
# The original marker tracked the total, which against these capacity floors
# would always latch frame 0 - Renderer::Init allocates AT the floor and the
# demo scene crosses it immediately - so asserting the total's first occurrence
# happened late would assert the opposite of the truth.
if (-not $markers.ContainsKey("first_in_flight_reallocation_frame")) {
    throw "Smoke test did not emit the 'first_in_flight_reallocation_frame' marker."
}
$firstInFlight = [uint64]$markers["first_in_flight_reallocation_frame"]
Write-Host "First in-flight structured-buffer replacement: frame $firstInFlight"
#
# ON THE MARGIN, since it is thin and that is deliberate. The bound is 3 =
# kFrameCount: frames 1..3 are what fill the three slots, so a replacement at or
# below frame 3 cannot have had a full complement of frames outstanding behind
# it. MEASURED, with the ramp now running in both modes: frame 5 under
# -RasterOnly and frame 5 in the default mode, so the margin is two frames.
#
# Two frames is small, and widening the bound would NOT make it safer - it would
# make it wrong. This is a structural bound, not a tuned one: 3 is the value
# kFrameCount takes, and a run that first replaced in flight at frame 4 would be
# perfectly correct and would fail a bound of 5. The run is also deterministic -
# fixed 60 Hz timestep, fixed scene, fixed ramp - so there is no sampling noise
# for a margin to absorb. What a widened bound would actually catch is the ramp
# getting steeper, which is not a defect.
#
# The complementary risk - the value drifting UP until in-flight replacement
# stops happening in any useful quantity - is covered by the reallocsInFlight
# floor above, which is the assertion with teeth.
#
# NOT UNDER -ForceGrow, and this is a limitation of the bound rather than of the
# switch. `3` is a PROXY for "this was not a start-up grow with nothing behind
# it"; it works because the default ramp needs a few frames to cross the first
# capacity boundary. -ForceGrow front-loads growth deliberately and reaches its
# first in-flight replacement on frame 2 - MEASURED - where one frame is
# genuinely outstanding and the deferred-release fence is genuinely exercised.
# The proxy calls that a failure; the hazard says otherwise, and the hazard is
# right. Under -ForceGrow the count assertion above is the one that carries the
# claim. Narrowing the bound here rather than loosening it for every run keeps
# the default path's guarantee intact.
if (-not $ForceGrow -and $firstInFlight -le 3) {
    throw ("The first in-flight structured-buffer replacement happened on frame " +
           "$firstInFlight; it must happen after all three frame slots can be in " +
           "flight (kFrameCount = 3), or the deferred-release fence is being " +
           "exercised only during start-up when there is nothing outstanding to " +
           "protect.")
}

# Cross-pass record parity. Scene::RenderShadowCasters and Scene::RenderEntities
# walk the same MeshInstance pool in the same order with identical
# visible/Transform/Material filters, so they issue the same number of draws.
# That is what makes 2 x MeshInstanceCount() sufficient capacity for the shared
# object buffer - and it is enforced nowhere in the code. Asserting the
# INVARIANT rather than a magic entity count means it survives the demo scene
# changing, and a future divergence (frustum culling, a castsShadow flag, an LOD
# cut) surfaces here as a test failure instead of as a wrong-looking frame.
foreach ($k in @("shadow_records", "main_records", "object_records_peak", "object_capacity")) {
    if (-not $markers.ContainsKey($k)) { throw "Smoke test did not emit the '$k' marker." }
}
$shadowRecords = [uint32]$markers["shadow_records"]
$mainRecords   = [uint32]$markers["main_records"]
if ($shadowRecords -ne $mainRecords) {
    throw (("Cross-pass record parity broken: shadow_records={0} but main_records={1}. " +
            "The two scene walks no longer issue the same draws.") -f $shadowRecords, $mainRecords)
}
if ($shadowRecords -lt 1) {
    throw "No per-object records were written by either pass; the raster path drew nothing."
}
# The object buffer is shared by both passes at disjoint index ranges, so its
# peak occupancy must be exactly the two counts summed, and must fit.
if ([uint32]$markers["object_records_peak"] -lt ($shadowRecords + $mainRecords)) {
    throw (("object_records_peak={0} is below shadow+main={1}; the two passes are " +
            "overlapping in the object buffer rather than taking disjoint ranges.") -f `
            $markers["object_records_peak"], ($shadowRecords + $mainRecords))
}
if ([uint32]$markers["object_records_peak"] -gt [uint32]$markers["object_capacity"]) {
    throw ("object_records_peak={0} exceeds object_capacity={1}; draws were skipped." -f `
           $markers["object_records_peak"], $markers["object_capacity"])
}

# =============================================================================
# THE MERGED DRAW-RECORD PROBE
# =============================================================================
# Direct GPU evidence for the root-SRV contract, which nothing else in the
# runtime can supply: SetGraphicsRootShaderResourceView takes a bare GPU virtual
# address, so there is no descriptor, no StructureByteStride, and nothing for the
# debug layer to cross-check. A scene drawn entirely from record 0 renders a
# perfectly plausible image.
#
# One UAV carries two independent claims per record, because two verification
# schemes were built for this feature independently and each is blind to
# something the other catches:
#
#   HASHES   every field of the record the shader loaded. Catches CPU/HLSL
#            layout drift - a stride or offset disagreement that shifts every
#            element after the first - which an index witness cannot see,
#            because shifted garbage still comes from the right element.
#   MARKERS  the recordId stored INSIDE the loaded record, + 1. Catches
#            wrong-element indexing, which hashes alone cannot: two records with
#            equal contents hash equal, so a shader stuck on element 0 stays
#            green for as long as the scene agrees with element 0.
#
# The distinct counts are what localise a failure. Every classic breakage here -
# a hardcoded element, a root constant that stopped reaching a stage,
# SV_InstanceID at SM 5.1 excluding StartInstanceLocation - collapses a whole
# pass to ONE distinct marker, and the pass it collapses in names the shader.
#
# BOTH MODES. This block used to be gated on -RasterOnly, "because the probe is
# skipped on a path-traced frame and the default smoke run ends in RT" - which
# was an accurate description of a hole, not a reason for one. The probe ran on
# the FINAL frame, the default mode path-traces the final frame, so the default
# run - the one that gets run - executed none of this. App::Run now schedules the
# probe on the last RASTER frame of whichever mode is active (see
# smokeDrawProbeFrame), so both modes carry it and the gate is gone.
#
# In the default mode that frame is mid-run, not the end frame, and it sits
# inside ApplySmokeGrowthStress's frame 8..16 window - so the probe covers a
# larger draw population there than under -RasterOnly, and covers it against a
# buffer that has already been grown and deferred-released. That is why nothing
# below compares probe counts against the end-of-run shadow_records/main_records
# markers: those are a different frame's population. The probe is self-describing
# and the assertions are internal to it.
if (-not $markers.ContainsKey("draw_probe_frame")) {
    throw ("Smoke test did not emit 'draw_probe_frame'. The draw-record probe was " +
           "never armed. It must run on the last raster frame in BOTH smoke modes - " +
           "see smokeDrawProbeFrame in App::Run. A missing marker here means the " +
           "probe went back to riding on the final frame, which is path-traced in " +
           "the default mode and therefore never probes anything.")
}
Write-Host "Draw probe frame: $($markers['draw_probe_frame'])"
if (-not $RasterOnly -and [uint64]$markers["draw_probe_frame"] -ge [uint64]$markers["frames"]) {
    throw ("draw_probe_frame=$($markers['draw_probe_frame']) is not before the end of " +
           "the run ($($markers['frames']) frames). In the default mode the probe must " +
           "land on a raster frame, which means BEFORE path tracing starts.")
}
Assert-Marker "draw_probe" "ok"
foreach ($k in @("draw_probe_shadow_records", "draw_probe_shadow_distinct",
                 "draw_probe_shadow_mismatches",
                 "draw_probe_main_records", "draw_probe_main_distinct",
                 "draw_probe_main_mismatches",
                 "draw_probe_material_records", "draw_probe_material_distinct",
                 "draw_probe_material_mismatches",
                 "draw_probe_material_unshaded")) {
    if (-not $markers.ContainsKey($k)) { throw "Smoke test did not emit the '$k' marker." }
}

$probeShadow         = [uint32]$markers["draw_probe_shadow_records"]
$probeShadowDistinct = [uint32]$markers["draw_probe_shadow_distinct"]
$probeMain           = [uint32]$markers["draw_probe_main_records"]
$probeMainDistinct   = [uint32]$markers["draw_probe_main_distinct"]
$probeMaterial       = [uint32]$markers["draw_probe_material_records"]
$probeMatDistinct    = [uint32]$markers["draw_probe_material_distinct"]
$probeMatUnshaded    = [uint32]$markers["draw_probe_material_unshaded"]
Write-Host ("Draw probe: shadow $probeShadow/$probeShadowDistinct distinct, " +
            "main $probeMain/$probeMainDistinct distinct, " +
            "material $probeMaterial/$probeMatDistinct distinct " +
            "($probeMatUnshaded unshaded)")

# The same pixel-stage probe permutation witnesses the cascade cross-fade at
# the value returned by ComputeShadow. A branch-executed flag is insufficient:
# expected/output are quantized per pixel, and the shader counts mismatches
# before atomically reducing them. signal_q8 must also be nonzero, proving the
# adjacent samples differed somewhere and a hard-selection negative control has
# observable work to break.
Assert-Marker "shadow_blend_probe" "ok"
foreach ($k in @("shadow_blend_records", "shadow_blend_pair_mask",
                 "shadow_blend_pixels", "shadow_blend_expected_q8",
                 "shadow_blend_output_q8", "shadow_blend_primary_q8",
                 "shadow_blend_signal_q8", "shadow_blend_mismatch_pixels")) {
    if (-not $markers.ContainsKey($k)) { throw "Smoke test did not emit the '$k' marker." }
}
$blendRecords  = [uint32]$markers["shadow_blend_records"]
$blendPairMask = [uint32]$markers["shadow_blend_pair_mask"]
$blendPixels   = [uint64]$markers["shadow_blend_pixels"]
$blendExpected = [uint64]$markers["shadow_blend_expected_q8"]
$blendOutput   = [uint64]$markers["shadow_blend_output_q8"]
$blendSignal   = [uint64]$markers["shadow_blend_signal_q8"]
$blendMismatch = [uint64]$markers["shadow_blend_mismatch_pixels"]
if ($blendRecords -eq 0 -or $blendPixels -eq 0) {
    throw "No raster pixels exercised a shadow cascade fade band."
}
if (($blendPairMask -band 1) -eq 0) {
    throw "The scene did not exercise the cascade 0-to-1 transition."
}
if ($blendSignal -eq 0) {
    throw "Cascade blend samples never differed; the hard-selection negative control is vacuous."
}
if ($blendMismatch -ne 0 -or $blendExpected -ne $blendOutput) {
    throw ("Cascade blend output differs from the expected adjacent-sample blend: " +
           "mismatchPixels=$blendMismatch expectedQ8=$blendExpected outputQ8=$blendOutput")
}
Write-Host ("Shadow blend probe: $blendPixels pixels across $blendRecords records, " +
            "pair-mask=$blendPairMask signal-q8=$blendSignal")

# ---- VACUITY GUARDS, restored -----------------------------------------
#
# Zero records trivially give zero distinct and zero mismatches, and ONE record
# trivially gives one distinct - so with fewer than two draws per pass every
# assertion below passes while proving nothing. These floors were dropped during
# the merge on the reasoning that the property held transitively through the
# shadow/main parity check further up; it does, today, for this scene, which is
# exactly the kind of coupling that decays silently. An explicit floor costs two
# comparisons and states the requirement where the requirement is used.
#
# TWO, not one: the whole failure mode being hunted is every draw collapsing onto
# a single record, and a one-draw pass cannot distinguish that from correctness.
if ($probeShadow -lt 2) {
    throw ("draw_probe_shadow_records=$probeShadow; fewer than two shadow draws means " +
           "the distinct-index check below cannot distinguish correct indexing from " +
           "every draw sharing one record.")
}
if ($probeMain -lt 2) {
    throw ("draw_probe_main_records=$probeMain; fewer than two main-pass draws means " +
           "the distinct-index check below cannot distinguish correct indexing from " +
           "every draw sharing one record.")
}
# Cross-pass parity AT THE PROBE FRAME. The shadow_records/main_records markers
# checked earlier are the END-OF-RUN population, which in the default mode is a
# different frame from the probe frame, so this is the parity claim that applies
# to the numbers actually being asserted on.
if ($probeShadow -ne $probeMain) {
    throw ("At the probe frame the shadow pass issued $probeShadow object records and " +
           "the main pass $probeMain. The two scene walks must issue the same draws; " +
           "that parity is what makes 2 x MeshInstanceCount() sufficient capacity.")
}

# ---- shadow_vs: object indexing --------------------------------------
if ([uint32]$markers["draw_probe_shadow_mismatches"] -ne 0) {
    throw ("shadow_vs consumed object records that differ from the CPU upload " +
           "contract. Either the record layout drifted between " +
           "src/render/gpu_draw_records.h and shaders/gpu_draw_records.hlsli, or " +
           "the pass is reading the wrong element.")
}
if ($probeShadowDistinct -ne $probeShadow) {
    throw ("shadow_vs read only $probeShadowDistinct distinct object records across " +
           "$probeShadow draws. Every draw must load its OWN record; a collapse to " +
           "one is objectBuffer[0], a lost b3 root constant, or SV_InstanceID, all of " +
           "which render a plausible image.")
}

# ---- basic_vs: object indexing ---------------------------------------
if ([uint32]$markers["draw_probe_main_mismatches"] -ne 0) {
    throw "basic_vs consumed object records that differ from the CPU upload contract."
}
if ($probeMainDistinct -ne $probeMain) {
    throw ("basic_vs read only $probeMainDistinct distinct object records across " +
           "$probeMain draws. See the shadow-pass message above - same failure, " +
           "different shader.")
}

# ---- basic_ps: MATERIAL indexing -------------------------------------
#
# THIS IS THE ASSERTION THIS MERGE EXISTS FOR. Both incoming schemes left the
# material index unwitnessed at its point of use: one checked only the object
# half of the b3 root constant, and the other hashed materialBuffer from
# basic_vs - a stage that reads it for no other purpose, so the check
# witnessed its own load and stayed green with basic_ps hardcoded to
# materialBuffer[0]. The material words now come from basic_ps, beside the
# `mat` every shading line below it reads.
#
# The counts are inequalities rather than equalities on purpose. The pixel
# stage does not run for a draw that shades no pixels, so an occluded or
# fully back-facing draw legitimately leaves its slot unwritten; those are
# reported as unshaded rather than counted as mismatches. What must hold is
# that enough draws DID shade and that they read DIFFERENT records.
if ([uint32]$markers["draw_probe_material_mismatches"] -ne 0) {
    throw ("basic_ps consumed material records that differ from the CPU upload " +
           "contract. The pixel shader is shading with a material record other than " +
           "the one the CPU wrote for its draw.")
}
if ($probeMaterial -lt 2) {
    throw ("Only $probeMaterial material records were witnessed by basic_ps. The " +
           "material half of the probe needs at least two shaded draws to say " +
           "anything; check that the probe UAV is still PIXEL-visible in BOTH " +
           "hand-written branches of Renderer::CreateRootSignature.")
}
if (($probeMaterial + $probeMatUnshaded) -ne $probeMain) {
    throw ("Material probe slots ($probeMaterial shaded + $probeMatUnshaded unshaded) " +
           "do not account for all $probeMain main-pass draws at the probe frame.")
}
if ($probeMatDistinct -lt 2) {
    throw ("basic_ps read only $probeMatDistinct distinct material record(s) across " +
           "$probeMaterial shaded draws. This is the materialBuffer[0] failure: the " +
           "b3 material index is not reaching the pixel stage, or it is being " +
           "ignored. It renders a plausible image and no other check in this tree " +
           "would notice.")
}
# WHAT USED TO BE HERE: a hard `$probeMatDistinct -ne $probeMaterial` throw,
# i.e. exactly one material record per shaded draw. Removed, for two reasons.
#
# It could not fire. Renderer::ReadDrawProbe already requires, per slot, that
# materialMarker == materialIndex + 1 where materialIndex is derived from the
# slot's own position. If draw_probe_material_mismatches is 0 then every checked
# slot carried its own distinct index, so distinct == checked follows
# arithmetically. The assertion was restating its predecessor.
#
# And it pinned a policy. One material record per draw is the CURRENT allocation
# behaviour of the material cursor, not a property of correct indexing: a change
# that shared one record between draws with the same ecs::Material would be
# correct and would trip this. That coupling is real, but it belongs in
# Renderer::ReadDrawProbe - which encodes it deliberately, in C++, next to the
# cursor it describes - and not additionally in the harness, where it reads as an
# independent check and is not one.
#
# The claims that survive are the ones that do work: mismatches == 0 (each draw
# shaded with ITS OWN record, verified per slot on the CPU) and distinct >= 2
# (the population did not collapse onto one record).

if ([uint64]$markers["descriptors_pending_after_scene_shutdown"] -lt 1) {
    throw "Scene shutdown did not retire any raster texture descriptors."
}

if ($markers["fixed_hz"] -ne "60") {
    throw "Smoke timeline frequency was '$($markers['fixed_hz'])', expected '60'."
}
if ([uint64]$markers["frames"] -ne [uint64]$markers["target_frames"]) {
    throw "Smoke test stopped on frame $($markers['frames']); expected exact target frame $($markers['target_frames'])."
}
if ($GPUValidation -and $logText -notmatch "D3D12 GPU-based validation enabled") {
    throw "GPU-based validation was requested but the D3D12 debug interface did not enable it."
}

if ([double]$markers["elapsed_ms"] -le 0.0) {
    throw "Smoke elapsed time was '$($markers['elapsed_ms'])'; expected a positive measurement."
}
if ([double]$markers["throughput_fps"] -le 0.0) {
    throw "Smoke throughput was '$($markers['throughput_fps'])'; expected a positive measurement."
}

if ($RasterOnly) {
    Assert-Marker "mode"      "raster"
    Assert-Marker "rt_active" "no"
} else {
    Assert-Marker "mode"         "rt"
    Assert-Marker "rt_available" "yes"
    Assert-Marker "rt_active"    "yes"
    Assert-Marker "rt_quality"   $(if ($FullQuality) { "full" } else { "stable" })
    Assert-Marker "rt_accumulation_frame" "0"
    Assert-Marker "rt_texture_churn" "passed"
    Assert-Marker "rt_topology_churn" "passed"
    # GPU-based validation may intentionally drain/serialize command execution.
    # The ordinary unlocked run proves overlap; this run proves validator-clean work.
    if ($Unlocked -and !$GPUValidation -and
        [uint32]$markers["max_outstanding_submissions"] -lt 2) {
        throw "Unlocked RT smoke observed only $($markers['max_outstanding_submissions']) outstanding submission(s); expected at least 2."
    }
}

if ([int]$markers["frames"] -lt 2) {
    throw "Smoke test rendered only $($markers['frames']) frame(s); expected a sustained run."
}

# -----------------------------------------------------------------------------
# Pixel assertions
# -----------------------------------------------------------------------------
# Everything above proves the engine did not crash. Only this section proves it
# drew something. A black screen, inverted culling, a shader that outputs nothing,
# or NaN-poisoned output would all pass every check above.
if (!$NoCapture) {
    Assert-Marker "capture" "ok"

    if (!(Test-Path -LiteralPath $capture)) {
        throw "Smoke test did not produce a capture: $capture"
    }

    $bytes = [IO.File]::ReadAllBytes($capture)
    if ($bytes.Length -lt 32) { throw "Capture file is truncated ($($bytes.Length) bytes)." }
    if ($bytes[0] -ne 0x50 -or $bytes[1] -ne 0x36) { throw "Capture is not a binary P6 PPM." }

    # Skip the three newline-terminated header fields.
    $offset = 0; $newlines = 0
    while ($newlines -lt 3 -and $offset -lt $bytes.Length) {
        if ($bytes[$offset] -eq 10) { $newlines++ }
        $offset++
    }

    $expectedW = [int]$markers["w"]
    $expectedH = [int]$markers["h"]
    $expectedBytes = $expectedW * $expectedH * 3
    if (($bytes.Length - $offset) -ne $expectedBytes) {
        throw ("Capture payload is {0} bytes; expected {1} for {2}x{3}." -f `
               ($bytes.Length - $offset), $expectedBytes, $expectedW, $expectedH)
    }

    # Sample rather than walk every pixel; 1920x1080 in PowerShell is slow.
    $pixels = $expectedBytes / 3
    $step = 7
    $sum = 0.0; $nonBlack = 0; $count = 0
    $buckets = @{}
    for ($i = 0; $i -lt $pixels; $i += $step) {
        $b = $offset + $i * 3
        $r = $bytes[$b]; $g = $bytes[$b + 1]; $bl = $bytes[$b + 2]
        $lum = 0.2126 * $r + 0.7152 * $g + 0.0722 * $bl
        $sum += $lum
        if ($lum -gt 8) { $nonBlack++ }
        $buckets["$([int]($r / 32)),$([int]($g / 32)),$([int]($bl / 32))"] = 1
        $count++
    }

    $meanLum      = $sum / $count
    $nonBlackFrac = $nonBlack / $count
    $distinct     = $buckets.Count

    Write-Host ("Capture stats: {0}x{1}  mean-luminance={2:N1}  non-black={3:P1}  distinct-buckets={4}" -f `
                $expectedW, $expectedH, $meanLum, $nonBlackFrac, $distinct)

    # Thresholds are deliberately loose. They are chosen to catch catastrophic
    # failure (black frame, flat fill, blown-out white), not to pin down exact
    # appearance - that would flake on any legitimate lighting change. A reference
    # image comparison is the right tool for appearance and is future work.
    if ($meanLum -lt 10)      { throw "Capture is essentially black (mean luminance $([math]::Round($meanLum,1)))." }
    if ($meanLum -gt 245)     { throw "Capture is blown out (mean luminance $([math]::Round($meanLum,1)))." }
    if ($nonBlackFrac -lt 0.10) { throw "Only $([math]::Round($nonBlackFrac*100,1))% of sampled pixels are non-black." }
    if ($distinct -lt 4)      { throw "Capture has only $distinct distinct colour buckets; the frame is effectively a flat fill." }

    # -------------------------------------------------------------------------
    # There is DELIBERATELY no golden-value gate on these statistics.
    # -------------------------------------------------------------------------
    # One used to live here: exact mean luminance and an exact colour-bucket
    # count, +/-0.5. It was deleted rather than repaired, and the reasoning is
    # worth keeping so nobody reintroduces it.
    #
    # It was guarding a real invariant - that the per-draw root constant at b3
    # reaches every draw path, so each draw reads its own object record, which a
    # root SRV leaves completely unvalidated at runtime. But it guarded that
    # invariant THROUGH THE RENDERED IMAGE, and the rendered image is a function
    # of which assets happen to sit in build/<Config>. That is build output, not
    # tracked source, so two checkouts of the same commit legitimately produce
    # different numbers. The gate was mis-calibrated twice in a single round on
    # exactly that: 128.3 measured before four-cascade shadows landed, then 122.9
    # measured on a clean 17-entity clone, while the tree the harness actually
    # runs in has a gitignored corridor GLB loaded and renders 64 buckets.
    #
    # It also could not do the job even when calibrated. Pinning shadow_vs.hlsl
    # to record 0 was measured at mean 123.6 / 58 buckets against a correct
    # 122.9 / 59 - one bucket and 0.7 luminance, well inside the cross-checkout
    # drift that destabilised the gate. A tolerance loose enough to be stable
    # could not see the failure; one tight enough to see it could not stay green.
    #
    # The invariant is now asserted directly and on the GPU side, by the
    # draw_probe_* markers above. Those hashes and mismatch counts are produced
    # from the records the shaders consumed and are independent of lighting.
    # DO NOT put a golden-value gate back here. If you want appearance
    # regression testing, the right tool is a reference-image comparison against
    # a committed image, which is a different mechanism with different
    # requirements.
}

if ($RasterOnly) {
    Write-Host "Smoke test passed (raster)."
} elseif ($FullQuality) {
    Write-Host "Smoke test passed (path tracing full)."
} else {
    Write-Host "Smoke test passed (path tracing stable)."
}
