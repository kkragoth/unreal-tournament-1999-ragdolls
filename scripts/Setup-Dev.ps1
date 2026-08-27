<#
.SYNOPSIS
    One-command dev bootstrap: fetch UT99 public source + provision thirdparty/ + configure build.

.DESCRIPTION
    Convenience wrapper around Fetch-Ut99Src.ps1 + setup.ps1 for new clones.

    - If -Ut99Src is omitted, clones/updates https://github.com/FaultyRAM/Ut99PubSrc
      via scripts/Fetch-Ut99Src.ps1 (default cache: thirdparty/_cache/Ut99PubSrc,
      gitignored) and uses that as the source.
    - If -Ut99Src is provided, uses that existing checkout directly (no fetch).
    - Forwards Box3D / configure options to setup.ps1 and optionally passes
      -DGAME_SYSTEM_DIR to CMake for direct deploy into a retail install.

.PARAMETER Ut99Src
    Path to an existing UT99 public source checkout (any clone of
    https://github.com/FaultyRAM/Ut99PubSrc). If omitted, the source is
    auto-fetched.

.PARAMETER Ut99SrcUrl
    Git URL used when auto-fetching. Defaults to https://github.com/FaultyRAM/Ut99PubSrc.git

.PARAMETER Ut99SrcDir
    Cache directory for the auto-fetched source when -Ut99Src is omitted.
    Defaults to <repo>/thirdparty/_cache/Ut99PubSrc. Alias for Fetch-Ut99Src.ps1 -Destination.

.PARAMETER Box3dSrc
    Optional existing Box3D checkout to copy instead of cloning from GitHub (forwarded to setup.ps1).

.PARAMETER Box3dRef
    Box3D git ref to checkout when cloning (forwarded to setup.ps1).

.PARAMETER Configure
    Also run CMake configure after provisioning (forwarded to setup.ps1, plus
    -DGAME_SYSTEM_DIR handling).

.PARAMETER GameSystemDir
    Retail UT99 System directory that should receive RagdollPkg.dll. When
    combined with -Configure, configures CMake with -DGAME_SYSTEM_DIR=<path>
    so `cmake --build` deploys directly. Without -Configure the value is
    still validated and a hint is printed.

.PARAMETER Branch
    Branch to fetch when auto-cloning (forwarded to Fetch-Ut99Src.ps1).

.EXAMPLE
    .\scripts\Setup-Dev.ps1 -Configure
    # fetch -> thirdparty\_cache\Ut99PubSrc, provision, cmake -A Win32

.EXAMPLE
    .\scripts\Setup-Dev.ps1 -Ut99Src "C:\dev\Ut99PubSrc" -Configure

.EXAMPLE
    .\scripts\Setup-Dev.ps1 -Configure -GameSystemDir "C:\Unreal Tournament\System"
#>
[CmdletBinding()]
param(
    [string]$Ut99Src,
    [string]$Ut99SrcUrl = "https://github.com/FaultyRAM/Ut99PubSrc.git",
    [string]$Ut99SrcDir,
    [string]$Box3dSrc,
    [string]$Box3dRef,
    [switch]$Configure,
    [string]$GameSystemDir,
    [string]$Branch
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$FetchScript = Join-Path $PSScriptRoot "Fetch-Ut99Src.ps1"
$SetupScript = Join-Path $RepoRoot "setup.ps1"

if (-not (Test-Path -LiteralPath $FetchScript)) { throw "Missing helper: $FetchScript" }
if (-not (Test-Path -LiteralPath $SetupScript)) { throw "Missing: $SetupScript" }

Write-Host "== RagdollPkg dev setup ==" -ForegroundColor Cyan

# --------------------------------------------------------------------------
# Resolve UT99 source (fetch if needed)
# --------------------------------------------------------------------------
$resolvedUt99Src = $null
if ($Ut99Src) {
    if (-not (Test-Path -LiteralPath $Ut99Src)) { throw "Missing -Ut99Src path: $Ut99Src" }
    $resolvedUt99Src = (Resolve-Path -LiteralPath $Ut99Src).Path
    Write-Host "[use] existing UT99 source: $resolvedUt99Src" -ForegroundColor DarkGray
} else {
    $fetchParams = @{
        Url = $Ut99SrcUrl
    }
    if ($Ut99SrcDir) { $fetchParams.Destination = $Ut99SrcDir }
    if ($Branch)     { $fetchParams.Branch = $Branch }

    Write-Host "..  no -Ut99Src given — fetching from $Ut99SrcUrl" -ForegroundColor DarkGray
    $fetched = & $FetchScript @fetchParams
    if ($LASTEXITCODE -ne 0) { throw "Fetch-Ut99Src.ps1 failed." }
    # Fetch script emits path as last output; handle array output
    if ($fetched -is [Array]) { $resolvedUt99Src = $fetched[-1] } else { $resolvedUt99Src = $fetched }
    $resolvedUt99Src = $resolvedUt99Src.ToString().Trim()
    Write-Host "[ok] UT99 source: $resolvedUt99Src" -ForegroundColor Green
}

# --------------------------------------------------------------------------
# Validate GameSystemDir early
# --------------------------------------------------------------------------
if ($GameSystemDir) {
    $GameSystemDir = [System.IO.Path]::GetFullPath($GameSystemDir)
    if (-not (Test-Path -LiteralPath $GameSystemDir)) {
        Write-Warning "GameSystemDir does not exist yet: $GameSystemDir (CMake will create it on configure, but verify the path is correct.)"
    }
}

# --------------------------------------------------------------------------
# Invoke setup.ps1 (provision thirdparty/ut99sdk + thirdparty/box3d)
# --------------------------------------------------------------------------
$setupParams = @{
    Ut99Src = $resolvedUt99Src
}
if ($Box3dSrc) { $setupParams.Box3dSrc = $Box3dSrc }
if ($Box3dRef) { $setupParams.Box3dRef = $Box3dRef }

# When GameSystemDir is requested we handle CMake configure ourselves so we
# can inject -DGAME_SYSTEM_DIR. Otherwise forward -Configure directly.
$doConfigure = $Configure.IsPresent
$forwardConfigure = $doConfigure -and -not $GameSystemDir

if ($forwardConfigure) { $setupParams.Configure = $true }

# Build display string for logging (hashtable -> args)
$displayArgs = @("-Ut99Src `"$resolvedUt99Src`"")
if ($Box3dSrc) { $displayArgs += "-Box3dSrc `"$Box3dSrc`"" }
if ($Box3dRef) { $displayArgs += "-Box3dRef $Box3dRef" }
if ($forwardConfigure) { $displayArgs += "-Configure" }

Write-Host ""
Write-Host ">> & setup.ps1 $($displayArgs -join ' ')" -ForegroundColor DarkGray
& $SetupScript @setupParams
# setup.ps1 uses robocopy which returns 1-7 on success; only >=8 is failure
# (already handled inside setup.ps1). Reset LASTEXITCODE so callers don't
# see a non-zero success code.
if ($LASTEXITCODE -ge 8) { throw "setup.ps1 failed (robocopy exit $LASTEXITCODE)." }
$global:LASTEXITCODE = 0

# If GameSystemDir + Configure, run cmake configure with the extra define
if ($doConfigure -and $GameSystemDir) {
    Write-Host ""
    Write-Host ">> cmake configure with -DGAME_SYSTEM_DIR=`"$GameSystemDir`"" -ForegroundColor DarkGray
    $cmakeArgs = @("-S", $RepoRoot, "-B", (Join-Path $RepoRoot "build"), "-A", "Win32", "-DGAME_SYSTEM_DIR=$GameSystemDir")
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
    Write-Host "[ok] configured with GAME_SYSTEM_DIR=$GameSystemDir" -ForegroundColor Green
} elseif ($GameSystemDir -and -not $doConfigure) {
    Write-Host ""
    Write-Warning "GameSystemDir was provided but -Configure was not. Run with -Configure to apply it, or configure manually:"
    Write-Host "  cmake -S . -B build -A Win32 -DGAME_SYSTEM_DIR=`"$GameSystemDir`"" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "== Done ==" -ForegroundColor Green
Write-Host "  UT99 source : $resolvedUt99Src"
Write-Host "  thirdparty  : $RepoRoot\thirdparty"
if ($doConfigure) {
    Write-Host "  next      : cmake --build build --config Release --target RagdollPkg"
    if ($GameSystemDir) {
        Write-Host "            (DLL will land in $GameSystemDir)"
    } else {
        Write-Host "            copy System\RagdollPkg.dll `"<retail>\System\`" to deploy"
    }
} else {
    Write-Host "  next      : .\scripts\Setup-Dev.ps1 -Configure  # or: cmake -S . -B build -A Win32"
}
