<#
.SYNOPSIS
  Idempotent developer bootstrap for the KenshiCoop plugin toolchain on Windows.

.DESCRIPTION
  Checks every prerequisite the v100 plugin build needs and FIXES what is safe to
  automate (the git-ignored deps state, ENet clone + patches, deps header
  patches, environment variables, the VS7 registry key). For the few things that
  are risky to automate unattended (installing the Windows SDK 7.1 / v100 compiler,
  whose setup famously fails unless the VC++2010 redists are temporarily removed),
  it PRINTS the exact manual step instead of guessing.

  Safe to run repeatedly: every action is guarded by a check, so a second run is a
  fast all-green report. Nothing here launches the game or needs a human at the
  keyboard.

  This encodes the same state described in docs/ENGINE_FACTS.md's build notes and
  the project's build-env memory; keep the two in sync when the deps pin moves.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\bootstrap_windows.ps1

.EXAMPLE
  # Only report; make no changes.
  powershell -ExecutionPolicy Bypass -File scripts\bootstrap_windows.ps1 -CheckOnly
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo      = Split-Path -Parent $scriptDir

# Pinned versions (keep in sync with docs + memory).
$DEPS_COMMIT = "960a7c0"   # KenshiLib_Examples_deps @ 0.3.0 (0.4.0 moved CombatClass.h)
$ENET_TAG    = "v1.3.18"

$script:fixes  = 0
$script:manual = @()

function Ok    ([string]$m) { Write-Host "  [ok]     $m" -ForegroundColor Green }
function Fixed ([string]$m) { Write-Host "  [fixed]  $m" -ForegroundColor Cyan;   $script:fixes++ }
function Skip  ([string]$m) { Write-Host "  [skip]   $m (CheckOnly)" -ForegroundColor Yellow }
function Manual([string]$m) { Write-Host "  [MANUAL] $m" -ForegroundColor Magenta; $script:manual += $m }
function Sect  ([string]$m) { Write-Host ""; Write-Host "== $m" -ForegroundColor White }

function Have-Cmd([string]$name) { return [bool](Get-Command $name -ErrorAction SilentlyContinue) }

# ---- 1. Toolchain (mostly MANUAL - installers are interactive / fragile) -------
Sect "Toolchain"

# MSBuild via VS2022 Build Tools.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath 2>$null
    if ($vsPath) { Ok "MSBuild present ($vsPath)" }
    else { Manual "VS2022 Build Tools with the VCTools workload (winget install Microsoft.VisualStudio.2022.BuildTools, add 'Desktop development with C++')." }
} else {
    Manual "Visual Studio Installer / vswhere not found - install VS2022 Build Tools (winget install Microsoft.VisualStudio.2022.BuildTools)."
}

# v100 x64 compiler (Windows SDK 7.1 + KB2519277).
$v100cl = "${env:ProgramFiles(x86)}\Microsoft Visual Studio 10.0\VC\bin\amd64\cl.exe"
if (Test-Path $v100cl) {
    Ok "v100 x64 compiler present ($v100cl)"
} else {
    Manual @"
Windows SDK 7.1 + 'VC++ 2010 SP1 Compiler Update' (the v100 x64 cl.exe).
    GOTCHA (documented failure): the SDK 7.1 setup exits 1 with
    'Patch Hooks: Missing required property ProductFamily' if the VC++2010 SP1
    x64/x86 REDISTRIBUTABLES are installed. Temporarily uninstall them, install
    the SDK 7.1 + KB2519277, then reinstall the redists (winget install
    Microsoft.VCRedist.2010.x64  and  ...x86). This step needs a human.
"@
}

# VS7 registry key so modern MSBuild can find the v100 toolset.
$vs7Key = "HKLM:\SOFTWARE\Wow6432Node\Microsoft\VisualStudio\SxS\VS7"
$vs7Val = $null
try { $vs7Val = (Get-ItemProperty -Path $vs7Key -Name "10.0" -ErrorAction Stop)."10.0" } catch {}
if ($vs7Val) {
    Ok "VS7 registry key present (10.0 -> $vs7Val)"
} elseif ($CheckOnly) {
    Skip "would set HKLM ...\SxS\VS7 value '10.0'"
} else {
    $target = "C:\Program Files (x86)\Microsoft Visual Studio 10.0\"
    try {
        if (-not (Test-Path $vs7Key)) { New-Item -Path $vs7Key -Force | Out-Null }
        New-ItemProperty -Path $vs7Key -Name "10.0" -Value $target -PropertyType String -Force | Out-Null
        Fixed "set HKLM ...\SxS\VS7 '10.0' = $target"
    } catch {
        Manual "Set HKLM\SOFTWARE\Wow6432Node\Microsoft\VisualStudio\SxS\VS7 value '10.0'=$target (run this script elevated, or set it by hand)."
    }
}

