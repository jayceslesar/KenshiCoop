<#
.SYNOPSIS
  Does world parity degrade at the moment the co-location collapse releases?

.DESCRIPTION
  The collapse hands NPC authorship to the host while the squads share a cell and
  hands it back when they part, so every flip of the [cell] MAP 'collapse=' field
  is an authority handover - and a handover is where a body can fall between two
  authors. This lines the flips up against a per-dump roster-miss count and asks
  whether the misses cluster around them.

  The miss count is deliberately cruder than the world_parity gate: bodies the
  host lists in a SCENARIO WNPC dump that the join's nearest dump does not, keyed
  on the full 5-component hand. That is enough to see a cliff, and unlike the
  gate's verdict it is resolved in TIME, which is the whole question here.

  Windowing: +-15 s around each flip, against the run's own baseline outside
  those windows, so a run that is uniformly bad reads as uniformly bad rather
  than as a handover problem.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\collapse_handover.ps1 -BatchDir tools\test-runs\ab_escape_cohesion_20260809_144835
#>
param(
    [Parameter(Mandatory = $true)][string]$BatchDir,
    [double]$WindowSec = 15
)

$ErrorActionPreference = "Stop"

function Get-StampMs([string]$Line) {
    if ($Line -match '^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]') {
        return ([int]$Matches[1] * 3600000) + ([int]$Matches[2] * 60000) +
               ([int]$Matches[3] * 1000) + [int]$Matches[4]
    }
    return -1
}

function Get-Dumps([string]$LogFile) {
    # WNPC rows burst out together; a gap over 1500 ms starts a new dump. Same
    # grouping the motion oracles use, so the two agree on what a sample is.
    $dumps = New-Object System.Collections.ArrayList
    $curT = -1
    $cur = $null
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $LogFile).Path)) {
        if ($line -notlike '*SCENARIO WNPC*') { continue }
        if ($line -notmatch 'SCENARIO WNPC hand=(\d+),(\d+),(\d+),(\d+),(\d+)') { continue }
        $t = Get-StampMs $line
        if ($t -lt 0) { continue }
        $hand = "$($Matches[1]),$($Matches[2]),$($Matches[3]),$($Matches[4]),$($Matches[5])"
        if ($curT -lt 0 -or ($t - $curT) -gt 1500) {
            if ($null -ne $cur) { [void]$dumps.Add($cur) }
            $cur = [pscustomobject]@{ t = $t; hands = @{} }
        }
        $cur.hands[$hand] = 1
        $curT = $t
    }
    if ($null -ne $cur) { [void]$dumps.Add($cur) }
    return $dumps
}

function Get-Flips([string]$LogFile) {
    $flips = New-Object System.Collections.ArrayList
    $last = -1
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $LogFile).Path)) {
        if ($line -notmatch '\[cell\] MAP .*collapse=(\d)') { continue }
        $v = [int]$Matches[1]
        if ($last -ge 0 -and $v -ne $last) {
            [void]$flips.Add([pscustomobject]@{ t = (Get-StampMs $line); to = $v })
        }
        $last = $v
    }
    return $flips
}

$winMs = [int]($WindowSec * 1000)
$rows = New-Object System.Collections.ArrayList

foreach ($d in (Get-ChildItem -Directory $BatchDir | Sort-Object Name)) {
    $hostLog = Join-Path $d.FullName "host.log"
    $joinLog = Join-Path $d.FullName "join.log"
    if (-not (Test-Path $hostLog) -or -not (Test-Path $joinLog)) { continue }

    $hd = Get-Dumps $hostLog
    $jd = Get-Dumps $joinLog
    if ($hd.Count -eq 0 -or $jd.Count -eq 0) { continue }

    # The last flip of a run is the peer disconnecting, which empties the claim
    # set and legitimately un-collapses. That is teardown, not a handover, so it
    # is dropped: keeping it would score the shutdown as a parity cliff.
    $flips = @(Get-Flips $hostLog)
    $lastJoinT = ($jd | Select-Object -Last 1).t
    $flips = @($flips | Where-Object { $_.t -lt $lastJoinT })

    $inMiss = New-Object System.Collections.ArrayList
    $outMiss = New-Object System.Collections.ArrayList
    foreach ($h in $hd) {
        # Pair each host dump with the join dump nearest in time.
        $best = $null; $bestD = [int]::MaxValue
        foreach ($j in $jd) {
            $dt = [math]::Abs($j.t - $h.t)
            if ($dt -lt $bestD) { $bestD = $dt; $best = $j }
        }
        if ($null -eq $best -or $bestD -gt 2500) { continue }
        $miss = 0
        foreach ($k in $h.hands.Keys) { if (-not $best.hands.ContainsKey($k)) { $miss++ } }
        $nearFlip = $false
        foreach ($f in $flips) { if ([math]::Abs($h.t - $f.t) -le $winMs) { $nearFlip = $true } }
        if ($nearFlip) { [void]$inMiss.Add($miss) } else { [void]$outMiss.Add($miss) }
    }
    function Mean($a) {
        if ($a.Count -eq 0) { return $null }
        $s = 0.0; foreach ($v in $a) { $s += $v }
        return [math]::Round($s / $a.Count, 2)
    }
    [void]$rows.Add([pscustomobject]@{
        run        = $d.Name
        flips      = $flips.Count
        flipTimes  = (($flips | ForEach-Object { "{0:hh\:mm\:ss}->{1}" -f [timespan]::FromMilliseconds($_.t), $_.to }) -join " ")
        dumpsNear  = $inMiss.Count
        missNear   = (Mean $inMiss)
        dumpsAway  = $outMiss.Count
        missAway   = (Mean $outMiss)
    })
}

$rows | Format-Table -AutoSize run, flips, dumpsNear, missNear, dumpsAway, missAway, flipTimes |
    Out-String | Write-Host
Write-Host "  missNear / missAway = mean host-listed bodies absent from the join's"
Write-Host "  paired dump, inside vs outside +-${WindowSec}s of a collapse flip."
Write-Host "  A handover cost would show as missNear well above missAway."
