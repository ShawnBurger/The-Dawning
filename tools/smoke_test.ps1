param(
    [double]$Seconds = 4.0,
    [switch]$RasterOnly,
    [switch]$FullQuality,
    [int]$TimeoutSeconds = 15
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\Debug\TheDawningV3.exe"
$log = Join-Path $root "build\Debug\TheDawning.log"
$shaderInclude = Join-Path $root "build\Debug\shaders\display_common.hlsli"

if (!(Test-Path -LiteralPath $exe)) {
    throw "Executable not found: $exe. Run .\SETUP_AND_BUILD.bat first."
}
if (!(Test-Path -LiteralPath $shaderInclude)) {
    throw "Shader include not found in output: $shaderInclude. Rebuild before running smoke tests."
}

if (Test-Path -LiteralPath $log) {
    Remove-Item -LiteralPath $log -Force
}

$arguments = @("--smoke", "--smoke-seconds=$Seconds")
if (!$RasterOnly) {
    $arguments += "--smoke-rt"
}
if ($FullQuality) {
    $arguments += "--smoke-full"
}

$process = Start-Process -FilePath $exe `
    -WorkingDirectory (Split-Path -Parent $exe) `
    -ArgumentList $arguments `
    -PassThru

if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-Process -Id $process.Id -Force
    throw "Smoke test timed out after $TimeoutSeconds seconds."
}

if ($process.ExitCode -ne 0) {
    if (Test-Path -LiteralPath $log) {
        Get-Content -LiteralPath $log -Tail 80
    }
    throw "Smoke test failed with exit code $($process.ExitCode)."
}

if (!(Test-Path -LiteralPath $log)) {
    throw "Smoke test did not create a log: $log"
}

$logText = Get-Content -LiteralPath $log -Raw

$errors = Select-String -LiteralPath $log -Pattern "\[ERROR\]"
if ($errors) {
    $errors | ForEach-Object { $_.Line }
    throw "Smoke test log contains errors."
}

$warnings = Select-String -LiteralPath $log -Pattern "\[WARN\]"
if ($warnings) {
    Write-Host "Smoke test completed with warnings:"
    $warnings | ForEach-Object { Write-Host $_.Line }
}

if ($logText -notmatch "Smoke mode complete") {
    throw "Smoke test log did not record completion."
}
if ($logText -notmatch "Compiled shader: shaders/basic_ps.hlsl") {
    throw "Smoke test did not compile the raster pixel shader."
}

if ($RasterOnly) {
    if ($logText -match "Path tracing initialized") {
        throw "Raster-only smoke unexpectedly initialized path tracing."
    }
    Write-Host "Smoke test passed (raster)."
} else {
    if ($logText -notmatch "Path tracing initialized") {
        throw "Path tracing smoke did not initialize DXR."
    }
    if ($logText -notmatch "Smoke mode: path tracing enabled") {
        throw "Path tracing smoke did not enable path tracing."
    }
    if ($logText -notmatch "DXC compiled: shaders/path_trace.hlsl") {
        throw "Path tracing smoke did not compile the DXR shader library."
    }

    if ($FullQuality) {
        if ($logText -notmatch "Smoke mode enabled \(rt=yes, full=yes") {
            throw "Full-quality smoke did not request full RT quality."
        }
        Write-Host "Smoke test passed (path tracing full)."
    } else {
        Write-Host "Smoke test passed (path tracing stable)."
    }
}
