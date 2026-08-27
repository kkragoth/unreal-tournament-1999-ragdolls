# One-time environment setup: provisions the gitignored thirdparty/ directory.
#
#   .\setup.ps1 -Ut99Src "C:\path\to\ut99-public-source"
#   # or auto-fetch from GitHub (see scripts/Setup-Dev.ps1):
#   .\scripts\Setup-Dev.ps1 -Configure
#
#   -Ut99Src     Path to an Unreal Tournament '99 public source distribution
#                tree (must contain Core\Inc, Engine\Inc, Core\Lib\Core.lib,
#                Engine\Lib\Engine.lib). These Epic files are NOT in this repo.
#                Any clone of https://github.com/FaultyRAM/Ut99PubSrc satisfies
#                this. Use scripts/Setup-Dev.ps1 to auto-clone it.
#   -Box3dSrc    Optional existing Box3D checkout to copy instead of cloning
#                from GitHub.
#   -Box3dRef    Ref to check out when cloning (default: the commit this
#                project was tested against). Upstream rewrote its history, so
#                the ref may not be fetchable - the script then falls back to
#                the default branch with a warning.
#   -Configure   Also run the CMake configure step afterwards.
#
# Idempotent: safe to re-run to refresh thirdparty/.
# See also: scripts/Fetch-Ut99Src.ps1 (clone/update Ut99PubSrc alone),
#           scripts/Setup-Dev.ps1 (fetch + setup + configure in one step).
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Ut99Src,
    [string]$Box3dSrc,
    [string]$Box3dRef = "67c92ab78285c0cd8fbc3120977953220c0b3b30",
    [switch]$Configure
)

$ErrorActionPreference = "Stop"
$RepoRoot   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ThirdParty = Join-Path $RepoRoot "thirdparty"

function Assert-PathExists([string]$Path, [string]$Hint) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing: $Path`n$Hint"
    }
}

function ResolveExisting([string]$Path, [string]$Hint) {
    Assert-PathExists $Path $Hint
    return (Resolve-Path -LiteralPath $Path).Path
}

Write-Host "== RagdollPkg thirdparty setup ==" -ForegroundColor Cyan

# --------------------------------------------------------------------------
# UE1 SDK subset from the UT99 public source distribution -> thirdparty/ut99sdk
# --------------------------------------------------------------------------
$Ut99Src = ResolveExisting $Ut99Src `
    "Pass -Ut99Src <path to the Unreal Tournament public source distribution>."

$coreInc    = Join-Path $Ut99Src "Core\Inc"
$engineInc  = Join-Path $Ut99Src "Engine\Inc"
$coreLib    = Join-Path $Ut99Src "Core\Lib\Core.lib"
$engineLib  = Join-Path $Ut99Src "Engine\Lib\Engine.lib"

Assert-PathExists $coreLib   "The distribution must contain the prebuilt import libraries."
Assert-PathExists $engineLib "The distribution must contain the prebuilt import libraries."

$ut99sdk = Join-Path $ThirdParty "ut99sdk"
foreach ($d in @(
    "$ut99sdk\Core\Inc",  "$ut99sdk\Core\Lib",
    "$ut99sdk\Engine\Inc", "$ut99sdk\Engine\Lib"
)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

foreach ($pair in @(
    @{ Src = $coreInc;   Dst = "$ut99sdk\Core\Inc" },
    @{ Src = $engineInc; Dst = "$ut99sdk\Engine\Inc" }
)) {
    if (Test-Path -LiteralPath $pair.Dst) { Remove-Item -Recurse -Force -LiteralPath $pair.Dst }
    Copy-Item -Recurse -Force -LiteralPath $pair.Src -Destination $pair.Dst
}
Copy-Item -Force -LiteralPath $coreLib   -Destination "$ut99sdk\Core\Lib\Core.lib"
Copy-Item -Force -LiteralPath $engineLib -Destination "$ut99sdk\Engine\Lib\Engine.lib"
Write-Host "[ok] thirdparty\ut99sdk  <- headers + import libs from $Ut99Src"

# --------------------------------------------------------------------------
# Box3D -> thirdparty/box3d (library sources only)
# --------------------------------------------------------------------------
$tmpClone = $null
if (-not $Box3dSrc) {
    Write-Host "..  cloning erincatto/box3d from GitHub..."
    $tmpClone = Join-Path ([System.IO.Path]::GetTempPath()) ("box3d-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
    git clone --quiet https://github.com/erincatto/box3d.git $tmpClone
    if ($LASTEXITCODE -ne 0) { throw "git clone of https://github.com/erincatto/box3d.git failed." }

    Push-Location $tmpClone
    git checkout --quiet $Box3dRef 2>$null
    if ($LASTEXITCODE -ne 0) {
        $head = (git rev-parse --short HEAD)
        Write-Warning "Tested ref $Box3dRef is not fetchable (upstream rewrote history). Using default branch HEAD ($head) - newer Box3D contains larger, untested changes."
        $Box3dRefActual = $head
    } else {
        $Box3dRefActual = $Box3dRef
    }
    Pop-Location
    $Box3dSrc = $tmpClone
} else {
    $Box3dSrc = ResolveExisting $Box3dSrc "Pass -Box3dSrc <path to an existing box3d checkout> or omit it to clone from GitHub."
    $Box3dRefActual = (git -C $Box3dSrc rev-parse HEAD 2>$null)
}

Assert-PathExists (Join-Path $Box3dSrc "CMakeLists.txt") "-Box3dSrc must point at a box3d repo checkout (root CMakeLists.txt missing)."

$boxDst = Join-Path $ThirdParty "box3d"
if (Test-Path -LiteralPath $boxDst) { Remove-Item -Recurse -Force -LiteralPath $boxDst }
robocopy $Box3dSrc $boxDst /E /NFL /NDL /NJH /NP /NJS `
    /XD .github benchmark data docs samples test shared extern .git .fetchcontent-cache `
    /XF build.sh build_vs2026.bat deploy_docs.sh CMakePresets.json .clang-format .gitattributes .gitignore | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with code $LASTEXITCODE" }

@"
box3d provisioned by setup.ps1 on $(Get-Date -Format "yyyy-MM-dd HH:mm")
source : $((Resolve-Path -LiteralPath $Box3dSrc).Path)
ref    : $(if ($Box3dRefActual) { $Box3dRefActual } else { "(working copy, unknown ref)" })
tested : 67c92ab78285c0cd8fbc3120977953220c0b3b30
"@ | Set-Content -Encoding ascii (Join-Path $boxDst "PROVENANCE.txt")
Write-Host "[ok] thirdparty\box3d    <- library snapshot ($(if ($Box3dRefActual) { $Box3dRefActual.Substring(0, [Math]::Min(12, $Box3dRefActual.Length)) } else { 'local copy' }))"

if ($tmpClone -and (Test-Path -LiteralPath $tmpClone)) {
    Remove-Item -Recurse -Force -LiteralPath $tmpClone
}

# --------------------------------------------------------------------------
Write-Host ""
if ($Configure) {
    cmake -S $RepoRoot -B (Join-Path $RepoRoot "build") -A Win32
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
    Write-Host "`nNext: cmake --build build --config Release --target RagdollPkg"
} else {
    Write-Host "Next:"
    Write-Host "  cmake -S . -B build -A Win32"
    Write-Host "  cmake --build build --config Release --target RagdollPkg"
}
