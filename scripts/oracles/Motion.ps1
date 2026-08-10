# oracles/Motion.ps1 - locomotion-quality + travel-parity oracles (monolith
# split of CoopOracles.psm1, 2026-07-12): Test-Smoothness, Test-AnimTruth,
# Test-MarchInPlace, Test-SnapRate, Test-SuppressChurn, Test-SpawnFarBind,
# Test-RestFlap, Test-ExistenceParity, the travel_parity family (Get-WnpcRows,
# Get-WorldRows, Group-WnpcSamples, Test-FollowTravel, Test-TravelParity),
# Test-AntiZombie, Test-Lifecycle, Test-MintDistance.
# Dot-sourced by CoopOracles.psm1 (module scope).
# Must NOT: change gate names or the [drive]/[snap]/[mint]/WNPC regexes -
# they are the C++ log contract (resources/CODE_MAP.md).
# ---- Locomotion-quality oracles ----------------------------------------------------

# Smoothness (zero-advance fraction while the source moved). No SMOOTH line or a
# scenario that never drove a moving body -> SKIP.
# Since 2026-07-10 the plugin EXCLUDES frames captured during the session-start
# clock-slew catch-up (join sims at up to 2x while the host streams at 1x - a
# structural zero-step source that measured the slew, not the interp pipeline;
# it drove the historical 0.2-0.9 zeroFrac flake). The excluded count is
# reported as slewSkip=; a run whose motion fell almost entirely inside the
# slew window has too few scored frames for the gate to mean anything -> SKIP.
function Test-Smoothness {
    param([string]$File, [string]$Label = "join", [double]$MaxZeroFrac = 0.40,
          [int]$MinActiveFrames = 200)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "smoothness" -Status SKIP -Detail "no log")
    }
    $line = Select-String -Path $File -Pattern "SCENARIO SMOOTH active=(\d+) .*zeroFrac=([\d\.]+).*maxStep=([\d\.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $line) {
        Write-Host "  [$Label] no SCENARIO SMOOTH line (skipped)"
        return (Add-GateResult -Name "smoothness" -Status SKIP -Detail "no SMOOTH line")
    }
    $active   = [int]$line.Matches[0].Groups[1].Value
    $zeroFrac = [double]$line.Matches[0].Groups[2].Value
    $maxStep  = [double]$line.Matches[0].Groups[3].Value
    $slewSkip = 0
    if ($line.Line -match "slewSkip=(\d+)") { $slewSkip = [int]$Matches[1] }
    # Motion-onset audit (SCENARIO ONSET). Reported, never gated, like ZEROPOP.
    # It asked why zeroFrac tracks the SAMPLE SIZE rather than the motion
    # (corr(log(active), zeroFrac) = -0.603 over 91 npc_sync runs), and the
    # answer is the oracle's own admission test: a body is scored on its
    # INSTANTANEOUS velocity, so one hovering near the threshold re-enters over
    # and over and is charged a fresh render catch-up each time. Measured on 4
    # npc_sync runs, onsetEarlyFrac is a constant 0.954-0.965 while whole-run
    # zeroFrac ranges 0.282-0.614, and corr(onsetEarlyShare, zeroFrac) = 0.998:
    # the verdict is set by how much of the sample sat in that first 100 ms, not
    # by how well the run rendered. Read onsetSteadyFrac for the motion quality
    # (0.048-0.247 on the same runs, all well inside the bound). Parsed BEFORE
    # the sample-floor check on purpose: the under-sampled runs now SKIP, and
    # they are the ones whose onset behaviour the question is about.
    $onset = @{}
    $on = Select-String -Path $File -ErrorAction SilentlyContinue -Pattern ("SCENARIO ONSET reentries=(\d+) " +
          "earlyN=(\d+) earlyZero=(\d+) earlyFrac=([\d\.]+) " +
          "midN=(\d+) midZero=(\d+) midFrac=([\d\.]+) " +
          "steadyN=(\d+) steadyZero=(\d+) steadyFrac=([\d\.]+)") | Select-Object -Last 1
    if ($null -ne $on) {
        $g = $on.Matches[0].Groups
        $onset.onsetReentries  = [int]$g[1].Value
        $onset.onsetEarlyN     = [int]$g[2].Value
        $onset.onsetEarlyFrac  = [double]$g[4].Value
        $onset.onsetMidN       = [int]$g[5].Value
        $onset.onsetMidFrac    = [double]$g[7].Value
        $onset.onsetSteadyN    = [int]$g[8].Value
        $onset.onsetSteadyFrac = [double]$g[10].Value
        Write-Host ("  [$Label] onset audit - reentries=$($onset.onsetReentries), " +
                    "early $($onset.onsetEarlyFrac) (n=$($onset.onsetEarlyN)), " +
                    "mid $($onset.onsetMidFrac) (n=$($onset.onsetMidN)), " +
                    "steady $($onset.onsetSteadyFrac) (n=$($onset.onsetSteadyN)) [reported, not gated]")
    }
    if ($active -lt $MinActiveFrames) {
        Write-Host "  [$Label] smoothness SKIP - active=$active (< $MinActiveFrames scored frames; slewSkip=$slewSkip fell in the clock-slew window)"
        $sm = @{ active = $active; slewSkip = $slewSkip }
        foreach ($k in $onset.Keys) { $sm[$k] = $onset[$k] }
        return (Add-GateResult -Name "smoothness" -Status SKIP `
                    -Metrics $sm `
                    -Detail "only $active scored frames (slewSkip=$slewSkip)")
    }
    # Population audit (SCENARIO ZEROPOP). Reported, never gated - it exists to say
    # what the zero frames WERE. The premise it was built to test (that zeroFrac is
    # inflated by bodies which structurally cannot walk - down, carried, in a bed)
    # is false: those bodies leave the drive before the scoring block, and the audit
    # measures 0 in every such bucket. What it does split is the free upright body:
    #   alias - it advanced within the last 100ms, so the render frame merely outran
    #           the engine's character-update step. Nothing a player can see.
    #   stall - it has not advanced for 100ms+ while its source walks. The defect.
    # Measured ~20% alias / ~80% stall on npc_sync and leader_move, so the metric is
    # honest and stallFrac is the number worth driving down.
    $m = @{ zeroFrac = $zeroFrac; maxStep = $maxStep; active = $active; slewSkip = $slewSkip }
    $zp = Select-String -Path $File -Pattern "SCENARIO ZEROPOP .*alias=(\d+) stall=(\d+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $zp) {
        $alias = [int]$zp.Matches[0].Groups[1].Value
        $stall = [int]$zp.Matches[0].Groups[2].Value
        $m.zeroAlias = $alias
        $m.zeroStall = $stall
        $m.stallFrac = if ($active -gt 0) { [Math]::Round($stall / $active, 3) } else { 0 }
    }
    foreach ($k in $onset.Keys) { $m[$k] = $onset[$k] }
    $ok = ($zeroFrac -le $MaxZeroFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] smoothness $v - zeroFrac=$zeroFrac (<= $MaxZeroFrac), stallFrac=$($m.stallFrac), maxStep=$maxStep, active=$active, slewSkip=$slewSkip"
    return (Add-GateResult -Name "smoothness" -Status $v -Metrics $m)
}

# Anim-truth (the float-bug detector). Too few translating frames -> SKIP.
function Test-AnimTruth {
    param([string]$File, [string]$Label = "join", [double]$MaxFloatFrac = 0.30)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "anim_truth" -Status SKIP -Detail "no log")
    }
    $line = Select-String -Path $File -Pattern "SCENARIO ANIM .*translate=(\d+) walkTruth=(\d+) floatFrac=([\d\.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $line) {
        Write-Host "  [$Label] no SCENARIO ANIM line (skipped)"
        return (Add-GateResult -Name "anim_truth" -Status SKIP -Detail "no ANIM line")
    }
    $translate = [int]$line.Matches[0].Groups[1].Value
    $floatFrac = [double]$line.Matches[0].Groups[3].Value
    if ($translate -lt 30) {
        Write-Host "  [$Label] anim-truth SKIP - only $translate translating frame(s)"
        return (Add-GateResult -Name "anim_truth" -Status SKIP -Metrics @{ translate = $translate } -Detail "too few translating frames")
    }
    $ok = ($floatFrac -le $MaxFloatFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] anim-truth $v - floatFrac=$floatFrac (<= $MaxFloatFrac), translateFrames=$translate"
    return (Add-GateResult -Name "anim_truth" -Status $v -Metrics @{ floatFrac = $floatFrac; translate = $translate })
}

# March-in-place (inverse of anim-truth). Too few rest samples -> SKIP.
function Test-MarchInPlace {
    param([string]$File, [string]$Label = "join", [double]$MaxMarchFrac = 0.20,
          [double]$MaxIdleFrac = 0.01)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "march" -Status SKIP -Detail "no log")
    }
    $line = Select-String -Path $File -Pattern "SCENARIO MARCH restSamples=(\d+) march=(\d+) marchFrac=([\d\.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $line) {
        Write-Host "  [$Label] no SCENARIO MARCH line (skipped)"
        return (Add-GateResult -Name "march" -Status SKIP -Detail "no MARCH line")
    }
    $rest  = [int]$line.Matches[0].Groups[1].Value
    $marchFrac = [double]$line.Matches[0].Groups[3].Value
    if ($rest -lt 30) {
        Write-Host "  [$Label] march-in-place SKIP - only $rest at-rest frame(s)"
        return (Add-GateResult -Name "march" -Status SKIP -Metrics @{ restSamples = $rest } -Detail "too few rest samples")
    }
    # Score holdStop, not marchFrac (2026-08-01). marchFrac counts every frame the
    # drive held a walk verdict while the oracle called the host at rest, and the
    # C++ attribution measured 86-93% of those as holdDip: the source is genuinely
    # WALKING and its instantaneous velocity is in the sample-boundary dip the
    # walk/rest debounce exists to bridge, while this oracle decides "at rest" from
    # that same instantaneous velocity. Scoring the disagreement made a ~13-19x
    # regression read as 100x+ and sent a fix attempt after the wrong lever.
    # holdStop - the debounce hold outliving a REAL stop, leaving a walk order on a
    # body already standing where it belongs - is the visible idle jitter.
    # marchFrac stays recorded for continuity with pre-2026-08-01 history.
    #
    # The 0.01 ceiling is ~2x the worst run measured after the near-tier hold floor
    # was cut to 300 ms (leader_move 0.0024 mean / 0.0041 worst, craft_order 0.0042
    # mean / 0.0052 worst over 4 clean runs each). Before that cut the same
    # scenarios sat at 0.0086 and 0.0165, so this ceiling would catch a relapse.
    if ($line.Line -match "holdStop=(\d+)") {
        $holdStop = [int]$Matches[1]
        $dip = 0
        if ($line.Line -match "holdDip=(\d+)") { $dip = [int]$Matches[1] }
        # march decomposes exactly: hold (= holdDip + holdStop) + settle + rlps.
        # Measured over 19 runs on 2026-08-07, holdDip is 94-98% of march, which
        # is why the gate drops it. settle is the bounded endAction transition.
        # rlps - a PARKED body re-acquiring a walk - is a visible defect in
        # principle and is recorded here, but it is not gated: in that same
        # sample every meaningful relapse count came from combat_probe (188 and
        # 247 frames, against <= 26 everywhere else), whose baseline hold
        # re-parks and re-clears its duelists' goals on every tick and so
        # manufactures the relapses it then reports. A bound belongs per
        # scenario, not here.
        $rlps = 0
        if ($line.Line -match "rlps=(\d+)") { $rlps = [int]$Matches[1] }
        $idleFrac    = [math]::Round($holdStop / [double]$rest, 4)
        $relapseFrac = [math]::Round($rlps / [double]$rest, 4)
        $ok = ($idleFrac -le $MaxIdleFrac)
        $v = if ($ok) { "PASS" } else { "FAIL" }
        Write-Host "  [$Label] march-in-place $v - idleFrac=$idleFrac (<= $MaxIdleFrac), holdStop=$holdStop, holdDip=$dip (scored at rest, not a defect), relapseFrac=$relapseFrac (rlps=$rlps, recorded not gated), marchFrac=$marchFrac, restSamples=$rest"
        return (Add-GateResult -Name "march" -Status $v -Metrics @{
                    idleFrac = $idleFrac; holdStop = $holdStop; holdDip = $dip
                    relapseFrac = $relapseFrac; relapses = $rlps
                    marchFrac = $marchFrac; restSamples = $rest })
    }
    # Pre-attribution DLL: only the conflated metric exists.
    $ok = ($marchFrac -le $MaxMarchFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] march-in-place $v - marchFrac=$marchFrac (<= $MaxMarchFrac, conflated: no holdStop in this log), restSamples=$rest"
    return (Add-GateResult -Name "march" -Status $v -Metrics @{ marchFrac = $marchFrac; restSamples = $rest })
}

# Snap-rate (2026-07-11 rubber-banding validation): hard teleports per minute of
# steady-state run, from the join's cumulative [interp] counters (snapSq+snapNpc).
# The session-start clock-slew window is excluded (the 2x catch-up legitimately
# outruns the walk-drive): counting starts at the last [interp] sample before the
# first slew=1.0 OFFSET report. When the slew never converges (fast_march holds
# 5x, which pins the slew at its cap) the whole run is scored - the velocity-
# aware snap gate must hold regardless of game speed.
function Test-SnapRate {
    param([string]$File, [string]$Label = "join", [double]$MaxPerMin = 3.0,
          [int]$MinWindowSec = 20, [switch]$SquadOnly,
          [string]$GateName = "snap_rate")
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name $GateName -Status SKIP -Detail "no log")
    }
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[interp\] .*snapSq=(\d+) snapNpc=(\d+)"
    $lines = @(Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)
    if ($lines.Count -lt 2) {
        Write-Host "  [$Label] snap-rate SKIP - no [interp] counter series"
        return (Add-GateResult -Name $GateName -Status SKIP -Detail "no [interp] series")
    }
    $series = @(foreach ($m in $lines) {
        $g = $m.Matches[0].Groups
        [pscustomobject]@{
            t   = Convert-StampToMs -Groups $g -OffsetMs 0
            sq  = [long]$g[5].Value
            npc = [long]$g[6].Value
        }
    })
    # Skip the clock catch-up window: baseline at the last sample before the
    # first slew=1.0 report (same exclusion the smoothness oracle applies).
    $startIdx = 0
    $om = Select-String -Path $File -Pattern "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[time\] OFFSET .*slew=1\.0" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $om) {
        $slewT = Convert-StampToMs -Groups $om.Matches[0].Groups -OffsetMs 0
        for ($i = 0; $i -lt $series.Count; $i++) {
            if ($series[$i].t -le $slewT) { $startIdx = $i }
        }
    }
    $base = $series[$startIdx]
    $last = $series[$series.Count - 1]
    $winSec = ($last.t - $base.t) / 1000.0
    if ($winSec -lt $MinWindowSec) {
        Write-Host "  [$Label] snap-rate SKIP - scored window ${winSec}s (< ${MinWindowSec}s past the slew)"
        return (Add-GateResult -Name $GateName -Status SKIP `
                    -Metrics @{ windowSec = $winSec } -Detail "window too short")
    }
    $dSq  = $last.sq - $base.sq
    $dNpc = $last.npc - $base.npc
    # SquadOnly (fast_march): at 5x a background NPC resting between stream
    # updates legitimately falls 100+ u behind, and the far-behind teleport is
    # the correct convergence tool - only PLAYER-SQUAD snaps (the visible
    # rubber banding) gate there. At 1x both classes gate.
    $gated = if ($SquadOnly) { $dSq } else { $dSq + $dNpc }
    $rate  = [math]::Round($gated / ($winSec / 60.0), 2)
    $ok = ($rate -le $MaxPerMin)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $scope = if ($SquadOnly) { "squad" } else { "squad+npc" }
    Write-Host "  [$Label] snap-rate $v - $gated $scope hard snap(s) over $([math]::Round($winSec,0))s = $rate/min (<= $MaxPerMin/min; sq=$dSq npc=$dNpc)"
    return (Add-GateResult -Name $GateName -Status $v `
                -Metrics @{ snapsSq = $dSq; snapsNpc = $dNpc; windowSec = [math]::Round($winSec, 1); ratePerMin = $rate })
}

