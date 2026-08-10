---
name: coop-save-orchestration
description: >-
  Load and orchestrate Kenshi saves for KenshiCoop debugging and validation.
  Covers how saves auto-load (env vars + %LOCALAPPDATA%\kenshi\save), which
  script drives which job (manual_session.ps1 for hands-on debugging,
  run_test.ps1 / regress.ps1 for validation, bake_scene.ps1 for fixtures), the
  catalog of named saves and their uses (sync, squad1, duel1, camp, separate,
  together, ...), and the rule that validation saves must never be overwritten
  because the host connect-push bakes the live world over the loaded save and
  clobbers the fixture. Use when launching a co-op session, picking a save for a
  scenario, baking a fixture, or before doing anything that loads/writes a save.
---

# KenshiCoop Save Orchestration

## How saves load

- Saves live in `%LOCALAPPDATA%\kenshi\save\<name>\` (one folder per save,
  containing `quick.save`). Both the host Steam install and the `Kenshi-Join`
  install read this SAME per-user folder, so a save is visible to both clients
  with no copy step.
- The plugin auto-loads a save from the title screen via env var
  `KENSHICOOP_SAVE=<name>`. The orchestration scripts set this for you.
- **Validation fixtures are version-controlled** under `fixtures/saves/<name>\`.
  `run_test.ps1` restores the pristine repo copy over the AppData copy right
  before every run (via `deploy_saves.ps1`), so a prior co-op run's connect-push /
  stream-commit can never permanently drift a fixture - drift is discarded each
  run. See the fixture workflow below.
- Identity is **resolve-by-hand**: NPC/squad sync REQUIRES both clients on the
  IDENTICAL save. Two different saves mint different hands and desync. For two
  players sharing one world, use one save + partitioned ownership (`-Inhabit`),
  not two distinct saves.

## Pick the right orchestrator

| Job | Script | Notes |
|-----|--------|-------|
| Hands-on debugging / play | `scripts/manual_session.ps1` | Launches host+join, NO self-exit, NO screenshots. You drive and watch. |
| Validate ONE scenario | `scripts/run_test.ps1 -Scenario <name>` | Timed, self-exits, judged by oracles → `verdict.json`. Save/setup default from the manifest. |
| Full regression matrix | `scripts/regress.ps1 [-Tier smoke\|full]` | Builds once, runs the manifest matrix, single PASS/FAIL + `history.jsonl`. |
| Bake a fixture save | `scripts/bake_scene.ps1 -Setup <s> -BaseSave <b> -BakeSave <out> [-Promote]` | Headless: load base, run setup scene, auto-write the fixture. `-Promote` captures it into the repo. |
| Restore fixture(s) repo -> AppData | `scripts/deploy_saves.ps1 -Save <name>` (or `-All`) | Mirrors the pristine repo copy over AppData. `run_test.ps1` calls it automatically. |
| Capture a save AppData -> repo | `scripts/capture_save.ps1 -Save <name>` | The ONLY sanctioned way to update a tracked fixture (review the git diff). |

The scenario manifest `scripts/scenarios.psd1` is the single source of truth for
which save + setup each scenario uses. When adding/altering a scenario, edit it
there — do not hardcode saves in the runners.

### Debugging: manual_session.ps1

```
powershell -ExecutionPolicy Bypass -File scripts/manual_session.ps1 -Save "together" -Inhabit
```

Common flags:
- `-Inhabit` — shared save, partitioned squad ownership (host owns rank 0, join
  owns the rest). The supported two-player path; forces shared save + no autospawn.
- `-Sync` — mirror the host save into the join install first (only needed for a
  real second machine; same-machine installs already share the folder).
- `-SkipBuild` — reuse the current DLL (still deploys it).
- `-DebugMarkers` — colored authority labels on the join (green DRV / red HID / yellow LOC).
- `-AutoSpawn N` — host spawns N distinct-hand squad members to exercise cross-client render.

### Validation: run_test.ps1 / regress.ps1

```
powershell -ExecutionPolicy Bypass -File scripts/run_test.ps1 -Scenario combat_kill
```

```
powershell -ExecutionPolicy Bypass -File scripts/regress.ps1 -Tier smoke
```

## Save catalog

VALIDATION saves are referenced by `scripts/scenarios.psd1` and must stay stable
(see the golden rule below). DEBUG saves are for manual/exploratory sessions.

| Save | Class | Use |
|------|-------|-----|
| `sync` | validation | Bar town + 2-tab squad + armed NPCs. The workhorse "live town": npc_sync, player_combat, assault_town, combat_crowd/battle/win, and most probe/sync scenarios. |
| `squad1` | validation | Baked 2-tab squad. coop_presence, inventory, medical, KO, stats, carry, sneak_pose, squad_sync. |
| `squad2` | validation (fixture) | Two squads whose PCs WEAR backpacks (itemType 46): inv_backpack_drop, inv_nested_bag, inv_dump_all(_forget/_transient). The only fixture with a worn container - `squad1` has none, so those gates read `have=0` there. |
| `c` | validation | combat_probe / spike captures. |
| `duel1` | validation (fixture) | Two nearby non-squad NPCs for a duel: combat_order, combat_kill. Baked via bake_scene. |
| `down1` | validation | down_order, death_order. |
| `craft1` | validation | Dense town with node-anchored crafters: craft_order. |
| `bedcage1` | validation (fixture) | Baked Camp Bed + Prisoner Cage: bed_*, cage_*, chain_put. |
| `pole1` | validation (fixture) | Baked standing Prisoner Pole: pole_put. |
| `camp` | validation | Dense prison save (many NPCs): camp_approach, world_parity, shackle_*. |
| `splitfar1` | validation (fixture) | The pair SPLIT ~5200 u apart with NPCs at BOTH ends: split_far. Baked with `bake_scene.ps1 -Setup splitfar -BaseSave separate`, which parks the non-leader tab at a measured populated point. Re-make it that way, never via `capture_save.ps1` - see the capture-after-a-run warning below. |
| `jailed` | validation | Join PC caged: jail_probe / jail_soak. |
| `rebirth1` | validation (fixture) | Rebirth: the JOIN's whole squad is caged as slaves, with the two squads co-located and a zone-cell boundary a short walk away. Drives lockpick_escape and escape_cohesion, and is the co-location bed for authority A/Bs (`tools/authority_ab.ps1`). |
| `coopresume` | validation (auto-written) | Coordinated save/load transfer target (protocol 31/32). Written by the tests — never hand-edit. |
| `zoom` | debug | Outside town, camera far out — long-run pop/snap inspection. |
| `separate` | debug | The pair SEPARATED — testing independent actions across distance. |
| `together` | debug | The pair TOGETHER — testing independent actions co-located. |

Other saves in the folder (`battle10/20/40`, `slaves save`, `cage2`, `on-pole`,
etc.) are ad-hoc testbeds; confirm intent before reusing them.

## Fixture store: repo-tracked pristine saves

Validation fixtures live under `fixtures/saves/<name>\` (full save folder:
`quick.save`, `platoon/`, `zone/`, `portraits_texture.png`) - the pristine source
of truth. The tracked set matches the saves in `scripts/scenarios.psd1`:
`sync, squad1, squad2, c, duel1, down1, craft1, bedcage1, pole1, camp, jailed, coopresume,
splitfar1, rebirth1`.
Debug saves (`together`, `separate`, `zoom`) are NOT tracked; drift there is fine.

- **Restore (automatic):** `run_test.ps1` restores the scenario's save from the
  repo over AppData before every launch, so `regress.ps1` (which calls `run_test`
  per scenario) is self-healing. Restore manually with
  `scripts/deploy_saves.ps1 -Save <name>` or `-All`.
- **Update a fixture (deliberate only):** re-make/bake the save, then promote it:
  - baked fixtures: `bake_scene.ps1 -Setup <s> -BaseSave <b> -BakeSave <name> -Promote`
  - hand-made saves: `scripts/capture_save.ps1 -Save <name>`
  Both mirror AppData -> repo; review the git diff and commit.

## Why drift can't persist (and where it comes from)

When the host runs under save-sync and a peer connects, `armConnectPush()`
(`src/plugin/Plugin.cpp`) bakes the live world with `saveGameAs(<currentGame>)`
— i.e. it writes the CURRENT world state OVER the folder of the save that is
loaded; the join also stream-commits the received world into the same folder.
Both mutate the AppData copy in place. Historically this silently corrupted
`pole1` / `duel1` mid-regression and made `combat_order` / `combat_kill` fail for
reasons unrelated to the code under test. The repo fixture store fixes this: the
game only ever writes AppData, which `run_test.ps1` overwrites from the pristine
repo copy each run, so drift is discarded.

Rules:
- Do co-op experiments on a DEBUG save (`separate`, `together`, `zoom`, or a
  throwaway you create), not a validation/fixture save. (Even if a fixture drifts,
  the next `run_test.ps1` restores it - but keep manual play off fixtures anyway.)
- NEVER hand-edit `fixtures/saves/*` directly. Change a fixture only via
  `bake_scene.ps1 -Promote` or `capture_save.ps1`, then commit the reviewed diff.
- Do NOT `capture_save.ps1` a save that a CO-OP run just used to record the scene
  that run set up. `armConnectPush` bakes the live world early, before scenario
  positioning, so the captured folder holds the pre-scenario layout rather than
  the one you watched. This produced a `splitfar1` with the squads together when
  the whole point was to have them 9800 u apart. Positioned scenes must be built
  by a `bake_scene.ps1` SETUP (single host, no connect-push, explicit write).
- To (re)create a baked fixture, use `bake_scene.ps1` deliberately - that is the
  ONLY sanctioned way to write a fixture folder. After baking, spot-check that the
  scenario it feeds still passes, then `-Promote` / capture it.
- If a functional test regresses for no code reason, suspect fixture rot in the
  REPO copy: re-bake (or re-make) the save, promote it, and re-run.
