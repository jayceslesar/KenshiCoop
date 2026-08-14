# Handoff: free-items fix (branch fix/free-items)

Context transfer from the Mac session that diagnosed and wrote this fix, for
the Claude Code session on the Windows build/test PC. Read this fully, then
follow "Your job" at the bottom. Delete this file before any upstream PR.

## The bug (upstream nhoral/KenshiCoop#63 and #66; related #44)

Faction-owned "red" (stealing-flagged) world items in towns/shops randomly
show gray and can be taken with no theft flag; items also appear duplicated.

Root cause, fully traced:

1. `captureWorldItems` (src/plugin/game/EngineInventory.cpp) enumerated EVERY
   free ground item within 60m of the leader with no ownership read at all —
   `WorldItemRaw` (src/plugin/game/Engine.h) has no owner field.
2. The only guard against streaming save-native items is the one-shot
   first-scan baseline in `publishWorldItems`
   (src/plugin/sync/ReplicatorItems.cpp, `worldSeeded_`), which only covers
   items within 60m of wherever the leader stands at the first publish pass
   after load. Any save-native item encountered LATER (walking into a town
   mid-session) was treated as a fresh session drop and streamed.
3. The peer's `spawnWorldItemProxy` (EngineInventory.cpp) fabricates proxies
   with `/*owner*/0` — an unowned, gray, freely-takeable duplicate stacked on
   the peer's own red native. The stream is bidirectional, so both clients
   mint gray copies of each other's town items.
4. Worse: picking up the gray proxy fires the CLAIM channel (protocol 47,
   ReplicatorItems.cpp ~line 582), telling the author to destroy its REAL red
   item — an engine destroy, no crime. Theft system fully bypassed.

## The fix (commit 0a1fbbe, deliberately minimal: 2 files, ~20 lines)

`captureWorldItems` now reads each scanned item's owner faction
(`o->getFaction()`, a `RootObjectBase` virtual — same header that provides
`getGameData`; the deed-purchase path already calls the sibling `setFaction`
on non-character objects) and SKIPS any item owned by a non-player faction.
Owned items are world furniture both clients already hold from the shared
save. This covers both replicator consumers of the scan (baseline seeding and
spatial discovery). Real player/NPC drops still stream via the query-free
`Inventory::dropItem` hook (Discovery 1), which does not pass through the scan.

Not touched: wire protocol, replicator logic, gear/conservation channel
(itemTypes 2/3/46), drop hook, claim channel. All existing world-item
scenarios census harness-dropped unowned items, so the filter never fires in
them.

Known risk: if the build errors because `getFaction` is not visible on
`RootObject`/`Item`, the fallback is to read the owner faction through a
resolved engine accessor instead (mirror how EngineInternal.cpp resolves
other engine functions). Expected to compile as-is.

## Upstream state (checked 2026-08-14)

- Fork main == upstream main; latest upstream release v0.51 == local HEAD's
  base. No upstream fix in flight for this (checked all open PRs).
- Issue #66 is the fork owner's own report; #63 describes this exact bug.
  #44 (stolen ground pickups only removed locally) is related but distinct —
  this fix removes owned items from the proxy path entirely, which should
  also stop #44's duplication variant for owned items, but unowned-drop
  pickup mirroring is a separate mechanism (already handled by CLAIM).

## Build/setup notes for this PC

- Tag `v0.51-freeitems.1` and prerelease exist on the fork with test notes.
- Canonical build doc was deleted upstream; recover it with:
  `git show 451da9b:resources/BUILD_SETUP.md > BUILD_SETUP_RECOVERED.md`
- Needs: VS2022 Build Tools (MSBuild), VC++2010 v100 x64 compiler
  (Win SDK 7.1 + KB2519277), git-LFS clone of
  BFrizzleFoShizzle/KenshiLib_Examples_deps into third_party/KenshiLib_deps
  (+ Boost 1.60 extracted inside it), env vars KENSHILIB_DIR /
  BOOST_INCLUDE_PATH, ENet C89 patch
  (third_party/enet/patches/0001-enet-c89-for-loops.patch), RE_Kenshi 0.3.1+.
- Copy `.cursor/skills/*` to `.claude/skills/` — manual-freeplay-session and
  coop-save-orchestration describe the harness. Key rules: both clients must
  load the IDENTICAL save; free-play on debug saves (`together`, `separate`),
  NEVER validation fixtures (`sync`, `squad1`, `duel1`) — connect-push bakes
  the live world over them.

## Your job

1. `scripts\build_plugin.cmd` (Harness config) — must compile clean.
2. `scripts\regress.ps1 -Tier smoke` — the world-item scenarios exercise the
   paths around the change; they must still pass (drop/pickup/rejoin).
3. `scripts\manual_session.ps1 -TitleScreen` — user loads a debug save in
   both windows, F2 host/join. Verification: in a town, red items stay red in
   BOTH windows (hold alt), no doubled items, pickup of a red item flags
   stealing; a normal drop from one window still appears in the other (town
   AND wilderness) — that guards against over-filtering.
4. Use a save WITHOUT already-baked duplicates; pre-existing dupes persist.
5. When the user confirms with their friend over a real network, offer to
   draft the upstream PR (closes #63/#66; drop this file from the PR).
