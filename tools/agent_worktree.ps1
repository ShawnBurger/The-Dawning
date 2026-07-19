[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("codex", "claude")]
    [string]$Agent,

    [Parameter(Mandatory = $true)]
    [string]$Task,

    [string]$Base = "main",

    [string]$Root = "",

    [switch]$Build,

    [switch]$RunSmoke,

    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Invoke-GitChecked {
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$GitArgs
    )

    & git @GitArgs
    if ($LASTEXITCODE -ne 0) {
        throw "git $($GitArgs -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Invoke-CommandChecked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory
    )

    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "$FilePath failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}

function Copy-DxcRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceRoot,

        [Parameter(Mandatory = $true)]
        [string]$DestinationRoot
    )

    $sourceDir = Join-Path $SourceRoot "build\Debug"
    $destinationDir = Join-Path $DestinationRoot "build\Debug"
    $dlls = @("dxcompiler.dll", "dxil.dll")

    foreach ($dll in $dlls) {
        $source = Join-Path $sourceDir $dll
        if (Test-Path -LiteralPath $source) {
            New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
            Copy-Item -LiteralPath $source -Destination (Join-Path $destinationDir $dll) -Force
            Write-Host "Copied $dll to $destinationDir"
        }
        else {
            Write-Warning "$dll was not found in $sourceDir. Raster builds still work; RT smoke tests need this DLL."
        }
    }
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$gitCommonDir = (& git -C $repo rev-parse --path-format=absolute --git-common-dir).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($gitCommonDir)) {
    throw "Unable to resolve the shared Git directory."
}
$integrationRepo = Split-Path -Parent $gitCommonDir
$slug = ($Task.ToLowerInvariant() -replace "[^a-z0-9]+", "-").Trim("-")
if ([string]::IsNullOrWhiteSpace($slug)) {
    $slug = "task"
}

$branch = "$Agent/$slug"
$rootPath = if ([string]::IsNullOrWhiteSpace($Root)) {
    Join-Path (Split-Path -Parent $integrationRepo) ".agents\worktrees"
}
elseif ([System.IO.Path]::IsPathRooted($Root)) {
    [System.IO.Path]::GetFullPath($Root)
}
else {
    [System.IO.Path]::GetFullPath((Join-Path $integrationRepo $Root))
}
$worktree = Join-Path $rootPath "$Agent-$slug"

Push-Location $repo
try {
    & git rev-parse --verify $Base *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "Base ref '$Base' does not exist."
    }

    $canonicalDirty = & git -C $integrationRepo status --porcelain
    if ($canonicalDirty) {
        Write-Warning "Integration checkout has uncommitted changes. The new worktree still starts from '$Base'."
    }

    & git show-ref --verify --quiet "refs/heads/$branch"
    $branchExists = ($LASTEXITCODE -eq 0)

    if (Test-Path -LiteralPath $worktree) {
        throw "Worktree path already exists: $worktree"
    }

    Write-Host "Agent:    $Agent"
    Write-Host "Task:     $Task"
    Write-Host "Branch:   $branch"
    Write-Host "Base:     $Base"
    Write-Host "Main:     $integrationRepo"
    Write-Host "Worktree: $worktree"

    if ($DryRun) {
        Write-Host "Dry run only. No branch or worktree was created."
        return
    }

    New-Item -ItemType Directory -Force -Path $rootPath | Out-Null

    if ($branchExists) {
        Invoke-GitChecked worktree add $worktree $branch
    }
    else {
        Invoke-GitChecked worktree add -b $branch $worktree $Base
    }
}
finally {
    Pop-Location
}

if ($RunSmoke -and -not $Build) {
    Write-Host "RunSmoke requested; enabling Build for the fresh worktree."
    $Build = $true
}

if ($Build) {
    Invoke-CommandChecked -FilePath (Join-Path $worktree "SETUP_AND_BUILD.bat") -WorkingDirectory $worktree
}

Copy-DxcRuntime -SourceRoot $integrationRepo -DestinationRoot $worktree

if ($RunSmoke) {
    $smoke = Join-Path $worktree "tools\smoke_test.cmd"
    Invoke-CommandChecked -FilePath $smoke -Arguments @("-RasterOnly", "-Seconds", "1.5", "-TimeoutSeconds", "8") -WorkingDirectory $worktree
    Invoke-CommandChecked -FilePath $smoke -Arguments @("-Seconds", "3", "-TimeoutSeconds", "12") -WorkingDirectory $worktree
    Invoke-CommandChecked -FilePath $smoke -Arguments @("-FullQuality", "-Seconds", "3", "-TimeoutSeconds", "12") -WorkingDirectory $worktree
}

Write-Host ""
Write-Host "Ready:"
Write-Host "  cd `"$worktree`""

if ($Agent -eq "claude") {
    Write-Host "  .\tools\claude.cmd"
}
