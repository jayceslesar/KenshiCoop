# oracles/CoreChecks.ps1 - run-health gates (monolith split of
# CoopOracles.psm1, 2026-07-12): Test-LogHealth, Test-NoCheckFail,
# Test-ScenarioResultPass, Test-ClockSync - the gates every scenario runs
# regardless of domain. Dot-sourced by CoopOracles.psm1 (module scope).
# Must NOT: change gate names or "SCENARIO RESULT"/"CHECK FAIL" phrasing -
# scenarios.psd1 and the C++ log contract key on them (resources/CODE_MAP.md).
# ---- Log health / scenario-result gates ---------------------------------------

# Evaluate one client's log health. PASS if it reached gameplay, exited cleanly
# (did NOT crash mid-run), and logged no ERROR lines. The clean-exit marker
# differs by mode: the self-exit timer logs "test duration elapsed", while a
# scenario logs "SCENARIO RESULT".
function Test-LogHealth {
    param([string]$File, [string]$Label, [bool]$Required, [string]$CleanPattern)
    $gate = "health_$Label"
    if (-not (Test-Path $File)) {
        if ($Required) {
            Write-Host "  [$Label] FAIL - no log produced"
            return (Add-GateResult -Name $gate -Status FAIL -Detail "no log produced")
        }
        Write-Host "  [$Label] skipped (not launched)"
        return (Add-GateResult -Name $gate -Status SKIP -Detail "not launched")
    }
    $text     = Get-Content $File -Raw
    $errs     = @(Select-String -Path $File -Pattern "\] ERROR:" -ErrorAction SilentlyContinue)
    $reached  = $text -match "gameplay started"
    $cleanEnd = $text -match $CleanPattern
    $ok = ($reached -and $cleanEnd -and $errs.Count -eq 0)
    $why = @()
    if (-not $reached)  { $why += "never reached gameplay" }
    if (-not $cleanEnd) { $why += "no clean exit (likely crashed)" }
    if ($errs.Count -gt 0) { $why += "$($errs.Count) error line(s)" }
    $verdict = if ($ok) { "PASS" } else { "FAIL - " + ($why -join "; ") }
    Write-Host "  [$Label] $verdict"
    foreach ($e in ($errs | Select-Object -First 5)) { Write-Host "      $($e.Line)" }
    return (Add-GateResult -Name $gate -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ errors = $errs.Count } -Detail ($why -join "; "))
}

# Both processes must have loaded the SAME plugin build ("KenshiCoop: build
# <date> <time>" is __DATE__/__TIME__ of the DLL each process actually mapped).
# A stale DLL on one install invalidates every cross-client conclusion, and a
# protocol bump is the only thing that ever made it visible before this gate.
function Test-BuildMatch {
    param([string]$HostFile, [string]$JoinFile)
    $gate = "build_match"
    $rx = "KenshiCoop: build (\w{3} +\d+ \d{4} \d\d:\d\d:\d\d)"
    $h = $null; $j = $null
    if (Test-Path $HostFile) { $m = Select-String -Path $HostFile -Pattern $rx -ErrorAction SilentlyContinue | Select-Object -First 1; if ($m) { $h = $m.Matches[0].Groups[1].Value } }
    if (Test-Path $JoinFile) { $m = Select-String -Path $JoinFile -Pattern $rx -ErrorAction SilentlyContinue | Select-Object -First 1; if ($m) { $j = $m.Matches[0].Groups[1].Value } }
    if ($null -eq $h -or $null -eq $j) {
        Write-Host "  BUILD-MATCH SKIP - build stamp missing (host='$h' join='$j')"
        return (Add-GateResult -Name $gate -Status SKIP -Detail "build stamp missing")
    }
    $ok = ($h -eq $j)
    Write-Host ("  BUILD-MATCH " + $(if ($ok) { "PASS" } else { "FAIL" }) + " - host build '$h' join build '$j'")
    if (-not $ok) { Write-Host "    NOTE: one install ran a STALE DLL - the run is invalid; redeploy (scripts\deploy.cmd) and rerun" }
    return (Add-GateResult -Name $gate -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostBuild = $h; joinBuild = $j } -Detail $(if ($ok) { "" } else { "host '$h' != join '$j' (stale DLL)" }))
}

