<#
.SYNOPSIS
    Clone or update a local checkout of the UT99 public source distribution.

.DESCRIPTION
    Fetches https://github.com/FaultyRAM/Ut99PubSrc (mirror of Epic's UT99
    public source distribution) to a local directory. Idempotent: if the
    destination already contains a git checkout it is updated via `git fetch`
    / `git pull`; otherwise a fresh `git clone` is performed.

    After fetching, validates that the expected SDK subset is present
    (Core/Inc, Engine/Inc, Core/Lib/Core.lib, Engine/Lib/Engine.lib) which
    is consumed by setup.ps1 -> thirdparty/ut99sdk.

.PARAMETER Destination
    Local directory to clone/update. Defaults to <repo>/thirdparty/_cache/Ut99PubSrc
    (gitignored via thirdparty/). Any existing clone of FaultyRAM/Ut99PubSrc or
    the original Epic distribution tree can be pointed at via setup.ps1 -Ut99Src
    instead — this script is just the auto-fetch helper.

.PARAMETER Url
    Git URL to clone from. Defaults to https://github.com/FaultyRAM/Ut99PubSrc.git

.PARAMETER Branch
    Branch to checkout (passed as `git clone --branch <Branch>` / `git checkout <Branch>`).
    Omit to use the remote's default branch.

.PARAMETER Depth
    Shallow clone depth (0 = full history, default). Passed as `git clone --depth <Depth>`.

.PARAMETER Force
    If Destination exists but is not a git repo, delete and re-clone instead of failing.

.EXAMPLE
    .\scripts\Fetch-Ut99Src.ps1
    # -> thirdparty\_cache\Ut99PubSrc

.EXAMPLE
    .\scripts\Fetch-Ut99Src.ps1 -Destination "C:\dev\Ut99PubSrc" -Branch master
#>
[CmdletBinding()]
param(
    [string]$Destination,
    [string]$Url = "https://github.com/FaultyRAM/Ut99PubSrc.git",
    [string]$Branch,
    [int]$Depth = 0,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Destination) {
    $Destination = Join-Path $RepoRoot "thirdparty\_cache\Ut99PubSrc"
}

# Resolve to absolute path without requiring it to exist yet
$Destination = [System.IO.Path]::GetFullPath($Destination)

function Assert-GitAvailable {
    try { git --version | Out-Null } catch { throw "git not found on PATH. Install Git for Windows (https://git-scm.com/download/win)." }
    if ($LASTEXITCODE -ne 0) { throw "git not found on PATH. Install Git for Windows." }
}

function Test-Ut99SrcLayout([string]$Root) {
    $missing = @()
    foreach ($p in @("Core\Inc", "Engine\Inc", "Core\Lib\Core.lib", "Engine\Lib\Engine.lib")) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $p))) { $missing += $p }
    }
    return $missing
}

Assert-GitAvailable

$gitDir = Join-Path $Destination ".git"
$destExists = Test-Path -LiteralPath $Destination

Write-Host "== Fetch UT99 public source ==" -ForegroundColor Cyan
Write-Host "  url  : $Url"
Write-Host "  dest : $Destination"

if ($destExists) {
    if (Test-Path -LiteralPath $gitDir) {
        Write-Host "..  existing git checkout found — updating..." -ForegroundColor DarkGray
        $branchArgs = @()
        if ($Branch) { $branchArgs = @("--branch", $Branch) }

        # Fetch latest
        git -C $Destination fetch --quiet --prune
        if ($LASTEXITCODE -ne 0) { throw "git fetch failed for $Destination" }

        if ($Branch) {
            # Ensure branch exists locally or track remote
            $hasBranch = $false
            git -C $Destination rev-parse --verify $Branch 2>$null
            if ($LASTEXITCODE -eq 0) { $hasBranch = $true }
            git -C $Destination rev-parse --verify "origin/$Branch" 2>$null
            if ($LASTEXITCODE -eq 0) { $hasBranch = $true }

            git -C $Destination checkout $Branch --quiet 2>$null
            if ($LASTEXITCODE -ne 0) {
                git -C $Destination checkout -b $Branch "origin/$Branch" --quiet
                if ($LASTEXITCODE -ne 0) { throw "git checkout $Branch failed." }
            }
        }

        # Fast-forward pull (or just reset to origin branch if diverged)
        $currentBranch = (git -C $Destination rev-parse --abbrev-ref HEAD 2>$null).Trim()
        if ($currentBranch -and $currentBranch -ne "HEAD") {
            git -C $Destination pull --ff-only --quiet 2>$null
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "git pull --ff-only failed (diverged?). Trying reset to origin/$currentBranch..."
                git -C $Destination fetch --quiet origin $currentBranch 2>$null
                git -C $Destination reset --hard "origin/$currentBranch" --quiet 2>$null
                if ($LASTEXITCODE -ne 0) { throw "Failed to update $Destination to origin/$currentBranch" }
            }
        } else {
            # Detached HEAD — just fetch, leave as-is
            Write-Warning "Detached HEAD in $Destination — fetched but not advancing HEAD."
        }
        Write-Host "[ok] updated $Destination" -ForegroundColor Green
    } else {
        if ($Force) {
            Write-Warning "Destination exists but is not a git repo — -Force: removing and re-cloning."
            Remove-Item -Recurse -Force -LiteralPath $Destination
            $destExists = $false
        } else {
            throw "Destination exists but is not a git repo: $Destination`nUse -Force to replace it, or pass a different -Destination."
        }
    }
}

if (-not (Test-Path -LiteralPath $Destination)) {
    $parent = Split-Path -Parent $Destination
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    $cloneArgs = @("clone", "--quiet")
    if ($Branch) { $cloneArgs += @("--branch", $Branch) }
    if ($Depth -gt 0) { $cloneArgs += @("--depth", "$Depth") }
    $cloneArgs += @($Url, $Destination)
    Write-Host "..  cloning..." -ForegroundColor DarkGray
    & git @cloneArgs
    if ($LASTEXITCODE -ne 0) { throw "git clone failed (url: $Url, dest: $Destination)" }
    Write-Host "[ok] cloned $Destination" -ForegroundColor Green
}

# Validate layout
$missing = Test-Ut99SrcLayout $Destination
if ($missing.Count -gt 0) {
    throw "Checkout at $Destination is missing expected UT99 SDK files:`n  $($missing -join "`n  ")`nIs this the correct UT99 public source tree? Expected https://github.com/FaultyRAM/Ut99PubSrc"
}

Write-Host "[ok] UT99 source ready at $Destination" -ForegroundColor Green
# Emit path for composability (Setup-Dev.ps1 captures this)
$Destination
