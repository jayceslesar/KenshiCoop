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
    if ($active -lt $MinActiveFrames) {
        Write-Host "  [$Label] smoothness SKIP - active=$active (< $MinActiveFrames scored frames; slewSkip=$slewSkip fell in the clock-slew window)"
        return (Add-GateResult -Name "smoothness" -Status SKIP `
                    -Metrics @{ active = $active; slewSkip = $slewSkip } `
                    -Detail "only $active scored frames (slewSkip=$slewSkip)")
    }
    $ok = ($zeroFrac -le $MaxZeroFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] smoothness $v - zeroFrac=$zeroFrac (<= $MaxZeroFrac), maxStep=$maxStep, active=$active, slewSkip=$slewSkip"
    return (Add-GateResult -Name "smoothness" -Status $v -Metrics @{ zeroFrac = $zeroFrac; maxStep = $maxStep; active = $active; slewSkip = $slewSkip })
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
        $idleFrac = [math]::Round($holdStop / [double]$rest, 4)
        $ok = ($idleFrac -le $MaxIdleFrac)
        $v = if ($ok) { "PASS" } else { "FAIL" }
        Write-Host "  [$Label] march-in-place $v - idleFrac=$idleFrac (<= $MaxIdleFrac), holdStop=$holdStop, holdDip=$dip (scored at rest, not a defect), marchFrac=$marchFrac, restSamples=$rest"
        return (Add-GateResult -Name "march" -Status $v -Metrics @{
                    idleFrac = $idleFrac; holdStop = $holdStop; holdDip = $dip
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
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WNPC hand=(\d+),(\d+),[\d,]+ pos=([\-\d\.,]+) cls=(\w+) name='([^']*)'(?: task=(\d+) pelvis=(-?[\d\.]+) mv=(-?\d+)(?: carry=(-?\d+))?)?"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $p = $g[7].Value.Split(',') | ForEach-Object { [double]$_ }
        $task = -1; $pelvis = -1.0; $mv = -1; $carry = -1
        if ($g[10].Success) { $task = [int]$g[10].Value }
        if ($g[11].Success) { $pelvis = [double]$g[11].Value }
        if ($g[12].Success) { $mv = [int]$g[12].Value }
        if ($g[13].Success) { $carry = [int]$g[13].Value }
        [void]$rows.Add(@{ t = $t; hand = "$($g[5].Value),$($g[6].Value)"
                           pos = $p; cls = $g[8].Value; name = $g[9].Value
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