# Suppress-churn (2026-07-11 pop-in/out fix): a join-side NPC must never cycle
# hidden -> restored -> hidden (the 'Saint'/'Kumo' stream-boundary flicker the
# census-existence veto eliminates). Counts suppress/cull events per hand; any
# hand hidden more than once is churn. Zero suppression activity passes - the
# invariant holds vacuously (ghost culls of join-only spawns stay legitimate,
# each firing once per hand).
function Test-SuppressChurn {
    param([string]$File, [string]$Label = "join", [int]$MaxPerHand = 1)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "suppress_churn" -Status SKIP -Detail "no log")
    }
    $counts = @{}
    foreach ($p in @('\[authority\] suppress NPC hand=(\d+,\d+)',
                     '\[census\] cull NPC hand=(\d+,\d+)')) {
        foreach ($m in @(Select-String -Path $File -Pattern $p -ErrorAction SilentlyContinue)) {
            $h = $m.Matches[0].Groups[1].Value
            if ($counts.ContainsKey($h)) { $counts[$h]++ } else { $counts[$h] = 1 }
        }
    }
    $restores = @(Select-String -Path $File -Pattern '\[(?:authority|census)\] restore NPC hand=' -ErrorAction SilentlyContinue).Count
    $worst = 0; $churned = @()
    foreach ($k in $counts.Keys) {
        if ($counts[$k] -gt $worst) { $worst = $counts[$k] }
        if ($counts[$k] -gt $MaxPerHand) { $churned += $k }
    }
    $ok = ($churned.Count -eq 0)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($ok) { "" } else { "churned hands: " + ($churned -join " ") }
    Write-Host "  [$Label] suppress-churn $v - $($counts.Count) hand(s) hidden, worst=$worst per hand (<= $MaxPerHand), restores=$restores $detail"
    return (Add-GateResult -Name "suppress_churn" -Status $v `
                -Metrics @{ hands = $counts.Count; worstPerHand = $worst; restores = $restores } -Detail $detail)
}

# spawn_far (2026-07-11 "NPCs spawn on top of the join player" fix): census-range
# proxy minting. The host spawns a runtime squad ~620 u out and walks it toward
# the co-located leaders; the join must mint the proxies while they are still
# FAR away (census-missing scan + reply-side mint gate) instead of at the ~200 u
# stream bubble. Gates:
#   1. BIND coverage: every far hand drew a "[spawn] proxy BOUND" line.
#   2. FAR bind: every bind happened >= MinBindDist from the join's leader
#      anchor (no more materializing on top of the player).
#   3. NO DUPES: at most one BOUND per hand (the mint gate must not double-mint
#      a hand that later streams normally).
#   4. TAKEOVER: at least one proxy's SCENARIO PROXY series reaches within
#      ApproachDist of the anchor - the walking squad entered the stream bubble
#      and the SAME proxy body was driven in (no fresh mint at the boundary).
function Test-SpawnFarBind {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MinBindDist = 400.0, [double]$ApproachDist = 350.0)
    if (-not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "spawn_far" -Status SKIP -Detail "no join log")
    }
    $hostLegs = Get-SpawnHands -File $HostFile
    $far = if ($hostLegs.ContainsKey('far')) { @($hostLegs['far']) } else { @() }
    if ($far.Count -eq 0) {
        Write-Host "  SPAWN-FAR FAIL - host never spawned the far squad"
        return (Add-GateResult -Name "spawn_far" -Status FAIL -Detail "no far spawns")
    }
    $am = Select-String -Path $JoinFile -Pattern 'SCENARIO FARBIND anchor=([-\d\.]+),([-\d\.]+),([-\d\.]+) have=1' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $am) {
        Write-Host "  SPAWN-FAR FAIL - join never logged its leader anchor"
        return (Add-GateResult -Name "spawn_far" -Status FAIL -Detail "no join anchor")
    }
    $ax = [double]$am.Matches[0].Groups[1].Value
    $ay = [double]$am.Matches[0].Groups[2].Value
    $az = [double]$am.Matches[0].Groups[3].Value

    # BOUND lines with the mint position (the host-authoritative spawn point).
    $bounds = @{}
    foreach ($m in @(Select-String -Path $JoinFile -Pattern '\[spawn\] proxy BOUND hand=([\d,]+) .*pos=([-\d\.]+),([-\d\.]+),([-\d\.]+)' -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $h = $g[1].Value
        if (-not $bounds.ContainsKey($h)) { $bounds[$h] = New-Object System.Collections.ArrayList }
        $dx = [double]$g[2].Value - $ax
        $dy = [double]$g[3].Value - $ay
        $dz = [double]$g[4].Value - $az
        [void]$bounds[$h].Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
    }

    $boundN = 0; $dupN = 0; $closestBind = [double]::MaxValue; $farOk = $true
    foreach ($h in $far) {
        if (-not $bounds.ContainsKey($h)) { continue }
        $boundN++
        if ($bounds[$h].Count -gt 1) { $dupN++ }
        $d = $bounds[$h][0]
        if ($d -lt $closestBind) { $closestBind = $d }
        if ($d -lt $MinBindDist) { $farOk = $false }
    }
    $bindOk = ($boundN -eq $far.Count)
    $dupOk  = ($dupN -eq 0)
    $bindTxt = if ($closestBind -eq [double]::MaxValue) { "n/a" } else { [Math]::Round($closestBind, 0) }
    Write-Host ("  SPAWN-FAR bind " + $(if ($bindOk) { "PASS" } else { "FAIL" }) +
                " - $boundN/$($far.Count) far hands minted")
    Write-Host ("  SPAWN-FAR distance " + $(if ($farOk) { "PASS" } else { "FAIL" }) +
                " - closest bind $bindTxt u from join anchor (>= $MinBindDist)")
    Write-Host ("  SPAWN-FAR dupes " + $(if ($dupOk) { "PASS" } else { "FAIL" }) +
                " - $dupN hand(s) bound more than once")

    # TAKEOVER: the proxy body itself must close on the anchor (stream drive).
    $P = Get-ScenarioSeries -File $JoinFile -Kind "PROXY"
    $minApproach = [double]::MaxValue
    foreach ($h in $far) {
        $key = Convert-SpawnHandToSeriesKey -Hand $h
        if ($null -eq $key -or -not $P.ContainsKey($key)) { continue }
        foreach ($ps in $P[$key]) {
            $dx = $ps.p[0]-$ax; $dy = $ps.p[1]-$ay; $dz = $ps.p[2]-$az
            $d = [Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz)
            if ($d -lt $minApproach) { $minApproach = $d }
        }
    }
    $approachOk = ($minApproach -le $ApproachDist)
    $appTxt = if ($minApproach -eq [double]::MaxValue) { "n/a" } else { [Math]::Round($minApproach, 0) }
    Write-Host ("  SPAWN-FAR takeover " + $(if ($approachOk) { "PASS" } else { "FAIL" }) +
                " - closest proxy approach $appTxt u (<= $ApproachDist)")

    $ok = $bindOk -and $farOk -and $dupOk -and $approachOk
    $why = @()
    if (-not $bindOk)     { $why += "far hands never minted" }
    if (-not $farOk)      { $why += "a proxy minted inside $MinBindDist u (on-top materialization)" }
    if (-not $dupOk)      { $why += "duplicate mints" }
    if (-not $approachOk) { $why += "no proxy walked into the stream bubble" }
    $detail = $why -join "; "
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  SPAWN-FAR $v $detail"
    return (Add-GateResult -Name "spawn_far" -Status $v `
                -Metrics @{ far = $far.Count; bound = $boundN; dupes = $dupN
                            closestBind = $bindTxt; minApproach = $appTxt } -Detail $detail)
}

# Rest-flap (2026-07-11 choppiness fix): the join's walk/rest classifier used the
# instantaneous 2-sample velocity, which dips below threshold at every sample
# pair of a walking NPC - each dip parks/halts the body, each recovery restarts
# the walk (the observed stutter, several flips per second). The velPeak
# debounce makes rest entry require ~1-2 s of genuinely still samples, so the
# cumulative restFlip counter on the [interp] line must now grow at genuine-stop
# rate only. The session-start clock-slew window is excluded like Test-SnapRate.
function Test-RestFlap {
    param([string]$File, [string]$Label = "join", [double]$MaxPerMin = 60.0,
          [int]$MinWindowSec = 20)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "rest_flap" -Status SKIP -Detail "no log")
    }
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[interp\] .*restFlip=(\d+)"
    $lines = @(Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)
    if ($lines.Count -lt 2) {
        Write-Host "  [$Label] rest-flap SKIP - no [interp] restFlip series"
        return (Add-GateResult -Name "rest_flap" -Status SKIP -Detail "no restFlip series")
    }
    $series = @(foreach ($m in $lines) {
        $g = $m.Matches[0].Groups
        [pscustomobject]@{
            t = Convert-StampToMs -Groups $g -OffsetMs 0
            n = [long]$g[5].Value
        }
    })
    $startIdx = 0
    $om = Select-String -Path $File -Pattern "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[time\] OFFSET .*slew=1\.0" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $om) {
        $slewT = Convert-StampToMs -Groups $om.Matches[0].Groups -OffsetMs 0
        for ($i = 0; $i -lt $series.Count; $i++) {
            if ($series[$i].t -le $slewT) { $startIdx = $i }
        }
    }
    $base = $series[$startIdx]
    $last = $series[$series.Count - 1]
    $winSec = ($last.t - $base.t) / 1000.0
    if ($winSec -lt $MinWindowSec) {
        Write-Host "  [$Label] rest-flap SKIP - scored window ${winSec}s (< ${MinWindowSec}s past the slew)"
        return (Add-GateResult -Name "rest_flap" -Status SKIP `
                    -Metrics @{ windowSec = $winSec } -Detail "window too short")
    }
    $flips = $last.n - $base.n
    $rate  = [math]::Round($flips / ($winSec / 60.0), 2)
    $ok = ($rate -le $MaxPerMin)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] rest-flap $v - $flips walk->rest flip(s) over $([math]::Round($winSec,0))s = $rate/min (<= $MaxPerMin/min)"
    return (Add-GateResult -Name "rest_flap" -Status $v `
                -Metrics @{ flips = $flips; windowSec = [math]::Round($winSec, 1); ratePerMin = $rate })
}

# Existence parity (pack-hidden investigation, 2026-07-11). The join emits an
# "[audit] exist ..." line every 5 s classifying every enumerated NPC:
#   drv (streamed/driven) / cen (census-present local copy) / hid (suppressed)
#   / dorm (attention gate: census-absent but nobody watching, left alone)
#   / ghost (census-absent, WATCHED, NOT suppressed - the visible-on-join-only
#     class).
# A ghost is legitimate only transiently (the ~1 s suppress debounce), so with
# a FRESH census the fraction of samples showing ghosts must stay low, and no
# ghost population may PERSIST (a wildlife pack the authority never judges
# would show as sustained ghost>0). Samples with fresh=0 are excluded (wide
# culling is deliberately disabled on a stale census).
#
# The gate makes this test view-conditional by construction: a body only both
# sides can see is only compared while somebody is looking at it. dorm is
# reported, never judged - a large dorm count is the gate working, not a
# failure. Runs from builds without the gate omit the field and report dorm=0.
#
# dormPc IS judged, and it is the gate's safety property: a body next to a
# player character (within attnR of any squad member, camera or not) must never
# be dormant, because every tab leader anchors attention. Non-zero means the
# gate stopped reconciling somebody's own surroundings - a hard fail.
function Test-ExistenceParity {
    param([string]$File, [string]$Label = "join",
          [double]$MaxGhostFrac = 0.35, [int]$MaxGhostRun = 4,
          [int]$MinSamples = 4)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "existence_parity" -Status SKIP -Detail "no log")
    }
    $pat = "\[audit\] exist near=(\d+) wide=(\d+) drv=(\d+) cen=(\d+) hid=(\d+) ghost=(\d+) supp=(\d+) census=(\d+) fresh=(\d)(?:.* dorm=(\d+))?(?:.* dormPc=(\d+))?(?:.* pcs=(\d+))?"
    $lines = @(Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)
    $samples = @(foreach ($m in $lines) {
        $g = $m.Matches[0].Groups
        if ($g[9].Value -eq "1") {
            $dorm = 0
            if ($g[10].Success) { $dorm = [int]$g[10].Value }
            $dormPc = 0
            if ($g[11].Success) { $dormPc = [int]$g[11].Value }
            $pcs = 0
            if ($g[12].Success) { $pcs = [int]$g[12].Value }
            [pscustomobject]@{ ghost = [int]$g[6].Value; wide = [int]$g[2].Value
                               dorm = $dorm; dormPc = $dormPc; pcs = $pcs }
        }
    })
    if ($samples.Count -lt $MinSamples) {
        Write-Host "  [$Label] existence-parity SKIP - $($samples.Count) fresh-census audit sample(s) (< $MinSamples)"
        return (Add-GateResult -Name "existence_parity" -Status SKIP `
                    -Metrics @{ samples = $samples.Count } -Detail "too few fresh audit samples")
    }
    $withGhost = @($samples | Where-Object { $_.ghost -gt 0 }).Count
    $frac = [math]::Round($withGhost / $samples.Count, 3)
    $maxGhost = ($samples | Measure-Object -Property ghost -Maximum).Maximum
    # Longest consecutive run of ghost>0 samples = persistence signal
    # (5 s cadence, so a run of 4 means >= ~15 s of unjudged join-only NPCs).
    $run = 0; $maxRun = 0
    foreach ($s in $samples) {
        if ($s.ghost -gt 0) { $run++; if ($run -gt $maxRun) { $maxRun = $run } }
        else { $run = 0 }
    }
    $peakDorm = ($samples | Measure-Object -Property dorm -Maximum).Maximum
    $peakDormPc = ($samples | Measure-Object -Property dormPc -Maximum).Maximum
    # pcs is the squad size dormPc was measured against; 0 everywhere means the
    # safety zero is vacuous (gate off, or the squad never captured).
    $peakPcs = ($samples | Measure-Object -Property pcs -Maximum).Maximum
    $ok = ($frac -le $MaxGhostFrac) -and ($maxRun -le $MaxGhostRun) `
          -and ($peakDormPc -eq 0)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] existence-parity $v - ghosts in $withGhost/$($samples.Count) samples (frac=$frac <= $MaxGhostFrac), longest run $maxRun (<= $MaxGhostRun), peak ghost=$maxGhost, peak dorm=$peakDorm, peak dormPc=$peakDormPc of $peakPcs squad member(s) (must be 0)"
    return (Add-GateResult -Name "existence_parity" -Status $v `
                -Metrics @{ samples = $samples.Count; ghostFrac = $frac
                            maxRun = $maxRun; peakGhost = $maxGhost
                            peakDorm = $peakDorm; peakDormPc = $peakDormPc
                            peakPcs = $peakPcs })
}

# ---- travel_parity oracles ------------------------------------------------------

# Parse timestamped "SCENARIO WNPC hand=.. pos=.. cls=.. name=.." rows into a list
# of @{t; hand(i,s); pos; cls}, times in the HOST clock frame. Rows are grouped
# into dump samples by the caller (a dump emits its rows in one burst).
# world_parity fields (task=/pelvis=/mv=, appended after name) are optional so
# logs from older builds still parse; task=-1/pelvis=-1/mv=-1 when absent.
# carry= is appended after mv= and is optional for the same reason (-1 = the
# build predates it, which the PC gate treats as "unknown, judge it").
function Get-WnpcRows {
    param([string]$File)
    $rows = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return $rows }
    $off = Get-LogClockOffsetMs -File $File
    # hand5 keeps the WHOLE emitted tuple (index,serial,type,container,
    # containerSerial). 'hand' stays index,serial because every existing caller
    # pairs on it, but index,serial alone is a weak cross-client key for MINTED
    # bodies - two engines allocate independently, so the same pair can name two
    # different bodies. Callers that need identity rather than a pairing hint
    # (dual_drive's roster signal) match on hand5.
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WNPC hand=(\d+),(\d+),(\d+),(\d+),(\d+) pos=([\-\d\.,]+) cls=(\w+) name='([^']*)'(?: task=(\d+) pelvis=(-?[\d\.]+) mv=(-?\d+)(?: carry=(-?\d+))?)?"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $p = $g[10].Value.Split(',') | ForEach-Object { [double]$_ }
        $task = -1; $pelvis = -1.0; $mv = -1; $carry = -1
        if ($g[13].Success) { $task = [int]$g[13].Value }
        if ($g[14].Success) { $pelvis = [double]$g[14].Value }
        if ($g[15].Success) { $mv = [int]$g[15].Value }
        if ($g[16].Success) { $carry = [int]$g[16].Value }
        [void]$rows.Add(@{ t = $t; hand = "$($g[5].Value),$($g[6].Value)"
                           hand5 = "$($g[5].Value),$($g[6].Value),$($g[7].Value),$($g[8].Value),$($g[9].Value)"
                           pos = $p; cls = $g[11].Value; name = $g[12].Value
                           task = $task; pelvis = $pelvis; mv = $mv; carry = $carry })
    }
    return $rows
}

# Parse timestamped "SCENARIO WORLD n=.. cls=.." dump-summary rows into a list of
# @{t; n; ghost}. One row per 5 s worldstate dump, emitted even when the dump is
# EMPTY (n=0) - the hop corridor is mostly wilderness, so empty dumps are the
# common case and still count as judged parity samples (0 ghosts vs 0 host rows).
# The join row carries the class counts; the host row is just n= (cls=host).
function Get-WorldRows {
    param([string]$File)
    $rows = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return $rows }
    $off = Get-LogClockOffsetMs -File $File
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WORLD n=(\d+) cls=(\w+)(?: drv=\d+ cen=\d+ hid=\d+ ghost=(\d+))?"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $ghost = 0
        if ($g[7].Success) { $ghost = [int]$g[7].Value }
        [void]$rows.Add(@{ t = $t; n = [int]$g[5].Value; cls = $g[6].Value; ghost = $ghost })
    }
    return $rows
}

# Group WNPC rows into dump samples: rows closer than $GapMs to the previous row
# belong to the same 5 s worldstate dump.
function Group-WnpcSamples {
    param($Rows, [int]$GapMs = 1500)
    $samples = New-Object System.Collections.ArrayList
    $cur = $null; $lastT = -1
    foreach ($r in ($Rows | Sort-Object { $_.t })) {
        if ($null -eq $cur -or ($r.t - $lastT) -gt $GapMs) {
            $cur = @{ t = $r.t; rows = (New-Object System.Collections.ArrayList) }
            [void]$samples.Add($cur)
        }
        [void]$cur.rows.Add($r); $lastT = $r.t
    }
    return $samples
}

# travel_parity gate 1: the pair actually traveled. The JOIN's own MEMBER series
# must cover >= $MinTravel from its first sample (the hop trek is ~60,000 u),
# and the host's SCENARIO FOLLOW series (self/peer/gap each ~1 s) must show the
# follow HOLDING: median gap <= $MaxMedianGap after the grace window, and every
# hop-opened gap (a teleport leg legitimately opens ~4000 u for a sample or
# two) must CLOSE within $MaxLagRun consecutive samples above $LagGap - the
# host's teleport catch-up has to actually land. If the follow never holds,
# the parity numbers describe two separated worlds and the run is meaningless -
# this gates before travel_parity for that reason.
function Test-FollowTravel {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MinTravel = 40000.0, [double]$MaxMedianGap = 120.0,
          [double]$LagGap = 300.0, [int]$MaxLagRun = 6, [int]$GraceMs = 20000)
    # Join travel: the mover is the join's MEMBER hand with the most samples.
    $mem = Get-ScenarioSeries -File $JoinFile -Kind "MEMBER"
    $best = $null
    foreach ($h in $mem.Keys) {
        if ($null -eq $best -or $mem[$h].Count -gt $mem[$best].Count) { $best = $h }
    }
    if ($null -eq $best -or $mem[$best].Count -lt 5) {
        Write-Host "  follow-travel SKIP - no join MEMBER series"
        return (Add-GateResult -Name "follow_travel" -Status SKIP -Detail "no join MEMBER series")
    }
    $s0 = $mem[$best][0]
    $travel = 0.0
    foreach ($s in $mem[$best]) {
        $dx = $s.p[0] - $s0.p[0]; $dz = $s.p[2] - $s0.p[2]
        $d = [math]::Sqrt($dx * $dx + $dz * $dz)
        if ($d -gt $travel) { $travel = $d }
    }
    $travel = [math]::Round($travel, 1)
    # Host follow quality from the FOLLOW series.
    $gaps = New-Object System.Collections.ArrayList
    $t0 = $null
    if (Test-Path $HostFile) {
        $off = Get-LogClockOffsetMs -File $HostFile
        $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO FOLLOW self=[\-\d\.,]+ peer=[\-\d\.,]+ gap=([\d\.]+)"
        foreach ($m in (Select-String -Path $HostFile -Pattern $pat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            $t = Convert-StampToMs -Groups $g -OffsetMs $off
            if ($null -eq $t0) { $t0 = $t }
            if (($t - $t0) -lt $GraceMs) { continue } # follow spin-up
            [void]$gaps.Add([double]$g[5].Value)
        }
    }
    if ($gaps.Count -lt 5) {
        Write-Host "  follow-travel SKIP - $($gaps.Count) host FOLLOW sample(s) after grace"
        return (Add-GateResult -Name "follow_travel" -Status SKIP `
                    -Metrics @{ travel = $travel; followSamples = $gaps.Count } `
                    -Detail "too few FOLLOW samples")
    }
    $sorted = @($gaps | Sort-Object)
    $median = [math]::Round($sorted[[int]($sorted.Count / 2)], 1)
    # Lag runs: consecutive samples above $LagGap. A hop legitimately opens a
    # ~4000 u gap; the host's teleport catch-up must close it within
    # $MaxLagRun samples (~seconds) or the follow is not actually holding.
    $maxRun = 0; $run = 0
    foreach ($g in $gaps) {
        if ($g -gt $LagGap) { $run++; if ($run -gt $maxRun) { $maxRun = $run } }
        else { $run = 0 }
    }
    $ok = ($travel -ge $MinTravel) -and ($median -le $MaxMedianGap) -and ($maxRun -le $MaxLagRun)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  follow-travel $v - join traveled $travel u (>= $MinTravel), host follow median gap $median u (<= $MaxMedianGap), max lag run $maxRun (<= $MaxLagRun samples > $LagGap u), $($gaps.Count) samples"
    return (Add-GateResult -Name "follow_travel" -Status $v `
                -Metrics @{ travel = $travel; medianGap = $median
                            maxLagRun = $maxRun; followSamples = $gaps.Count })
}

