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

# -----------------------------------------------------------------------------
# The draw-index witness: every draw read ITS OWN per-object record
# -----------------------------------------------------------------------------
# THE assertion this harness exists to make about the per-object structured
# buffer, and the one nothing else can make.
#
# Per-object and per-material data live in structured buffers indexed by a
# per-draw root constant at b3. Those are ROOT SRVs: a bare GPU virtual address,
# no descriptor, no StructureByteStride. Nothing in the runtime, the debug layer
# or the PIX capture cross-checks that basic_vs.hlsl indexed objectBuffer with
# the constant it was handed rather than with a literal 0 - and a scene rendered
# entirely from record 0 draws every entity with the first entity's transform,
# which still looks like a scene and passes every loose threshold above.
#
# The CPU-side counters next door cannot cover it either. shadow_records,
# main_records and object_records_peak count what was WRITTEN; all three stay
# perfectly correct while the GPU reads the wrong element.
#
# So the shaders write it down. ObjectData carries recordId, set by the CPU to
# the element's own index; both vertex shaders write the recordId they LOADED
# into a UAV slot chosen by the root constant they were GIVEN. Correct indexing
# leaves the identity permutation. See gpu_draw_records.h.
#
# Raster only: the probe runs on the capture frame, and a path-traced frame runs
# neither raster pass.
if ($RasterOnly) {
    foreach ($k in @("draw_index_identity", "draw_index_mismatches",
                     "draw_index_shadow_records", "draw_index_shadow_distinct",
                     "draw_index_main_records",   "draw_index_main_distinct")) {
        if (-not $markers.ContainsKey($k)) { throw "Smoke test did not emit the '$k' marker." }
    }

    $diShadowRecords  = [uint32]$markers["draw_index_shadow_records"]
    $diShadowDistinct = [uint32]$markers["draw_index_shadow_distinct"]
    $diMainRecords    = [uint32]$markers["draw_index_main_records"]
    $diMainDistinct   = [uint32]$markers["draw_index_main_distinct"]

    Write-Host ("Draw-index witness: shadow {0}/{1} distinct, main {2}/{3} distinct, {4} mismatches" -f `
                $diShadowDistinct, $diShadowRecords, $diMainDistinct, $diMainRecords,
                $markers["draw_index_mismatches"])

    # Both passes must have drawn something, or the checks below are vacuous:
    # zero records trivially give zero distinct and zero mismatches.
    if ($diShadowRecords -lt 2) {
        throw (("draw_index_shadow_records={0}; fewer than two shadow draws means the " +
                "distinct-index check cannot distinguish correct indexing from every " +
                "draw sharing one record.") -f $diShadowRecords)
    }
    if ($diMainRecords -lt 2) {
        throw (("draw_index_main_records={0}; fewer than two main-pass draws means the " +
                "distinct-index check cannot distinguish correct indexing from every " +
                "draw sharing one record.") -f $diMainRecords)
    }

    # DISTINCT INDEX COUNT PER PASS. N draws must have read N different records.
    # Checked per pass because the two failure modes are one-sided: basic_vs
    # reading record 0 leaves the shadow pass flawless and vice versa, so a
    # single combined count would be dragged only halfway by either.
    #
    # This is what collapses to 1 when the root constant stops reaching a shader.
    if ($diShadowDistinct -ne $diShadowRecords) {
        throw (("Shadow pass read {0} distinct object records across {1} draws. " +
                "The per-draw root constant at b3 is not reaching shadow_vs.hlsl, or " +
                "shadow_vs.hlsl is not indexing objectBuffer with it - so multiple " +
                "shadow draws are rasterising with one entity's transform. A count of " +
                "1 means every draw read the same record.") -f `
                $diShadowDistinct, $diShadowRecords)
    }
    if ($diMainDistinct -ne $diMainRecords) {
        throw (("Main pass read {0} distinct object records across {1} draws. " +
                "The per-draw root constant at b3 is not reaching basic_vs.hlsl, or " +
                "basic_vs.hlsl is not indexing objectBuffer with it - so multiple " +
                "draws are shading with one entity's transform. A count of 1 means " +
                "every draw read the same record.") -f $diMainDistinct, $diMainRecords)
    }

    # THE STRONG FORM: slot i holds record i, for every allocated slot in both
    # passes. Asserted as well as the distinct counts because distinctness alone
    # would accept a PERMUTATION - every draw reading a different record, but not
    # its own - which renders every object with another object's transform and is
    # exactly what the disjoint shadow/main ranges in gpu_draw_records.h exist to
    # prevent. A permutation leaves both distinct counts perfect.
    Assert-Marker "draw_index_identity" "yes"
    if ([uint32]$markers["draw_index_mismatches"] -ne 0) {
        throw (("draw_index_mismatches={0}: that many draws read a record other than " +
                "the one the cursor allocated for them. Diff shaders/basic_vs.hlsl, " +
                "shaders/shadow_vs.hlsl and src/render/gpu_draw_records.h - ObjectData " +
                "must be byte-identical in all three, and a layout disagreement reads " +
                "shifted garbage that nothing else validates.") -f `
                $markers["draw_index_mismatches"])
    }

    # The witness must agree with the counters that are derived independently, or
    # one of the two is measuring a different frame than it claims.
    if ($diShadowRecords -ne $shadowRecords -or $diMainRecords -ne $mainRecords) {
        throw (("Draw-index witness saw shadow={0} main={1} but the record counters " +
                "reported shadow={2} main={3}. The witness readback and the record " +
                "counters are describing different frames.") -f `
                $diShadowRecords, $diMainRecords, $shadowRecords, $mainRecords)
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
    # draw_index_* markers below. Those are exact integers produced by the draw
    # loop and are independent of the scene, the assets and the lighting.
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
