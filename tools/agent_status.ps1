[CmdletBinding()]
param(
    [string]$Root = "",

    [switch]$All
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$gitCommonDir = (& git -C $repo rev-parse --path-format=absolute --git-common-dir).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($gitCommonDir)) {
    throw "Unable to resolve the shared Git directory."
}
$integrationRepo = Split-Path -Parent $gitCommonDir
$rootPath = if ([string]::IsNullOrWhiteSpace($Root)) {
    Join-Path (Split-Path -Parent $integrationRepo) ".agents\worktrees"
}
elseif ([System.IO.Path]::IsPathRooted($Root)) {
    [System.IO.Path]::GetFullPath($Root)
}
else {
    [System.IO.Path]::GetFullPath((Join-Path $integrationRepo $Root))
}
$agentRoot = $rootPath.TrimEnd("\") + "\"

Push-Location $repo
try {
    $raw = @(git worktree list --porcelain)
}
finally {
    Pop-Location
}

$items = @()
$current = @{}

foreach ($line in $raw) {
    if ([string]::IsNullOrWhiteSpace($line)) {
        if ($current.ContainsKey("worktree")) {
            $items += [pscustomobject]$current
        }
        $current = @{}
        continue
    }

    $parts = $line -split " ", 2
    if ($parts.Count -lt 2) {
        continue
    }

    switch ($parts[0]) {
        "worktree" { $current["worktree"] = $parts[1] }
        "HEAD" { $current["head"] = $parts[1] }
        "branch" { $current["branch"] = ($parts[1] -replace "^refs/heads/", "") }
    }
}

if ($current.ContainsKey("worktree")) {
    $items += [pscustomobject]$current
}

$shown = 0
foreach ($item in $items) {
    $fullPath = [System.IO.Path]::GetFullPath($item.worktree)
    if (-not $All -and -not $fullPath.StartsWith($agentRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }

    $branch = if ($item.PSObject.Properties.Name -contains "branch") { $item.branch } else { "(detached)" }
    $status = @(git -C $item.worktree status --short)
    $state = if ($status.Count -gt 0) { "dirty" } else { "clean" }
    $last = git -C $item.worktree log -1 --pretty=format:"%h %s"

    Write-Host "$branch [$state]"
    Write-Host "  path: $($item.worktree)"
    Write-Host "  head: $last"

    foreach ($line in $status) {
        Write-Host "  $line"
    }

    $shown++
}

if ($shown -eq 0) {
    Write-Host "No agent worktrees found under $agentRoot"
}