# Engine-side bookkeeping integrity, read from the archived kenshi_info.log
# (run_test.ps1 copies it as <label>_engine.log; absent for older runs -> SKIP).
#
# Two lines Kenshi prints when its object bookkeeping has been damaged:
#   "<obj> was not removed from any zones"  - destroyed while a zone still holds
#                                             a pointer to it (a dangling entry)
#   "alredy has destroy reason <a> (<b>)"   - destroyed twice; the engine's typo
#
# The second names both destroy reasons, so a line mentioning "coop-" is one WE
# caused - that is the tripwire, and it is what the 2026-08-03 join crash left
# behind (a world-item proxy culled after its block had already unloaded).
# Unattributed lines are reported but not judged: Kenshi does this on its own
# too, and we have no baseline for how often.
#
# ADVISORY for now (not in the always-on gate list) precisely because that
# baseline is missing. Promote it to gating once a full tier run shows the
# coop-attributed count is reliably zero.
function Test-EngineIntegrity {
    param([string]$OutDir)
    $gate  = "engine_integrity"
    $dblRe = "al(?:ready|redy) has destroy reason"
    $zoneRe = "was not removed from any zones"
    $ours = @(); $dbl = 0; $zone = 0; $seen = 0
    if ($OutDir -eq "" -or -not (Test-Path $OutDir)) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no run dir")
    }
    foreach ($label in @("host", "join")) {
        $f = Join-Path $OutDir "${label}_engine.log"
        if (-not (Test-Path $f)) { continue }
        $seen++
        foreach ($m in @(Select-String -Path $f -Pattern $dblRe -ErrorAction SilentlyContinue)) {
            $dbl++
            if ($m.Line -match "coop-") { $ours += "[$label] $($m.Line.Trim())" }
        }
        $zone += @(Select-String -Path $f -Pattern $zoneRe -ErrorAction SilentlyContinue).Count
    }
    if ($seen -eq 0) {
        return (Add-GateResult -Name $gate -Status SKIP -Detail "no engine log archived")
    }
    $m = @{ doubleDestroy = $dbl; coopAttributed = $ours.Count; zoneLeak = $zone }
    if ($ours.Count -gt 0) {
        Write-Host "  ENGINE-INTEGRITY FAIL - $($ours.Count) object(s) destroyed twice with a coop reason"
        foreach ($l in ($ours | Select-Object -First 5)) { Write-Host "      $l" }
        return (Add-GateResult -Name $gate -Status FAIL -Metrics $m `
                    -Detail "coop-attributed double destroy")
    }
    Write-Host "  ENGINE-INTEGRITY PASS - no coop-attributed double destroy (engine's own: doubleDestroy=$dbl, zoneLeak=$zone)"
    if ($dbl -gt 0 -or $zone -gt 0) {
        Write-Host "  FINDING: engine reported $dbl double-destroy and $zone zone-leak line(s) with no coop reason attached"
    }
    return (Add-GateResult -Name $gate -Status PASS -Metrics $m)
}

# Report any in-plugin "CHECK <key> FAIL" lines across both logs.
function Test-NoCheckFail {
    param([string]$HostFile, [string]$JoinFile)
    $count = 0
    foreach ($entry in @(@{ f = $HostFile; l = "host" }, @{ f = $JoinFile; l = "join" })) {
        if ($entry.f -eq "" -or -not (Test-Path $entry.f)) { continue }
        $fails = @(Select-String -Path $entry.f -Pattern "CHECK \S+ FAIL" -ErrorAction SilentlyContinue)
        foreach ($f in $fails) { Write-Host "  [$($entry.l)] $($f.Line.Trim())" }
        $count += $fails.Count
    }
    return (Add-GateResult -Name "check_fail" -Status $(if ($count -eq 0) { "PASS" } else { "FAIL" }) `
                -Metrics @{ failLines = $count })
}

# The in-plugin scenario verdict ("SCENARIO RESULT PASS") for one client.
function Test-ScenarioResultPass {
    param([string]$File, [string]$Label, [bool]$Required)
    $gate = "result_$Label"
    if ($File -eq "" -or -not (Test-Path $File)) {
        if (-not $Required) { return (Add-GateResult -Name $gate -Status SKIP -Detail "not launched") }
        Write-Host "  $Label SCENARIO RESULT missing (no log)"
        return (Add-GateResult -Name $gate -Status FAIL -Detail "no log")
    }
    $ok = ((Get-Content $File -Raw) -match "SCENARIO RESULT PASS")
    if (-not $ok) { Write-Host "  $Label SCENARIO RESULT is not PASS" }
    return (Add-GateResult -Name $gate -Status $(if ($ok) { "PASS" } else { "FAIL" }))
}

# ---- Clock-sync gate ------------------------------------------------------------

# Validates the wire time-sync itself. SKIP when the join log carries no CLOCKSYNC
# lines (pre-time-sync DLL). When -ExpectedSkewMs is provided (a run with injected
# fake clock skew), the estimated offset must recover the injected skew: the join's
# wall clock was shifted +S, so its estimated host-relative offset must be ~ -S.
function Test-ClockSync {
    param([string]$HostFile, [string]$JoinFile, $ExpectedSkewMs = $null, [int]$MinSamples = 3)
    $s = Get-ClockSyncStats -File $JoinFile
    if ($s.samples -eq 0) {
        Write-Host "  CLOCK-SYNC SKIP - no CLOCKSYNC lines in the join log (plugin without time-sync?)"
        return (Add-GateResult -Name "clock_sync" -Status SKIP -Detail "no CLOCKSYNC lines")
    }
    $m = @{ samples = $s.samples; offsetMs = $s.offsetMs; rttMs = $s.rttMs }
    if ($s.samples -lt $MinSamples) {
        Write-Host "  CLOCK-SYNC SKIP - only $($s.samples) CLOCKSYNC sample(s) (need >= $MinSamples)"
        return (Add-GateResult -Name "clock_sync" -Status SKIP -Metrics $m -Detail "too few samples")
    }
    if ($null -ne $ExpectedSkewMs -and "$ExpectedSkewMs" -ne "") {
        $exp = [int]$ExpectedSkewMs
        $tol = [Math]::Max(100, 2 * $s.rttMs)
        $err = [Math]::Abs($s.offsetMs + $exp)
        $m.expectedSkewMs = $exp; $m.recoverErrMs = $err; $m.tolMs = $tol
        $ok = ($err -le $tol)
        $v = if ($ok) { "PASS" } else { "FAIL" }
        Write-Host "  CLOCK-SYNC $v - injected skew ${exp}ms, estimated offset $($s.offsetMs)ms (|err|=${err}ms <= ${tol}ms, rtt=$($s.rttMs)ms, n=$($s.samples))"
        return (Add-GateResult -Name "clock_sync" -Status $v -Metrics $m)
    }
    Write-Host "  CLOCK-SYNC PASS - offset=$($s.offsetMs)ms rtt=$($s.rttMs)ms n=$($s.samples)"
    return (Add-GateResult -Name "clock_sync" -Status PASS -Metrics $m)
}

