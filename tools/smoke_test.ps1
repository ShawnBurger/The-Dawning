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

if (!(Test-Path -LiteralPath $exe)) {
    throw "Executable not found: $exe. Run .\SETUP_AND_BUILD.bat first."
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

$errors = Select-String -LiteralPath $log -Pattern "\[ERROR\]"
if ($errors) {
    $errors | ForEach-Object { $_.Line }
    throw "Smoke test log contains errors."
}

$warnings = Select-String -LiteralPath $log -Pattern "\[WARN\]"
if ($warnings) {
    Write-Host "Smoke test completed with warnings:"
    $warnings | ForEach-Object { Write-Host $_.Line }
} else {
    Write-Host "Smoke test passed."
}