# travel_parity gate 2: worldstate cross-comparison while traveling. Every 5 s
# both sides dumped SCENARIO WNPC rows (host cls=host from the census walk, join
# rows carrying the authority class drv/cen/hid/ghost). Per join dump sample:
#   ghost rows   - cls=ghost (visible, census-absent, unsuppressed: the
#                  yellow-name/visible-on-join-only class). Gate on the fraction
#                  of samples containing any + the longest consecutive run,
#                  mirroring Test-ExistenceParity but measured WHILE ROAMING.
#   laggard      - ghost whose hand IS in the host's dump (+-$WinMs): coverage
#                  lag, the join authority just hasn't judged it yet (metric).
#   diverged     - cen row whose host counterpart sits > $MaxDiverge away
#                  (parking should bound census-present divergence; metric).
#   hostOnly     - host rows within $HostOnlyRange of the join leader with no
#                  join row for the hand (invisible-to-join candidates; the
#                  range limit keeps out-of-zone census rows - the host census
#                  reaches 2500 u, far past what the join has loaded - from
#                  drowning the metric; minting legitimately lags).
function Test-TravelParity {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MaxGhostFrac = 0.35, [int]$MaxGhostRun = 4,
          [double]$MaxDiverge = 250.0, [double]$HostOnlyRange = 400.0,
          [int]$WinMs = 6000, [int]$MinSamples = 6)
    # Samples are anchored on the WORLD summary rows, NOT the WNPC row groups:
    # a WORLD row is emitted even for an EMPTY dump (n=0), and the hop corridor
    # is mostly wilderness - an empty join dump aligned with an empty host dump
    # is a perfectly judged parity sample (nothing there on either side).
    $joinWorld = Get-WorldRows -File $JoinFile | Where-Object { $_.cls -ne "host" }
    $hostWorld = Get-WorldRows -File $HostFile | Where-Object { $_.cls -eq "host" }
    $joinRows  = Get-WnpcRows -File $JoinFile
    $hostRows  = Get-WnpcRows -File $HostFile
    # Join leader position over time (for the hostOnly range limit): the join's
    # MEMBER hand with the most samples is the traveling mover.
    $mem = Get-ScenarioSeries -File $JoinFile -Kind "MEMBER"
    $mover = $null
    foreach ($h in $mem.Keys) {
        if ($null -eq $mover -or $mem[$h].Count -gt $mem[$mover].Count) { $mover = $h }
    }
    if ($joinWorld.Count -lt $MinSamples -or $hostWorld.Count -eq 0) {
        Write-Host "  travel-parity SKIP - $($joinWorld.Count) join dump(s), $($hostWorld.Count) host dump(s)"
        return (Add-GateResult -Name "travel_parity" -Status SKIP `
                    -Metrics @{ joinSamples = $joinWorld.Count; hostSamples = $hostWorld.Count } `
                    -Detail "too few worldstate dumps")
    }
    $ghostSamples = 0; $run = 0; $maxRun = 0; $used = 0
    $laggards = 0; $diverged = 0; $hostOnly = 0; $trueGhosts = 0; $peakGhost = 0
    foreach ($ws in $joinWorld) {
        $hostAligned = @($hostWorld | Where-Object { [math]::Abs($_.t - $ws.t) -le $WinMs })
        if ($hostAligned.Count -eq 0) { continue } # no host dump nearby - can't judge
        $used++
        # Detail rows for this sample: WNPC rows within the dump's burst window
        # on the join side, within $WinMs of the sample on the host side.
        $jrows = @($joinRows | Where-Object { [math]::Abs($_.t - $ws.t) -le 1500 })
        $hwin  = @($hostRows | Where-Object { [math]::Abs($_.t - $ws.t) -le $WinMs })
        $hByHand = @{}
        foreach ($hr in $hwin) { $hByHand[$hr.hand] = $hr }
        $ghosts = 0
        $joinHands = @{}
        foreach ($jr in $jrows) {
            $joinHands[$jr.hand] = $true
            if ($jr.cls -eq "ghost") {
                $ghosts++
                if ($hByHand.ContainsKey($jr.hand)) { $laggards++ } else { $trueGhosts++ }
            } elseif ($jr.cls -eq "cen" -and $hByHand.ContainsKey($jr.hand)) {
                $hp = $hByHand[$jr.hand].pos
                $dx = $jr.pos[0] - $hp[0]; $dz = $jr.pos[2] - $hp[2]
                if ([math]::Sqrt($dx * $dx + $dz * $dz) -gt $MaxDiverge) { $diverged++ }
            }
        }
        # Range-limit hostOnly to the join leader's surroundings at this time.
        $jl = $null
        if ($null -ne $mover) {
            foreach ($s in $mem[$mover]) {
                if ($null -eq $jl -or [math]::Abs($s.t - $ws.t) -lt [math]::Abs($jl.t - $ws.t)) { $jl = $s }
            }
        }
        foreach ($hr in $hwin) {
            if ($joinHands.ContainsKey($hr.hand)) { continue }
            if ($null -ne $jl) {
                $dx = $hr.pos[0] - $jl.p[0]; $dz = $hr.pos[2] - $jl.p[2]
                if ([math]::Sqrt($dx * $dx + $dz * $dz) -gt $HostOnlyRange) { continue }
            }
            $hostOnly++
        }
        if ($ghosts -gt $peakGhost) { $peakGhost = $ghosts }
        if ($ghosts -gt 0) { $ghostSamples++; $run++; if ($run -gt $maxRun) { $maxRun = $run } }
        else { $run = 0 }
    }
    if ($used -lt $MinSamples) {
        Write-Host "  travel-parity SKIP - only $used join dump(s) had a host dump within $($WinMs)ms"
        return (Add-GateResult -Name "travel_parity" -Status SKIP `
                    -Metrics @{ judged = $used } -Detail "too few aligned dumps")
    }
    $frac = [math]::Round($ghostSamples / $used, 3)
    $ok = ($frac -le $MaxGhostFrac) -and ($maxRun -le $MaxGhostRun)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  travel-parity $v - ghosts in $ghostSamples/$used samples (frac=$frac <= $MaxGhostFrac), longest run $maxRun (<= $MaxGhostRun), peak=$peakGhost; trueGhost=$trueGhosts laggard=$laggards diverged=$diverged hostOnly=$hostOnly"
    return (Add-GateResult -Name "travel_parity" -Status $v `
                -Metrics @{ judged = $used; ghostFrac = $frac; maxRun = $maxRun
                            peakGhost = $peakGhost; trueGhosts = $trueGhosts
                            laggards = $laggards; diverged = $diverged
                            hostOnly = $hostOnly })
}

# split_far gate: the two squads held apart in two POPULATED regions, past the
# census radius. Parses the SPLITFAR rows both sides emit:
#   SCENARIO SPLITFAR side=.. hop=.. sep=.. popMover=.. popStay=.. zMover=..
#                     zStay=.. settled=..
#
# What is judged is host/join AGREEMENT about the mover's neighbourhood while
# the pair is settled and apart. The join is standing there, so its popMover is
# ground truth; the host's popMover is the most its census could ever publish
# about that place. A host that sees far fewer bodies than the join is
# broadcasting "nobody there" for an inhabited region, and that is precisely how
# the join's real local NPCs end up in the ghost bucket.
#
# zMover from the HOST separates the two explanations: zMover=0 means the host's
# engine has not streamed that zone at all (the far-apart root cause - authority
# over a region the authority cannot see), while zMover=1 with a low popMover
# points at the census caps or anchor budgeting instead.
#
# SKIPs rather than passes when the precondition was not met - the pair never
# separated, or the join never found an inhabited stop. An empty corridor cannot
# judge the mechanism, and silently passing on one is how travel_parity kept
# reporting green for a bug it never exercised.
#
# MinPop is deliberately low. The signal this gate reads is a RATIO, and the
# observed failure is host=0 against join>0 - decisive at any join population.
# A high bar only buys vacuous SKIPs on a fixture whose far end is a hamlet.
function Test-SplitFar {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MinSep = 4000.0, [int]$MinPop = 3,
          [double]$MinSeeFrac = 0.5, [int]$MinSamples = 5)

    function Get-SplitFarRows([string]$File) {
        $rows = New-Object System.Collections.ArrayList
        if (-not (Test-Path $File)) { return $rows }
        $off = Get-LogClockOffsetMs -File $File
        # phase= is optional so pre-viewpoint-phase logs still parse (they are
        # all one continuous 'own' window by construction).
        $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SPLITFAR side=(\w+) " +
               "hop=(\d+) sep=([\d\.]+) popMover=(\d+) popStay=(\d+) " +
               "zMover=(\d+) zStay=(\d+) settled=(\d+)( phase=(\w+))?"
        foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            $ph = if ($g[14].Success) { $g[14].Value } else { "own" }
            [void]$rows.Add(@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                side = $g[5].Value; hop = [int]$g[6].Value
                sep = [double]$g[7].Value
                popMover = [int]$g[8].Value; popStay = [int]$g[9].Value
                zMover = [int]$g[10].Value; zStay = [int]$g[11].Value
                settled = [int]$g[12].Value; phase = $ph })
        }
        return $rows
    }
    function Get-Med($values) {
        $s = @($values | Sort-Object)
        if ($s.Count -eq 0) { return 0.0 }
        return [double]$s[[int]([Math]::Floor($s.Count / 2))]
    }

    $jrows = @(Get-SplitFarRows -File $JoinFile)
    $hrows = @(Get-SplitFarRows -File $HostFile)
    if ($jrows.Count -eq 0) {
        Write-Host "  SPLIT-FAR SKIP - no join SPLITFAR rows (scenario never armed)"
        return (Add-GateResult -Name "split_far" -Status SKIP -Detail "no join rows")
    }

    # The settle window: the host never hops, so 'settled' is only meaningful on
    # the join side. Take its first settled row and judge both sides from there.
    $settleRow = @($jrows | Where-Object { $_.settled -eq 1 } | Sort-Object { $_.t })
    if ($settleRow.Count -eq 0) {
        Write-Host "  SPLIT-FAR SKIP - join never settled (spiral found no inhabited stop in the window)"
        return (Add-GateResult -Name "split_far" -Status SKIP `
                    -Metrics @{ joinRows = $jrows.Count } -Detail "never settled")
    }
    $t0 = $settleRow[0].t
    $jAll = @($jrows | Where-Object { $_.t -ge $t0 -and $_.sep -ge $MinSep })
    $hAll = @($hrows | Where-Object { $_.t -ge $t0 -and $_.sep -ge $MinSep })

    # VIEWPOINT PHASES (see the split_far camera comment). Judge the gate on
    # 'own' alone - that is the configuration players are actually in, and it is
    # the one the reported symptom comes from. Folding 'cross' in would let the
    # host's count at the mover be inflated by the host looking straight at it,
    # which is the effect being measured rather than a property of replication.
    $jw = @($jAll | Where-Object { $_.phase -eq "own" })
    $hw = @($hAll | Where-Object { $_.phase -eq "own" })
    if ($jw.Count -eq 0) { $jw = $jAll; $hw = $hAll }
    if ($jw.Count -lt $MinSamples -or $hw.Count -lt $MinSamples) {
        Write-Host "  SPLIT-FAR SKIP - only join=$($jw.Count) host=$($hw.Count) settled samples past sep>=$MinSep (< $MinSamples)"
        return (Add-GateResult -Name "split_far" -Status SKIP `
                    -Metrics @{ joinSamples = $jw.Count; hostSamples = $hw.Count } `
                    -Detail "too few settled+apart samples")
    }

    $joinPop = Get-Med @($jw | ForEach-Object { $_.popMover })
    $sepMed  = [Math]::Round((Get-Med @($jw | ForEach-Object { $_.sep })), 0)
    if ($joinPop -lt $MinPop) {
        Write-Host "  SPLIT-FAR SKIP - settle point not inhabited (join popMover median=$joinPop < $MinPop); sep=$sepMed"
        return (Add-GateResult -Name "split_far" -Status SKIP `
                    -Metrics @{ joinPop = $joinPop; sep = $sepMed } `
                    -Detail "settle point empty - cannot judge")
    }

    $hostPop   = Get-Med @($hw | ForEach-Object { $_.popMover })
    $hostStay  = Get-Med @($hw | ForEach-Object { $_.popStay })
    $blind     = @($hw | Where-Object { $_.zMover -eq 0 }).Count
    $blindFrac = [Math]::Round($blind / $hw.Count, 3)
    $seeFrac   = if ($joinPop -gt 0) { [Math]::Round($hostPop / $joinPop, 3) } else { 0.0 }

    $ok = ($seeFrac -ge $MinSeeFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $why = if ($blindFrac -ge 0.5) { "host zone UNLOADED at the mover in $blindFrac of samples" }
           elseif (-not $ok) { "host zone loaded but census-blind (caps/anchor budget)" }
           else { "host sees the mover's region" }
    Write-Host "  SPLIT-FAR $v - sep=$sepMed, join sees $joinPop at the mover, host sees $hostPop (seeFrac=$seeFrac >= $MinSeeFrac); hostZoneUnloaded=$blindFrac, hostStayPop=$hostStay, samples join=$($jw.Count)/host=$($hw.Count) - $why"

    # Per-phase observation. Not a gate yet (a gate nobody has calibrated is a
    # gate that cannot fail - see pitfalls S7); this exists to characterise the
    # swap first. Read it as: does the HOST's count at the mover rise when the
    # host looks there ('cross'), and does it stay up after it looks away
    # ('back')? A rise says camera presence is what materialises the bodies; a
    # rise that persists says each client accumulates its own population over a
    # session, which is the long-session drift.
    $metrics = @{ sep = $sepMed; joinPop = $joinPop; hostPop = $hostPop
                  seeFrac = $seeFrac; zoneUnloadedFrac = $blindFrac
                  hostStayPop = $hostStay
                  joinSamples = $jw.Count; hostSamples = $hw.Count }
    # Drop each phase's opening seconds: a camera swap is followed by a zone
    # stream and a census round, so the first samples of a phase describe the
    # transition rather than the configuration.
    $PhaseSkipMs = 15000
    $phases = @("own", "both_stay", "both_mover", "back")
    if (@($jAll | Where-Object { $_.phase -ne "own" }).Count -gt 0) {
        foreach ($p in $phases) {
            $jp = @($jAll | Where-Object { $_.phase -eq $p })
            $hp = @($hAll | Where-Object { $_.phase -eq $p })
            if ($jp.Count -eq 0 -and $hp.Count -eq 0) { continue }
            # Rows are hashtables; Measure-Object -Property reads real
            # properties and finds nothing on one, so take the min by hand.
            $jp0 = @(@($jp | ForEach-Object { $_.t }) | Sort-Object)[0]
            $jp = @($jp | Where-Object { $_.t -ge ($jp0 + $PhaseSkipMs) })
            $hp = @($hp | Where-Object { $_.t -ge ($jp0 + $PhaseSkipMs) })
            if ($jp.Count -eq 0 -or $hp.Count -eq 0) { continue }
            $jm = Get-Med @($jp | ForEach-Object { $_.popMover })
            $hm = Get-Med @($hp | ForEach-Object { $_.popMover })
            $js = Get-Med @($jp | ForEach-Object { $_.popStay })
            $hs = Get-Med @($hp | ForEach-Object { $_.popStay })
            # In the overlap phases both clients are drawing the SAME squad, so
            # the gap between their counts at that squad is the one measurement
            # here with no viewpoint confound in it.
            $mark = ""
            if ($p -eq "both_stay")  { $mark = "  <- both drawing stay,  gap=" + [Math]::Abs($js - $hs) }
            if ($p -eq "both_mover") { $mark = "  <- both drawing mover, gap=" + [Math]::Abs($jm - $hm) }
            Write-Host ("    phase={0,-10} mover: join={1,-4} host={2,-4} | stay: join={3,-4} host={4,-4} (n={5}/{6}){7}" -f `
                        $p, $jm, $hm, $js, $hs, $jp.Count, $hp.Count, $mark)
            $metrics["${p}_joinMover"] = $jm; $metrics["${p}_hostMover"] = $hm
            $metrics["${p}_joinStay"]  = $js; $metrics["${p}_hostStay"]  = $hs
        }
    }
    return (Add-GateResult -Name "split_far" -Status $v -Metrics $metrics -Detail $why)
}

