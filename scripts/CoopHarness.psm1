# CoopHarness.psm1 - shared launch/environment helpers for the KenshiCoop test
# harness. Phase 2 extraction: the per-scenario ENVIRONMENT (channel A/B knobs +
# log-only diagnostic traces) is now OWNED by the manifest (scripts/scenarios.psd1
# `DiagEnv` block per scenario) and applied here, so run_test.ps1 and
# run_lan_test.ps1 stop carrying duplicated, drifting name-match chains and the
# plugin's Config.cpp stops hard-coding scenario names.
#
# Design:
#   * DiagEnv is MODE-AGNOSTIC (host and join get the same knobs) - every entry
#     here is a channel toggle or a trace flag, never an endpoint/role value.
#   * The applier is HERMETIC: it clears the whole managed keyset to "" first,
#     then applies the scenario's DiagEnv, so no knob leaks from a previous
#     scenario when several run in one process.
#   * Clearing a channel key to "" means the plugin's Config default applies
#     (envOr("...","1")!="0" for the always-on channels; the scenario==""/setup
#     auto rules for invSync/worldSync). DiagEnv only carries the DELTAS from
#     those defaults (a probe forcing a channel OFF, or a scenario forcing
#     invSync/worldSync ON), exactly the set Config.cpp used to name.

Set-StrictMode -Version Latest

# Canonical set of every env var any scenario's DiagEnv may set. Kept in ONE
# place so the applier can clear them all before applying a scenario's deltas,
# and so Contract.Tests can assert the manifest never introduces an unknown key.
$script:CoopDiagEnvKeys = @(
    # --- channel A/B knobs (Config.cpp reads these; DiagEnv carries the deltas) ---
    'KENSHICOOP_INV_SYNC'
    'KENSHICOOP_XFER_SYNC'
    'KENSHICOOP_BLOCK_XFER'
    'KENSHICOOP_WORLD_SYNC'
    'KENSHICOOP_SPEED_SYNC'
    'KENSHICOOP_MONEY_SYNC'
    'KENSHICOOP_SPAWN_SYNC'
    'KENSHICOOP_RECRUIT_SYNC'
    'KENSHICOOP_FACTION_SYNC'
    'KENSHICOOP_TIME_SYNC'
    'KENSHICOOP_DOOR_SYNC'
    'KENSHICOOP_BUILD_SYNC'
    'KENSHICOOP_BDOOR_SYNC'
    'KENSHICOOP_HUNGER_SYNC'
    'KENSHICOOP_SAVE_SYNC'
    'KENSHICOOP_LOAD_SYNC'
    'KENSHICOOP_PROD_SYNC'
    'KENSHICOOP_RESEARCH_SYNC'
    'KENSHICOOP_DEED_SYNC'
    'KENSHICOOP_STORE_SYNC'
    'KENSHICOOP_SQUAD_SYNC'
    'KENSHICOOP_LATEJOIN_SYNC'
    # --- log-only diagnostic traces (read outside Config; never gate behavior) ---
    'KENSHICOOP_INV_DUMP'
    'KENSHICOOP_DEBUG_SPEED'
    'KENSHICOOP_DEBUG_SHACKLE'
    # On-screen authority labels (Replicator::debugMark): green DRV/HI, red
    # CULLED, yellow PARKED, gray MID, pinned over each judged body. Purely
    # visual - nothing is logged per body - so this earns its place in the
    # keyset only for scenarios whose SCREENSHOTS are part of the evidence
    # (escape_cohesion). Note the join renders the full set but the host only
    # renders the drive tiers unless KENSHICOOP_CELL_AUTH=1 also puts it through
    # enforceHostAuthority, so a scenario wanting both sides labelled must set
    # both keys.
    'KENSHICOOP_DEBUG_MARKERS'
    # Co-location collapse: while the two squads claim the SAME zone cell, every
    # claimed cell resolves to the host, so a shared camp behaves as v0.46 did
    # while separated squads keep per-cell authorship. Only meaningful with
    # KENSHICOOP_CELL_AUTH=1 - with authority off the host already owns the map.
    'KENSHICOOP_CELL_COLLAPSE'
    # Per-row census publish dump, plus the '[census] auth' provenance summary
    # that says whether an authored row came from a live claim, the vacated-cell
    # rule, or a never-claimed cell falling open to the host.
    'KENSHICOOP_DEBUG_CENSUS'
    'KENSHICOOP_JAIL_PROBE'
    'KENSHICOOP_TASK_SPIKE'
    'KENSHICOOP_JAIL_OBSERVE'
    # --- test-only load-path knob (read in Plugin.cpp, not Config) --------------
    # Forces the join to NACK a MATCHing LOAD_GO so the real folder-transfer path
    # runs on one machine (bootstrap_stream / stream_test.ps1).
    'KENSHICOOP_FORCE_STREAM'
    # --- test-only W2 fault injection (read in ReplicatorItems, not Config) ------
    # Refuses the FIRST conservation re-home so inv_regear_refuse can exercise the
    # retry + verify-then-destroy recovery. A real refusal depends on the receiving
    # bag's state, which a scenario cannot arrange.
    'KENSHICOOP_WD_REFUSE_REHOME'
    'KENSHICOOP_WD_REFUSE_REHOME_ALL'
    # Discards the author's ground track at birth, so a peer's pickup arrives NAMED but
    # unmatchable - the state that leaves a picked-up item lying on the author's ground.
    'KENSHICOOP_WD_FORGET_TRACK'
    # Shrinks the per-tick W1 send batch so a handful of drops can overflow it. NOT a 0/1
    # gate - it carries the cap itself.
    'KENSHICOOP_WI_BATCH_MAX'
    # Makes the first N pickup-time resolutions of a tracked ground object read as dead, then lets
    # them succeed - the transient the engine produces by streaming an object out and back, which
    # is what made an author conclude an item had left the ground while its copy was still there.
    # NOT a 0/1 gate: it carries the count.
    'KENSHICOOP_WD_TRANSIENT_DEAD'
    # Frees a world-item proxy through the engine immediately before the cull that is
    # about to run on it - the state a zone teardown leaves when players travel out of
    # a block. The real window is one frame wide (the publish sweep clears dead proxies
    # every tick), so it cannot be reached by scheduling a scenario into it.
    'KENSHICOOP_WI_TEST_STALE'
    # --- presence authority (Config.cpp) ----------------------------------------
    # Off everywhere except split_far2 and run_apart, which is what makes the tier
    # a fail-open proof rather than a co-test. Note this one is not merely cleared
    # but pinned to "0" below: the DLL default is ON, so clearing would OPT IN.
    'KENSHICOOP_CELL_AUTH'
    # --- combat speed cap (Config.cpp) ------------------------------------------
    # The speed arbiter pins the sim to 1x while either player squad fights, which
    # is right for players and wrong for a scenario whose job is to cover 100 k u:
    # bandit country along the way turned a 3-minute crossing into a 15-minute one.
    # Set to 0 ONLY by run_apart. Anything measuring combat must leave it alone,
    # since the cap is exactly what stops a fight resolving at 5x.
    'KENSHICOOP_SPEED_COMBAT_CAP'
    # 1 Hz position track per squad tab, log-only. For RECORDING a route: cell
    # claims fire once per 4608 u cell, which is far too sparse to walk between.
    'KENSHICOOP_TRACK_MOVE'
    # --- gear GRADE on fabricate (protocol 51, EngineInventory createItemAndAdd) --
    # Ships ON: a rebuilt piece of gear is minted at the craft level the wire carried, so a
    # traded Masterwork stays Masterwork. Set to 0 to restore the pre-fix constant
    # levelOverride - the rollback for the spike-451 weapon-mint recipe this argument sits
    # inside, and how trade_peer's grade gate is shown to be able to fail (with it off the
    # host's rebuilt armour reads alv=-1, not even Gear). Managed here rather than left
    # ambient so a stale shell value cannot silently decide what a run measured.
    'KENSHICOOP_GEAR_LEVEL'
    # --- census adoption reach (Config.cpp) --------------------------------------
    # Ships at 250 u: the join binds a census row to one of its OWN bodies of that
    # template instead of minting a proxy beside it, which is what stops a
    # separately-generated town from being built twice on the join. NOT a 0/1 gate -
    # it carries the radius, and "0" turns adoption off. Managed here so town_arrive
    # can pin the value it measured against instead of inheriting whatever the DLL
    # currently ships, and so the A/B is a manifest edit rather than a rebuild.
    'KENSHICOOP_ADOPT_RADIUS'
)

