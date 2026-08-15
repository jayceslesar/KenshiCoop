# CLAUDE.md — working guide for this repo

KenshiCoop is a co-op multiplayer plugin for **Kenshi 1.0.65**, built as an
RE_Kenshi / KenshiLib plugin (`KenshiCoop.dll`). This file is the operating guide
for an agent working in the repo: how to build, test, and ship, and the rules
that keep changes safe. Read `README.md` for what the mod is; read the `docs/`
below for depth.

## This is a maintained fork

`jayceslesar/KenshiCoop`. **`main` is the canonical line** — merge work there as
you gain confidence in it. Do **not** open upstream PRs unless explicitly asked;
this fork is the product. Releases (matched builds players install) are cut from
`main` when a meaningful batch of fixes is verified. Do not tag routine
build/infra commits — tags are for real releases (working toward a `1.0.0` once
the item/inventory bugs are fixed).

## The core loop

```
scripts\build_plugin.cmd Harness     # build the test DLL (Release = ship build)
scripts\deploy.cmd                   # push DLL to the Steam install + %USERPROFILE%\Kenshi-Join
scripts\regress.ps1 -Tier smoke      # 15-scenario behaviour suite (build+deploy+run+judge)
scripts\run_test.ps1 -Scenario X     # one scenario, judged by oracles -> verdict.json
```

- **Toolchain:** the plugin needs the VC++2010 (v100) x64 compiler driven by
  VS2022 MSBuild. `scripts\bootstrap_windows.ps1` checks/repairs the whole
  environment idempotently (deps pin, ENet patches, env vars, VS7 key) and prints
  manual steps for the interactive installs. Run it first on a fresh machine.
- **CI (game-free only):** `.github/workflows/ci.yml` builds+runs `prototest`
  (the protocol/hash/interp unit layer) and lints PowerShell. The behaviour suite
  needs two real game clients on a desktop and stays a **local** gate — never try
  to run it in CI, and never fake it.

## How testing works (and how to remove the human from a check)

Almost nothing needs a human. The harness launches both clients, clicks through
the launcher, auto-loads the save, runs a **compiled scenario** (C++ in
`src/plugin/test/Scenario*.cpp` that drives characters — drops/picks up items,
walks them, fights), self-exits, and judges from **log-oracles** (`scripts/oracles/*.ps1`)
that read engine state. A behaviour you can only "eyeball" today just needs a
scenario written for it:

- To add coverage: write a `Scenario` subclass, register it in the factory at the
  bottom of its `Scenario*.cpp`, add a manifest entry in `scripts/scenarios.psd1`
  (save + oracle set), and an oracle in `scripts/oracles/` if the gate is new.
  Mirror an existing close cousin (e.g. `world_pickup_mirror` for item pickup).
- The oracle reads a data-level fact, not a pixel: "red item" = the item's faction
  sid (`nofac` vs owned), "vanished on the peer" = its ground-object count → 0.
- `SCENARIO RESULT PASS`/`FAIL` is the universal per-client verdict line the
  oracles key on. Don't rename it.

The **only** genuinely human step is a two-client manual session
(`manual_session.ps1 -TitleScreen`) for a subjective look — use it sparingly, and
prefer to encode the check as a scenario instead.

## Golden rules (learned the hard way)

- **Protocol version:** any wire change bumps `PROTOCOL_VERSION` in
  `src/netproto/Wire.h`. Both clients must run the same build; the HELLO handshake
  rejects a mismatch. Update `src/prototest/main.cpp` for any packet shape change.
- **Never conclude from a single engine read**, and **never hand-edit fixtures**.
  Change a fixture only via `bake_scene.ps1 -Promote` or `capture_save.ps1`; do
  co-op free-play on a *debug* save, never a validation/fixture save (the host's
  connect-push bakes the live world over the loaded save). See
  `docs/SAVE_ORCHESTRATION.md`.
- **The engine owns every object's lifetime.** A stored `RootObject*` becomes
  freed memory when its zone unloads; re-resolve through the hand every time.
- **Loose-item hands are per-session**, not save-stable — identify a loose item
  by template + position across clients, not by hand (baked buildings/furniture
  hands *are* stable). See `docs/ENGINE_FACTS.md` #1.
- **SEH-guard every raw engine call** (`__try/__except`), and log unconditionally
  for anything a player can see.
- **Commit messages:** lower-case subsystem prefix (`coop:`, `test:`, `ci:`,
  `docs:`, `sdk:`), imperative, explain the *why* and the measured evidence. End
  with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## Docs index

- `docs/API_REFERENCE.md` — the engine surface (types, calls, safety rules, recipes).
- `docs/REPLICATION_PITFALLS.md` — the design rules, each tied to the bug that produced it.
- `docs/ENGINE_FACTS.md` — the empirical ledger: measured engine behaviours + the log line that proves each.
- `docs/SAVE_ORCHESTRATION.md` — save catalog, fixture rules, manual-session flow.
- `docs/DEVELOPING.md` — the build/test/ship loop and how to add a fix with coverage.
