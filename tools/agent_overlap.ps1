[CmdletBinding()]
param(
    [string]$Base = "main",

    [string[]]$Branches = @()
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

Push-Location $repo
try {
    & git rev-parse --verify $Base *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "Base ref '$Base' does not exist."
    }

    if ($Branches.Count -eq 0) {
        $Branches = @(git for-each-ref --format="%(refname:short)" refs/heads/codex refs/heads/claude)
    }

    if ($Branches.Count -lt 2) {
        Write-Host "Need at least two branches to check overlap."
        exit 0
    }

    $changesByBranch = @{}
    $ownersByFile = @{}

    foreach ($branch in $Branches) {
        & git rev-parse --verify $branch *> $null
        if ($LASTEXITCODE -ne 0) {
            throw "Branch '$branch' does not exist."
        }

        $files = @(git diff --name-only "$Base...$branch")
        $changesByBranch[$branch] = $files

        foreach ($file in $files) {
            if (-not $ownersByFile.ContainsKey($file)) {
                $ownersByFile[$file] = New-Object System.Collections.Generic.List[string]
            }
            $ownersByFile[$file].Add($branch)
        }
    }

    foreach ($branch in $Branches) {
        Write-Host "$branch touches $($changesByBranch[$branch].Count) file(s)."
    }

    $overlaps = @()
    foreach ($entry in $ownersByFile.GetEnumerator()) {
        if ($entry.Value.Count -gt 1) {
            $overlaps += [pscustomobject]@{
                File = $entry.Key
                Branches = ($entry.Value -join ", ")
            }
        }
    }

    if ($overlaps.Count -eq 0) {
        Write-Host "No overlapping files detected."
        exit 0
    }

    Write-Host ""
    Write-Host "Overlapping files:"
    foreach ($overlap in ($overlaps | Sort-Object File)) {
        Write-Host "  $($overlap.File): $($overlap.Branches)"
    }

    exit 1
}
finally {
    Pop-Location
}