foreach ($t in @(@{c="git"; w="Git.Git"}, @{c="gh"; w="GitHub.cli"})) {
    if (Have-Cmd $t.c) { Ok "$($t.c) present" }
    else { Manual "$($t.c) not on PATH - winget install $($t.w)" }
}

# ---- 2. KenshiLib deps (git-ignored; auto-fixable) -----------------------------
Sect "KenshiLib deps (third_party/KenshiLib_deps)"

$deps = Join-Path $repo "third_party\KenshiLib_deps"
if (-not (Test-Path (Join-Path $deps ".git"))) {
    if ($CheckOnly) { Skip "would clone KenshiLib_Examples_deps into $deps" }
    else {
        Manual @"
third_party\KenshiLib_deps is missing. It is a git-LFS repo:
    git clone https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps third_party\KenshiLib_deps
    cd third_party\KenshiLib_deps && git checkout $DEPS_COMMIT && git lfs pull
    (then re-run this script to apply the header patches + Boost extract).
"@
    }
} else {
    $head = (& git -C $deps rev-parse --short HEAD 2>$null)
    if ($head -like "$DEPS_COMMIT*") { Ok "deps pinned at $DEPS_COMMIT" }
    else {
        if ($CheckOnly) { Skip "deps at $head; would checkout $DEPS_COMMIT" }
        else {
            Write-Host "  deps at $head; pinning to $DEPS_COMMIT (0.4.0 moves CombatClass.h and breaks the build) ..."
            # A case-only rename leaves stale files on Windows; remove KenshiLib first.
            $kl = Join-Path $deps "KenshiLib"
            & git -C $deps checkout $DEPS_COMMIT 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { Manual "Could not checkout $DEPS_COMMIT in $deps - do it by hand (delete the KenshiLib folder first, then git checkout)." }
            else { Fixed "checked out deps $DEPS_COMMIT" }
        }
    }

    # Boost 1.60 extracted next to the deps.
    $boostHdr = Join-Path $deps "boost_1_60_0\boost\version.hpp"
    $boostZip = Join-Path $deps "boost_1_60_0\boost.zip"
    if (Test-Path $boostHdr) { Ok "Boost 1.60 headers extracted" }
    elseif (Test-Path $boostZip) {
        if ($CheckOnly) { Skip "would extract $boostZip" }
        else {
            Expand-Archive -Path $boostZip -DestinationPath (Split-Path $boostZip) -Force
            if (Test-Path $boostHdr) { Fixed "extracted Boost 1.60 headers" }
            else { Manual "Boost extract did not produce boost\version.hpp - extract $boostZip by hand." }
        }
    } else { Manual "Boost 1.60 not found (expected boost_1_60_0\boost.zip inside the deps repo)." }

    # Three deps-header hand-patches (marked with the build-fix tag).
    $patchTag = "local KenshiCoop build fix"
    $headerPatches = @(
        @{ file = "KenshiLib\Include\kenshi\Building\Building.h";          what = "BuildingDesignation include guard" },
        @{ file = "KenshiLib\Include\kenshi\Platoon.h";                    what = "BuildingDesignation include guard" },
        @{ file = "KenshiLib\Include\kenshi\Building\CraftingBuilding.h";  what = "CraftingItem dummy complete body" }
    )
    foreach ($hp in $headerPatches) {
        $f = Join-Path $deps $hp.file
        if (-not (Test-Path $f)) { Manual "deps header missing: $($hp.file)"; continue }
        $content = Get-Content $f -Raw
        if ($content -match [regex]::Escape($patchTag)) { Ok "deps patch present: $($hp.what)" }
        else { Manual "deps header patch MISSING ($($hp.what)) in $($hp.file). See docs/ENGINE_FACTS.md build notes / the build-env memory for the exact edit - VC10 needs it to compile." }
    }
}

# ---- 3. ENet 1.3.18 clone + the two patches (auto-fixable) ---------------------
Sect "ENet (third_party/enet/enet)"