# split_far2 gate: presence authority. Reads three things, and the order they
# are checked in is the order they can invalidate each other:
#
#   0. PRECONDITION. The two tabs must be in DIFFERENT cells and claims must
#      actually be flowing. Same cell, or no [cell] lines at all (the flag was
#      off, or nothing claimed), and there is no authority to move - SKIP, not
#      pass. A gate that greens on a run which never exercised the mechanism is
#      worse than no gate (pitfalls S7).
#
#   1. AGREEMENT. Both clients dump their resolved map as
#        [cell] MAP cells=N slots=M X,Y=owner ...
#      and for every cell BOTH have an entry for, the owner must match. This is
#      the property the whole design rests on: two owners for one cell means two
#      clients authoring the same bodies, and nobody owning it means neither
#      does. Disagreement is a hard FAIL regardless of how the populations look.
#
#   2. PRESENCE. The join's own cell must resolve to the JOIN (a non-zero owner)
#      on both sides. Host id is 0 and unclaimed cells fall back to host, so
#      "the host owns the host's cell" is true by default and proves nothing;
#      the join owning the join's cell is the entire claim, and it is only true
#      if a claim was published, delivered and applied.
#
#   3. THE OWNER KEEPS ITS WORLD. The reported symptom is a population being
#      DESTROYED: "NPCs disappear as I get close and get replaced by the
#      host's". Its measurable form is the owner's own count of its own town
#      across the moment the peer's camera arrives - it must not be cut down
#      toward the peer's view. Both towns are checked, so the assertion is
#      symmetric rather than a statement about the join.
#
#      [attn] attach is REPORTED here rather than gated, and the reason is
#      worth stating because gating it was the first draft. A camera entering a
#      cold zone makes the LOCAL engine stream it and generate its own ambient
#      bodies; the cell's author never authored those, so culling them is the
#      convergence working, and it happens on the WATCHER, in a cell it does
#      not own. Counting those culls measures how differently two Kenshi
#      instances populate a zone - the content-parity problem this plan
#      explicitly does not solve - and a gate on it would fail for a cause the
#      code under test cannot affect. The culls that WOULD be a defect, an
#      author culling in its own cell, cannot occur: enforcement skips every
#      body in a cell we author.
function Test-SplitFar2 {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MinSamples = 5, [double]$MaxOwnerDrop = 0.25)

    function Get-SF2Rows([string]$File) {
        $rows = New-Object System.Collections.ArrayList
        if (-not (Test-Path $File)) { return $rows }
        $off = Get-LogClockOffsetMs -File $File
        $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SPLITFAR2 side=(\w+) " +
               "phase=(\w+) sep=([\d\.]+) hostCell=(\d)\((-?\d+),(-?\d+)\) " +
               "joinCell=(\d)\((-?\d+),(-?\d+)\) popHost=(\d+) popJoin=(\d+) " +
               "zHost=(\d+) zJoin=(\d+) arrived=(\d+)"
        foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$rows.Add(@{
                t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                side = $g[5].Value; phase = $g[6].Value; sep = [double]$g[7].Value
                hostCellOk = [int]$g[8].Value
                hostCell = "$($g[9].Value),$($g[10].Value)"
                joinCellOk = [int]$g[11].Value
                joinCell = "$($g[12].Value),$($g[13].Value)"
                popHost = [int]$g[14].Value; popJoin = [int]$g[15].Value
                zHost = [int]$g[16].Value; zJoin = [int]$g[17].Value
                arrived = [int]$g[18].Value })
        }
        return $rows
    }
    # The resolved cell->owner map as of $BeforeMs. Latest wins within the
    # window: claims are latest-sequence-wins and re-asserted, so the last dump
    # inside the run is the settled answer.
    #
    # The cutoff is not cosmetic. A session teardown clears every claim and the
    # surviving client immediately re-claims its own cell, so the very last
    # dumps in a log describe a world with one player in it - reading those made
    # the first version of this gate report "the host never learned the join's
    # cell" on a run whose logs show it learning it 200 s earlier and holding it.
    function Get-CellMap([string]$File, [double]$BeforeMs) {
        $map = @{}
        $n = 0
        if (-not (Test-Path $File)) { return @{ map = $map; dumps = 0 } }
        $off = Get-LogClockOffsetMs -File $File
        $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[cell\] MAP cells=(\d+) slots=(\d+)(.*)$"
        foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            if ((Convert-StampToMs -Groups $g -OffsetMs $off) -gt $BeforeMs) { continue }
            $n++
            $cur = @{}
            foreach ($p in ([regex]::Matches($g[7].Value, "(-?\d+),(-?\d+)=(\d+)"))) {
                $cur["$($p.Groups[1].Value),$($p.Groups[2].Value)"] = [int]$p.Groups[3].Value
            }
            $map = $cur
        }
        return @{ map = $map; dumps = $n }
    }
    function Get-AttachEvents([string]$File) {
        $ev = New-Object System.Collections.ArrayList
        if (-not (Test-Path $File)) { return $ev }
        $off = Get-LogClockOffsetMs -File $File
        $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[attn\] attach bodies=(\d+) " +
               "hid=\+(\d+) culled=\+(\d+) minted=([+-]?\d+)"
        foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$ev.Add(@{ t = (Convert-StampToMs -Groups $g -OffsetMs $off)
                             bodies = [int]$g[5].Value; hid = [int]$g[6].Value
                             culled = [int]$g[7].Value; minted = [int]$g[8].Value })
        }
        return $ev
    }
    function Get-Med($values) {
        $s = @($values | Sort-Object)
        if ($s.Count -eq 0) { return 0.0 }
        return [double]$s[[int]([Math]::Floor($s.Count / 2))]
    }

    $jrows = @(Get-SF2Rows -File $JoinFile)
    $hrows = @(Get-SF2Rows -File $HostFile)
    if ($jrows.Count -lt $MinSamples -or $hrows.Count -lt $MinSamples) {
        Write-Host "  SPLIT-FAR2 SKIP - join=$($jrows.Count) host=$($hrows.Count) SPLITFAR2 rows (< $MinSamples)"
        return (Add-GateResult -Name "split_far2" -Status SKIP `
                    -Metrics @{ joinRows = $jrows.Count; hostRows = $hrows.Count } `
                    -Detail "scenario produced too few rows")
    }

    # ---- 0. precondition --------------------------------------------------
    $last = $jrows[$jrows.Count - 1]
    $hostCell = $last.hostCell; $joinCell = $last.joinCell
    $sepMed = [Math]::Round((Get-Med @($jrows | ForEach-Object { $_.sep })), 0)
    if ($last.hostCellOk -ne 1 -or $last.joinCellOk -ne 1) {
        Write-Host "  SPLIT-FAR2 SKIP - cellAt() did not resolve (hostOk=$($last.hostCellOk) joinOk=$($last.joinCellOk)); the sector mapping is unavailable"
        return (Add-GateResult -Name "split_far2" -Status SKIP -Detail "cellAt unavailable")
    }
    if ($hostCell -eq $joinCell) {
        Write-Host "  SPLIT-FAR2 SKIP - both tabs in cell ($hostCell) at sep=$sepMed; authority cannot move within one cell"
        return (Add-GateResult -Name "split_far2" -Status SKIP `
                    -Metrics @{ sep = $sepMed; cell = $hostCell } `
                    -Detail "squads share a cell")
    }
    # Read both maps as of the last measured sample, not the end of the log.
    $endMs = $last.t
    $jm = Get-CellMap -File $JoinFile -BeforeMs $endMs
    $hm = Get-CellMap -File $HostFile -BeforeMs $endMs
    if ($jm.dumps -eq 0 -and $hm.dumps -eq 0) {
        Write-Host "  SPLIT-FAR2 SKIP - no [cell] MAP dumps in either log (KENSHICOOP_CELL_AUTH off?)"
        return (Add-GateResult -Name "split_far2" -Status SKIP `
                    -Metrics @{ sep = $sepMed; hostCell = $hostCell; joinCell = $joinCell } `
                    -Detail "cell authority disabled")
    }

    # ---- 1. agreement ------------------------------------------------------
    $disagree = New-Object System.Collections.ArrayList
    foreach ($k in $jm.map.Keys) {
        if ($hm.map.ContainsKey($k) -and $hm.map[$k] -ne $jm.map[$k]) {
            [void]$disagree.Add("$k join=$($jm.map[$k]) host=$($hm.map[$k])")
        }
    }

    # ---- 2. presence -------------------------------------------------------
    # Absent from a map = unclaimed = host by fail-open, which for the join's
    # own cell is exactly the failure this checks for.
    $jSaysJoin = if ($jm.map.ContainsKey($joinCell)) { $jm.map[$joinCell] } else { 0 }
    $hSaysJoin = if ($hm.map.ContainsKey($joinCell)) { $hm.map[$joinCell] } else { 0 }
    $presenceOk = ($jSaysJoin -ne 0) -and ($hSaysJoin -ne 0)

    # ---- attach, observed ---------------------------------------------------
    # Windows come from the JOIN's rows (both sides share the armed clock) and
    # each is trimmed to the phase as logged rather than to a nominal length,
    # so a slow arm shifts the window with the run.
    $attachCull = 0; $attachHid = 0; $attachEvents = 0
    foreach ($p in @("both_join", "both_host")) {
        $pr = @($jrows | Where-Object { $_.phase -eq $p })
        if ($pr.Count -eq 0) { continue }
        $ts = @(@($pr | ForEach-Object { $_.t }) | Sort-Object)
        $t0 = $ts[0]; $t1 = $ts[$ts.Count - 1]
        foreach ($f in @($HostFile, $JoinFile)) {
            foreach ($e in (Get-AttachEvents -File $f)) {
                if ($e.t -ge $t0 -and $e.t -le $t1) {
                    $attachCull += $e.culled; $attachHid += $e.hid; $attachEvents++
                }
            }
        }
    }

    # ---- per-phase populations, and gate 3 over them ------------------------
    # Each town read on BOTH sides. The owner's own column is the one that is
    # judged; the peer's is printed beside it so the read is legible.
    $PhaseSkipMs = 12000
    $ownerHost = @{}   # phase -> host's count of the HOST town (host owns it)
    $ownerJoin = @{}   # phase -> join's count of the JOIN town (join owns it)
    $table = New-Object System.Collections.ArrayList
    $metrics = @{ sep = $sepMed; hostCell = $hostCell; joinCell = $joinCell
                  joinCellOwnerJoin = $jSaysJoin; joinCellOwnerHost = $hSaysJoin
                  disagreements = $disagree.Count
                  attachCulled = $attachCull; attachHid = $attachHid
                  joinRows = $jrows.Count; hostRows = $hrows.Count }
    foreach ($p in @("walk", "own", "both_join", "both_host", "back")) {
        $jp = @($jrows | Where-Object { $_.phase -eq $p })
        $hp = @($hrows | Where-Object { $_.phase -eq $p })
        if ($jp.Count -eq 0 -or $hp.Count -eq 0) { continue }
        $t0 = @(@($jp | ForEach-Object { $_.t }) | Sort-Object)[0]
        $jp = @($jp | Where-Object { $_.t -ge ($t0 + $PhaseSkipMs) })
        $hp = @($hp | Where-Object { $_.t -ge ($t0 + $PhaseSkipMs) })
        if ($jp.Count -eq 0 -or $hp.Count -eq 0) { continue }
        $jH = Get-Med @($jp | ForEach-Object { $_.popHost })
        $hH = Get-Med @($hp | ForEach-Object { $_.popHost })
        $jJ = Get-Med @($jp | ForEach-Object { $_.popJoin })
        $hJ = Get-Med @($hp | ForEach-Object { $_.popJoin })
        [void]$table.Add(("    phase={0,-10} host town: join={1,-4} host={2,-4}* | join town: join={3,-4}* host={4,-4} (n={5}/{6})" -f `
                    $p, $jH, $hH, $jJ, $hJ, $jp.Count, $hp.Count))
        $metrics["${p}_hostTown_join"] = $jH; $metrics["${p}_hostTown_host"] = $hH
        $metrics["${p}_joinTown_join"] = $jJ; $metrics["${p}_joinTown_host"] = $hJ
        $ownerHost[$p] = $hH; $ownerJoin[$p] = $jJ
    }

    # ---- 3. the owner keeps its world --------------------------------------
    # Baseline is 'own': both cameras on their own squad, disjoint attention,
    # nobody looking at anybody else's region. Then the peer looks. The owner's
    # count may RISE (its zone finishes streaming, ambient spawns) - what it
    # must not do is fall toward the peer's, which is the reported symptom.
    $drops = New-Object System.Collections.ArrayList
    foreach ($t in @(@{ n = "host town"; s = $ownerHost }, @{ n = "join town"; s = $ownerJoin })) {
        if (-not $t.s.ContainsKey("own")) { continue }
        $base = [double]$t.s["own"]
        if ($base -le 0) { continue }
        $floor = [Math]::Max($base * (1.0 - $MaxOwnerDrop), $base - 2.0)
        foreach ($p in @("both_join", "both_host", "back")) {
            if (-not $t.s.ContainsKey($p)) { continue }
            if ([double]$t.s[$p] -lt $floor) {
                [void]$drops.Add("$($t.n) owner $base -> $($t.s[$p]) in $p")
            }
        }
    }
    $ownerOk = ($drops.Count -eq 0)

    $ok = ($disagree.Count -eq 0) -and $presenceOk -and $ownerOk
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $why = if ($disagree.Count -gt 0) { "clients disagree on cell ownership: $($disagree -join '; ')" }
           elseif (-not $presenceOk) { "join's cell ($joinCell) resolves to owner join=$jSaysJoin host=$hSaysJoin - authority did not follow presence" }
           elseif (-not $ownerOk) { "an owner's population was cut down when the peer looked: $($drops -join '; ')" }
           else { "each client authors its own cell, both agree, and neither loses its population when the peer looks" }
    Write-Host "  SPLIT-FAR2 $v - sep=$sepMed, hostCell=($hostCell) joinCell=($joinCell); joinCell owner: join says $jSaysJoin, host says $hSaysJoin; maps join=$($jm.map.Count)/host=$($hm.map.Count) cells, $($disagree.Count) disagreement(s); overlap-phase attach hid=$attachHid culled=$attachCull over $attachEvents event(s), reported not gated - $why"
    Write-Host "    (* = the cell's owner, the column gate 3 judges)"
    foreach ($line in $table) { Write-Host $line }
    return (Add-GateResult -Name "split_far2" -Status $v -Metrics $metrics -Detail $why)
}

# dual_drive gate: no body may be FOLLOWED by both clients at the same time.
#
# A census enforcement line ([census] park / walk / FREEZE) is a client saying
# "I am correcting my local copy of this body toward a position the PEER
# authored" - i.e. I am the FOLLOWER here, not the author. Exactly one client
# may say that about a given body at a given moment. When both do, neither is
# authoritative: each is chasing the other, and the body is driven twice.
#
# Found live on 2026-08-08 with presence authority on and both squads standing
# in ONE cell (22,19) that the host owned. The join owned no cells at all, yet
# published 43 census rows every 10 s ([census] sent n=43 enum=139 notmine=96)
# and the host obeyed them - 253 culls, 270 parks, 1727 freezes. 43 hands were
# followed by both clients, 10 of them concurrently, the worst for 314 s.
#
# Scored on OVERLAP DURATION, not on the mere fact of overlap. Authority
# legitimately hands over as squads move between cells, and a handover has a
# brief window where both sides act on one body; that is the mechanism working.
# Minutes of it is the defect. So a hand counts only once its concurrent
# follow time passes $MinOverlapMs.
#
# Two properties of the evidence shape the algorithm:
#
# 1. The census log lines are rate limited by a GLOBAL static tick, not a
#    per-hand one (ReplicatorAuthority.cpp: park ~4 lines/s, walk ~1 line/s for
#    the whole roster). With 43 bodies parked, any ONE hand surfaces roughly
#    every ten seconds even while it is being followed every frame. So the log
#    is a sparse SAMPLE of following, never a complete trace, and a detector
#    that tried to reconstruct exact intervals from it would mostly measure the
#    limiter. This one instead takes each side's first-to-last span for a hand
#    and then demands corroboration: $MinSamples enforcement lines from EACH
#    side landing INSIDE the shared window. Sparse sampling makes that harder
#    to satisfy, never easier, so the bias is toward missing a marginal case
#    rather than failing a good run.
# 2. [census] logs the LOCAL key (index,serial), and for census-band bodies -
#    world NPCs baked into the save both clients loaded - that key agrees
#    across clients, which is what makes the cross-log join valid at all. It
#    would NOT hold for minted bodies. The [life] lines the same bug report
#    quotes are in the 5-component wire keyspace and deliberately are not mixed
#    in here.
function Test-DualDrive {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MinOverlapMs = 5000, [int]$MaxHands = 0, [int]$MinSamples = 2)

    # ---- Signal 2: the driven ROSTER, straight from the 5 s WNPC dumps --------
    # Added 2026-08-08 after watching the DRV labels contradict this gate. A
    # scenario run showed 18 bodies wearing the green DRV marker on BOTH clients
    # at once, nine of them across consecutive dumps, while the census signal
    # below reported a worst overlap of 779 ms and PASSED.
    #
    # The two signals are not the same measurement and the census one is the
    # weaker: [census] park/walk/FREEZE fires only when a followed body has
    # DIVERGED enough to need correcting, and then only as the global rate
    # limiter allows. cls=drv is the drive set itself - proxy, or a hand with a
    # FRESH streamed sample in targets_, or drivenChars_ - which is exactly what
    # debugMark paints green. So this reads what the screen shows.
    #
    # Scored on CONSECUTIVE dumps, not on the count of them. Dumps are 5 s apart
    # and pairing allows +-$WinMs, so a handover landing between the host's dump
    # and the join's can put one body in one paired dump legitimately. A run of
    # two consecutive dumps spans >= 5 s and cannot be that. Measuring the run's
    # first-to-last span in ms makes a single-dump coincidence score 0 and lets
    # the same $MinOverlapMs bar mean the same thing for both signals.
    #
    # Requires auditRows (the manifest's scenario allowlist in Plugin.cpp). When
    # the dumps are absent this contributes nothing and the census signal decides
    # alone, which is the pre-2026-08-08 behaviour.
    #
    # Matched on the FULL five-component hand, not on index,serial. index,serial
    # is a local key that agrees across clients for world NPCs baked into the
    # shared save but not for MINTED bodies, where the two engines allocate
    # independently and the same pair can name two different bodies - the caveat
    # the census signal below already documents. run_apart makes it concrete: its
    # spawned Dust Bandit raids produced five bodies each apparently "drv on
    # both" for exactly 25 s, with the paired positions a median 1014-2408 u
    # apart, i.e. two separate raids beside two separate squads.
    #
    # Position was tried as the guard first and REJECTED, which is worth
    # recording: requiring the pair within 120 u cleared run_apart but also
    # erased a live-observed double-drive on rebirth1. For a body driven by both
    # clients, positional divergence is the SYMPTOM - each side keeps applying the
    # other's stream - so proximity is exactly the wrong thing to demand of the
    # cases that matter most. The container half of the hand carries identity
    # without assuming anything about where the body ended up.
    function Get-DrvBothRuns([string]$HostF, [string]$JoinF, [int]$WinMs) {
        $hostSamples = Group-WnpcSamples -Rows (Get-WnpcRows -File $HostF)
        $joinSamples = Group-WnpcSamples -Rows (Get-WnpcRows -File $JoinF)
        $paired = New-Object System.Collections.ArrayList
        foreach ($hsamp in $hostSamples) {
            $jsamp = $null
            foreach ($cand in $joinSamples) {
                if ([math]::Abs($cand.t - $hsamp.t) -gt $WinMs) { continue }
                if ($null -eq $jsamp -or
                    [math]::Abs($cand.t - $hsamp.t) -lt [math]::Abs($jsamp.t - $hsamp.t)) { $jsamp = $cand }
            }
            if ($null -eq $jsamp) { continue }
            $hDrv = @{}
            foreach ($row in $hsamp.rows) { if ($row.cls -eq 'drv') { $hDrv[$row.hand5] = $row } }
            $both = @{}
            foreach ($row in $jsamp.rows) {
                if ($row.cls -ne 'drv') { continue }
                if (-not $hDrv.ContainsKey($row.hand5)) { continue }
                $both[$row.hand5] = $row.name
            }
            [void]$paired.Add(@{ t = $hsamp.t; both = $both })
        }
        $best = @{}; $runStart = @{}; $runLast = @{}
        for ($i = 0; $i -lt $paired.Count; $i++) {
            $p = $paired[$i]
            foreach ($hand in $p.both.Keys) {
                if (-not $runStart.ContainsKey($hand) -or $runLast[$hand] -ne ($i - 1)) {
                    $runStart[$hand] = $i
                }
                $runLast[$hand] = $i
                $span = $p.t - $paired[$runStart[$hand]].t
                if (-not $best.ContainsKey($hand) -or $span -gt $best[$hand].ms) {
                    $best[$hand] = @{ ms = $span; name = $p.both[$hand]
                                      dumps = ($i - $runStart[$hand] + 1) }
                }
            }
        }
        return @{ paired = $paired.Count; hands = $best }
    }

    # Per-hand sorted enforcement sample times on one client, shared clock.
    # park has FUTILE and ANCHOR-BREAK variants that are equally evidence of
    # following, hence the optional qualifier.
    function Get-FollowSamples([string]$File) {
        $s = @{}
        if (-not (Test-Path $File)) { return $s }
        $off = Get-LogClockOffsetMs -File $File
        $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[census\] " +
               "(?:park|walk|FREEZE)(?: [A-Z\-]+)? hand=(\d+,\d+)"
        foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            $h = $g[5].Value
            if (-not $s.ContainsKey($h)) { $s[$h] = New-Object System.Collections.ArrayList }
            [void]$s[$h].Add((Convert-StampToMs -Groups $g -OffsetMs $off))
        }
        return $s
    }

    if (-not (Test-Path $HostFile) -or -not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "dual_drive" -Status SKIP -Detail "missing log")
    }
    # Deliberately NOT $H/$J: PowerShell variable names are case-insensitive, so
    # a $h loop variable below would silently overwrite the host table.
    $roster = Get-DrvBothRuns $HostFile $JoinFile 3000
    $rosterBad = @($roster.hands.GetEnumerator() | Where-Object { $_.Value.ms -ge $MinOverlapMs })
    $rosterWorst = 0; $rosterWorstHand = ""
    foreach ($e in $roster.hands.GetEnumerator()) {
        if ($e.Value.ms -gt $rosterWorst) { $rosterWorst = $e.Value.ms; $rosterWorstHand = $e.Key }
    }

    $hostSeen = Get-FollowSamples -File $HostFile
    $joinSeen = Get-FollowSamples -File $JoinFile
    # Both sides must be following SOMETHING before "both at once" is a question
    # worth asking. With presence authority off the split is exclusive by
    # construction - the host follows nothing - so the intersection would be
    # trivially empty and the gate would report a green it never tested.
    #
    # The roster signal is exempt from this: it does not need either side to have
    # logged a reconciliation event, so when it has paired dumps it can answer
    # even where the census signal is mute. Only skip when NEITHER can speak.
    if (($hostSeen.Count -eq 0 -or $joinSeen.Count -eq 0) -and $roster.paired -eq 0) {
        $which = if ($hostSeen.Count -eq 0 -and $joinSeen.Count -eq 0) { "neither client" }
                 elseif ($hostSeen.Count -eq 0) { "the host" } else { "the join" }
        Write-Host "  DUAL-DRIVE SKIP - $which follows any body (host=$($hostSeen.Count) join=$($joinSeen.Count) hands); exclusive authority, nothing to contend"
        return (Add-GateResult -Name "dual_drive" -Status SKIP `
                    -Metrics @{ hostHands = $hostSeen.Count; joinHands = $joinSeen.Count } `
                    -Detail "one side follows nothing (cell authority off?)")
    }

    $shared = @($hostSeen.Keys | Where-Object { $joinSeen.ContainsKey($_) })
    $offenders = New-Object System.Collections.ArrayList
    $worst = 0.0; $worstHand = ""
    foreach ($hand in $shared) {
        $hs = @($hostSeen[$hand] | Sort-Object); $js = @($joinSeen[$hand] | Sort-Object)
        $lo = [Math]::Max($hs[0], $js[0])
        $hi = [Math]::Min($hs[$hs.Count - 1], $js[$js.Count - 1])
        $ov = $hi - $lo
        if ($ov -le 0) { continue }          # sequential handover, not concurrent
        if ($ov -gt $worst) { $worst = $ov; $worstHand = $hand }
        $hIn = @($hs | Where-Object { $_ -ge $lo -and $_ -le $hi }).Count
        $jIn = @($js | Where-Object { $_ -ge $lo -and $_ -le $hi }).Count
        if ($ov -ge $MinOverlapMs -and $hIn -ge $MinSamples -and $jIn -ge $MinSamples) {
            [void]$offenders.Add(@{ hand = $hand; ms = $ov; hostN = $hIn; joinN = $jIn })
        }
    }

    # Either signal alone is enough to condemn a run: they see different parts of
    # the same fault and neither is a check on the other.
    $ok = ($offenders.Count -le $MaxHands) -and ($rosterBad.Count -le $MaxHands)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $metrics = @{
        hostHands = $hostSeen.Count; joinHands = $joinSeen.Count
        sharedHands = $shared.Count
        concurrentHands = $offenders.Count
        worstOverlapMs = [int]$worst
        drvPairedDumps = $roster.paired
        drvBothHands = $roster.hands.Count
        drvSustainedHands = $rosterBad.Count
        drvWorstMs = [int]$rosterWorst
        drvWorstHand = $rosterWorstHand
    }
    $top = @($offenders | Sort-Object { -$_.ms } | Select-Object -First 5 |
             ForEach-Object { "$($_.hand) $([int]($_.ms / 1000))s (host $($_.hostN)/join $($_.joinN) lines)" }) -join ', '
    $drvTop = @($rosterBad | Sort-Object { -$_.Value.ms } | Select-Object -First 5 |
                ForEach-Object { "$($_.Key) '$($_.Value.name)' $([int]($_.Value.ms / 1000))s ($($_.Value.dumps) dumps)" }) -join ', '
    $why = if (-not $ok -and $rosterBad.Count -gt $MaxHands) {
               "$($rosterBad.Count) body(ies) in the DRIVEN SET of both clients across " +
               "consecutive dumps: $drvTop" +
               $(if ($offenders.Count -gt $MaxHands) { "; census signal agrees on $($offenders.Count): $top" } else { "" })
           } elseif (-not $ok) {
               "$($offenders.Count) body(ies) followed by BOTH clients at once: $top"
           } else {
               "no body corroborated as followed by both clients for more than $([int]($MinOverlapMs / 1000))s"
           }
    Write-Host "  DUAL-DRIVE $v - $why (census: shared=$($shared.Count) of host $($hostSeen.Count)/join $($joinSeen.Count) followed hands, widest=$([int]($worst / 1000))s on $worstHand; roster: $($roster.hands.Count) hand(s) drv on both over $($roster.paired) paired dump(s), widest=$([int]($rosterWorst / 1000))s on $rosterWorstHand)"
    return (Add-GateResult -Name "dual_drive" -Status $v -Metrics $metrics -Detail $why)
}

# world_parity gate: full-roster tiered cross-comparison on a dense save.
# Both sides dump SCENARIO WNPC rows every 5 s (with task=/pelvis=/mv= parity
# fields and cls=pc player rows). Each HOST dump sample is paired with the
# nearest join dump (+-$WinMs) and judged in three tiers, anchored on the
# host's own cls=pc positions:
#   PC tier     - every host pc hand must exist in the join dump (presence
#                 ratio >= $PcExistMin) and its per-hand MEDIAN distance must
#                 be <= $PcTol. A diverged host-PC is exactly the class every
#                 other oracle excludes (NPC dumps skip the player squad).
#   near tier   - host NPC rows within $NearRange of a PC anchor: the join
#                 must hold the hand (exist ratio >= $NearExistMin, cls=hid
#                 counts as MISSING - a suppressed body the host vouches for
#                 is wrong), track it (pair dist <= $NearPosTol at ratio >=
#                 $PosOkMin), and reproduce its task (equality ratio >=
#                 $NearTaskMin; task is the MEMBER/RECV pose vocabulary, the
#                 agreed anim stand-in).
#   census tier - $NearRange..$CensusRange: existence (>= $CensusExistMin)
#                 and position within the park bound ($CensusPosTol at ratio
#                 >= $PosOkMin); task NOT judged (legit local-sim copies).
# The first $GraceMs after the join's first dump are skipped (clock catch-up
# slew + mint/far-mint spin-up are startup transients, not steady state), and
# so is everything more than $TailGraceMs past the join's LAST dump: the host
# deliberately OUTLIVES the join (~20 s), and a host sample paired backwards
# onto the join's final dump compares a live host sim against a join that has
# stopped simming. That tail alone produced the whole verdict in 2 of 3 camp
# runs - 20260806_095354's ONLY rest pair sat 3.4 s past the join's last dump
# (gap 16.2) and 20260806_113219's worst sat 1.6 s past it (gap 214.3, in a run
# whose PC otherwise held 0.9 u for eight consecutive samples).
# Missing hands and the worst POSITION offenders are named per tier in the
# detail, so a failure reports every tier that broke rather than just the PC.

# Tally a body that BLEW its tier's position bound: how often, and how far at
# its worst. Keyed by hand so one persistently mis-tracked body is reported as
# one named offender instead of N anonymous bad pairs (run 20260806_100102: a
# single census-parked Holy Sentinel owned 25 of the 61 near-band failures).
function Add-PosOffender {
    param($Tally, $Row, [double]$Gap)
    $k = $Row.hand
    if (-not $Tally.ContainsKey($k)) {
        $Tally[$k] = @{ bad = 0; worst = 0.0; name = $Row.name }
    }
    $Tally[$k].bad++
    if ($Gap -gt $Tally[$k].worst) { $Tally[$k].worst = $Gap }
}

# Format the worst $Top position offenders as "hand:'name'xN(worst)".
function Format-PosOffenders {
    param($Tally, [int]$Top = 5)
    return (($Tally.GetEnumerator() | Sort-Object { $_.Value.bad } -Descending |
             Select-Object -First $Top | ForEach-Object {
                "$($_.Key):'$($_.Value.name)'x$($_.Value.bad)(worst $([math]::Round($_.Value.worst,1)))"
             }) -join ", ")
}

# True median. The previous inline sorted[count/2] is the upper element on an
# even count, which on the 2-sample series world_parity often ends up with
# reported the WORSE of the two as the "median" (run 20260806_113219: 12.1 and
# 214.3 judged as 214.3).
function Get-Median {
    param($Values)
    $s = @($Values | Sort-Object)
    if ($s.Count -eq 0) { return -1.0 }
    if ($s.Count % 2 -eq 1) { return [double]$s[[int]($s.Count / 2)] }
    return (([double]$s[$s.Count / 2 - 1] + [double]$s[$s.Count / 2]) / 2.0)
}

function Test-WorldParity {
    # Position pairs are judged strictly only when the HOST row is AT REST
    # (mv=0): paired dumps are up to $WinMs apart, so a walking body shows
    # ~walk-speed x misalignment of apparent gap (~30-80 u) with perfectly
    # healthy tracking. Moving pairs get $MoverAllow on top of the tier bound.
    # $CensusRange stays inside the JOIN's 2000 u enumeration edge (the host
    # census reaches 2500 u; a hand at 1900-2000 u from the join's anchors
    # flaps in/out of its dumps as pure enumeration jitter, not desync).
    # $NearRange is the HOST's position-stream bubble (~200 u - see the
    # "Position streaming stays at the ~200 u" note in ReplicatorPublish.cpp and
    # the 200 u interest capture radius in ReplicatorSpawn.cpp), NOT a free
    # choice. Beyond it the host broadcasts EXISTENCE only (protocol 36) and the
    # join reconciles with parkDivergedCopy's 120 u threshold, so a body out
    # there physically cannot hold a 10 u gap: judging the old 260 u band at the
    # near bound charged the census contract with stream-grade precision. Run
    # 20260806_100102 bucketed by anchor distance: 0-199 u failed 26 of 164
    # pairs while 200-249 u failed 33 of 47 (70%), and by the join's own class,
    # census-parked bodies failed 29 of 35 (83%) against 32 of 182 (18%) for
    # driven ones. The annulus now falls into the census tier, which judges it
    # at $CensusPosTol - the bound the protocol actually promises there.
    # $PcMinRest guards the other direction: a median over 1-2 samples is not a
    # measurement, and the old sorted[count/2] returned the LARGER of two.
    param([string]$HostFile, [string]$JoinFile,
          [double]$PcTol = 5.0, [double]$PcExistMin = 0.9,
          [double]$NearRange = 200.0, [double]$NearPosTol = 10.0,
          [double]$NearExistMin = 0.9, [double]$NearTaskMin = 0.8,
          [double]$CensusRange = 1800.0, [double]$CensusPosTol = 120.0,
          [double]$CensusExistMin = 0.7, [double]$PosOkMin = 0.8,
          [double]$MoverAllow = 100.0, [int]$PcMinRest = 3,
          [int]$WinMs = 6000, [int]$MinSamples = 6, [int]$GraceMs = 45000,
          [int]$TailGraceMs = 1000)
    $hostAll = Get-WnpcRows -File $HostFile
    $joinAll = Get-WnpcRows -File $JoinFile
    $hostSamples = Group-WnpcSamples -Rows $hostAll
    $joinSamples = Group-WnpcSamples -Rows $joinAll
    if ($hostSamples.Count -lt $MinSamples -or $joinSamples.Count -lt $MinSamples) {
        Write-Host "  world-parity SKIP - $($hostSamples.Count) host / $($joinSamples.Count) join dump sample(s)"
        return (Add-GateResult -Name "world_parity" -Status SKIP `
                    -Metrics @{ hostSamples = $hostSamples.Count; joinSamples = $joinSamples.Count } `
                    -Detail "too few worldstate dumps")
    }
    $joinT0 = $joinSamples[0].t
    $joinTEnd = $joinSamples[$joinSamples.Count - 1].t
    # Per-PC-hand accumulators; per-tier counters; named missing tallies.
    # pcDists = rest-paired (host mv=0) distances; pcDistsAll = every pair
    # (fallback for a PC that never rests - a chained PC streams mv=1
    # continuously - judged against PcTol + MoverAllow).
    # pcCarried counts the PASSENGER samples dropped, for the metrics.
    $pcSeen = @{}; $pcHave = @{}; $pcDists = @{}; $pcDistsAll = @{}; $pcNames = @{}
    $nearTotal = 0; $nearHave = 0; $nearPosPairs = 0; $nearPosOk = 0
    $taskPairs = 0; $taskMatch = 0; $combatPairs = 0; $combatMatch = 0
    $cenTotal = 0; $cenHave = 0; $cenPosPairs = 0; $cenPosOk = 0
    $missNear = @{}; $missCen = @{}
    # Worst POSITION offenders per tier: hand -> @{ n; bad; worst; name }. The
    # old detail named only MISSING hands, so a near/census position failure
    # printed nothing and read as a PC-only bug (run 20260806_100102: near
    # pos=0.719 failed silently behind 'pc-bad: Flashbox').
    $posNear = @{}; $posCen = @{}
    $pcCarried = 0; $tailSkipped = 0
    $used = 0
    foreach ($hs in $hostSamples) {
        if (($hs.t - $joinT0) -lt $GraceMs) { continue }
        # Tail guard: the join's dump series must BRACKET the host sample. Past
        # its final dump the nearest-dump pairing stops being an interpolation
        # and becomes an extrapolation against a join that is shutting down, so
        # the host's continued motion is charged to the peer. $TailGraceMs is
        # only slop for residual clock skew between the two logs.
        if (($hs.t - $joinTEnd) -gt $TailGraceMs) { $tailSkipped++; continue }
        $js = $null
        foreach ($cand in $joinSamples) {
            if ([math]::Abs($cand.t - $hs.t) -gt $WinMs) { continue }
            if ($null -eq $js -or [math]::Abs($cand.t - $hs.t) -lt [math]::Abs($js.t - $hs.t)) { $js = $cand }
        }
        if ($null -eq $js) { continue }
        $used++
        $jByHand = @{}
        foreach ($jr in $js.rows) { $jByHand[$jr.hand] = $jr }
        # Anchors: the host's own PC positions this sample.
        $anchors = @($hs.rows | Where-Object { $_.cls -eq "pc" })
        foreach ($hr in $hs.rows) {
            if ($hr.cls -eq "pc") {
                # PC tier
                if (-not $pcSeen.ContainsKey($hr.hand)) {
                    $pcSeen[$hr.hand] = 0; $pcHave[$hr.hand] = 0
                    $pcDists[$hr.hand] = New-Object System.Collections.ArrayList
                    $pcDistsAll[$hr.hand] = New-Object System.Collections.ArrayList
                    $pcNames[$hr.hand] = $hr.name
                }
                $pcSeen[$hr.hand]++
                if ($jByHand.ContainsKey($hr.hand) -and $jByHand[$hr.hand].cls -eq "pc") {
                    $pcHave[$hr.hand]++
                    # Strict distance judged only when BOTH sides are at rest
                    # (mv=0): dumps pair up to $WinMs apart, so any PC that is
                    # walking on either side (an escorted/marched PC walks on
                    # the join while the host's driven copy trails or rests)
                    # shows walk-speed x misalignment of apparent gap while
                    # tracking perfectly. All pairs also recorded for the
                    # never-at-rest fallback.
                    $jrow = $jByHand[$hr.hand]
                    # PASSENGER guard: a body on someone's shoulder has no
                    # position of its own on either client - it is wherever its
                    # carrier is - so its gap measures the CARRIER's tracking,
                    # which the carrier's own NPC row already judges. Worse, a
                    # passenger is not locomoting, so it reports mv=0 and the
                    # rest filter below SELECTS it preferentially. On the camp
                    # save the host's Sentinels arrest the join-owned PC and
                    # haul it around, which is how 10 of 15 rest pairs became
                    # carry samples and pulled the median from 10.0 to 63.5 u
                    # (run 20260806_100102, where PCgap equalled carrierGap to
                    # within 0.1 u sample for sample).
                    if ($hr.carry -eq 1 -or $jrow.carry -eq 1) { $pcCarried++; continue }
                    $jp = $jrow.pos
                    $dx = $hr.pos[0] - $jp[0]; $dz = $hr.pos[2] - $jp[2]
                    $dd = [math]::Sqrt($dx * $dx + $dz * $dz)
                    [void]$pcDistsAll[$hr.hand].Add($dd)
                    if ($hr.mv -eq 0 -and $jrow.mv -eq 0) { [void]$pcDists[$hr.hand].Add($dd) }
                }
                continue
            }
            # NPC tiers: band by distance to the nearest host PC anchor.
            $band = $null
            foreach ($a in $anchors) {
                $dx = $hr.pos[0] - $a.pos[0]; $dz = $hr.pos[2] - $a.pos[2]
                $d = [math]::Sqrt($dx * $dx + $dz * $dz)
                if ($d -le $NearRange) { $band = "near"; break }
                if ($d -le $CensusRange -and $null -eq $band) { $band = "cen" }
            }
            if ($null -eq $band) { continue } # beyond the census reach: unjudged
            $jr = $null
            if ($jByHand.ContainsKey($hr.hand)) { $jr = $jByHand[$hr.hand] }
            $present = ($null -ne $jr -and $jr.cls -ne "hid")
            $posBound = if ($hr.mv -eq 0) { 0.0 } else { $MoverAllow }
            if ($band -eq "near") {
                $nearTotal++
                if ($present) {
                    $nearHave++
                    # Fight-class = synthetic combat stances (65000-65534:
                    # TASK_COMBAT_*) plus the native TaskType attack family
                    # (4/5/9/10/11/13/16/21 = MELEE_ATTACK..ATTACK_ENEMIES_
                    # AND_NEUTRALS), CHASE (46) and combat-aftermath
                    # FIRST_AID_ORDER (25; run 020025: host sentinels
                    # bandaging their recapture victims). Event-driven and
                    # timing-jittered across dumps up to $WinMs apart.
                    $fight = @(4, 5, 9, 10, 11, 13, 16, 21, 25, 46)
                    $hCombat = ($hr.task -ge 65000 -and $hr.task -lt 65535) -or ($fight -contains [int]$hr.task)
                    $jCombat = ($jr.task -ge 65000 -and $jr.task -lt 65535) -or ($fight -contains [int]$jr.task)
                    $inFight = $hCombat -or $jCombat
                    # Melee footwork reports mv=0 (stance, not locomotion) while
                    # the body still slides 10-100 u between paired dumps
                    # (world_parity residual: 13 of 25 in-bubble position
                    # failures were combat+hmv=0 inside 50 u of a PC). Give
                    # fight-class pairs the same mover allowance the walk
                    # branch already gets; the 10 u rest bound stays for
                    # non-combat near bodies.
                    if ($inFight -and $posBound -lt $MoverAllow) { $posBound = $MoverAllow }
                    $dx = $hr.pos[0] - $jr.pos[0]; $dz = $hr.pos[2] - $jr.pos[2]
                    $nearPosPairs++
                    $gap = [math]::Sqrt($dx * $dx + $dz * $dz)
                    if ($gap -le ($NearPosTol + $posBound)) { $nearPosOk++ }
                    else { Add-PosOffender -Tally $posNear -Row $hr -Gap $gap }
                    if ($hr.task -ge 0 -and $jr.task -ge 0) {
                        # Fight-class task equality is advisory only (run
                        # 014948: 265 combat pairs in one camp fight drowned
                        # the job/pose signal). Tracked separately, not gated.
                        if ($inFight) {
                            $combatPairs++
                            if ($hr.task -eq $jr.task) { $combatMatch++ }
                        } else {
                            $taskPairs++
                            if ($hr.task -eq $jr.task) { $taskMatch++ }
                        }
                    }
                } else {
                    $k = "$($hr.hand):'$($hr.name)'"
                    if (-not $missNear.ContainsKey($k)) { $missNear[$k] = 0 }
                    $missNear[$k]++
                }
            } else {
                $cenTotal++
                if ($present) {
                    $cenHave++
                    $dx = $hr.pos[0] - $jr.pos[0]; $dz = $hr.pos[2] - $jr.pos[2]
                    $cenPosPairs++
                    $gap = [math]::Sqrt($dx * $dx + $dz * $dz)
                    if ($gap -le ($CensusPosTol + $posBound)) { $cenPosOk++ }
                    else { Add-PosOffender -Tally $posCen -Row $hr -Gap $gap }
                } else {
                    $k = "$($hr.hand):'$($hr.name)'"
                    if (-not $missCen.ContainsKey($k)) { $missCen[$k] = 0 }
                    $missCen[$k]++
                }
            }
        }
    }
    if ($used -lt $MinSamples) {
        Write-Host "  world-parity SKIP - only $used aligned dump sample(s) after grace"
        return (Add-GateResult -Name "world_parity" -Status SKIP `
                    -Metrics @{ judged = $used } -Detail "too few aligned dumps")
    }
    # PC verdict: every host PC hand present at >= $PcExistMin of its samples,
    # per-hand MEDIAN distance <= $PcTol. The strict rest bound needs at least
    # $PcMinRest rest pairs behind it: below that the "median" is one or two
    # samples, and on this save those few are exactly the arrest/haul episodes
    # (see the passenger and tail guards above), so a PC that tracked at 0.9 u
    # for most of the run was failed at 214.3. With too few rest pairs, fall
    # back to the all-pairs mover bound - the same treatment a never-resting PC
    # already gets - and report restN so the thin sample is visible.
    $pcJudged = 0; $pcBad = New-Object System.Collections.ArrayList
    $pcWorst = 0.0
    foreach ($h in $pcSeen.Keys) {
        if ($pcSeen[$h] -lt 3) { continue } # too transient to judge
        $pcJudged++
        $ratio = $pcHave[$h] / $pcSeen[$h]
        $med = -1.0; $bound = $PcTol
        $restN = $pcDists[$h].Count
        if ($restN -ge $PcMinRest) {
            $med = [math]::Round((Get-Median -Values $pcDists[$h]), 1)
        } elseif ($pcDistsAll[$h].Count -gt 0) {
            # Never at rest (e.g. a chained PC streams mv=1 continuously), or
            # too few rest pairs to form a median: judge all pairs with the
            # mover misalignment allowance.
            $med = [math]::Round((Get-Median -Values $pcDistsAll[$h]), 1)
            $bound = $PcTol + $MoverAllow
        }
        if ($med -gt $pcWorst) { $pcWorst = $med }
        if ($ratio -lt $PcExistMin -or $med -lt 0 -or $med -gt $bound) {
            [void]$pcBad.Add("$($pcNames[$h])($h) exist=$([math]::Round($ratio,2)) med=$med bound=$bound restN=$restN")
        }
    }
    $pcOk = ($pcJudged -ge 1 -and $pcBad.Count -eq 0)
    # Near/census verdicts.
    $nearExist = if ($nearTotal -gt 0) { [math]::Round($nearHave / $nearTotal, 3) } else { -1 }
    $nearPosR  = if ($nearPosPairs -gt 0) { [math]::Round($nearPosOk / $nearPosPairs, 3) } else { -1 }
    $taskR     = if ($taskPairs -gt 0) { [math]::Round($taskMatch / $taskPairs, 3) } else { -1 }
    $combatR   = if ($combatPairs -gt 0) { [math]::Round($combatMatch / $combatPairs, 3) } else { -1 }
    $cenExist  = if ($cenTotal -gt 0) { [math]::Round($cenHave / $cenTotal, 3) } else { -1 }
    $cenPosR   = if ($cenPosPairs -gt 0) { [math]::Round($cenPosOk / $cenPosPairs, 3) } else { -1 }
    $nearOk = ($nearTotal -eq 0) -or
              (($nearExist -ge $NearExistMin) -and
               ($nearPosPairs -lt 10 -or $nearPosR -ge $PosOkMin) -and
               ($taskPairs -lt 10 -or $taskR -ge $NearTaskMin))
    $cenOk  = ($cenTotal -eq 0) -or
              (($cenExist -ge $CensusExistMin) -and
               ($cenPosPairs -lt 10 -or $cenPosR -ge $PosOkMin))
    $ok = $pcOk -and $nearOk -and $cenOk
    $v = if ($ok) { "PASS" } else { "FAIL" }
    # Name the worst offenders for direct diagnosis.
    $topNear = ($missNear.GetEnumerator() | Sort-Object Value -Descending |
                Select-Object -First 5 | ForEach-Object { "$($_.Key)x$($_.Value)" }) -join ", "
    $topCen  = ($missCen.GetEnumerator() | Sort-Object Value -Descending |
                Select-Object -First 5 | ForEach-Object { "$($_.Key)x$($_.Value)" }) -join ", "
    # Position offenders are named whenever their tier's ratio gate FAILED -
    # that is the diagnosis the old detail omitted entirely.
    $topNearPos = ""
    if ($nearPosPairs -ge 10 -and $nearPosR -lt $PosOkMin) {
        $topNearPos = Format-PosOffenders -Tally $posNear
    }
    $topCenPos = ""
    if ($cenPosPairs -ge 10 -and $cenPosR -lt $PosOkMin) {
        $topCenPos = Format-PosOffenders -Tally $posCen
    }
    $detailParts = New-Object System.Collections.ArrayList
    if ($pcBad.Count -gt 0) { [void]$detailParts.Add("pc-bad: $($pcBad -join '; ')") }
    if ($topNear) { [void]$detailParts.Add("near-missing: $topNear") }
    if ($topNearPos) { [void]$detailParts.Add("near-pos: $topNearPos") }
    if ($topCen) { [void]$detailParts.Add("census-missing: $topCen") }
    if ($topCenPos) { [void]$detailParts.Add("census-pos: $topCenPos") }
    $detail = $detailParts -join " | "
    Write-Host "  world-parity $v - $used samples (tailSkip=$tailSkipped); PC $pcJudged judged worstMed=$pcWorst (<= $PcTol, bad=$($pcBad.Count), carrySkip=$pcCarried); near exist=$nearExist (>= $NearExistMin) pos=$nearPosR task=$taskR (>= $NearTaskMin) combat=$combatR/$combatPairs (advisory) n=$nearTotal (band <= $NearRange u); census exist=$cenExist (>= $CensusExistMin) pos=$cenPosR n=$cenTotal"
    if ($detail) { Write-Host "    $detail" }
    return (Add-GateResult -Name "world_parity" -Status $v `
                -Metrics @{ judged = $used; pcJudged = $pcJudged; pcWorstMed = $pcWorst
                            pcBad = $pcBad.Count; nearExist = $nearExist
                            nearPosOk = $nearPosR; taskParity = $taskR
                            combatParity = $combatR; combatPairs = $combatPairs
                            nearTotal = $nearTotal; cenExist = $cenExist
                            cenPosOk = $cenPosR; cenTotal = $cenTotal
                            pcCarrySkip = $pcCarried; tailSkip = $tailSkipped
                            nearRange = $NearRange } `
                -Detail $detail)
}

