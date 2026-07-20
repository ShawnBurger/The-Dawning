param(
    [double]$Seconds = 4.0,
    [double]$RTDelaySeconds = 0.25,
    [switch]$RasterOnly,
    [switch]$FullQuality,
    [switch]$ResizeStress,
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

if ($markers.ContainsKey("cb_ring_peak") -and $markers.ContainsKey("cb_ring_capacity")) {
    $peak = [double]$markers["cb_ring_peak"]
    $cap  = [double]$markers["cb_ring_capacity"]
    $pct  = [int](100 * $peak / $cap)
    Write-Host "Constant ring peak: $peak / $cap bytes ($pct%)"
}
if ($ResizeStress) { Assert-Marker "resize_requests" "3" }

# Constant-ring pressure. An early-warning gate, deliberately well below the
# 100% point where UploadCB starts handing out GPU address zero to draws that
# get recorded anyway.
#
# THIS METRIC IS NOW FLAT IN THE ENTITY COUNT, and that is the whole point of
# the structured-buffer change. The history is worth keeping because it is what
# the gate is calibrated against:
#
#   before structured buffers, single cascade:  768 B per shadowed entity
#                                               (256 per-object + 256 material
#                                               + 256 shadow per-object)
#   before structured buffers, four cascades:  1536 B per shadowed entity, since
#                                               casters are walked once per
#                                               cascade - 57% of the ring at 97
#                                               entities, tripping this gate near
#                                               127 entities
#   after:                                      ZERO per entity
#
# Per-object and per-material data live in growable structured buffers indexed
# by a root constant, so the ring now carries only the 416-byte CBPerFrame plus
# one 256-byte CBPerPass per pass - four cascades plus the main pass, i.e. about
# 1.8 KB total, independent of how many entities exist. Adding a fifth cascade
# would cost 256 more bytes, not 96 more per entity.
#
# So this should read ~0. Anything approaching the gate means per-draw traffic
# has leaked back into the ring, which is a design regression rather than a
# capacity problem - do not fix it by raising kCBRingSize.
if (-not $markers.ContainsKey("cb_ring_pct")) {
    throw "Smoke test did not emit the 'cb_ring_pct' marker."
}
if ([uint32]$markers["cb_ring_pct"] -ge 75) {
    throw ("Constant ring peaked at {0}% ({1} of {2} bytes), at or past the 75% gate." -f `
           $markers["cb_ring_pct"], $markers["cb_ring_peak"], $markers["cb_ring_capacity"])
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

# All four cascades SHARE object range [0, N) rather than appending a private
# range each. That is what keeps object_capacity at 2 x MeshInstanceCount() and
# cb_ring_pct flat as cascades are added - without it the shadow range would be
# 4N and capacity would have to be 5N.
#
# The sharing is only valid while every cascade draws the same casters in the
# same order, so that draw i is the same entity in all four and each cascade
# rewrites byte-identical records. Per-cascade frustum culling is the obvious
# future change that breaks it, and it would break it SILENTLY: three cascades
# would rasterise the fourth's transforms and the shadows would merely look
# wrong. A record-count disagreement is the cheapest detectable symptom.
#
# Necessary, not sufficient - a reordering that preserved the count slips
# through - but it catches every filter change, which is the realistic failure.
if ($RasterOnly) {
    Assert-Marker "shadow_cascade_records_uniform" "yes"

    # With the range shared, shadow_records is ONE cascade's count, not the sum
    # over four. Cross-checked against the parity assertion above: if a cascade
    # ever started appending instead of rewinding, shadow_records would jump to
    # 4x main_records and that assertion would fire first.
    if ($shadowRecords -gt $mainRecords) {
        throw ("shadow_records={0} exceeds main_records={1}; a cascade is appending " +
               "object records instead of reusing range [0, N)." -f $shadowRecords, $mainRecords)
    }
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
    #   correct, clean-clone scene           mean 123.0   buckets 60
    #   correct, generated asset loaded      mean 122.5   buckets 64
    #   basic_vs objectBuffer[0] (clean)     mean 124.5   buckets 27
    #
    # RE-MEASURED after the cascade merge, not carried over. A +/-0.5 band on the
    # mean catches the mutation at 1.5 off, and the bucket count catches it
    # outright at 27 against 60. Raster only: the path-traced capture depends on
    # accumulation depth and is not a golden-value candidate.
    #
    # RE-BASELINED from 128.3/59 to 123.0/60 when four-cascade shadows merged.
    # The move is attributable to CASCADES, not to the structured-buffer change:
    # the four-cascade branch independently measured mean 123.0 / 60 buckets on
    # this same scene BEFORE per-draw data moved out of the constant ring, and
    # merging the two reproduces that number exactly. So the buffer refactor is
    # image-neutral, which is what a pure data-plumbing change should be - had
    # the mean landed anywhere else, that would itself have been the bug.
    #
    # This IS meant to trip on a deliberate lighting or scene change. When it
    # does, re-measure and update the two constants here - do not widen the band
    # until it stops failing, which would disarm exactly what it is for.
    # TWO BASELINES, selected by the generated_asset marker. The demo loads
    # assets/generated/.../model.glb when it is present, which adds an entity and
    # legitimately changes the image. Only the MANIFEST of that asset is tracked
    # in git - the .glb itself is not - so whether it loads is a property of the
    # checkout rather than of the commit. A single baseline would therefore pass
    # in a fresh worktree and fail in the canonical checkout, or vice versa, and
    # the failure would look like a rendering regression rather than a missing
    # file. Both values are measured; neither is a guess.
    if ($RasterOnly) {
        if (-not $markers.ContainsKey("generated_asset")) {
            throw "Smoke test did not emit the 'generated_asset' marker; cannot select a raster baseline."
        }
        if ($markers["generated_asset"] -eq "failed") {
            throw "The generated asset is present but failed to load; the raster baseline is undefined."
        }
        $withAsset = ($markers["generated_asset"] -eq "loaded")

        if ($withAsset) {
            $goldenMeanLum = 122.5
            $goldenBuckets = 64
        } else {
            $goldenMeanLum = 123.0
            $goldenBuckets = 60
        }
        $meanTolerance  = 0.5

        $fixHint = "The rendered image changed. If that was intended, re-measure and update the golden values in this script."

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