function Get-CoopDiagEnvKeys {
    <#
    .SYNOPSIS
      The canonical list of env vars the manifest DiagEnv is allowed to set.
    #>
    return @($script:CoopDiagEnvKeys)
}

function Set-CoopDiagEnv {
    <#
    .SYNOPSIS
      Apply a scenario's manifest DiagEnv to the current process environment.
    .DESCRIPTION
      Clears the whole managed keyset first (hermetic), then applies the given
      manifest entry's DiagEnv hashtable. Returns the applied map (for logging /
      run.json provenance). Pass $null / an entry without DiagEnv to just clear.
    #>
    param($Entry)

    foreach ($k in $script:CoopDiagEnvKeys) { Set-Item -Path "env:$k" -Value "" }

    # Presence authority ships ON (Config.cpp, v0.47), and clearing an env var on
    # Windows DELETES it, so a cleared keyset would hand every scenario the SHIPPED
    # default and quietly turn the whole tier into a co-test of that flag. Pin it
    # clear as the harness baseline instead: the tier's job is to prove the feature
    # is additive, which it can only do from the isHost branch. DiagEnv still wins,
    # which is how split_far2 and run_apart ask for it.
    Set-Item -Path "env:KENSHICOOP_CELL_AUTH" -Value "0"

    $applied = @{}
    if ($null -ne $Entry -and ($Entry -is [hashtable]) -and $Entry.ContainsKey('DiagEnv')) {
        $diag = $Entry.DiagEnv
        foreach ($k in $diag.Keys) {
            if ($script:CoopDiagEnvKeys -notcontains $k) {
                throw "DiagEnv key '$k' is not in CoopHarness::CoopDiagEnvKeys (manifest error)."
            }
            $val = "$($diag[$k])"
            Set-Item -Path "env:$k" -Value $val
            $applied[$k] = $val
        }
    }
    return $applied
}

function Stop-CoopKenshi {
    <#
    .SYNOPSIS
      Kill any stale Kenshi game processes from a previous (possibly crashed) run.
    #>
    param([int]$SettleSec = 2)
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) {
        $stale | Stop-Process -Force -ErrorAction SilentlyContinue
        if ($SettleSec -gt 0) { Start-Sleep -Seconds $SettleSec }
    }
    return $stale.Count
}

Export-ModuleMember -Function Get-CoopDiagEnvKeys, Set-CoopDiagEnv, Stop-CoopKenshi