# Phase 2 anti-zombie gate: census-band NPCs must MOVE on the join when their
# host originals move. For every pair of consecutive HOST worldstate dumps
# (5 s apart), each hand whose host copy advanced >= $HostMoveMin is a judged
# window; the join copy (nearest join dumps in time, cls != hid) must have
# advanced at least $MoveRatio of the host's distance. Before the mid-band
# tier, a bandit outside the ~200 u stream bubble got NO positional stream at
# all - its local copy stood frozen between 120 u census parks (the "zombie
# NPC" field report). SKIPs when the corridor produced too few judged windows
# (empty wilderness runs are common in travel_parity).
function Test-AntiZombie {
    param([string]$HostFile, [string]$JoinFile,
          [double]$HostMoveMin = 10.0, [double]$MoveRatio = 0.2,
          [double]$MaxZombieFrac = 0.30, [int]$MinWindows = 6,
          [int]$WinMs = 6000)
    if (-not (Test-Path $HostFile) -or -not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "anti_zombie" -Status SKIP -Detail "missing log")
    }
    $hostS = @(Group-WnpcSamples -Rows (Get-WnpcRows -File $HostFile))
    $joinS = @(Group-WnpcSamples -Rows (Get-WnpcRows -File $JoinFile))
    if ($hostS.Count -lt 2 -or $joinS.Count -lt 2) {
        Write-Host "  anti-zombie SKIP - $($hostS.Count) host / $($joinS.Count) join dump(s)"
        return (Add-GateResult -Name "anti_zombie" -Status SKIP `
                    -Metrics @{ hostDumps = $hostS.Count; joinDumps = $joinS.Count } `
                    -Detail "too few worldstate dumps")
    }
    # hand -> position per dump, keyed for pairwise lookup.
    function DumpMap($s) {
        $m = @{}
        foreach ($r in $s.rows) { if ($r.cls -ne 'hid') { $m[$r.hand] = $r.pos } }
        return $m
    }
    $judged = 0; $zombies = 0; $zombieHands = @{}
    for ($i = 0; $i + 1 -lt $hostS.Count; $i++) {
        $h0 = $hostS[$i]; $h1 = $hostS[$i + 1]
        if (($h1.t - $h0.t) -gt 3 * $WinMs) { continue } # dump gap; not a window
        $m0 = DumpMap $h0; $m1 = DumpMap $h1
        # Nearest join dumps to each host dump edge.
        $j0 = $joinS | Sort-Object { [Math]::Abs($_.t - $h0.t) } | Select-Object -First 1
        $j1 = $joinS | Sort-Object { [Math]::Abs($_.t - $h1.t) } | Select-Object -First 1
        if ($null -eq $j0 -or $null -eq $j1) { continue }
        if ([Math]::Abs($j0.t - $h0.t) -gt $WinMs -or
            [Math]::Abs($j1.t - $h1.t) -gt $WinMs -or $j0.t -eq $j1.t) { continue }
        $jm0 = DumpMap $j0; $jm1 = DumpMap $j1
        foreach ($hand in $m0.Keys) {
            if (-not $m1.ContainsKey($hand)) { continue }
            $a = $m0[$hand]; $b = $m1[$hand]
            $hostMove = [Math]::Sqrt(($b[0]-$a[0])*($b[0]-$a[0]) +
                                     ($b[1]-$a[1])*($b[1]-$a[1]) +
                                     ($b[2]-$a[2])*($b[2]-$a[2]))
            if ($hostMove -lt $HostMoveMin) { continue }
            if (-not ($jm0.ContainsKey($hand) -and $jm1.ContainsKey($hand))) { continue }
            $ja = $jm0[$hand]; $jb = $jm1[$hand]
            $joinMove = [Math]::Sqrt(($jb[0]-$ja[0])*($jb[0]-$ja[0]) +
                                     ($jb[1]-$ja[1])*($jb[1]-$ja[1]) +
                                     ($jb[2]-$ja[2])*($jb[2]-$ja[2]))
            $judged++
            if ($joinMove -lt $MoveRatio * $hostMove) {
                $zombies++
                $zombieHands[$hand] = $true
            }
        }
    }
    if ($judged -lt $MinWindows) {
        Write-Host "  anti-zombie SKIP - $judged judged window(s) (< $MinWindows)"
        return (Add-GateResult -Name "anti_zombie" -Status SKIP `
                    -Metrics @{ judged = $judged } -Detail "too few moving-NPC windows")
    }
    $frac = [math]::Round($zombies / $judged, 3)
    $ok = ($frac -le $MaxZombieFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  anti-zombie $v - $zombies/$judged moving-NPC windows frozen on the join (frac=$frac <= $MaxZombieFrac; $($zombieHands.Count) distinct hand(s))"
    return (Add-GateResult -Name "anti_zombie" -Status $v `
                -Metrics @{ judged = $judged; zombieFrac = $frac
                            zombieHands = $zombieHands.Count })
}

