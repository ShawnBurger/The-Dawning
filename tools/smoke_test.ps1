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
# Four cascades on ONE Texture2DArray. The slice count is asserted separately
# from the slot because they fail independently: the array can lose a slice
# without the descriptor moving, and vice versa. %u on the C++ side, never
# %.1f - this is a string compare, so "4.0" would not match "4".
Assert-Marker "shadow_cascades" "4"
# Raster only - the probe is skipped in path-tracing runs, which do not use the
# shadow map at all. This is the assertion with teeth: shadow_map=ok only proves
# the resource was created, and deleting the shadow pass entirely leaves the map
# at its cleared value with every pixel reading fully lit, which nothing else
# here would notice. Verified by deleting the caster draw and watching this flip
# to "no".
if ($RasterOnly) {
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
    # "no" while every shadow_cascade_written_* marker stays "yes".
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
}

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
    # CBPerFrame rounded up to the 256-byte constant alignment (512 with four
    # cascade matrices), plus one CBPerPass per shadow cascade and one for the
    # main pass, plus one spare slot of slack for a future per-pass constant.
    $flatBudget = 512 + (($cascades + 1) * 256) + 256

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
               "failure go away: the budget scales with cascade count on purpose and with " +
               "nothing else, and raising it re-creates the entity ceiling this change removed.")
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
# In an ordinary run it NEVER EXECUTES. The demo scene's draw count sits under
# kMinObjectCapacity, so after the first allocation the function early-outs on
# every frame. -ForceGrow ramps the sizing hint so the buffers reallocate
# repeatedly, mid-run, with frames genuinely in flight.
#
# Assert it actually happened rather than assuming the flag worked: a hint ramp
# that stopped crossing capacity boundaries would leave this whole path
# uncovered again while the run still passed.
if ($markers.ContainsKey("structured_buffer_reallocations")) {
    $reallocs = [int]$markers["structured_buffer_reallocations"]
    Write-Host "Structured-buffer reallocations: $reallocs"
    if ($ForceGrow -and $reallocs -lt 5) {
        throw ("-ForceGrow produced only $reallocs structured-buffer reallocations. " +
               "The grow branch is meant to run many times under this switch; if the " +
               "sizing hint no longer crosses capacity boundaries then the one code " +
               "path here that can use-after-free is going untested. Check the " +
               "--smoke-force-grow ramp in App::RenderFrame against kCapacityHeadroom.")
    }
    if (-not $ForceGrow -and $reallocs -ne 0) {
        Write-Host "  (note: buffers reallocated without -ForceGrow; the scene outgrew the capacity floor)"
    }
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
    throw ("Cross-pass record parity broken: shadow_records={0} but main_records={1}. " +
           "The two scene walks no longer issue the same draws." -f $shadowRecords, $mainRecords)
}
if ($shadowRecords -lt 1) {
    throw "No per-object records were written by either pass; the raster path drew nothing."
}
# The object buffer is shared by both passes at disjoint index ranges, so its
# peak occupancy must be exactly the two counts summed, and must fit.
if ([uint32]$markers["object_records_peak"] -lt ($shadowRecords + $mainRecords)) {
    throw ("object_records_peak={0} is below shadow+main={1}; the two passes are " +
           "overlapping in the object buffer rather than taking disjoint ranges." -f `
           $markers["object_records_peak"], ($shadowRecords + $mainRecords))
}
if ([uint32]$markers["object_records_peak"] -gt [uint32]$markers["object_capacity"]) {
    throw ("object_records_peak={0} exceeds object_capacity={1}; draws were skipped." -f `
           $markers["object_records_peak"], $markers["object_capacity"])
}

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
    # Golden-value gate for the RASTER capture
    # -------------------------------------------------------------------------
    # The loose thresholds above catch catastrophic failure and nothing else.
    # They are not enough: forcing every draw to read per-object record 0 - the
    # single highest-prior failure mode of per-draw structured-buffer indexing,
    # and precisely what happens if someone "simplifies" the root constant to
    # SV_InstanceID at SM 5.1 - renders the whole scene with the first entity's
    # transform, and STILL passes every check above. That was verified, not
    # assumed, by building both mutations and watching the harness pass.
    #
    # CALIBRATION, measured on this scene at a fixed timestep and a fixed camera,
    # so the capture is deterministic (confirmed identical across repeated runs):
    #
    #   correct                              mean 122.9   buckets 59
    #   shadow_vs objectBuffer[0]            mean 123.6   buckets 58
    #   basic_vs  objectBuffer[0]            mean 124.4   buckets 27
    #
    # Re-measured against THIS tree, not carried over: the mutation figures in
    # the previous version of this block were taken before four-cascade shadows
    # and no longer describe either failure.
    #
    # A +/-0.5 band on the mean catches both (0.7 and 1.5 off), and the bucket
    # count catches both outright. Raster only: the path-traced capture depends
    # on accumulation depth and is not a golden-value candidate.
    #
    # RECALIBRATED for four-cascade shadows. The previous figure, 128.3, was
    # measured before cascades landed on main and was already stale when this
    # gate was written - it failed on the first run after merge, against a
    # correct image. If you are reading this because it failed again, see the
    # message below.
    if ($RasterOnly) {
        $goldenMeanLum  = 122.9
        $goldenBuckets  = 59
        $meanTolerance  = 0.5

        # DELIBERATELY does not tell you to update the numbers. The previous
        # wording ("If that was intended, re-measure and update the golden
        # values") is what a reader follows on autopilot, and doing that on a
        # real regression silently retires the only check that pins the HLSL row
        # layout against gpu_draw_records.h - the layout nothing validates at
        # runtime, because a root SRV has no descriptor and no stride. Updating
        # the constants is sometimes right, but it is the LAST step, not the
        # first.
        $fixHint = @"
The rendered raster image changed. Work out WHY before touching these constants.
  1. Check the bucket count in the same failure. 59 -> ~25 means most draws are
     reading one object record: the per-draw root constant at b3 is not reaching
     basic_vs.hlsl. 59 -> 58 points at shadow_vs.hlsl doing the same.
  2. Diff shaders/basic_vs.hlsl, shaders/shadow_vs.hlsl and
     src/render/gpu_draw_records.h against each other. ObjectData must be
     byte-identical in all three; nothing checks this at runtime.
  3. Compare the capture against a build of origin/main to see whether the
     change is yours.
Only once you can NAME the rendering change that moved the mean should you
re-measure and update goldenMeanLum and goldenBuckets - and record what changed
in the calibration block above. Widening meanTolerance is not a fix.
"@

        if ([math]::Abs($meanLum - $goldenMeanLum) -gt $meanTolerance) {
            $msg = "Raster capture mean luminance is {0:N1}, expected {1:N1} +/- {2:N1}. {3}" -f `
                   $meanLum, $goldenMeanLum, $meanTolerance, $fixHint
            throw $msg
        }
        if ($distinct -ne $goldenBuckets) {
            $msg = "Raster capture has {0} distinct colour buckets, expected {1}. {2}" -f `
                   $distinct, $goldenBuckets, $fixHint
            throw $msg
        }
    }
}

if ($RasterOnly) {
    Write-Host "Smoke test passed (raster)."
} elseif ($FullQuality) {
    Write-Host "Smoke test passed (path tracing full)."
} else {
    Write-Host "Smoke test passed (path tracing stable)."
}
