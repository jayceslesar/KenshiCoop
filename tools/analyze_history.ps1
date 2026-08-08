# Aggregate tools/test-runs/history.jsonl into per-gate reliability figures.
# Answers "which gates actually fail, which never get measured, and which
# scenarios cannot make up their mind" over a chosen window of history.
param(
    [int]$Days = 0,          # 0 = all history
    [string]$Path = "tools/test-runs/history.jsonl"
)

$cut = if ($Days -gt 0) { (Get-Date).AddDays(-$Days) } else { [datetime]::MinValue }

$runs = @()
foreach ($line in Get-Content $Path) {
    if (-not $line.Trim()) { continue }
    try { $r = $line | ConvertFrom-Json } catch { continue }
    if ([datetime]$r.timestamp -lt $cut) { continue }
    $runs += $r
}

Write-Host ("runs in window: {0}" -f $runs.Count)
if (-not $runs.Count) { return }
Write-Host ("window: {0} -> {1}" -f $runs[0].timestamp, $runs[-1].timestamp)
Write-Host ("run pass rate: {0:P1}" -f (@($runs | Where-Object { $_.pass }).Count / $runs.Count))
Write-Host ""

# --- per-gate reliability -------------------------------------------------
$byGate = @{}
foreach ($r in $runs) {
    foreach ($g in $r.gates) {
        if ($null -eq $g -or $null -eq $g.gate) { continue }
        if (-not $byGate.ContainsKey($g.gate)) {
            $byGate[$g.gate] = [pscustomobject]@{
                Gate = $g.gate; Seen = 0; Fail = 0; Skip = 0; Scenarios = @{}
            }
        }
        $e = $byGate[$g.gate]
        $e.Seen++
        if ($g.status -eq 'FAIL') {
            $e.Fail++
            $e.Scenarios[$r.scenario] = 1
        }
        if ($g.status -eq 'SKIP') { $e.Skip++ }
    }
}

Write-Host "=== gates by FAIL rate ==="
'{0,-20} {1,6} {2,6} {3,8} {4,6} {5,8}  {6}' -f 'GATE','SEEN','FAIL','FAIL%','SKIP','SKIP%','SCENARIOS'
$byGate.Values | Sort-Object { -($_.Fail / [Math]::Max(1,$_.Seen)) } | ForEach-Object {
    if ($_.Fail -eq 0 -and $_.Skip -eq 0) { return }
    '{0,-20} {1,6} {2,6} {3,7:P1} {4,6} {5,7:P1}  {6}' -f $_.Gate, $_.Seen, $_.Fail,
        ($_.Fail / $_.Seen), $_.Skip, ($_.Skip / $_.Seen),
        (($_.Scenarios.Keys | Sort-Object | Select-Object -First 4) -join ',')
}

# --- scenarios that cannot make up their mind ----------------------------
Write-Host ""
Write-Host "=== flakiest scenarios (verdict flips, >= 4 runs) ==="
'{0,-24} {1,6} {2,6} {3,8}  {4}' -f 'SCENARIO','RUNS','FAILS','FAIL%','LAST 12 (newest last)'
$runs | Group-Object scenario | Where-Object { $_.Count -ge 4 } | ForEach-Object {
    $seq = @($_.Group | Sort-Object timestamp | ForEach-Object { if ($_.pass) { '.' } else { 'X' } })
    $fails = @($_.Group | Where-Object { -not $_.pass }).Count
    [pscustomobject]@{
        Scenario = $_.Name; Runs = $_.Count; Fails = $fails
        Rate = $fails / $_.Count
        Tail = -join ($seq | Select-Object -Last 12)
    }
} | Where-Object { $_.Fails -gt 0 } | Sort-Object { -$_.Rate } | ForEach-Object {
    '{0,-24} {1,6} {2,6} {3,7:P1}  {4}' -f $_.Scenario, $_.Runs, $_.Fails, $_.Rate, $_.Tail
}
