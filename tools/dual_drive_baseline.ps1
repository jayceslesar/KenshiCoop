<#
.SYNOPSIS
  Replay the dual_drive oracle over archived runs of the cell-authority
  scenarios, so its baseline comes from history instead of an hour of fresh
  game time.

.DESCRIPTION
  dual_drive is the mutual-drive detector added for the 2026-08-08 cell
  authority bug. Before it is allowed to gate anything it needs a baseline: how
  often does it fire on runs nobody considered broken? Every scenario that
  opts into KENSHICOOP_CELL_AUTH already has archived host/join log pairs under
  tools\test-runs, and the oracle is a pure log reader, so the baseline is a
  replay rather than a re-run.

  Prints one row per run and a per-scenario tally.
#>
param(
    [string]$RunsDir = (Join-Path $PSScriptRoot "test-runs"),
    [string[]]$Scenarios = @('split_far2', 'run_apart', 'town_arrive',
                             'town_arrive_far', 'cell_auth_together')
)

Import-Module (Join-Path $PSScriptRoot "..\scripts\CoopOracles.psm1") -Force

$rows = @()
foreach ($d in (Get-ChildItem $RunsDir -Directory | Sort-Object Name)) {
    $rj = Join-Path $d.FullName "run.json"
    if (-not (Test-Path $rj)) { continue }
    try { $j = Get-Content $rj -Raw | ConvertFrom-Json } catch { continue }
    if ($Scenarios -notcontains $j.scenario) { continue }
    $h = Join-Path $d.FullName "host.log"
    $jl = Join-Path $d.FullName "join.log"
    if (-not (Test-Path $h) -or -not (Test-Path $jl)) { continue }

    Reset-GateResults
    $status = Test-DualDrive -HostFile $h -JoinFile $jl
    # -Last, not -First: Get-GateResults accumulates across the whole replay, so
    # -First pinned every row to the FIRST run's numbers and the table disagreed
    # with its own verdict column.
    $m = (Get-GateResults | Where-Object { $_.gate -eq 'dual_drive' } | Select-Object -Last 1).metrics
    $rows += [pscustomobject]@{
        run      = $d.Name
        scenario = $j.scenario
        status   = $status
        hostH    = $m.hostHands
        joinH    = $m.joinHands
        shared   = $m.sharedHands
        concur   = $m.concurrentHands
        worstS   = if ($null -ne $m.worstOverlapMs) { [int]($m.worstOverlapMs / 1000) } else { 0 }
        drvBoth  = $m.drvBothHands
        drvSust  = $m.drvSustainedHands
        drvWorstS = if ($null -ne $m.drvWorstMs) { [int]($m.drvWorstMs / 1000) } else { 0 }
    }
}

Write-Host ""
Write-Host "== dual_drive replay over archived cell-authority runs =="
$rows | Format-Table -AutoSize

Write-Host "== per-scenario tally =="
$rows | Group-Object scenario | ForEach-Object {
    $f = @($_.Group | Where-Object { $_.status -eq 'FAIL' }).Count
    $p = @($_.Group | Where-Object { $_.status -eq 'PASS' }).Count
    $s = @($_.Group | Where-Object { $_.status -eq 'SKIP' }).Count
    "{0,-18} runs={1,-3} FAIL={2,-3} PASS={3,-3} SKIP={4}" -f $_.Name, $_.Count, $f, $p, $s
}
