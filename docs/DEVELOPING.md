# Developing KenshiCoop

The build/test/ship loop, and how to land a fix with automated coverage so it
never regresses. See `CLAUDE.md` for the short version and the golden rules.

## 1. Environment (once per machine)

```
powershell -ExecutionPolicy Bypass -File scripts\bootstrap_windows.ps1
```

Idempotent: it checks every prerequisite the v100 plugin build needs and
auto-fixes the git-ignored deps state (the KenshiLib deps pin, the two ENet
patches, the deps header patches, user-scope env vars, the VS7 registry key). The
interactive installs it can't safely automate — the **Windows SDK 7.1 / v100
compiler** (with its documented "uninstall the VC++2010 redists first" gotcha)
and VS2022 Build Tools — it prints as explicit manual steps. Re-run until it
reports all-green, then build.

Why v100: KenshiLib plugins must be compiled with the VC++2010 (v100) x64
toolset, matching `KenshiLib_Examples`. Modern MSBuild drives the v100 toolset;
the deps and Boost 1.60 are fetched into `third_party/` (git-ignored).

## 2. Build & deploy

```
scripts\build_plugin.cmd Harness     # test build: includes the scenario runner (default)
scripts\build_plugin.cmd Release     # ship build: excludes the harness
scripts\deploy.cmd                   # copy the DLL to the Steam install + Kenshi-Join
```

Output: `src/plugin/x64/<Config>/KenshiCoop.dll`. The **Harness** build is what
the automated suite needs; **Release** is what a player installs (and what the
release kit ships).

## 3. Test

Three layers, cheapest first:

| Layer | Command | Needs the game? | What it proves |
|-------|---------|-----------------|----------------|
| Unit | `cmd /c scripts\build_prototest.cmd && dist\prototest.exe` | no | wire format, hashes, interpolation, save-transfer framing |
| One scenario | `scripts\run_test.ps1 -Scenario <name>` | yes (two clients, self-exit) | one behaviour, judged by oracles |
| Suite | `scripts\regress.ps1 -Tier smoke` (15) or `-Tier full` (76) | yes | the whole matrix, one PASS/FAIL |

The unit layer also runs in cloud CI (`.github/workflows/ci.yml`) along with
PowerShell lint. The scenario/suite layers launch real Kenshi windows and screenshot
them, so they only run **locally** on a desktop — CI cannot and must not fake them.

Runs land in `tools/test-runs/<timestamp>/` (per-client logs, screenshots,
`verdict.json`) and append to `tools/test-runs/history.jsonl` for trending.

### Flakes

Perf-sensitive oracles (`smoothness`, `snap_rate`, `pose_state`, `npc_track`)
can flake on a busy machine; `regress.ps1` retries a failed scenario once. Judge a
scenario by its **functional** oracles; if a perf oracle fails persistently,
verify against an unpatched baseline build before blaming your change. Keep the
mod list trimmed to `KenshiCoop.mod` for test runs — the fixtures are vanilla, so
the heavy real mod list only adds perf noise.

## 4. Add a fix *with coverage* (the important part)

A fix is not done until a scenario proves it without a human. The pattern:

1. **Reproduce first.** Write (or extend) a compiled scenario that FAILS on the
   current build, so you know the oracle actually catches the bug. Scenarios live
   in `src/plugin/test/Scenario*.cpp`; register the class in the `makeScenario`
   factory at the bottom of the file.
2. **Add the manifest entry** in `scripts/scenarios.psd1`: the save it loads, its
   oracle set, and any DiagEnv it needs. This is the single source of truth for
   what a scenario runs against — never hardcode a save in a runner.
3. **Add/extend the oracle** in `scripts/oracles/*.ps1` if the gate is new. An
   oracle reads a **data-level** fact from the logs (a count, a sid, a position),
   never a pixel. Emit a clear `SCENARIO <NAME> ...` log line from the scenario
   and match it in the oracle.
4. **Implement the fix**, rebuild, and confirm the scenario now PASSES (and still
   FAILS on a reverted build if you can afford the A/B).
5. **Run the smoke tier** to catch collateral damage.
6. **Commit to `main`** with a message that states the measured evidence.

Mirror the closest existing scenario when writing a new one — e.g.
`world_pickup_mirror` (`ScenarioWorldItems.cpp`) for an item-pickup mirror, the
`inv_*` family (`ScenarioInventory.cpp`) for inventory transfers.

## 5. Ship a release

Cut from `main` when a batch of fixes is verified:

```
git checkout main
scripts\make_mod_kit.ps1              # builds Release, assembles dist\KenshiCoop-kit.zip
gh release create vX.Y.Z --title "..." --notes "..." dist\KenshiCoop-kit.zip
```

The kit is a drop-in `KenshiCoop` folder players copy into `<Kenshi>\mods\`.
Because the protocol version is checked at the handshake, **both players must
install the same release** — say so in the release notes whenever the protocol
bumped.

## Layout

```
src/plugin/net/         ENet transport, packet pump/dispatch (NetLink)
src/plugin/sync/        Replicator: publish/apply per channel (items, inventory, npc, combat, ...)
src/plugin/game/        Engine facade: SEH-guarded engine reads/writes (Engine*, EngineInventory, ...)
src/plugin/core/        Config, Inbound mailboxes, logging, small value types
src/plugin/test/        Compiled scenarios (the behaviour suite's drivers)
src/netproto/           Wire.h (protocol), ContentHash.h — plain C++03, shared by prototest
scripts/                Build/deploy/session/test tooling + oracles + the scenario manifest
docs/                   Reference, pitfalls, engine facts, save orchestration, this file
third_party/            ENet (+patches), KenshiLib deps, vc10_compat — fetched, git-ignored
```