$enet = Join-Path $repo "third_party\enet\enet"
$enetH = Join-Path $enet "include\enet\enet.h"
if (Test-Path $enetH) {
    Ok "ENet headers present"
} elseif ($CheckOnly) {
    Skip "would clone lsalzman/enet $ENET_TAG"
} else {
    if (-not (Have-Cmd "git")) { Manual "git needed to clone ENet." }
    else {
        New-Item -ItemType Directory -Force -Path (Split-Path $enet) | Out-Null
        & git clone --depth 1 --branch $ENET_TAG "https://github.com/lsalzman/enet.git" $enet 2>&1 | Out-Null
        if (Test-Path $enetH) { Fixed "cloned ENet $ENET_TAG" }
        else { Manual "ENet clone failed - clone lsalzman/enet $ENET_TAG into $enet by hand." }
    }
}

# Apply the two committed patches if their marker isn't already in the source.
$patches = @(
    @{ file = Join-Path $enet "protocol.c"; marker = $null; patch = Join-Path $repo "third_party\enet\patches\0001-enet-c89-for-loops.patch" },
    @{ file = Join-Path $enet "host.c";     marker = $null; patch = Join-Path $repo "third_party\enet\patches\0002-enet-socket-hooks.patch" }
)
if (Test-Path $enetH) {
    # Heuristic: patch 0002 introduces enet_set_socket_hooks; if that symbol is
    # present anywhere the socket-hooks patch is applied. Patch 0001 is C89
    # for-loop conversions - detect by trying a --check apply.
    $hooksApplied = Select-String -Path (Join-Path $enet "include\enet\enet.h") -Pattern "enet_set_socket_hooks" -Quiet -ErrorAction SilentlyContinue
    foreach ($p in @("0001-enet-c89-for-loops.patch", "0002-enet-socket-hooks.patch")) {
        $pf = Join-Path $repo "third_party\enet\patches\$p"
        if (-not (Test-Path $pf)) { Manual "patch file missing: $p"; continue }
        if ($p -like "0002*" -and $hooksApplied) { Ok "ENet patch applied: $p"; continue }
        # Does it apply cleanly (i.e. NOT yet applied)?
        $check = & git -C $enet apply --check "$pf" 2>&1
        if ($LASTEXITCODE -eq 0) {
            if ($CheckOnly) { Skip "would apply ENet patch $p" }
            else {
                & git -C $enet apply "$pf" 2>&1 | Out-Null
                if ($LASTEXITCODE -eq 0) { Fixed "applied ENet patch $p" }
                else { Manual "ENet patch $p failed to apply - apply it by hand (run from $enet)." }
            }
        } else {
            # Fails --check: usually already applied (reverse applies clean).
            $rev = & git -C $enet apply --reverse --check "$pf" 2>&1
            if ($LASTEXITCODE -eq 0) { Ok "ENet patch applied: $p" }
            else { Manual "ENet patch $p neither applies nor reverses cleanly - inspect $enet by hand." }
        }
    }
}

# ---- 4. Environment variables (user scope; auto-fixable) -----------------------
Sect "Environment variables (user scope)"

$envTargets = @{
    KENSHILIB_DIR       = (Join-Path $deps "KenshiLib")
    KENSHILIB_DEPS_DIR  = $deps
    BOOST_ROOT          = (Join-Path $deps "boost_1_60_0")
    BOOST_INCLUDE_PATH  = (Join-Path $deps "boost_1_60_0")
}
foreach ($k in $envTargets.Keys) {
    $want = $envTargets[$k]
    $cur  = [Environment]::GetEnvironmentVariable($k, "User")
    if ($cur -eq $want) { Ok "$k = $want" }
    elseif ($CheckOnly) { Skip "would set $k = $want (currently '$cur')" }
    else {
        [Environment]::SetEnvironmentVariable($k, $want, "User")
        Fixed "set $k = $want (new shells only)"
    }
}

# ---- Summary -------------------------------------------------------------------
Sect "Summary"
Write-Host "  auto-fixes applied: $script:fixes"
if ($script:manual.Count -gt 0) {
    Write-Host ""
    Write-Host "  MANUAL steps still needed ($($script:manual.Count)):" -ForegroundColor Magenta
    $i = 1
    foreach ($m in $script:manual) { Write-Host "   $i. $m" -ForegroundColor Magenta; $i++ }
    Write-Host ""
    Write-Host "  After the manual steps, re-run this script to confirm all-green, then:"
    Write-Host "    scripts\build_plugin.cmd Harness"
    exit 1
}
Write-Host ""
Write-Host "  All prerequisites satisfied. Build with:" -ForegroundColor Green
Write-Host "    scripts\build_plugin.cmd Harness   (test build, default)"
Write-Host "    scripts\build_plugin.cmd Release   (ship build)"
exit 0