# Phase 3 unified entity lifecycle: every join-side authority/mint/drive
# decision reports its outcome as a "[life] hand=.. from=.. to=.. reason=.."
# transition; the sweep self-audits hands stuck in DISCOVERED while MINTABLE
# (census position in a locally loaded zone) as "[life] STUCK .. (mintable)".
# Gates:
#   1. STUCK = 0: a mintable census-vouched hand must resolve through the
#      REQ/mint pipeline within its dwell budget - a stuck hand is the
#      invisible-raid failure (the join fights nothing the host sees).
#   2. ILLEGAL = 0: existence-authority contradictions. DISCOVERED means "no
#      local body exists for this hand"; CULLED means "we hold a local body
#      suppressed under it". A direct edge between them in either direction
#      says two subsystems disagree about whether the body exists.
# Everything else (tier handoffs, mint/cull/restore churn) is recorded as
# metrics for trend analysis, not gated here (churn has its own oracle).
function Test-Lifecycle {
    param([string]$JoinFile, [string]$Label = "join")
    if (-not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "lifecycle" -Status SKIP -Detail "no join log")
    }
    $trans = @(Select-String -Path $JoinFile `
        -Pattern '\[life\] hand=(\d+,\d+,\d+,\d+,\d+) from=(\w+) to=(\w+) reason=(\S+)' `
        -ErrorAction SilentlyContinue)
    $stuck = @(Select-String -Path $JoinFile `
        -Pattern '\[life\] STUCK hand=(\d+,\d+,\d+,\d+,\d+) state=DISCOVERED age=(\d+)s \(mintable\)' `
        -ErrorAction SilentlyContinue)
    if ($trans.Count -eq 0 -and $stuck.Count -eq 0) {
        Write-Host "  [$Label] lifecycle SKIP - no [life] lines (plugin predates Phase 3?)"
        return (Add-GateResult -Name "lifecycle" -Status SKIP -Detail "no lifecycle lines")
    }
    $hands = @{}; $illegal = @(); $perHand = @{}
    foreach ($m in $trans) {
        $h = $m.Matches[0].Groups[1].Value
        $f = $m.Matches[0].Groups[2].Value
        $t = $m.Matches[0].Groups[3].Value
        $hands[$h] = $true
        if ($perHand.ContainsKey($h)) { $perHand[$h]++ } else { $perHand[$h] = 1 }
        if (($f -eq 'DISCOVERED' -and $t -eq 'CULLED') -or
            ($f -eq 'CULLED' -and $t -eq 'DISCOVERED')) {
            $illegal += "$h ($f->$t)"
        }
    }
    $stuckHands = @{}
    foreach ($m in $stuck) { $stuckHands[$m.Matches[0].Groups[1].Value] = $true }
    $worstHand = 0
    foreach ($k in $perHand.Keys) {
        if ($perHand[$k] -gt $worstHand) { $worstHand = $perHand[$k] }
    }
    $ok = ($stuckHands.Count -eq 0 -and $illegal.Count -eq 0)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = ""
    if ($stuckHands.Count -gt 0) { $detail += "stuck-mintable hands: " + (@($stuckHands.Keys) -join " ") + "; " }
    if ($illegal.Count -gt 0)    { $detail += "illegal transitions: " + ($illegal -join " ") }
    Write-Host "  [$Label] lifecycle $v - $($trans.Count) transition(s) over $($hands.Count) hand(s), worst=$worstHand/hand, stuck-mintable=$($stuckHands.Count), illegal=$($illegal.Count) $detail"
    return (Add-GateResult -Name "lifecycle" -Status $v `
                -Metrics @{ transitions = $trans.Count; hands = $hands.Count
                            worstPerHand = $worstHand
                            stuckMintable = $stuckHands.Count
                            illegal = $illegal.Count } -Detail $detail)
}

# Phase 1 spawn parity: proxy mint-distance distribution. Every join "[spawn]
# proxy BOUND" line carries mintDist (distance from the join squad at mint) and
# cen (census-sourced vs stream-sourced). Spawn parity means host runtime
# spawns appear on the join at the distance the HOST spawned them (usually far
# - wilderness spawn range is 1000-2000 u), not materializing at the old 600 u
# radius gate. Gate: at most $MaxNearFrac of census-sourced mints land below
# $NearDist. Near mints are not individually wrong (the host CAN spawn an
# ambush on top of the players - dialog ambushes do), so the gate is on the
# distribution, not each mint.
function Test-MintDistance {
    param([string]$JoinFile, [double]$NearDist = 300.0,
          [double]$MaxNearFrac = 0.34, [int]$MinMints = 3)
    if (-not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "mint_dist" -Status SKIP -Detail "no join log")
    }
    $pat = '\[spawn\] proxy BOUND hand=[\d,]+ .*mintDist=([-\d\.]+) cen=(\d)'
    $cen = New-Object System.Collections.ArrayList
    $all = New-Object System.Collections.ArrayList
    foreach ($m in @(Select-String -Path $JoinFile -Pattern $pat -ErrorAction SilentlyContinue)) {
        $d = [double]$m.Matches[0].Groups[1].Value
        if ($d -lt 0) { continue } # no own-squad reference at mint time
        [void]$all.Add($d)
        if ($m.Matches[0].Groups[2].Value -eq "1") { [void]$cen.Add($d) }
    }
    if ($cen.Count -lt $MinMints) {
        Write-Host "  mint-dist SKIP - $($cen.Count) census-sourced mint(s) (< $MinMints; total $($all.Count))"
        return (Add-GateResult -Name "mint_dist" -Status SKIP `
                    -Metrics @{ mints = $all.Count; censusMints = $cen.Count } `
                    -Detail "too few census mints")
    }
    $near = @($cen | Where-Object { $_ -lt $NearDist }).Count
    $frac = [math]::Round($near / $cen.Count, 3)
    $sorted = @($cen | Sort-Object)
    $median = [math]::Round($sorted[[int]($sorted.Count / 2)], 0)
    $minD   = [math]::Round($sorted[0], 0)
    $ok = ($frac -le $MaxNearFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  mint-dist $v - $near/$($cen.Count) census mints below $NearDist u (frac=$frac <= $MaxNearFrac), median $median u, min $minD u"
    return (Add-GateResult -Name "mint_dist" -Status $v `
                -Metrics @{ censusMints = $cen.Count; nearFrac = $frac
                            medianDist = $median; minDist = $minD })
}


