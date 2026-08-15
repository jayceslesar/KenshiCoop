# Engine facts ledger (measured, not assumed)

Kenshi ships no modding API for any of this; every entry below was established by
instrumenting a running two-client session on **Kenshi 1.0.65** and reading the
`[wi]`/`SCENARIO`/`[load]` log tags, not by reading engine source (there is none).
Each fact carries the **evidence** that produced it and a **confidence** so a
future change knows how much to trust it. When a fact contradicts an assumption
baked into older code, that is called out — those are the expensive ones.

This complements the two existing deep docs:
- [`API_REFERENCE.md`](API_REFERENCE.md) — the engine surface (types, calls, safety rules).
- [`REPLICATION_PITFALLS.md`](REPLICATION_PITFALLS.md) — the design rules those facts imply.

Keep this file **empirical**: a new row is a thing you *measured*, with the log
line that shows it. Move a well-worn fact into the reference docs once it is load-bearing.

---

## 1. An object's engine hand is not one kind of identity — baked hands are save-stable, loose-item hands are per-session

The 5-field engine hand `(type, container, containerSerial, index, serial)` is
the cross-client identity for **baked world objects** — buildings, furniture,
doors, mines, the cage/pole/container a setup scene spawns and saves. Both
clients load the identical save, both re-resolve the same hand, and the
deed/door/build/furniture channels rely on this and work.

**It is NOT stable for loose ground items.** A save-native item's `(index,
serial)` is assigned when the item is instantiated for the session, so the hand
the *sender* reads does not resolve to the same object — often to no object — on
the *receiver*, even though both loaded the same save.

- **Evidence.** The first cut of the native-pickup notice (upstream #44) keyed by
  hand. Host log: `NATIVE-TAKEN send ... hand=4,0,0,10055,2354614272`. Join log
  the same instant: `NATIVE-TAKEN apply ... destroyed=0 why='unresolved'`. The
  identical save was loaded on both. Switching the notice to **template
  (GameData stringID) + world position** made it resolve and destroy first try
  (`destroyed=1 why='ok'`, 13–59 ms cross-client).
- **Confidence: high** for "hand-only cross-client item identity fails in this
  save"; **medium** for the general claim that *all* loose-item hands are
  per-session (measured on the bar-town shop-floor natives; not exhaustively
  swept across item sources).
- **Rule.** For a loose item both clients hold from the shared save, identify it
  by **sid + position within a few units**, not by hand. Carry the hand only as a
  same-client fast path. See `destroyNativeGroundItem` (`EngineInventory.cpp`) and
  `WorldNativeTakenPacket` (`Wire.h`).
- **Contradicts:** comments of the form "both clients loaded the same save, so the
  hand resolves on both" are correct for baked buildings and *wrong* for loose
  items — the distinction is the object's origin, not the shared save.

## 2. "Unowned" is a real sentinel Faction (`nofac`), never a null pointer

`RootObject::getFaction()` on a free ground item does not return null. The engine
parks ownerless items — player drops and mod-minted proxies included — on a real
sentinel `Faction` whose `GameData` stringID is **`nofac`**.

- **Evidence.** A pointer-only `fac != playerFac` ownership test read *every*
  ground item as owned and blinded the whole world-item scan
  (`world_pickup_mirror` stopped finding the proxy it exists to pick up). A DIAG
  probe printed the sentinel's sid as `nofac`; passing that sentinel through the
  filter fixed it.
- **Confidence: high** (1.0.65).
- **Rule.** An ownership filter must treat `getFaction()` returning the `nofac`
  sentinel as *unowned*. Resolve the sid with `facSidOf(Faction*, buf, len)` and
  compare strings; never compare only pointers. See the ownership filter in
  `captureWorldItems` (`EngineInventory.cpp`).
- **Player faction sid:** `204-gamedata.base` (the player-owned items to *keep* in
  the stream, vs `nofac` free items and non-player faction town/shop stock).

## 3. The world does not exist yet on the first publish pass after a load

The first tick after a save loads runs before the zone's contents have streamed
in. A one-shot "first scan is the baseline" latch therefore latches an *empty*
world, and everything that streams in afterwards looks like a fresh session event.

- **Evidence.** Host log: `BASELINE seeded=0`, then 14 `SEND` rows 130–200 ms
  later, and again another batch when a bar interior finished streaming **20+ s
  after connect** — each batch minted grey duplicates on the peer.
- **Confidence: high.**
- **Rule.** Do not gate "is this original save content?" on a timer or a single
  early scan. Make it structural: in the world-item stream, an item the drop-hook
  did not author is a save-native regardless of when the scan first sees it. See
  the scan-attribution rule in `publishWorldItems` (`ReplicatorItems.cpp`) and
  pitfall #2 ("never conclude from a single engine read").

## 4. Shop/town stock lives in containers, not as ground items

The red, stealable goods on a shop's shelves are **container contents**, not free
ground objects, so the ground-item spatial scan (`getObjectsWithinSphere`) does
not enumerate them. Only items actually lying on the floor/street are ground items.

- **Evidence.** In the bar-town save, the manual native-pickup test could only be
  staged on the loose floor items; the shelf stock never appeared in
  `captureWorldItems` rows.
- **Confidence: medium-high** (observed, not container-dumped to confirm every case).
- **Consequence.** A "steal from a shelf syncs" test must drive the *container*
  path (see #39/#40 in the tracker), not the ground-item path.

## 5. Two identical templates within a few units are interchangeable copies

Because loose items are identified by sid + position (fact #1), two stacks of the
same template lying within the position tolerance are indistinguishable and are
treated as the same logical item across clients. The receiver destroys the
*nearest* live ground item of the template within tolerance.

- **Confidence: high** by construction; the tolerance (`NATIVE_POS_TOL = 4.0` u)
  was chosen so float noise and physics settle are covered without admitting a
  neighbouring stack. If a future case needs finer discrimination, add quantity or
  quality to the match key, not a tighter radius.

## 6. The two harness clients share one save folder; the host writes to it mid-session

Both the host install and the `Kenshi-Join` install load saves from the same
`%LOCALAPPDATA%\kenshi\save\<name>`. On connect the host bakes the live world over
the loaded save and the join stream-commits into the same folder, so a validation
fixture silently rots across runs unless it is restored first.

- **Confidence: high** (this is why `fixtures/saves/*` are repo-tracked and
  `run_test.ps1` restores the pristine copy before every run — see pitfall #13).
- **Corollary for testing.** The fixtures are **vanilla** — every `*.mod` sid in
  them is a Kenshi developer data file, none are third-party mods — so the mod
  list can be trimmed to `KenshiCoop.mod` for faster, cleaner test runs.

---

### How to add a fact here

1. You must have a **log line or measurement** that shows it — not a plausible
   reading of behaviour. If you cannot point at the evidence, it is a hypothesis,
   not a fact.
2. State the **confidence** and the **save/version** it was measured on.
3. If it contradicts an existing assumption in the code, say where, so the next
   person fixes the stale comment instead of trusting it.
