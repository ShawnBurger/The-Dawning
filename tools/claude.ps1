[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ClaudeArgs
)

$ErrorActionPreference = "Stop"

function Resolve-ClaudeCode {
    $command = Get-Command claude -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $knownPaths = @(
        (Join-Path $env:USERPROFILE ".local\bin\claude.exe"),
        (Join-Path $env:USERPROFILE ".claude\local\claude.exe"),
        (Join-Path $env:APPDATA "npm\claude.cmd"),
        (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links\claude.exe")
    )

    foreach ($path in $knownPaths) {
        if (Test-Path -LiteralPath $path) {
            return $path
        }
    }

    $desktopPatterns = @(
        (Join-Path $env:APPDATA "Claude\claude-code\*\claude.exe"),
        (Join-Path $env:LOCALAPPDATA "Packages\Claude_*\LocalCache\Roaming\Claude\claude-code\*\claude.exe")
    )
    $desktopBuild = $desktopPatterns |
        ForEach-Object { Get-ChildItem -Path $_ -File -ErrorAction SilentlyContinue } |
        Sort-Object -Property @{ Expression = {
            try {
                [version]$_.Directory.Name
            }
            catch {
                [version]"0.0.0"
            }
        } } -Descending |
        Select-Object -First 1

    if ($desktopBuild) {
        return $desktopBuild.FullName
    }

    return $null
}

$claude = Resolve-ClaudeCode
if (-not $claude) {
    throw "Claude Code was not found on PATH or in a standard Windows install location."
}

& $claude @ClaudeArgs
exit $LASTEXITCODE