# run_apart gate: did the pair actually RUN the distance?
#
# Everything about whether the two clients stayed in sync while they did is
# already judged by the advisory oracles attached to the scenario. What none of
# them can tell you is whether the run happened at all - and that is the whole
# reason this scenario exists, because split_far2's squads cover 600 u and look
# stationary. A run that never left the start, or one that quietly dropped to 1x
# and got a fifth of the way, must not read as a pass just because nothing
# desynced over ground nobody crossed.
#
# So: both sides present, speed really applied, each tab covered its own leg, and
# both arrived. Legs are asymmetric by design (~1200 u host, ~6400 u join), so
# each side is measured against ITS OWN leg rather than a shared threshold.
function Test-RunApart {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MinSamples = 5,
          # Floors, not fractions of the planned route: the scenario ABANDONS a
          # waypoint it has stopped closing on (a fight), which legitimately cuts
          # a corner, so "did it cover the ground" is the question and "did it
          # follow the plan exactly" is not. Both routes are ~160 k of path for
          # ~90-115 k of displacement, and each ends where a human took that
          # squad, so 80 k of covered ground is a comfortable two thirds.
          [double]$MinHostTravel = 80000.0, [double]$MinJoinTravel = 80000.0,
          # The endpoints are 138,465 u apart. 100 k still means fifty times the
          # census reach, and leaves room for a run that stalls short.
          [double]$MinSep = 100000.0, [double]$MinSpeed = 2.0)

    function Get-RARows([string]$File) {
        $o = @{ start = $null; arrived = $null; rows = (New-Object System.Collections.ArrayList) }
        if (-not (Test-Path $File)) { return $o }
        $sp = "SCENARIO RUNAPART start side=(\w+) have=(\d) sep=([\d\.]+) " +
              "from=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+) " +
              "dest=(-?[\d\.]+),(-?[\d\.]+) cell=(\d)\((-?\d+),(-?\d+)\) " +
              "wps=(\d+) leg=([\d\.]+) straight=([\d\.]+) speed=([\d\.]+)"
        $m = Select-String -Path $File -Pattern $sp -ErrorAction SilentlyContinue |
             Select-Object -First 1
        if ($m) {
            $g = $m.Matches[0].Groups
            $o.start = @{ side = $g[1].Value; have = [int]$g[2].Value
                          wps = [int]$g[12].Value
                          leg = [double]$g[13].Value
                          straight = [double]$g[14].Value
                          cell = "$($g[10].Value),$($g[11].Value)" }
        }
        $ap = "SCENARIO RUNAPART arrived side=(\w+) atMs=(\d+) d=([\d\.]+) travelled=([\d\.]+)"
        $m = Select-String -Path $File -Pattern $ap -ErrorAction SilentlyContinue |
             Select-Object -First 1
        if ($m) {
            $g = $m.Matches[0].Groups
            $o.arrived = @{ atMs = [long]$g[2].Value; d = [double]$g[3].Value
                            travelled = [double]$g[4].Value }
        }
        # togo/travelled can be -1 when the tab leader was momentarily
        # unresolvable, so the pattern allows the sign and the reader drops them.
        $rp = "SCENARIO RUNAPART side=(\w+) phase=(\w+) sep=([\d\.]+) .*" +
              "wp=(\d+)/(\d+) togo=(-?[\d\.]+) travelled=(-?[\d\.]+) " +
              "stalls=(\d+) speed=([\d\.]+)"
        foreach ($m in (Select-String -Path $File -Pattern $rp -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            [void]$o.rows.Add(@{ side = $g[1].Value; phase = $g[2].Value
                                 sep = [double]$g[3].Value
                                 wp = [int]$g[4].Value; wps = [int]$g[5].Value
                                 togo = [double]$g[6].Value
                                 travelled = [double]$g[7].Value
                                 stalls = [int]$g[8].Value
                                 speed = [double]$g[9].Value })
        }
        return $o
    }

    $h = Get-RARows -File $HostFile
    $j = Get-RARows -File $JoinFile
    if (-not $h.start -or -not $j.start) {
        return (Add-GateResult -Name "run_apart" -Status SKIP `
            -Detail "no RUNAPART start row on one or both sides (scenario did not arm)")
    }
    if ($h.start.have -ne 1 -or $j.start.have -ne 1) {
        return (Add-GateResult -Name "run_apart" -Status SKIP `
            -Detail "the save has no rank-0/rank-1 tab pair to send in two directions")
    }
    if ($h.rows.Count -lt $MinSamples -or $j.rows.Count -lt $MinSamples) {
        return (Add-GateResult -Name "run_apart" -Status SKIP `
            -Detail "too few samples (host=$($h.rows.Count) join=$($j.rows.Count) < $MinSamples)")
    }

    # Max of one key across a list of HASHTABLES. Not Measure-Object: it reads
    # .PSObject properties, and a hashtable's keys are not those, so it throws
    # "the property cannot be found in the input for any objects" on rows that
    # plainly have the key - which is how the first run of this gate ended,
    # after a 650 s scenario that had otherwise succeeded.
    function Get-MaxOf([object[]]$rows, [string]$key) {
        $best = $null
        foreach ($r in $rows) {
            $v = $r[$key]
            if ($null -eq $v) { continue }
            if ($null -eq $best -or $v -gt $best) { $best = $v }
        }
        if ($null -eq $best) { return 0 }
        return $best
    }

    # Each side's OWN progress comes from its own rows: 'side=host' rows in the
    # host log are the host describing its own tab.
    function Get-Own([hashtable]$o, [string]$side) {
        $mine = @($o.rows | Where-Object { $_.side -eq $side -and $_.travelled -ge 0 })
        if ($mine.Count -eq 0) { return @{ travelled = 0.0; togo = -1.0; wp = 0; wps = 0 } }
        return @{ travelled = [double](Get-MaxOf -rows $mine -key "travelled")
                  togo = [double]$mine[-1].togo
                  wp = [int](Get-MaxOf -rows $mine -key "wp")
                  wps = [int]$mine[-1].wps }
    }
    $ho = Get-Own -o $h -side "host"
    $jo = Get-Own -o $j -side "join"
    $maxSpeed = 0.0
    foreach ($r in @($h.rows) + @($j.rows)) { if ($r.speed -gt $maxSpeed) { $maxSpeed = $r.speed } }

    $metrics = @{
        hostLeg = [math]::Round($h.start.leg, 0); joinLeg = [math]::Round($j.start.leg, 0)
        hostTravelled = [math]::Round($ho.travelled, 0)
        joinTravelled = [math]::Round($jo.travelled, 0)
        hostToGo = [math]::Round($ho.togo, 0); joinToGo = [math]::Round($jo.togo, 0)
        hostStalls = [int](Get-MaxOf -rows @($h.rows) -key "stalls")
        joinStalls = [int](Get-MaxOf -rows @($j.rows) -key "stalls")
        hostArriveMs = if ($h.arrived) { $h.arrived.atMs } else { -1 }
        joinArriveMs = if ($j.arrived) { $j.arrived.atMs } else { -1 }
        maxSpeed = $maxSpeed
        hostSamples = $h.rows.Count; joinSamples = $j.rows.Count
        hostWp = "$($ho.wp)/$($ho.wps)"; joinWp = "$($jo.wp)/$($jo.wps)"
        # What this scenario exists for: how far apart they ended up.
        maxSep = [math]::Round((Get-MaxOf -rows (@($h.rows) + @($j.rows)) -key "sep"), 0)
    }

    $why = ""
    if ($maxSpeed -lt $MinSpeed) {
        $why = "the speed vote never took - peak speed $maxSpeed x (< $MinSpeed x), " +
               "so the legs were never affordable"
    } elseif ($ho.travelled -lt $MinHostTravel -or $jo.travelled -lt $MinJoinTravel) {
        $why = "a tab did not cover the ground - host $($metrics.hostTravelled) u " +
               "(need $MinHostTravel), join $($metrics.joinTravelled) u " +
               "(need $MinJoinTravel); stalls host=$($metrics.hostStalls) " +
               "join=$($metrics.joinStalls), so check whether a fight stopped one"
    } elseif ($metrics.maxSep -lt $MinSep) {
        $why = "they never got far apart - peak separation $($metrics.maxSep) u " +
               "(need $MinSep), which is the whole point of the run"
    } elseif (-not $h.arrived -or -not $j.arrived) {
        $why = "a tab never reached its last waypoint (host $($metrics.hostWp) " +
               "togo=$($metrics.hostToGo) u, join $($metrics.joinWp) " +
               "togo=$($metrics.joinToGo) u)"
    }
    $v = if ($why) { "FAIL" } else { "PASS" }
    if (-not $why) {
        $why = "both tabs ran their own route at ${maxSpeed}x and arrived - " +
               "host $($metrics.hostTravelled) u by $($metrics.hostArriveMs) ms, " +
               "join $($metrics.joinTravelled) u by $($metrics.joinArriveMs) ms, " +
               "ending $($metrics.maxSep) u apart"
    }
    Write-Host "  RUN-APART $v - $why"
    return (Add-GateResult -Name "run_apart" -Status $v -Metrics $metrics -Detail $why)
}


# ---- town_arrive oracles --------------------------------------------------------
#
# The pair walks into a town whose zone has never been loaded in this save. Two
# gates, because "did the walk happen" and "was the town right when we got there"
# fail for completely different reasons and a run that never arrived must not be
# read as a clean town.
#
#   town_arrive     (PRIMARY) - both squads walked in, and the town is populated.
#   town_pop_parity (GATING)  - what the join sees there is the host's town.
#
# Everything the second gate judges is measured AFTER arrival, keyed off the
# scenario's own arrived marker by LINE NUMBER rather than by clock: the approach
# legitimately produces the churn the town must not (the park snap, the peer's
# copy catching up, zones streaming), so judging the whole run would mix the two.

# Parse the scenario's TOWNARRIVE markers and rows out of one log. arrivedLine is
# how the parity gate finds the start of its window.
function Get-TownArriveData {
    param([string]$File)
    $o = @{ start = $null; settled = $null; arrived = $null; timeout = $null
            rows = (New-Object System.Collections.ArrayList); arrivedLine = -1 }
    if (-not (Test-Path $File)) { return $o }

    $sp = "SCENARIO TOWNARRIVE start side=(\w+) have=(\d) parked=(\d+) " +
          "from=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+) " +
          "target=(-?[\d\.]+),(-?[\d\.]+) straight=([\d\.]+) ground=(\d)"
    $m = Select-String -Path $File -Pattern $sp -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($m) {
        $g = $m.Matches[0].Groups
        $o.start = @{ side = $g[1].Value; have = [int]$g[2].Value
                      parked = [int]$g[3].Value
                      straight = [double]$g[9].Value; ground = [int]$g[10].Value }
    }
    $stp = "SCENARIO TOWNARRIVE settled side=(\w+) atMs=(\d+) zone=(\d) pop=(\d+) " +
           "d=([\d\.]+) drift=([\d\.]+)"
    $m = Select-String -Path $File -Pattern $stp -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($m) {
        $g = $m.Matches[0].Groups
        $o.settled = @{ atMs = [long]$g[2].Value; zone = [int]$g[3].Value
                        pop = [int]$g[4].Value; drift = [double]$g[6].Value }
    }
    $ap = "SCENARIO TOWNARRIVE arrived side=(\w+) atMs=(\d+) d=([\d\.]+) " +
          "travelled=([\d\.]+) straight=([\d\.]+) hops=(\d+) sidesteps=(\d+) " +
          "walkMs=(\d+)"
    $m = Select-String -Path $File -Pattern $ap -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($m) {
        $g = $m.Matches[0].Groups
        $o.arrived = @{ atMs = [long]$g[2].Value; d = [double]$g[3].Value
                        travelled = [double]$g[4].Value
                        straight = [double]$g[5].Value
                        hops = [int]$g[6].Value; sidesteps = [int]$g[7].Value
                        walkMs = [long]$g[8].Value }
        $o.arrivedLine = $m.LineNumber
    }
    $tp = "SCENARIO TOWNARRIVE timeout side=(\w+) d=(-?[\d\.]+) travelled=([\d\.]+) " +
          "hops=(\d+) sidesteps=(\d+) settled=(\d)"
    $m = Select-String -Path $File -Pattern $tp -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($m) {
        $g = $m.Matches[0].Groups
        $o.timeout = @{ d = [double]$g[2].Value; travelled = [double]$g[3].Value
                        settled = [int]$g[6].Value }
    }
    # d can be -1 for a sample where the tab leader was momentarily unresolvable.
    $rp = "SCENARIO TOWNARRIVE side=(\w+) phase=(\w+) d=(-?[\d\.]+) " +
          "travelled=([\d\.]+) popHost=(\d+) popJoin=(\d+) zHost=(\d) zJoin=(\d) " +
          "sep=([\d\.]+) cell=\d\((-?\d+),(-?\d+)\) arrived=(\d) hops=(\d+) " +
          "sidesteps=(\d+) bias=(-?[\d\.]+) speed=([\d\.]+)"
    foreach ($m in (Select-String -Path $File -Pattern $rp -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        [void]$o.rows.Add(@{ side = $g[1].Value; phase = $g[2].Value
                             d = [double]$g[3].Value
                             travelled = [double]$g[4].Value
                             popHost = [int]$g[5].Value; popJoin = [int]$g[6].Value
                             sep = [double]$g[9].Value
                             arrived = [int]$g[12].Value
                             hops = [int]$g[13].Value
                             sidesteps = [int]$g[14].Value
                             speed = [double]$g[16].Value })
    }
    return $o
}

# The join's "[audit] exist" samples from a given line onward. fresh=0 samples are
# excluded for the same reason Test-ExistenceParity excludes them: wide culling is
# deliberately off on a stale census, so the classification is not meaningful.
# RequireFresh: a row whose census is stale cannot be read for cen/hid, because
# those classify against the census - so the JOIN side demands fresh=1. The HOST in
# this scenario receives no census at all (it authors the town), and its rows read
# fresh=0 forever, while the only field wanted from them - wide, its own count of
# the town - does not depend on the census. Hence the switch.
function Get-TownAuditRows {
    param([string]$File, [int]$AfterLine = 0, [bool]$RequireFresh = $true)
    $rows = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return $rows }
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[audit\] exist near=(\d+) " +
           "wide=(\d+) drv=(\d+) cen=(\d+) hid=(\d+) ghost=(\d+) supp=(\d+) " +
           "census=(\d+) fresh=(\d) parks=(\d+)(?: walks=(\d+))?"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        if ($m.LineNumber -le $AfterLine) { continue }
        $g = $m.Matches[0].Groups
        if ($RequireFresh -and $g[13].Value -ne "1") { continue }
        $ms = ((([int]$g[1].Value * 60 + [int]$g[2].Value) * 60 +
                 [int]$g[3].Value) * 1000) + [int]$g[4].Value
        [void]$rows.Add(@{ ms = $ms
                           wide = [int]$g[6].Value; drv = [int]$g[7].Value
                           cen = [int]$g[8].Value; hid = [int]$g[9].Value
                           supp = [int]$g[11].Value; census = [int]$g[12].Value
                           parks = [long]$g[14].Value
                           # walks= post-dates the walk-converge band, so a log
                           # written before it has no field to read.
                           walks = if ($g[15].Success) { [long]$g[15].Value }
                                   else { [long]0 } })
    }
    return $rows
}

# Count matches of a pattern occurring after a line number (window = post-arrival).
function Measure-TownAfter {
    param([string]$File, [string]$Pattern, [int]$AfterLine)
    if (-not (Test-Path $File)) { return 0 }
    $n = 0
    foreach ($m in (Select-String -Path $File -Pattern $Pattern -ErrorAction SilentlyContinue)) {
        if ($m.LineNumber -gt $AfterLine) { $n++ }
    }
    return $n
}

function Get-TownMedian {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return -1.0 }
    $s = @($Values | Sort-Object)
    return [double]$s[[int]($s.Count / 2)]
}

# town_arrive (PRIMARY): did both squads actually WALK into a POPULATED town?
#
# Both halves matter as a no-signal guard. A run that parked outside and never
# moved, or that lost its 5x vote and timed out short, has not produced the
# streaming this scenario is about. And an empty field would sail through the
# parity gate for the worst possible reason - there was nothing there to get
# wrong - so the town being populated at all is part of the mechanism proof.
function Test-TownArrive {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MinSamples = 5,
          # Covered ground against the straight line. Sidesteps and detours only
          # ever make the path LONGER than the straight line, so anything below
          # the straight line means it did not walk the approach.
          [double]$MinTravelFrac = 0.7,
          # 5x is the scenario's own vote, and the point of it: town travel at
          # speed is what a player does and what stresses the streaming. 4x
          # allows for a sample taken mid-arbitration.
          [double]$MinSpeed = 4.0,
          # Bad Teeth held census=172 walked in and 160 loaded from save. 40 is
          # comfortably a town and comfortably above open country (census=0 at
          # the park point, tens on the road).
          [int]$MinCensus = 40)

    $h = Get-TownArriveData -File $HostFile
    $j = Get-TownArriveData -File $JoinFile
    if (-not $h.start -or -not $j.start) {
        return (Add-GateResult -Name "town_arrive" -Status SKIP `
            -Detail "no TOWNARRIVE start row on one or both sides (scenario did not arm)")
    }
    if ($h.start.have -ne 1 -or $j.start.have -ne 1) {
        return (Add-GateResult -Name "town_arrive" -Status SKIP `
            -Detail "the save has no rank-0/rank-1 tab pair, so there is no join-owned squad to walk in")
    }
    if ($h.rows.Count -lt $MinSamples -or $j.rows.Count -lt $MinSamples) {
        return (Add-GateResult -Name "town_arrive" -Status SKIP `
            -Detail "too few samples (host=$($h.rows.Count) join=$($j.rows.Count) < $MinSamples)")
    }

    $maxSpeed = 0.0
    foreach ($r in @($h.rows) + @($j.rows)) { if ($r.speed -gt $maxSpeed) { $maxSpeed = $r.speed } }
    $censusPeak = 0
    foreach ($a in (Get-TownAuditRows -File $JoinFile)) {
        if ($a.census -gt $censusPeak) { $censusPeak = $a.census }
    }
    $straight = $h.start.straight
    $need = [math]::Round($straight * $MinTravelFrac, 0)
    $hTrav = if ($h.arrived) { $h.arrived.travelled }
             elseif ($h.timeout) { $h.timeout.travelled } else { 0.0 }
    $jTrav = if ($j.arrived) { $j.arrived.travelled }
             elseif ($j.timeout) { $j.timeout.travelled } else { 0.0 }

    $metrics = @{
        straight = [math]::Round($straight, 0)
        hostTravelled = [math]::Round($hTrav, 0)
        joinTravelled = [math]::Round($jTrav, 0)
        needTravelled = $need
        hostArriveMs = if ($h.arrived) { $h.arrived.atMs } else { -1 }
        joinArriveMs = if ($j.arrived) { $j.arrived.atMs } else { -1 }
        hostSidesteps = if ($h.arrived) { $h.arrived.sidesteps } else { -1 }
        joinSidesteps = if ($j.arrived) { $j.arrived.sidesteps } else { -1 }
        hostParked = $h.start.parked; joinParked = $j.start.parked
        hostSettleMs = if ($h.settled) { $h.settled.atMs } else { -1 }
        joinSettleMs = if ($j.settled) { $j.settled.atMs } else { -1 }
        parkDrift = if ($j.settled) { [math]::Round($j.settled.drift, 0) } else { -1 }
        censusPeak = $censusPeak
        maxSpeed = $maxSpeed
        hostSamples = $h.rows.Count; joinSamples = $j.rows.Count
    }

    $why = ""
    if ($h.start.parked -eq 0 -or $j.start.parked -eq 0) {
        $why = "a side parked nobody at the approach point (host $($h.start.parked), " +
               "join $($j.start.parked)), so it never started outside the town"
    } elseif ($maxSpeed -lt $MinSpeed) {
        $why = "the 5x vote never took - peak speed ${maxSpeed}x (< ${MinSpeed}x), " +
               "so this is not the travel speed the scenario is meant to stress"
    } elseif (-not $h.arrived -or -not $j.arrived) {
        $hd = if ($h.timeout) { "$([math]::Round($h.timeout.d,0)) u short" } else { "no timeout row" }
        $jd = if ($j.timeout) { "$([math]::Round($j.timeout.d,0)) u short" } else { "no timeout row" }
        $why = "a squad never reached the town (host: $hd, join: $jd); covered " +
               "host $($metrics.hostTravelled) u, join $($metrics.joinTravelled) u " +
               "of a $($metrics.straight) u approach - check the stall rows for " +
               "whether it was a fight or the router"
    } elseif ($hTrav -lt $need -or $jTrav -lt $need) {
        $why = "a squad arrived without walking the approach - host " +
               "$($metrics.hostTravelled) u, join $($metrics.joinTravelled) u, " +
               "need $need u of a $($metrics.straight) u straight line"
    } elseif ($censusPeak -lt $MinCensus) {
        $why = "the town is not populated - peak census $censusPeak row(s) " +
               "(need $MinCensus), so there is nothing here to judge parity on"
    }
    $v = if ($why) { "FAIL" } else { "PASS" }
    if (-not $why) {
        $why = "both squads walked in at ${maxSpeed}x - host $($metrics.hostTravelled) u " +
               "by $($metrics.hostArriveMs) ms, join $($metrics.joinTravelled) u by " +
               "$($metrics.joinArriveMs) ms (straight $($metrics.straight) u, " +
               "sidesteps host=$($metrics.hostSidesteps) join=$($metrics.joinSidesteps)), " +
               "into a town of $censusPeak census row(s)"
    }
    Write-Host "  TOWN-ARRIVE $v - $why"
    return (Add-GateResult -Name "town_arrive" -Status $v -Metrics $metrics -Detail $why)
}

# town_pop_parity (GATING): once in town, is the join looking at ONE town?
#
# The bug the scenario was written for. A town's population is generated when its
# zone streams in, so two clients that WALK into one generate it separately: same
# spawn points, different engine hands. The join cannot resolve the host's census
# rows against its own bodies, so it mints a proxy for every row AND suppresses
# its own natively-spawned copy as unclaimed - two towns stacked on one spot, one
# of them hidden.
#
# WHAT THIS GATES ON took three tries to get right, and the two wrong answers are
# worth keeping written down, because each looked obviously correct.
#
# Try 1, cen/census: of the rows the host claims exist, how many resolve to a local
# body by hand. It separates the measured cases beautifully - 0.04 walked in against
# 0.78 loaded from a save - and it measures the MECHANISM, not the outcome. A body
# the join adopts and drives as a bound proxy is classified drv, not cen, so the fix
# for this bug leaves cen at zero. A gate on it would have failed the fix. Reported
# below, not judged.
#
# Try 2, an NPC count around the host's leader taken by both clients: apples to
# apples, and it read 2.46 broken. But it comes from countNpcsNear, a spatial query,
# and a SUPPRESSED body is hidden, not removed - it still answers the query. So the
# metric counts bodies nobody can see, and it read 1.43 on a run where the join was
# showing 104 bodies to the host's 105. Perfect parity, gate says two towns.
#
# What is judged is VISIBLE population, which is what a player can actually see,
# built from each side's own audit line so both are measured the same way:
#
#   visible  = wide - hid   (enumerated, minus the ones deliberately hidden)
#   visRatio = the join's visible population over the host's
#
# Measured, standing 9 u apart in Bad Teeth:
#   pre-fix   join wide=273 hid=135 -> 138 visible, host 111 -> ratio 1.24
#   post-fix  join wide=159 hid=55  -> 104 visible, host 105 -> ratio 0.99
#
# visPerCensus (visible over the host's census row count) is the same statement
# made against the host's own claim rather than its enumeration, and it is the
# tighter of the two because it does not depend on the host's audit firing.
#
# WHAT IS DELIBERATELY NOT JUDGED, and this is the substantive finding: hid. The
# instinct is that a join hiding 55 of its own townspeople is broken, and it was the
# gate here for two revisions. It is not. Both engines generate the town when the
# zone streams in, and they generate DIFFERENT PEOPLE - not the same people under
# different hands. Probing every unpaired census row against a 1200 u radius for a
# body of the same template and faction: 75 rows paired, 49 had a twin too far out,
# and 90 had no twin anywhere. Those 90 are population the host has and the join
# never generated, and the join's own equivalent surplus is what hid counts. The
# host is authoritative, so hiding it is the CORRECT outcome, and a gate on hid
# would fail a run for behaving properly. Reported, with its parts.
#
# parkRate is reported for the same kind of reason: a HEALTHY Bad Teeth logged 216
# parks/min against the broken run's 528, so a threshold there fails good runs.
function Test-TownPopParity {
    param([string]$HostFile, [string]$JoinFile,
          # Pre-fix 1.24, post-fix 0.99. Banded on BOTH sides: a join that
          # under-populates a town is also a bug, just a quieter one. The band is
          # wide because a town is genuinely in flux - patrols leave, bodies sit at
          # the streaming edge - and because the two sides' audits are not sampled
          # on the same tick.
          [double]$MinVisRatio = 0.75, [double]$MaxVisRatio = 1.18,
          [int]$MinSamples = 6, [int]$MinCensus = 40)

    $j = Get-TownArriveData -File $JoinFile
    $h = Get-TownArriveData -File $HostFile
    if (-not $j.start) {
        return (Add-GateResult -Name "town_pop_parity" -Status SKIP `
            -Detail "no TOWNARRIVE rows in the join log (scenario did not arm)")
    }
    if ($j.arrivedLine -lt 0) {
        return (Add-GateResult -Name "town_pop_parity" -Status SKIP `
            -Detail "the join never arrived, so there is no in-town window to judge (see town_arrive)")
    }

    $all = @(Get-TownAuditRows -File $JoinFile -AfterLine $j.arrivedLine)
    $win = @($all | Where-Object { $_.census -ge $MinCensus })
    if ($win.Count -lt $MinSamples) {
        return (Add-GateResult -Name "town_pop_parity" -Status SKIP `
            -Metrics @{ samples = $win.Count; auditRows = $all.Count } `
            -Detail ("only $($win.Count) post-arrival audit sample(s) with census >= " +
                     "$MinCensus (< $MinSamples) - too little town to judge"))
    }

    # VISIBLE population on each side, from each side's own audit: enumerated minus
    # deliberately hidden. The host's audit only exists when it runs the authority
    # pass too (cell authority on), so fall back to its census row count - which is
    # the host's claim about the same town - when it does not.
    $jVis = @($win | ForEach-Object { [double]($_.wide - $_.hid) })
    $medJoinVis = [int](Get-TownMedian -Values $jVis)
    $hAll = @(Get-TownAuditRows -File $HostFile -AfterLine $h.arrivedLine `
                  -RequireFresh $false)
    $hWin = @($hAll | Where-Object { $_.wide -ge $MinCensus })
    $hostSource = "audit"
    if ($hWin.Count -ge $MinSamples) {
        $hVis = @($hWin | ForEach-Object { [double]($_.wide - $_.hid) })
        $medHostVis = [int](Get-TownMedian -Values $hVis)
    } else {
        $hostSource = "census"
        $medHostVis = [int](Get-TownMedian -Values @($win | ForEach-Object { [double]$_.census }))
    }
    if ($medHostVis -le 0) {
        return (Add-GateResult -Name "town_pop_parity" -Status SKIP `
            -Metrics @{ hostVisible = $medHostVis; joinVisible = $medJoinVis } `
            -Detail "the host shows no town, so there is no ratio to take")
    }
    $visRatio = [math]::Round($medJoinVis / [double]$medHostVis, 3)

    $hidF = @($win | ForEach-Object { [double]$_.hid / [double]$_.census })
    $corr = @($win | ForEach-Object { [double]$_.cen / [double]$_.census })
    $visC = @($win | ForEach-Object { [double]($_.wide - $_.hid) / [double]$_.census })
    $medHid  = [math]::Round((Get-TownMedian -Values $hidF), 3)
    $medCorr = [math]::Round((Get-TownMedian -Values $corr), 3)
    $medVisC = [math]::Round((Get-TownMedian -Values $visC), 3)

    $spanMs = $win[-1].ms - $win[0].ms
    $spanMin = if ($spanMs -gt 0) { $spanMs / 60000.0 } else { 0.0 }
    $parkRate = if ($spanMin -gt 0) {
        [math]::Round(($win[-1].parks - $win[0].parks) / $spanMin, 0)
    } else { -1 }
    # The two corrections, side by side. A park is a teleport and the player sees
    # every one of them; a walk is the same correction carried on the body's own
    # feet. Neither is a fault on its own - a town in flux needs correcting - so
    # what is worth watching is the SPLIT, and a park rate that stays high while
    # walks stay near zero means the band is not catching the class it was aimed
    # at (the patrols whose host copy walks away from a frozen local twin).
    $walkRate = if ($spanMin -gt 0) {
        [math]::Round(($win[-1].walks - $win[0].walks) / $spanMin, 0)
    } else { -1 }
    $proxies = Measure-TownAfter -File $JoinFile -AfterLine $j.arrivedLine `
                   -Pattern "\[spawn\] proxy BOUND"
    $adopts  = Measure-TownAfter -File $JoinFile -AfterLine $j.arrivedLine `
                   -Pattern "\[spawn\] proxy ADOPT"
    $adoptMiss = Measure-TownAfter -File $JoinFile -AfterLine $j.arrivedLine `
                   -Pattern "\[spawn\] adopt MISS"
    $missing = Measure-TownAfter -File $JoinFile -AfterLine $j.arrivedLine `
                   -Pattern "\[spawn\] census-missing"

    $medCensus = [int](Get-TownMedian -Values @($win | ForEach-Object { [double]$_.census }))
    $medHidN   = [int](Get-TownMedian -Values @($win | ForEach-Object { [double]$_.hid }))
    $medWide   = [int](Get-TownMedian -Values @($win | ForEach-Object { [double]$_.wide }))
    $medDrv    = [int](Get-TownMedian -Values @($win | ForEach-Object { [double]$_.drv }))
    $metrics = @{
        samples = $win.Count; windowSec = [math]::Round($spanMs / 1000.0, 0)
        visRatio = $visRatio; visPerCensus = $medVisC
        joinVisible = $medJoinVis; hostVisible = $medHostVis
        hostVisibleFrom = $hostSource
        hidFrac = $medHid; corrFrac = $medCorr
        medCensus = $medCensus; medHid = $medHidN
        medWide = $medWide; medDrv = $medDrv
        proxiesBound = $proxies; proxiesAdopted = $adopts
        adoptMisses = $adoptMiss; censusMissing = $missing
        parksPerMin = $parkRate; walksPerMin = $walkRate
    }

    $why = ""
    if ($visRatio -gt $MaxVisRatio) {
        $why = "the join is showing more town than the host has - $medJoinVis " +
               "visible bodies to the host's $medHostVis (ratio=$visRatio > " +
               "$MaxVisRatio), from wide=$medWide less hid=$medHidN, against " +
               "$medCensus census row(s), with $proxies mint(s) and $adopts " +
               "adoption(s): the two clients are each contributing a population"
    } elseif ($visRatio -lt $MinVisRatio) {
        $why = "the join is missing town - $medJoinVis visible bodies to the host's " +
               "$medHostVis (ratio=$visRatio < $MinVisRatio), with $missing " +
               "census-missing row(s) and $adoptMiss unpaired"
    }
    $v = if ($why) { "FAIL" } else { "PASS" }
    if (-not $why) {
        $why = "one town on both clients - $medJoinVis visible bodies to the host's " +
               "$medHostVis (ratio=$visRatio, $medVisC per census row), from " +
               "wide=$medWide less hid=$medHidN, $proxies mint(s) + $adopts " +
               "adoption(s), $adoptMiss unpaired, $parkRate park(s)/min over " +
               "$($metrics.windowSec)s (hid frac=$medHid, cen frac=$medCorr, reported)"
    }
    Write-Host "  TOWN-POP-PARITY $v - $why"
    return (Add-GateResult -Name "town_pop_parity" -Status $v -Metrics $metrics -Detail $why)
}

# escape_cohesion primary gate: is the escaping PLAYER CHARACTER rendered once,
# and the same once, on both clients?
#
# This is deliberately narrower than world_parity, which asks whether paired
# bodies agree on POSITION. The failure this gate exists for is a body that
# should not exist at all: a proxy minted beside a native player body, so the
# player sees themselves twice. world_parity cannot see that - it pairs by hand,
# and a duplicate carries a DIFFERENT hand, so the extra body is simply never
# anybody's pair and drops out of every ratio it computes.
#
# Four questions per paired 5 s dump, three of them cheap identity checks and
# one that does the real work:
#   1. do host and join list the SAME set of cls=pc hands (nobody's PC is
#      missing from, or extra on, the other client)
#   2. does any cls=pc hand appear TWICE in one dump (a duplicate that made it
#      into playerCharacters)
#   3. is the PC count what the fixture says it should be
#   4. does a NON-pc body sit within $DupRadius of a PC
#
# (4) is the "two copies on screen" detector, and it works because listNpcsWide
# skips isPlayerSquad: a native player body can only ever appear as cls=pc, so a
# second copy of it necessarily surfaces as an NPC-classed row stacked on top of
# a PC row. It is reported at two strengths, and the measurements say to trust
# only one of them.
#
# A stack whose NAME matches the PC's is a copy of that character and very
# little else, and it is clean: across four dense 'camp' world_parity runs
# (20260806_100102 / _113219 / _115304 / _115852) nameStacks was 0 in all of
# them, at every radius tried. So a name match FAILS.
#
# An ANONYMOUS stack is just a body standing close, and in a prison fixture that
# is the normal state of affairs - those same four runs put an unrelated body
# within 2 u of a PC in 4% to 44% of client-samples, several at a measured
# distance of exactly 0 (a Holy Sentinel sharing the caged 'Flashbox' transform).
# There is therefore no radius at which "something is near a PC" means
# "duplicate", so $StackFracMax defaults to 1.0: the fraction is measured and
# printed, not gated. A sparse fixture can tighten it from the manifest.
function Test-PcDuplicates {
    param([string]$HostFile, [string]$JoinFile,
          [double]$DupRadius = 4.0, [int]$ExpectPcs = 2,
          [int]$MinSamples = 6, [int]$WinMs = 6000,
          [int]$GraceMs = 45000, [int]$TailGraceMs = 1000,
          [int]$MaxParityBad = 1, [double]$StackFracMax = 1.0)
    $hostSamples = Group-WnpcSamples -Rows (Get-WnpcRows -File $HostFile)
    $joinSamples = Group-WnpcSamples -Rows (Get-WnpcRows -File $JoinFile)
    if ($hostSamples.Count -lt $MinSamples -or $joinSamples.Count -lt $MinSamples) {
        Write-Host "  pc-dupes SKIP - $($hostSamples.Count) host / $($joinSamples.Count) join dump sample(s)"
        return (Add-GateResult -Name "pc_dupes" -Status SKIP `
                    -Metrics @{ hostSamples = $hostSamples.Count; joinSamples = $joinSamples.Count } `
                    -Detail "too few worldstate dumps")
    }
    $joinT0 = $joinSamples[0].t
    $joinTEnd = $joinSamples[$joinSamples.Count - 1].t

    $used = 0; $parityBad = 0; $repeatBad = 0; $countBad = 0
    $stackSamples = 0; $nameStacks = 0
    $worstStack = -1.0
    $offenders = @{}     # "side|npcName->pcName" -> hit count
    $parityWhy = @{}     # "side-only hand" -> count
    $countSeen = @{}     # "host=2 join=1" -> count

    foreach ($hs in $hostSamples) {
        if (($hs.t - $joinT0) -lt $GraceMs) { continue }
        if (($hs.t - $joinTEnd) -gt $TailGraceMs) { continue }
        $js = $null
        foreach ($cand in $joinSamples) {
            if ([math]::Abs($cand.t - $hs.t) -gt $WinMs) { continue }
            if ($null -eq $js -or [math]::Abs($cand.t - $hs.t) -lt [math]::Abs($js.t - $hs.t)) { $js = $cand }
        }
        if ($null -eq $js) { continue }
        $used++

        foreach ($pair in @(@{ side = "host"; s = $hs }, @{ side = "join"; s = $js })) {
            # (2) a hand listed twice in ONE EMIT. The timestamp has to be exact:
            # emitPcRows fires twice within ~25 ms on the host (run
            # 20260808_143039 logged every PC row at both .829 and .852), and the
            # 1500 ms grouping window folds those into one sample - so "twice in
            # this sample" would charge 237 duplicates to a run that had none. A
            # real duplicate is two rows for one hand inside a SINGLE pass over
            # playerCharacters, and those share a stamp.
            $seen = @{}
            foreach ($r in $pair.s.rows) {
                if ($r.cls -ne "pc") { continue }
                $k = "$($r.hand)@$($r.t)"
                if ($seen.ContainsKey($k)) { $repeatBad++ } else { $seen[$k] = 1 }
            }
            # One row per body for the geometry below, for the same reason.
            $pcByHand = @{}; $npcByHand = @{}
            foreach ($r in $pair.s.rows) {
                if ($r.cls -eq "pc") { $pcByHand[$r.hand] = $r } else { $npcByHand[$r.hand] = $r }
            }
            $pcs = @($pcByHand.Values)
            # (4) non-pc bodies stacked on a PC
            $stacked = $false
            foreach ($r in $npcByHand.Values) {
                foreach ($p in $pcs) {
                    $dx = $r.pos[0] - $p.pos[0]
                    $dy = $r.pos[1] - $p.pos[1]
                    $dz = $r.pos[2] - $p.pos[2]
                    $d = [math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz)
                    if ($d -gt $DupRadius) { continue }
                    $stacked = $true
                    if ($worstStack -lt 0 -or $d -lt $worstStack) { $worstStack = $d }
                    $key = "$($pair.side)|$($r.cls) '$($r.name)' on '$($p.name)'"
                    if ($offenders.ContainsKey($key)) { $offenders[$key]++ } else { $offenders[$key] = 1 }
                    if ($r.name -ne "" -and $r.name -eq $p.name) { $nameStacks++ }
                }
            }
            if ($stacked) { $stackSamples++ }
        }

        # (1) same PC roster on both clients, and (3) the expected count
        $hHands = @($hs.rows | Where-Object { $_.cls -eq "pc" } | ForEach-Object { $_.hand } | Sort-Object -Unique)
        $jHands = @($js.rows | Where-Object { $_.cls -eq "pc" } | ForEach-Object { $_.hand } | Sort-Object -Unique)
        $only = @($hHands | Where-Object { $jHands -notcontains $_ }) +
                @($jHands | Where-Object { $hHands -notcontains $_ })
        if ($only.Count -gt 0) {
            $parityBad++
            foreach ($o in $only) {
                if ($parityWhy.ContainsKey($o)) { $parityWhy[$o]++ } else { $parityWhy[$o] = 1 }
            }
        }
        if ($hHands.Count -ne $ExpectPcs -or $jHands.Count -ne $ExpectPcs) {
            $countBad++
            $ck = "host=$($hHands.Count) join=$($jHands.Count)"
            if ($countSeen.ContainsKey($ck)) { $countSeen[$ck]++ } else { $countSeen[$ck] = 1 }
        }
    }

    if ($used -lt $MinSamples) {
        Write-Host "  pc-dupes SKIP - only $used paired sample(s) after grace"
        return (Add-GateResult -Name "pc_dupes" -Status SKIP `
                    -Metrics @{ paired = $used; hostSamples = $hostSamples.Count
                                joinSamples = $joinSamples.Count } `
                    -Detail "too few paired dumps after the $([int]($GraceMs/1000))s grace")
    }

    # Per-SIDE denominator: checks 2 and 4 run once per client per sample.
    $stackFrac = [math]::Round($stackSamples / [double]($used * 2), 3)
    $metrics = @{
        paired = $used; expectPcs = $ExpectPcs; dupRadius = $DupRadius
        parityBad = $parityBad; repeatBad = $repeatBad; countBad = $countBad
        stackSamples = $stackSamples; stackFrac = $stackFrac
        nameStacks = $nameStacks
        worstStack = [math]::Round($worstStack, 2)
        hostSamples = $hostSamples.Count; joinSamples = $joinSamples.Count
    }
    $top = @($offenders.GetEnumerator() | Sort-Object -Property Value -Descending |
             Select-Object -First 3 | ForEach-Object { "$($_.Key) x$($_.Value)" })
    if ($top.Count -gt 0) { $metrics.stackTop = ($top -join "; ") }

    $why = ""
    if ($nameStacks -gt 0) {
        $why = "DUPLICATE player body: $nameStacks sample(s) with a non-pc row " +
               "carrying a PC's own name within $DupRadius u of it - $($top -join '; ')"
    } elseif ($repeatBad -gt 0) {
        $why = "a cls=pc hand was listed TWICE in one dump ($repeatBad time(s)) - " +
               "the duplicate reached playerCharacters"
    } elseif ($parityBad -gt $MaxParityBad) {
        $named = @($parityWhy.GetEnumerator() | Sort-Object -Property Value -Descending |
                   Select-Object -First 3 | ForEach-Object { "$($_.Key) x$($_.Value)" })
        $why = "PC rosters disagree in $parityBad of $used paired dump(s) " +
               "(budget $MaxParityBad) - one-sided: $($named -join ', ')"
    } elseif ($countBad -gt $MaxParityBad) {
        $named = @($countSeen.GetEnumerator() | Sort-Object -Property Value -Descending |
                   Select-Object -First 3 | ForEach-Object { "$($_.Key) x$($_.Value)" })
        $why = "PC count is not $ExpectPcs in $countBad of $used paired dump(s) " +
               "(budget $MaxParityBad) - saw $($named -join ', ')"
    } elseif ($stackFrac -gt $StackFracMax) {
        $why = "bodies stacked on a PC in $stackFrac of client-samples " +
               "(max $StackFracMax, closest $($metrics.worstStack) u) - $($top -join '; ')"
    }
    $v = if ($why) { "FAIL" } else { "PASS" }
    if (-not $why) {
        $why = "$ExpectPcs player character(s), same hands on both clients, no " +
               "duplicate of any of them over $used paired dump(s) (bodies within " +
               "$DupRadius u in $stackFrac of client-samples, none name-matching, " +
               "reported)"
    }
    Write-Host "  PC-DUPES $v - $why"
    return (Add-GateResult -Name "pc_dupes" -Status $v -Metrics $metrics -Detail $why)
}

