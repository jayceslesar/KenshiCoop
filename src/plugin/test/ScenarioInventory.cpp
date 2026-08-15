// ScenarioInventory.cpp - container/inventory scenarios (monolith split from
// Scenario.cpp, 2026-07-12): inv_order, inv_bidir, trade_probe, trade_peer,
// inv_equip/inv_reequip, inv_wpnseq, inv_addequip, wpn_relocate, and the
// protocol-46 item-loss regressions inv_overflow / inv_dropfull. Classes are
// TU-private (anonymous namespace); only makeInventoryScenario
// (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {

// Phase 4a: container-contents (inventory) replication. Both clients anchor on the
// SAME container (v1: the leader's own inventory - a save-stable hand that resolves
// cross-client). Each samples its LOCAL container's contents (count + order-
// independent content hash) every 500 ms; the host performs a LIVE add mid-run. The
// join must (a) observe a content CHANGE (>=2 distinct hashes - proving it wasn't a
// static loaded state) and (b) end with MORE items than its own baseline. The runner
// additionally cross-checks the host's and join's FINAL hashes match (same multiset).
class InventorySyncScenario : public TimedScenario {
public:
    InventorySyncScenario()
        : TimedScenario("inv_order", 0), haveContainer_(false), added_(false), lastLogMs_(0),
          samples_(0), distinct_(0), firstCount_(0), lastCount_(0),
          firstHash_(0), lastHash_(0), prevHash_(0) {
        for (int i = 0; i < 5; ++i) cHand_[i] = 0;
    }

    virtual void onStart(const ScenarioContext& ctx) {
        haveContainer_ = engine::pickInventoryContainer(ctx.gw, cHand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INV anchor have=%d hand=%u,%u,%u,%u,%u",
            haveContainer_ ? 1 : 0, cHand_[0], cHand_[1], cHand_[2], cHand_[3], cHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (haveContainer_ && (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0)) {
            lastLogMs_ = ctx.elapsedMs;

            InvItemEntry items[INV_ITEMS_MAX];
            unsigned int hash = 0;
            unsigned int n = engine::captureContainerContents(
                ctx.gw, cHand_, items, INV_ITEMS_MAX, &hash);

            if (samples_ == 0) { firstCount_ = n; firstHash_ = hash; prevHash_ = hash; }
            else if (hash != prevHash_) { ++distinct_; prevHash_ = hash; }
            lastCount_ = n; lastHash_ = hash; ++samples_;

            char b[160];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO INV %s t=%lu count=%u hash=%u",
                ctx.isHost ? "MEMBER" : "RECV",
                (unsigned long)ctx.elapsedMs, n, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);

            // Host performs the LIVE add once, after a short baseline window. The
            // content-change then rides the reliable snapshot channel to the join.
            if (ctx.isHost && !added_ && ctx.elapsedMs >= ADD_MS) {
                added_ = true;
                char sid[48]; sid[0] = '\0';
                int got = engine::addTestItemsToContainer(ctx.gw, cHand_, 1, sid, sizeof(sid));
                char m[200];
                _snprintf(m, sizeof(m) - 1,
                    "SCENARIO INV ADD added=%d sid='%s'", got, sid[0] ? sid : "(none)");
                m[sizeof(m) - 1] = '\0'; coop::logLine(m);
            }
        }

        // Host outlives the join so its stream/snapshot never goes stale mid-window.
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // Host: it performed the live add. Join: it ended holding synced (non-empty)
            // content. The launch stagger means the join may start sampling AFTER the
            // add already propagated (so it never sees the 0->1 edge itself); the
            // runner's INV-SYNC oracle is the authoritative cross-client proof
            // (host live-change + host/join final-hash match). distinct_ is advisory.
            if (ctx.isHost) passed_ = haveContainer_ && added_;
            else            passed_ = haveContainer_ && (lastCount_ > 0) && (lastHash_ != 0);
            return true;
        }
        return false;
    }

private:
    static const unsigned long HOST_DURATION_MS = 40000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 24000;
    static const unsigned long ADD_MS           = 8000;  // baseline, then add live

    bool          haveContainer_;
    bool          added_;
    unsigned long lastLogMs_;
    unsigned int  cHand_[5];
    unsigned int  samples_;
    unsigned int  distinct_;   // count of content-hash changes observed
    unsigned int  firstCount_;
    unsigned int  lastCount_;
    unsigned int  firstHash_;
    unsigned int  lastHash_;
    unsigned int  prevHash_;
};

// inv_bidir (Phase 4a, BIDIRECTIONAL container-contents): each client mutates ONLY the
// inventory of a squad member it OWNS (host = tab-rank 0, join = tab-rank 1 - the same
// partition the Replicator streams on) and samples BOTH squad-tab containers every
// 500 ms, logging OWN (authoritative) and PEER (reconciled) lines keyed by rank. Each
// side runs an ADD-then-REMOVE sequence with a DISTINCT net delta (host -> +2, join ->
// +1) so the runner's per-rank convergence check is unambiguous and removals (not just
// adds) must propagate. The runner cross-checks, per rank, that the NON-authoring side
// converged to the author's FINAL contents - proving inventory flows both ways with no
// loss/dupe on the supported (owned-container) path. Requires a shared save with >=2
// squad tabs (rank 0 and rank 1).
class InventoryBidirScenario : public TimedScenario {
public:
    InventoryBidirScenario()
        : TimedScenario("inv_bidir", 0), haveOwn_(false), added_(false), removed_(false),
          lastLogMs_(0), samples_(0), ownRank_(0),
          firstOwnCount_(0), lastOwnCount_(0), prevOwnHash_(0), distinctOwn_(0) {
        for (int i = 0; i < 5; ++i) ownHand_[i] = 0;
    }

    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.isHost ? 0u : 1u;
        haveOwn_ = resolveRankContainer(ctx.gw, ownRank_, ownHand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INVB anchor own_rank=%u have=%d hand=%u,%u,%u,%u,%u",
            ownRank_, haveOwn_ ? 1 : 0,
            ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3], ownHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // Sample BOTH squad-tab containers so each client logs its OWN container
            // (authoritative truth it streams) and the PEER's (the one it reconciles).
            for (unsigned int rank = 0; rank < 2; ++rank) {
                unsigned int cHand[5];
                if (!resolveRankContainer(ctx.gw, rank, cHand)) continue;
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int hash = 0;
                unsigned int n = engine::captureContainerContents(
                    ctx.gw, cHand, items, INV_ITEMS_MAX, &hash);
                const char* role = (rank == ownRank_) ? "OWN" : "PEER";
                if (rank == ownRank_) {
                    if (samples_ == 0) { firstOwnCount_ = n; prevOwnHash_ = hash; }
                    else if (hash != prevOwnHash_) { ++distinctOwn_; prevOwnHash_ = hash; }
                    lastOwnCount_ = n; ++samples_;
                }
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO INVB r=%u %s t=%lu count=%u hash=%u",
                    rank, role, (unsigned long)ctx.elapsedMs, n, hash);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }

            // Mutate ONLY our OWN container: ADD a burst, then REMOVE part of it. The
            // remove forces the peer to converge DOWN (not just up), so a removal that
            // failed to propagate would leave the peer stuck above the author's count.
            if (haveOwn_) {
                int addN = ctx.isHost ? 3 : 2; // host ends +2 over baseline, join ends +1
                int remN = 1;
                if (!added_ && ctx.elapsedMs >= ADD_MS) {
                    added_ = true;
                    char sid[48]; sid[0] = '\0';
                    int got = engine::addTestItemsToContainer(ctx.gw, ownHand_, addN, sid, sizeof(sid));
                    char m[200];
                    _snprintf(m, sizeof(m) - 1,
                        "SCENARIO INVB ADD r=%u n=%d sid='%s'", ownRank_, got, sid[0] ? sid : "(none)");
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
                if (added_ && !removed_ && ctx.elapsedMs >= REM_MS) {
                    removed_ = true;
                    int got = engine::removeTestItemsFromContainer(ctx.gw, ownHand_, remN);
                    char m[160];
                    _snprintf(m, sizeof(m) - 1, "SCENARIO INVB REM r=%u n=%d", ownRank_, got);
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // In-plugin verdict only confirms the scenario EXECUTED (resolved its owned
            // container, sampled, and - on each side - performed its add+remove). The
            // runner's per-rank cross-client convergence check (Test-InventoryBidir) is
            // the authoritative no-loss/no-dupe gate.
            passed_ = haveOwn_ && (samples_ > 0) && added_ && removed_;
            return true;
        }
        return false;
    }

private:
    // Resolve the lowest-hand squad member whose squad-tab CONTAINER has the given rank
    // (distinct hand-containers, sorted - the SAME key the Replicator partitions on) and
    // write its object hand (== its personal-inventory container hand) to out.
    static bool resolveRankContainer(GameWorld* gw, unsigned int rank, unsigned int out[5]) {
        for (int i = 0; i < 5; ++i) out[i] = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        if (n == 0) return false;
        int best = -1;
        for (unsigned int i = 0; i < n; ++i) {
            int cr = containerRankOf(sq, n, i);
            if (cr < 0 || (unsigned int)cr != rank) continue;
            if (best < 0 || handLess(sq[i], sq[best])) best = (int)i;
        }
        if (best < 0) return false;
        out[0] = sq[best].hType; out[1] = sq[best].hContainer;
        out[2] = sq[best].hContainerSerial; out[3] = sq[best].hIndex; out[4] = sq[best].hSerial;
        return true;
    }
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    static const unsigned long HOST_DURATION_MS = 44000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 28000;
    static const unsigned long ADD_MS           = 8000;  // baseline, then add
    static const unsigned long REM_MS           = 14000; // then remove part of it

    static const unsigned int  MAX_SQUAD        = 32;

    bool          haveOwn_;
    bool          added_;
    bool          removed_;
    unsigned long lastLogMs_;
    unsigned int  samples_;
    unsigned int  ownRank_;
    unsigned int  ownHand_[5];
    unsigned int  firstOwnCount_;
    unsigned int  lastOwnCount_;
    unsigned int  prevOwnHash_;
    unsigned int  distinctOwn_;
};

// trade_probe (protocol-36 BASELINE, evidence not a gate): characterize what happens
// TODAY when a player performs a direct CROSS-OWNER drag - the field-reported dupe /
// wipe / weapon-vanish. The HOST plays the "dragger": it locally relocates real items
// between the join-owned (rank 1) and host-owned (rank 0) squad containers via
// engine::moveItemBetweenContainers (the same engine mutation the UI drag performs),
// which violates the single-writer inventory model on purpose:
//   TAKE  @16s: 1 common item  rank1 -> rank0  (drag OUT of the peer's bag)
//   GIVE  @26s: 1 common item  rank0 -> rank1  (drag INTO the peer's bag)
//   WTAKE @36s: 1 WEAPON       rank1 -> rank0  (the vanish case: no fabrication path)
//   ATAKE @46s: 1 ARMOUR       rank1 -> rank0  (the GRADE case, below)
// Both clients seed their OWN container @6s (join +3 / host +2 commons) so material
// exists, and sample BOTH containers every 500 ms, logging per-container count/hash
// plus the tracked probe-item, weapon and armour quantities. The runner's Test-TradeProbe
// reads the series from both logs and reports the conservation outcome per move
// (dupe / loss / clean) - the log IS the deliverable; nothing here gates sync quality.
// GRADE: the armour leg exists because a traded item was arriving on the peer at the wrong
// quality - Masterwork landing as something plainer. Kenshi's named grades are points on a
// 1-100 craft level baked into Gear at construction, while the inventory wire carries only
// the Item::quality float, which the fabricate path writes back verbatim. So a rebuilt copy
// agreed on quality and disagreed on the grade the player actually reads, and the existing
// qual= assertions (weapon_loot) could not see it because they compare the same field the
// fabricate path patches. The leg therefore samples BOTH numbers per container - aq (the
// wire's bucket) and alv (getLevel(), the grade source) - and Test-TradePeer gates alv
// across clients and against its pre-drag baseline:
//   pick   @first sample: a NOVEL armour template, held by neither tab
//   seed   @12s (JOIN):   mint it into the join's OWN tab at level 95 (Masterwork),
//                         through the engine factory rather than the sync's fabricate
//                         path, so the reference is graded correctly even with the fix off
//   ATAKE  @46s (HOST):   drag it rank1 -> rank0
// The host can only obtain its copy through the inventory snapshot channel, so a grade
// dropped on fabricate shows up as host and join disagreeing on alv. Armour rather than a
// weapon because armour is also the case whose material spec has to resolve for the
// rebuilt piece to keep its stats.
class TradeScenario : public TimedScenario {
public:
    explicit TradeScenario(bool peer)
        : TimedScenario(peer ? "trade_peer" : "trade_probe", 0),
          peer_(peer), tag_(peer ? "TRDE" : "TRDP"),
          hostDur_(peer ? 78000UL : 68000UL), joinDur_(peer ? 64000UL : 52000UL),
          lastLogMs_(0), samples_(0),
          seedDone_(false), takeDone_(false), giveDone_(false), wpnDone_(false),
          armDone_(false),
          probeType_(0), wpnType_(0), wpnLatched_(false), armLatched_(false),
          armSeeded_(false), armBaseDone_(false),
          firstDone_(false), firstWpn0_(0), firstWpn1_(0),
          lastWpn0_(0), lastWpn1_(0),
          firstArm0_(0), firstArm1_(0), lastArm0_(0), lastArm1_(0) {
        probeSid_[0] = '\0'; wpnSid_[0] = '\0'; armSid_[0] = '\0';
        for (int r = 0; r < 2; ++r) { rankHave_[r] = false; for (int k = 0; k < 5; ++k) rankHand_[r][k] = 0; }
    }

    virtual void onStart(const ScenarioContext& ctx) {
        for (unsigned int r = 0; r < 2; ++r)
            rankHave_[r] = resolveRankContainer(ctx.gw, r, rankHand_[r]);
        engine::commonTestItemSid(ctx.gw, probeSid_, sizeof(probeSid_), &probeType_);
        char b[200];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO %s anchor host=%d r0=%d r1=%d probeSid='%s' probeType=%u",
            tag_, ctx.isHost ? 1 : 0, rankHave_[0] ? 1 : 0, rankHave_[1] ? 1 : 0,
            probeSid_[0] ? probeSid_ : "(none)", probeType_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // Latch the tracked WEAPON deterministically on BOTH clients: the
            // lexicographically smallest weapon sid in the join-owned (rank 1)
            // container at first sample. Same save -> same pick on each side, so
            // the two logs track the same item without exchanging anything.
            if (!wpnLatched_ && rankHave_[1]) latchWeapon(ctx.gw);
            // Armour tracking is trade_peer's (the grade gate); trade_probe is a frozen
            // baseline and keeps its three drags, logging arm=0 aq=-1 alv=-1 throughout.
            if (peer_ && !armLatched_ && rankHave_[0] && rankHave_[1]) pickArmour(ctx.gw);
            int wpnNow[2] = { 0, 0 };
            int armNow[2] = { 0, 0 };
            bool sampledBoth = true;
            for (unsigned int rank = 0; rank < 2; ++rank) {
                if (!rankHave_[rank]) { sampledBoth = false; continue; }
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int hash = 0;
                unsigned int n = engine::captureContainerContents(
                    ctx.gw, rankHand_[rank], items, INV_ITEMS_MAX, &hash);
                if (n == 0) sampledBoth = false;
                int probeQty = 0, wpnQty = 0, armQty = 0;
                for (unsigned int i = 0; i < n; ++i) {
                    if (probeSid_[0] && items[i].itemType == probeType_ &&
                        strcmp(items[i].stringID, probeSid_) == 0)
                        probeQty += (int)items[i].quantity;
                    if (wpnSid_[0] && items[i].itemType == WEAPON_CAT &&
                        strcmp(items[i].stringID, wpnSid_) == 0)
                        wpnQty += (int)items[i].quantity;
                    if (armSid_[0] && items[i].itemType == ARMOUR_CAT &&
                        strcmp(items[i].stringID, armSid_) == 0)
                        armQty += (int)items[i].quantity;
                }
                wpnNow[rank] = wpnQty;
                armNow[rank] = armQty;
                // The tracked armour's GRADE, both ways of reading it, so the oracle can
                // see them diverge: aq is the wire's bucket (Item::quality * 100), alv is
                // getLevel() - the craft level the displayed grade comes from. Both -1
                // when this container does not hold the piece.
                int armQual = -1, armLvl = -1;
                if (armSid_[0] && armQty > 0)
                    engine::readGearGradeBySid(ctx.gw, rankHand_[rank], armSid_,
                                               ARMOUR_CAT, &armQual, &armLvl);
                char b[260];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO %s r=%u %s t=%lu count=%u hash=%u probe=%d wpn=%d "
                    "arm=%d aq=%d alv=%d",
                    tag_, rank, (ctx.isHost == (rank == 0)) ? "OWN" : "PEER",
                    (unsigned long)ctx.elapsedMs, n, hash, probeQty, wpnQty,
                    armQty, armQual, armLvl);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                ++samples_;
            }
            // trade_peer conservation tracking; inert in probe mode (emits nothing).
            if (sampledBoth) {
                if (!firstDone_ && wpnLatched_) {
                    firstDone_ = true;
                    firstWpn0_ = wpnNow[0]; firstWpn1_ = wpnNow[1];
                }
                // The armour baseline waits for the seeded piece to EXIST. It is minted
                // mid-run, so a baseline taken at the first sample would read 0 and the
                // conservation check would score the mint itself as a duplication.
                if (!armBaseDone_ && (armNow[0] + armNow[1]) > 0) {
                    armBaseDone_ = true;
                    firstArm0_ = armNow[0]; firstArm1_ = armNow[1];
                }
                lastWpn0_ = wpnNow[0]; lastWpn1_ = wpnNow[1];
                lastArm0_ = armNow[0]; lastArm1_ = armNow[1];
            }

            // Seed material into the container each side OWNS (ordinary, supported
            // single-writer adds - these also prove baseline sync is alive).
            if (!seedDone_ && ctx.elapsedMs >= SEED_MS) {
                seedDone_ = true;
                unsigned int ownRank = ctx.isHost ? 0u : 1u;
                if (rankHave_[ownRank]) {
                    char sid[48]; sid[0] = '\0';
                    int got = engine::addTestItemsToContainer(
                        ctx.gw, rankHand_[ownRank], ctx.isHost ? 2 : 3, sid, sizeof(sid));
                    char m[200];
                    _snprintf(m, sizeof(m) - 1, "SCENARIO %s SEED r=%u n=%d sid='%s'",
                              tag_, ownRank, got, sid[0] ? sid : "(none)");
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
            }

            // The graded reference piece, minted by its OWNER (the join) into rank 1.
            if (peer_ && !ctx.isHost && !armSeeded_ && armLatched_ &&
                ctx.elapsedMs >= ARM_SEED_MS && rankHave_[1]) {
                seedArmour(ctx.gw);
            }

            // The cross-owner drags: HOST only (the "player A" of the field report).
            if (ctx.isHost && probeSid_[0] && rankHave_[0] && rankHave_[1]) {
                if (!takeDone_ && ctx.elapsedMs >= TAKE_MS) {
                    takeDone_ = true;
                    int got = engine::moveItemBetweenContainers(
                        ctx.gw, rankHand_[1], rankHand_[0], probeSid_, probeType_, 1);
                    logMove("TAKE", got, probeSid_);
                }
                if (!giveDone_ && ctx.elapsedMs >= GIVE_MS) {
                    giveDone_ = true;
                    int got = engine::moveItemBetweenContainers(
                        ctx.gw, rankHand_[0], rankHand_[1], probeSid_, probeType_, 1);
                    logMove("GIVE", got, probeSid_);
                }
                if (!wpnDone_ && ctx.elapsedMs >= WPN_MS) {
                    wpnDone_ = true;
                    int got = wpnSid_[0]
                        ? engine::moveItemBetweenContainers(
                              ctx.gw, rankHand_[1], rankHand_[0], wpnSid_, WEAPON_CAT, 1)
                        : -1; // no weapon found in the join-owned container
                    logMove("WTAKE", got, wpnSid_[0] ? wpnSid_ : "(none)");
                }
                if (peer_ && !armDone_ && ctx.elapsedMs >= ARM_MS) {
                    armDone_ = true;
                    int got = armSid_[0]
                        ? engine::moveItemBetweenContainers(
                              ctx.gw, rankHand_[1], rankHand_[0], armSid_, ARMOUR_CAT, 1)
                        : -1; // no reference armour was picked/seeded
                    logMove("ATAKE", got, armSid_[0] ? armSid_ : "(none)");
                }
            }
        }

        unsigned long dur = ctx.isHost ? hostDur_ : joinDur_;
        if (ctx.elapsedMs >= dur) {
            bool executed = rankHave_[0] && rankHave_[1] && samples_ > 0 && seedDone_ &&
                            (!ctx.isHost || (takeDone_ && giveDone_ && wpnDone_ &&
                                             (!peer_ || armDone_)));
            if (!peer_) {
                // trade_probe: verdict = the probe EXECUTED (containers resolved,
                // sampled, and - on the host - all three cross-owner drags fired). The
                // BEHAVIOR it recorded is judged by the runner's evidence report.
                passed_ = executed;
                return true;
            }
            // trade_peer: additionally gate LOCAL weapon conservation - total
            // unchanged (no vanish, no dupe) and, once a weapon was actually tracked,
            // it ended up in rank 0 (moved, not vanished).
            bool wpnOk = true;
            if (firstDone_ && wpnSid_[0]) {
                wpnOk = (lastWpn0_ + lastWpn1_) == (firstWpn0_ + firstWpn1_) &&
                        lastWpn0_ == firstWpn0_ + 1 && lastWpn1_ == firstWpn1_ - 1;
            }
            // Armour CONSERVATION only. Whether the grade survived is a cross-client
            // comparison, and neither client can see the other's alv - that judgement is
            // Test-TradePeer's, from both logs.
            bool armOk = true;
            if (armBaseDone_ && armSid_[0]) {
                armOk = (lastArm0_ + lastArm1_) == (firstArm0_ + firstArm1_) &&
                        lastArm0_ == firstArm0_ + 1 && lastArm1_ == firstArm1_ - 1;
            }
            char m[320];
            _snprintf(m, sizeof(m) - 1,
                "SCENARIO TRDE verdict executed=%d wpnOk=%d wpn r0 %d->%d r1 %d->%d sid='%s'"
                " armOk=%d arm r0 %d->%d r1 %d->%d armSid='%s'",
                executed ? 1 : 0, wpnOk ? 1 : 0, firstWpn0_, lastWpn0_,
                firstWpn1_, lastWpn1_, wpnSid_[0] ? wpnSid_ : "(none)",
                armOk ? 1 : 0, firstArm0_, lastArm0_, firstArm1_, lastArm1_,
                armSid_[0] ? armSid_ : "(none)");
            m[sizeof(m) - 1] = '\0'; coop::logLine(m);
            passed_ = executed && wpnOk && armOk;
            return true;
        }
        return false;
    }

private:
    void latchWeapon(GameWorld* gw) {
        InvItemEntry items[INV_ITEMS_MAX];
        unsigned int hash = 0;
        unsigned int n = engine::captureContainerContents(
            gw, rankHand_[1], items, INV_ITEMS_MAX, &hash);
        if (n == 0) return;          // container not readable yet - retry next sample
        wpnLatched_ = true;          // readable: latch now even if it holds no weapon
        for (unsigned int i = 0; i < n; ++i) {
            if (items[i].itemType != WEAPON_CAT) continue;
            if (!wpnSid_[0] || strcmp(items[i].stringID, wpnSid_) < 0) {
                strncpy(wpnSid_, items[i].stringID, sizeof(wpnSid_) - 1);
                wpnSid_[sizeof(wpnSid_) - 1] = '\0';
                wpnType_ = items[i].itemType;
            }
        }
        char b[160];
        _snprintf(b, sizeof(b) - 1, "SCENARIO %s wpn latched sid='%s'",
                  tag_, wpnSid_[0] ? wpnSid_ : "(none)");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    // The tracked ARMOUR template. NOVEL - held by neither tab - because the grade read
    // matches on (sid, itemType) and returns the first hit, so a pre-existing copy in
    // either container would leave it describing the wrong object. The shared save cannot
    // supply the piece: both starting characters wear the SAME trousers at the SAME grade,
    // which can neither be told apart nor demoted. So the join MINTS one (below) instead.
    // Both clients pick the sid from the shared gamedata in the same enumeration order, as
    // weapon_loot does, and therefore agree on it without exchanging anything.
    void pickArmour(GameWorld* gw) {
        armLatched_ = true;
        if (engine::commonNovelArmourSid(gw, rankHand_[1], rankHand_[0],
                                         armSid_, sizeof(armSid_)) == 0)
            armSid_[0] = '\0';
        char b[200];
        _snprintf(b, sizeof(b) - 1, "SCENARIO %s arm picked sid='%s'",
                  tag_, armSid_[0] ? armSid_ : "(none)");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Put the graded reference piece into the JOIN-owned tab, minted by the JOIN. Two
    // things make this the honest way round:
    //  * OWNERSHIP - rank 1 is the join's, and a host-authored item there would be a
    //    cross-owner write the reconcile is entitled to undo.
    //  * INDEPENDENCE - the mint goes straight to the engine factory at an explicit level,
    //    not through the sync's fabricate path, so it is graded correctly even in a run
    //    with KENSHICOOP_GEAR_LEVEL=0. The host's copy, by contrast, can only arrive
    //    through the inventory snapshot channel - the path under test. If that path drops
    //    the grade, host and join disagree before the drag even happens, and ATAKE then
    //    shows whether the trade itself preserves what each side has.
    void seedArmour(GameWorld* gw) {
        armSeeded_ = true;
        int lvl = -1, q = -1;
        int ok = armSid_[0]
            ? engine::mintGradedGearForTest(gw, rankHand_[1], armSid_, ARMOUR_CAT,
                                            ARM_LEVEL, &lvl, &q)
            : 0;
        char b[240];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO %s arm seeded ok=%d sid='%s' wantLvl=%d gotLvl=%d gotQ=%d",
                  tag_, ok, armSid_[0] ? armSid_ : "(none)", (int)ARM_LEVEL, lvl, q);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    void logMove(const char* what, int got, const char* sid) {
        char m[200];
        _snprintf(m, sizeof(m) - 1, "SCENARIO %s %s n=%d sid='%s'", tag_, what, got, sid);
        m[sizeof(m) - 1] = '\0'; coop::logLine(m);
    }
    // Same squad-tab -> rank partition the Replicator / inv_bidir use.
    static bool resolveRankContainer(GameWorld* gw, unsigned int rank, unsigned int out[5]) {
        for (int i = 0; i < 5; ++i) out[i] = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        if (n == 0) return false;
        int best = -1;
        for (unsigned int i = 0; i < n; ++i) {
            int cr = containerRankOf(sq, n, i);
            if (cr < 0 || (unsigned int)cr != rank) continue;
            if (best < 0 || handLess(sq[i], sq[best])) best = (int)i;
        }
        if (best < 0) return false;
        out[0] = sq[best].hType; out[1] = sq[best].hContainer;
        out[2] = sq[best].hContainerSerial; out[3] = sq[best].hIndex; out[4] = sq[best].hSerial;
        return true;
    }
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    // The last drag fires @36s; the slowest downstream machinery (W2 weapon census
    // 30-tick debounce + 1.8 s removal settle + snapshot travel) lands well inside
    // each mode's window. trade_peer runs ~2 s longer - the owner republish that
    // clears the 10 s reconcile-suppression latch must land - and the host always
    // outlives the join's window.
    static const unsigned long SEED_MS          = 6000;
    static const unsigned long TAKE_MS          = 16000;
    static const unsigned long GIVE_MS          = 26000;
    static const unsigned long WPN_MS           = 36000;
    // The graded reference piece is minted early so it has ~34 s to reach the host through
    // the inventory snapshot channel before the drag. The armour drag itself needs the same
    // downstream settle the weapon one gets, so it sits a full 10 s after WTAKE, and both
    // windows grew by 8 s to keep it. run_test.ps1 gives a named scenario a 150 s backstop,
    // so no manifest change.
    static const unsigned long ARM_SEED_MS      = 12000;
    static const unsigned long ARM_MS           = 46000;
    // Masterwork - the top named grade, and the one the field report lost. Any demotion
    // moves DOWN from here, so a wrong grade cannot coincidentally match.
    static const int           ARM_LEVEL        = 95;

    static const unsigned int  MAX_SQUAD  = 32;
    static const unsigned int  WEAPON_CAT = 2;
    static const unsigned int  ARMOUR_CAT = 3;

    bool          peer_;      // false = trade_probe (baseline), true = trade_peer (xfer)
    const char*   tag_;       // "TRDP" (probe) / "TRDE" (peer) - the oracle log tag
    unsigned long hostDur_;
    unsigned long joinDur_;
    unsigned long lastLogMs_;
    unsigned int  samples_;
    bool          seedDone_;
    bool          takeDone_;
    bool          giveDone_;
    bool          wpnDone_;
    bool          armDone_;
    char          probeSid_[48];
    unsigned int  probeType_;
    char          wpnSid_[48];
    unsigned int  wpnType_;
    bool          wpnLatched_;
    char          armSid_[48];            // trade_peer: the tracked (graded) armour
    bool          armLatched_;            // sid picked
    bool          armSeeded_;             // join minted the reference piece
    bool          armBaseDone_;           // conservation baseline taken (piece exists)
    bool          firstDone_;             // trade_peer: conservation baseline latched
    int           firstWpn0_, firstWpn1_; // trade_peer: tracked-weapon counts at baseline
    int           lastWpn0_,  lastWpn1_;  // trade_peer: latest tracked-weapon counts
    int           firstArm0_, firstArm1_; // trade_peer: tracked-armour counts at baseline
    int           lastArm0_,  lastArm1_;  // trade_peer: latest tracked-armour counts
    bool          rankHave_[2];
    unsigned int  rankHand_[2][5];
};

// xfer_block (cross-owner trade VETO validation): with KENSHICOOP_BLOCK_XFER on, a
// direct squad-to-squad drag between DIFFERENT-owner tabs must be REFUSED at the
// engine (the item is conserved in the source bag), while a SAME-owner drag still
// succeeds. The HOST drives both via engine::moveItemBetweenContainers(...,
// suspendVeto=false) - the exact remove+add a UI drag performs, subject to the veto:
//   GIVE @16s: 1 common  rank0(host) -> rank1(join)          -> BLOCKED (moved=0)
//   SELF @26s: 1 common  rank0.memberA -> rank0.memberB      -> ALLOWED (moved>=1)
// Both clients seed the host tab @6s and sample both tabs every 500 ms. The in-plugin
// verdict gates on: the cross-owner move returned 0 AND the source/dest probe counts
// are UNCHANGED after it (nothing crossed), and - when a second host-tab member exists
// - the same-owner move succeeded. Protocol 37 is retired under the veto, so no
// PKT_INV_XFER is emitted (the runner cross-checks the logs for the absence).
class XferBlockScenario : public TimedScenario {
public:
    XferBlockScenario()
        : TimedScenario("xfer_block", 0), lastLogMs_(0), samples_(0), seedDone_(false),
          giveDone_(false), selfDone_(false), probeType_(0), haveSelfDst_(false),
          giveMoved_(-2), selfMoved_(-2),
          r0BeforeGive_(-1), r1BeforeGive_(-1), r0AfterGive_(-1), r1AfterGive_(-1) {
        probeSid_[0] = '\0';
        for (int r = 0; r < 2; ++r) { rankHave_[r] = false; for (int k = 0; k < 5; ++k) rankHand_[r][k] = 0; }
        for (int k = 0; k < 5; ++k) selfDst_[k] = 0;
    }

    virtual void onStart(const ScenarioContext& ctx) {
        for (unsigned int r = 0; r < 2; ++r)
            rankHave_[r] = resolveRankMember(ctx.gw, r, 0, rankHand_[r]);
        haveSelfDst_ = resolveRankMember(ctx.gw, 0, 1, selfDst_); // 2nd host-tab member (if any)
        engine::commonTestItemSid(ctx.gw, probeSid_, sizeof(probeSid_), &probeType_);
        char b[220];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO XFB anchor host=%d r0=%d r1=%d selfDst=%d probeSid='%s' probeType=%u",
            ctx.isHost ? 1 : 0, rankHave_[0] ? 1 : 0, rankHave_[1] ? 1 : 0,
            haveSelfDst_ ? 1 : 0, probeSid_[0] ? probeSid_ : "(none)", probeType_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            int probe[2] = { -1, -1 };
            for (unsigned int rank = 0; rank < 2; ++rank) {
                if (!rankHave_[rank]) continue;
                InvItemEntry items[INV_ITEMS_MAX]; unsigned int hash = 0;
                unsigned int n = engine::captureContainerContents(
                    ctx.gw, rankHand_[rank], items, INV_ITEMS_MAX, &hash);
                int pq = 0;
                for (unsigned int i = 0; i < n; ++i)
                    if (probeSid_[0] && items[i].itemType == probeType_ &&
                        strcmp(items[i].stringID, probeSid_) == 0)
                        pq += (int)items[i].quantity;
                probe[rank] = pq;
                char b[200];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO XFB r=%u %s t=%lu count=%u hash=%u probe=%d",
                    rank, (ctx.isHost == (rank == 0)) ? "OWN" : "PEER",
                    (unsigned long)ctx.elapsedMs, n, hash, pq);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                ++samples_;
            }

            // Seed material into the container each side OWNS (baseline sync liveness).
            if (!seedDone_ && ctx.elapsedMs >= SEED_MS) {
                seedDone_ = true;
                unsigned int ownRank = ctx.isHost ? 0u : 1u;
                if (rankHave_[ownRank]) {
                    char sid[48]; sid[0] = '\0';
                    int got = engine::addTestItemsToContainer(ctx.gw, rankHand_[ownRank], 3, sid, sizeof(sid));
                    char m[200]; _snprintf(m, sizeof(m) - 1, "SCENARIO XFB SEED r=%u n=%d sid='%s'",
                                           ownRank, got, sid[0] ? sid : "(none)");
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
            }

            // HOST drives the drags AS a UI drag would (subject to the veto).
            if (ctx.isHost && probeSid_[0] && rankHave_[0] && rankHave_[1]) {
                if (!giveDone_ && ctx.elapsedMs >= GIVE_MS) {
                    giveDone_ = true;
                    r0BeforeGive_ = probe[0]; r1BeforeGive_ = probe[1];
                    giveMoved_ = engine::moveItemBetweenContainers(
                        ctx.gw, rankHand_[0], rankHand_[1], probeSid_, probeType_, 1, /*suspendVeto*/false);
                    char m[160]; _snprintf(m, sizeof(m) - 1,
                        "SCENARIO XFB GIVE moved=%d (expect 0=blocked)", giveMoved_);
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
                // Capture post-GIVE counts a sample later (nothing should have crossed).
                if (giveDone_ && r0AfterGive_ < 0 && ctx.elapsedMs >= GIVE_MS + 1000) {
                    r0AfterGive_ = probe[0]; r1AfterGive_ = probe[1];
                }
                if (!selfDone_ && ctx.elapsedMs >= SELF_MS) {
                    selfDone_ = true;
                    selfMoved_ = haveSelfDst_
                        ? engine::moveItemBetweenContainers(
                              ctx.gw, rankHand_[0], selfDst_, probeSid_, probeType_, 1, /*suspendVeto*/false)
                        : -1; // no second host-tab member on this save; sub-check skipped
                    char m[160]; _snprintf(m, sizeof(m) - 1,
                        "SCENARIO XFB SELF moved=%d (expect >=1=allowed; -1=skipped)", selfMoved_);
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            bool executed = rankHave_[0] && rankHave_[1] && samples_ > 0 && seedDone_ &&
                            (!ctx.isHost || (giveDone_ && selfDone_));
            bool blockOk = true, selfOk = true, conserveOk = true;
            if (ctx.isHost) {
                blockOk = (giveMoved_ == 0);                     // cross-owner drag refused
                if (r0AfterGive_ >= 0 && r1AfterGive_ >= 0 && r0BeforeGive_ >= 0)
                    conserveOk = (r0AfterGive_ == r0BeforeGive_) && (r1AfterGive_ == r1BeforeGive_);
                selfOk = !haveSelfDst_ || (selfMoved_ >= 1);     // same-owner drag still works
            }
            char m[220];
            _snprintf(m, sizeof(m) - 1,
                "SCENARIO XFB verdict executed=%d blockOk=%d conserveOk=%d selfOk=%d give=%d self=%d",
                executed ? 1 : 0, blockOk ? 1 : 0, conserveOk ? 1 : 0, selfOk ? 1 : 0,
                giveMoved_, selfMoved_);
            m[sizeof(m) - 1] = '\0'; coop::logLine(m);
            passed_ = executed && blockOk && conserveOk && selfOk;
            return true;
        }
        return false;
    }

private:
    // Resolve the ordinal-th lowest-hand member of the squad TAB with the given rank
    // (the same tab->rank partition the Replicator and the sibling scenarios use).
    static bool resolveRankMember(GameWorld* gw, unsigned int rank, unsigned int ordinal,
                                  unsigned int out[5]) {
        for (int i = 0; i < 5; ++i) out[i] = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        if (n == 0) return false;
        unsigned int idx[MAX_SQUAD]; unsigned int m = 0;
        for (unsigned int i = 0; i < n; ++i) {
            int cr = containerRankOf(sq, n, i);
            if (cr >= 0 && (unsigned int)cr == rank && m < MAX_SQUAD) idx[m++] = i;
        }
        for (unsigned int a = 1; a < m; ++a)
            for (unsigned int b = a; b > 0 && handLess(sq[idx[b]], sq[idx[b-1]]); --b) {
                unsigned int t = idx[b]; idx[b] = idx[b-1]; idx[b-1] = t;
            }
        if (ordinal >= m) return false;
        unsigned int i = idx[ordinal];
        out[0] = sq[i].hType; out[1] = sq[i].hContainer;
        out[2] = sq[i].hContainerSerial; out[3] = sq[i].hIndex; out[4] = sq[i].hSerial;
        return true;
    }
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    static const unsigned long HOST_DURATION_MS = 44000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 34000;
    static const unsigned long SEED_MS          = 6000;
    static const unsigned long GIVE_MS          = 16000; // cross-owner drag (blocked)
    static const unsigned long SELF_MS          = 26000; // same-owner drag (allowed)
    static const unsigned int  MAX_SQUAD        = 32;

    unsigned long lastLogMs_;
    unsigned int  samples_;
    bool          seedDone_;
    bool          giveDone_;
    bool          selfDone_;
    char          probeSid_[48];
    unsigned int  probeType_;
    bool          haveSelfDst_;
    int           giveMoved_;
    int           selfMoved_;
    int           r0BeforeGive_, r1BeforeGive_, r0AfterGive_, r1AfterGive_;
    bool          rankHave_[2];
    unsigned int  rankHand_[2][5];
    unsigned int  selfDst_[5];
};

// inv_equip: EQUIPPED-gear (armour/weapon slot) sync. Each client owns one squad tab
// (host rank 0, join rank 1). On the geared member of its OWN tab it UNEQUIPS one REAL
// (save-loaded) worn item and leaves it off - the "drop/unequip armour" action. Because
// the peer loaded the same save, its local copy of that character starts wearing the
// SAME gear, so converging to "one fewer worn item" forces it to actively REMOVE its
// worn copy; a removal that failed to propagate (loose-only sync's blind spot, the bug
// the user hit) would leave the peer still wearing it - a permanent eq/hash mismatch.
// The snapshot now carries each item's equipped flag + slot. The runner cross-checks,
// per rank, that the author's worn count dropped and the NON-authoring side converged
// to the author's FINAL worn state (count + equipped-count + content hash). Fabricated
// re-equips don't persist in the engine, so the equip (up) path is intentionally out of
// scope here. Requires a shared save with >=2 squad tabs whose members wear gear.
class InventoryEquipScenario : public TimedScenario {
public:
    explicit InventoryEquipScenario(bool reequipMode = false)
        : TimedScenario(reequipMode ? "inv_reequip" : "inv_equip", 0),
          haveOwn_(false), haveEq_(false), unequipped_(false),
          reequipped_(false), reequipMode_(reequipMode),
          lastLogMs_(0), samples_(0), ownRank_(0),
          baseEqCount_(0), baseType_(0), lastOwnEq_(0) {
        for (int i = 0; i < 5; ++i) ownHand_[i] = 0;
        for (int r = 0; r < 2; ++r) { rankHave_[r] = false; for (int i = 0; i < 5; ++i) rankHand_[r][i] = 0; }
        baseSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.isHost ? 0u : 1u;
        // Resolve+cache the geared member of BOTH tabs ONCE (the lead isn't always the
        // geared one). Caching keeps sampling locked to the same character even after we
        // unequip - so the OWN/PEER series track one member, not a shifting "first geared"
        // pick. Both clients share the save, so each rank resolves to the SAME member.
        for (unsigned int r = 0; r < 2; ++r)
            rankHave_[r] = resolveGearedRankContainer(ctx.gw, r, rankHand_[r]);
        haveOwn_ = rankHave_[ownRank_];
        if (haveOwn_) {
            for (int i = 0; i < 5; ++i) ownHand_[i] = rankHand_[ownRank_][i];
            haveEq_ = engine::findEquippedItemKey(ctx.gw, ownHand_, baseSid_, sizeof(baseSid_),
                                                  &baseType_, &baseEqCount_) != 0;
        }
        char b[220];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INVE anchor own_rank=%u have=%d eq=%d baseEq=%d sid='%s' type=%u",
            ownRank_, haveOwn_ ? 1 : 0, haveEq_ ? 1 : 0, baseEqCount_,
            baseSid_[0] ? baseSid_ : "(none)", baseType_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // Sample BOTH squad-tab containers: each client logs its OWN tab (the worn
            // state it streams) and the PEER's (the one it reconciles), with the count
            // of EQUIPPED items broken out so the runner can prove the slot - not just
            // a loose copy - converged.
            for (unsigned int rank = 0; rank < 2; ++rank) {
                if (!rankHave_[rank]) continue;
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int hash = 0;
                unsigned int n = engine::captureContainerContents(
                    ctx.gw, rankHand_[rank], items, INV_ITEMS_MAX, &hash);
                unsigned int eq = 0;
                for (unsigned int i = 0; i < n; ++i) if (items[i].equipped) ++eq;
                const char* role = (rank == ownRank_) ? "OWN" : "PEER";
                if (rank == ownRank_) { ++samples_; lastOwnEq_ = eq; }
                char b[180];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO INVE r=%u %s t=%lu count=%u eq=%u hash=%u",
                    rank, role, (unsigned long)ctx.elapsedMs, n, eq, hash);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }

            // UNEQUIP one REAL worn item from our OWN geared member and leave it off
            // (the "drop/unequip armour" action). Because the peer loaded the SAME save,
            // its local copy of this character starts wearing the SAME gear; to converge
            // to "one fewer worn item" it must actively REMOVE its worn copy. A removal
            // that failed to propagate (the user's bug) would leave the peer still
            // wearing it - a permanent eq/hash mismatch. Fabricated re-equips don't
            // persist in the engine, so we deliberately test only this reliable path.
            if (haveOwn_ && haveEq_) {
                unsigned long unequipAt = reequipMode_ ? RE_UNEQUIP_MS : UNEQUIP_MS;
                if (!unequipped_ && ctx.elapsedMs >= unequipAt) {
                    unequipped_ = true;
                    // inv_equip DESTROYS the worn item (down path, ends reduced). inv_reequip
                    // MOVES it to loose (preserving identity) so it can be re-equipped below -
                    // the faithful "drag worn item into the bag, then back onto the body" cycle.
                    int got = reequipMode_
                        ? engine::unequipItemToLoose(ctx.gw, ownHand_, baseSid_, baseType_, 1)
                        : engine::removeEquippedItem(ctx.gw, ownHand_, baseSid_, baseType_, 1);
                    logStep(ctx, "UNEQUIP", got);
                }
                // RE-EQUIP (up path): equip the REAL loose item we just unequipped. Equipping
                // a real (not fabricated) item persists, so the slot fills back in - and the
                // observer, which down-moved its copy to loose when it saw the unequip, must
                // now UP-move it to converge. A broken up path leaves the observer's copy loose.
                if (reequipMode_ && unequipped_ && !reequipped_ && ctx.elapsedMs >= RE_REEQUIP_MS) {
                    reequipped_ = true;
                    int got = engine::reequipLooseItem(ctx.gw, ownHand_, baseSid_, baseType_, 1);
                    logStep(ctx, "REEQUIP", got);
                }
            }
        }

        unsigned long dur;
        if (reequipMode_) dur = ctx.isHost ? RE_HOST_DURATION_MS : RE_JOIN_DURATION_MS;
        else              dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // In-plugin verdict confirms the scenario EXECUTED. For inv_equip: resolved a
            // geared member, sampled, unequipped. For inv_reequip: additionally re-equipped
            // AND the own worn count returned to its baseline peak (local proof the re-equip
            // of a REAL item PERSISTED - the d25 risk). The runner's per-rank cross-client
            // convergence check is the authoritative gate that the move replicated.
            if (reequipMode_)
                passed_ = haveOwn_ && haveEq_ && (samples_ > 0) && unequipped_ && reequipped_ &&
                          ((int)lastOwnEq_ >= baseEqCount_);
            else
                passed_ = haveOwn_ && haveEq_ && (samples_ > 0) && unequipped_;
            return true;
        }
        return false;
    }

private:
    void logStep(const ScenarioContext& ctx, const char* what, int got) {
        char m[200];
        _snprintf(m, sizeof(m) - 1, "SCENARIO INVE %s r=%u n=%d sid='%s'",
                  what, ownRank_, got, baseSid_[0] ? baseSid_ : "(none)");
        m[sizeof(m) - 1] = '\0'; coop::logLine(m);
        (void)ctx;
    }
    // Same squad-tab -> rank partitioning the Replicator and inv_bidir use, but among the
    // members of `rank`'s tab pick the LOWEST-HAND one that actually WEARS gear (eq >= 1).
    // Both clients share the save, so this resolves to the SAME member on each side - the
    // OWN/PEER comparison stays aligned. Falls back to false if no tab member wears gear.
    static bool resolveGearedRankContainer(GameWorld* gw, unsigned int rank, unsigned int out[5]) {
        for (int i = 0; i < 5; ++i) out[i] = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        if (n == 0) return false;
        // Index list of this rank's members, sorted ascending by hand (deterministic).
        unsigned int idx[MAX_SQUAD]; unsigned int m = 0;
        for (unsigned int i = 0; i < n; ++i) {
            int cr = containerRankOf(sq, n, i);
            if (cr >= 0 && (unsigned int)cr == rank && m < MAX_SQUAD) idx[m++] = i;
        }
        for (unsigned int a = 1; a < m; ++a)
            for (unsigned int b = a; b > 0 && handLess(sq[idx[b]], sq[idx[b-1]]); --b) {
                unsigned int t = idx[b]; idx[b] = idx[b-1]; idx[b-1] = t;
            }
        for (unsigned int a = 0; a < m; ++a) {
            unsigned int h[5] = { sq[idx[a]].hType, sq[idx[a]].hContainer,
                sq[idx[a]].hContainerSerial, sq[idx[a]].hIndex, sq[idx[a]].hSerial };
            char sid[48]; unsigned int ty = 0; int eq = 0;
            if (engine::findEquippedItemKey(gw, h, sid, sizeof(sid), &ty, &eq) && eq >= 1) {
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                return true;
            }
        }
        return false;
    }
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    static const unsigned long HOST_DURATION_MS = 44000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 28000;
    static const unsigned long UNEQUIP_MS       = 8000;  // baseline, then unequip (leave off)

    // inv_reequip cycle (longer): the author UNEQUIPS then RE-EQUIPS, and both actions
    // must land while the OTHER client is alive+synced so the observer can witness the
    // dip+restore (the join only reaches gameplay ~12-20 s into the host's window, so the
    // cycle runs late; the host outlives the join's later own-cycle for the reverse check).
    static const unsigned long RE_HOST_DURATION_MS = 58000;
    static const unsigned long RE_JOIN_DURATION_MS = 42000;
    static const unsigned long RE_UNEQUIP_MS       = 22000; // after the peer is up
    static const unsigned long RE_REEQUIP_MS       = 32000; // hold the dip, then restore

    static const unsigned int  MAX_SQUAD        = 32;

    bool          haveOwn_;
    bool          haveEq_;
    bool          unequipped_;
    bool          reequipped_;
    bool          reequipMode_;
    unsigned long lastLogMs_;
    unsigned int  samples_;
    unsigned int  ownRank_;
    unsigned int  ownHand_[5];
    int           baseEqCount_;
    char          baseSid_[48];
    unsigned int  baseType_;
    unsigned int  lastOwnEq_;
    bool          rankHave_[2];
    unsigned int  rankHand_[2][5];
};

// Local, single-client DIAGNOSTIC: reproduce the manual weapon-drag failure WITHOUT any
// UI by driving the reconcile (engine::applyContainerContents) directly through the exact
// snapshot sequence the join observed - start [weapon EQ + clothes EQ], then a snapshot
// with the weapon LOOSE only, then restore. dumpInventory after each step + the [recon]
// traces inside applyContainerContents show precisely which primitive loses the weapon.
// No network/invSync needed: the scenario's own applyContainerContents calls are the only
// inventory mutation, so the result is deterministic and reproducible from one instance.
class WeaponSeqScenario : public TimedScenario {
public:
    WeaponSeqScenario() : TimedScenario("inv_wpnseq", 0), have_(false), nbase_(0), wIdx_(-1), step_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
    }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = firstGeared(ctx.gw, hand_);
        if (have_) {
            nbase_ = engine::captureContainerContents(ctx.gw, hand_, base_, INV_ITEMS_MAX, 0);
            for (unsigned int i = 0; i < nbase_; ++i)
                if (base_[i].equipped && wIdx_ < 0) wIdx_ = (int)i; // first worn entry = the weapon
        }
        char b[160];
        _snprintf(b, sizeof(b) - 1, "WSEQ start have=%d nbase=%u wIdx=%d wsid='%s' wtype=%u",
                  have_ ? 1 : 0, nbase_, wIdx_,
                  (wIdx_ >= 0) ? base_[wIdx_].stringID : "(none)",
                  (wIdx_ >= 0) ? base_[wIdx_].itemType : 0u);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (have_) { coop::logLine("WSEQ initial-state:"); engine::dumpInventory(ctx.gw, hand_); }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || wIdx_ < 0) { if (ctx.elapsedMs >= 8000) { passed_ = false; return true; } return false; }
        // S1 @ 9s: weapon LOOSE + clothes EQ (clean unequip-to-bag) -> move-down weapon.
        if (step_ == 0 && ctx.elapsedMs >= 9000) {
            step_ = 1;
            InvItemEntry s[INV_ITEMS_MAX]; unsigned int ns = 0;
            for (unsigned int i = 0; i < nbase_; ++i) {
                s[ns] = base_[i];
                if ((int)i == wIdx_) { s[ns].equipped = 0; s[ns].slot = 0; } // weapon loose
                ++ns;
            }
            coop::logLine("WSEQ apply S1=[weapon LOOSE + clothes EQ]");
            engine::applyContainerContents(ctx.gw, hand_, s, ns);
            coop::logLine("WSEQ after-S1:"); engine::dumpInventory(ctx.gw, hand_);
        }
        // S2 @ 12s: weapon FULLY GONE (cursor-held transient) -> destroys the weapon.
        if (step_ == 1 && ctx.elapsedMs >= 12000) {
            step_ = 2;
            InvItemEntry s[INV_ITEMS_MAX]; unsigned int ns = 0;
            for (unsigned int i = 0; i < nbase_; ++i)
                if ((int)i != wIdx_) s[ns++] = base_[i]; // everything EXCEPT the weapon
            coop::logLine("WSEQ apply S2=[weapon GONE]");
            engine::applyContainerContents(ctx.gw, hand_, ns ? s : 0, ns);
            coop::logLine("WSEQ after-S2:"); engine::dumpInventory(ctx.gw, hand_);
        }
        // S3 @ 15s: restore baseline (weapon EQ again) -> only CREATE-EQ available (fabricate).
        if (step_ == 2 && ctx.elapsedMs >= 15000) {
            step_ = 3;
            coop::logLine("WSEQ apply S3=[restore baseline EQ]");
            engine::applyContainerContents(ctx.gw, hand_, base_, nbase_);
            coop::logLine("WSEQ after-S3 (immediate):"); engine::dumpInventory(ctx.gw, hand_);
        }
        // @ 18s: re-dump - did the fabricated equipped weapon SURVIVE several ticks? (d25)
        if (step_ == 3 && ctx.elapsedMs >= 18000) {
            step_ = 4;
            coop::logLine("WSEQ after-S3 (3s later, persistence check):");
            engine::dumpInventory(ctx.gw, hand_);
        }
        if (ctx.elapsedMs >= 20000) { passed_ = (step_ == 4); return true; }
        return false;
    }
private:
    static const unsigned int MAX_SQUAD = 32;
    static bool firstGeared(GameWorld* gw, unsigned int out[5]) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int h[5] = { sq[i].hType, sq[i].hContainer, sq[i].hContainerSerial,
                                  sq[i].hIndex, sq[i].hSerial };
            char sid[48]; unsigned int ty = 0; int eq = 0;
            if (engine::findEquippedItemKey(gw, h, sid, sizeof(sid), &ty, &eq) && eq >= 1) {
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                return true;
            }
        }
        return false;
    }
    bool         have_;
    unsigned int hand_[5];
    InvItemEntry base_[INV_ITEMS_MAX];
    unsigned int nbase_;
    int          wIdx_;
    int          step_;
};

// inv_addequip (LOCAL, single-client, DETERMINISTIC): prove the fix for the "picked-up
// weapon auto-equips into the empty slot, flickers, then VANISHES" bug (d25). When the
// reconcile must ADD an EQUIPPED item the container has NO copy of (curEq=0, curLoose=0),
// the old code fell to fabricate-AND-equip, which the engine's equipment validation
// discards within a tick. applyContainerContents now creates the missing item LOOSE (which
// persists) and equips the now-real loose copy on a LATER reconcile pass - exactly how the
// host re-publishes the same worn snapshot every few seconds. This scenario scripts that
// sequence on one client with NO network: remove a worn item (so there is no copy), then
// re-apply the worn baseline repeatedly, and asserts the slot fills back in AND stays
// filled when we STOP re-applying (the persistence proof the old fabricate path failed).
class InventoryAddEquipScenario : public TimedScenario {
public:
    InventoryAddEquipScenario()
        : TimedScenario("inv_addequip", 0), have_(false), nbase_(0), eIdx_(-1), baseType_(0), baseWorn_(0),
          step_(0), eqAfterCreate_(-1), eqAfterEquip_(-1), eqPersist_(-1) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        baseSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = firstGeared(ctx.gw, hand_);
        if (have_) {
            nbase_ = engine::captureContainerContents(ctx.gw, hand_, base_, INV_ITEMS_MAX, 0);
            // Prefer a NON-WEAPON worn item (armour). WEAPONS cannot currently be rebuilt by
            // the engine factory (createItem returns null for them), so the create-then-equip
            // path under test only applies to reconstructable gear; choosing armour isolates
            // the deferred-equip fix from that separate weapon-factory limitation.
            const unsigned int WEAPON_CAT = 2;
            for (unsigned int pass = 0; pass < 2 && eIdx_ < 0; ++pass)
                for (unsigned int i = 0; i < nbase_; ++i)
                    if (base_[i].equipped && (pass == 1 || base_[i].itemType != WEAPON_CAT)) {
                        eIdx_ = (int)i;
                        strncpy(baseSid_, base_[i].stringID, sizeof(baseSid_) - 1);
                        baseType_ = base_[i].itemType;
                        break;
                    }
            if (eIdx_ >= 0) baseWorn_ = wornCount(ctx.gw);
        }
        char b[200];
        _snprintf(b, sizeof(b) - 1,
            "ADDEQ start have=%d nbase=%u eIdx=%d sid='%s' type=%u baseWorn=%d",
            have_ ? 1 : 0, nbase_, eIdx_, baseSid_[0] ? baseSid_ : "(none)", baseType_, baseWorn_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (have_) { coop::logLine("ADDEQ initial:"); engine::dumpInventory(ctx.gw, hand_); }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || eIdx_ < 0) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }
        // S0 @6s: REMOVE the worn item entirely (apply baseline minus it) -> no copy left.
        if (step_ == 0 && ctx.elapsedMs >= 6000) {
            step_ = 1;
            InvItemEntry s[INV_ITEMS_MAX]; unsigned int ns = 0;
            for (unsigned int i = 0; i < nbase_; ++i) if ((int)i != eIdx_) s[ns++] = base_[i];
            coop::logLine("ADDEQ apply S0=[worn item GONE]");
            engine::applyContainerContents(ctx.gw, hand_, ns ? s : 0, ns);
            coop::logLine("ADDEQ after-S0:"); engine::dumpInventory(ctx.gw, hand_);
            logEq(ctx, "S0-removed", wornCount(ctx.gw));
        }
        // S1 @9s: re-apply baseline (worn DESIRED, no copy present) -> CREATE-LOOSE (deferred);
        //         the worn count may legitimately still be 0 here (that is the whole point).
        if (step_ == 1 && ctx.elapsedMs >= 9000) {
            step_ = 2;
            coop::logLine("ADDEQ apply S1=[restore worn] (expect CREATE-LOOSE, deferred)");
            engine::applyContainerContents(ctx.gw, hand_, base_, nbase_);
            engine::dumpInventory(ctx.gw, hand_);
            eqAfterCreate_ = wornCount(ctx.gw);
            logEq(ctx, "after-create", eqAfterCreate_);
        }
        // S2 @12s: re-apply baseline AGAIN -> MOVE-UP equips the now-real loose copy persistently.
        if (step_ == 2 && ctx.elapsedMs >= 12000) {
            step_ = 3;
            coop::logLine("ADDEQ apply S2=[restore worn] (expect MOVE-UP equip)");
            engine::applyContainerContents(ctx.gw, hand_, base_, nbase_);
            engine::dumpInventory(ctx.gw, hand_);
            eqAfterEquip_ = wornCount(ctx.gw);
            logEq(ctx, "after-equip", eqAfterEquip_);
        }
        // @15s: persistence check WITHOUT re-applying - did the equip SURVIVE several ticks?
        if (step_ == 3 && ctx.elapsedMs >= 15000) {
            step_ = 4;
            coop::logLine("ADDEQ persistence check (no re-apply):");
            engine::dumpInventory(ctx.gw, hand_);
            eqPersist_ = wornCount(ctx.gw);
            logEq(ctx, "persist", eqPersist_);
        }
        if (ctx.elapsedMs >= 18000) {
            // PASS: after the second apply the worn copy returned to its baseline count AND it
            // persisted to the no-reapply check. eqAfterCreate may be 0 (deferred) - allowed.
            passed_ = have_ && (eIdx_ >= 0) && (baseWorn_ >= 1) &&
                      (eqAfterEquip_ >= baseWorn_) && (eqPersist_ >= baseWorn_);
            char b[140];
            _snprintf(b, sizeof(b) - 1,
                "ADDEQ verdict pass=%d baseWorn=%d create=%d equip=%d persist=%d",
                passed_ ? 1 : 0, baseWorn_, eqAfterCreate_, eqAfterEquip_, eqPersist_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }
private:
    static const unsigned int MAX_SQUAD = 32;
    void logEq(const ScenarioContext& ctx, const char* what, int eq) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "ADDEQ eq-%s=%d (sid='%s')", what, eq, baseSid_[0] ? baseSid_ : "(none)");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        (void)ctx;
    }
    int wornCount(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, hand_, it, INV_ITEMS_MAX, 0);
        int c = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (it[i].equipped && it[i].itemType == baseType_ && strcmp(it[i].stringID, baseSid_) == 0) ++c;
        return c;
    }
    static bool firstGeared(GameWorld* gw, unsigned int out[5]) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int h[5] = { sq[i].hType, sq[i].hContainer, sq[i].hContainerSerial,
                                  sq[i].hIndex, sq[i].hSerial };
            char sid[48]; unsigned int ty = 0; int eq = 0;
            if (engine::findEquippedItemKey(gw, h, sid, sizeof(sid), &ty, &eq) && eq >= 1) {
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                return true;
            }
        }
        return false;
    }
    bool         have_;
    unsigned int hand_[5];
    InvItemEntry base_[INV_ITEMS_MAX];
    unsigned int nbase_;
    int          eIdx_;
    char         baseSid_[48];
    unsigned int baseType_;
    int          baseWorn_;
    int          step_;
    int          eqAfterCreate_;
    int          eqAfterEquip_;
    int          eqPersist_;
};

// wpn_relocate (SPIKE for the conservation model): prove that a WEAPON - which the engine
// factory CANNOT fabricate (createItem returns null) - can still be moved bag -> ground ->
// bag by RELOCATING the REAL object, and that it PERSISTS at each step. This validates the
// "don't create, conserve & move" trade model: both clients already own the weapon (shared
// save), so a drop/pickup is a relocation of each side's real copy, never a create/destroy.
// Single-client + deterministic (no network): unequip the weapon to loose, DROP it (real
// ground item), confirm it survives ticks, then PICK IT UP by re-homing the real object.
class WeaponRelocateScenario : public TimedScenario {
public:
    WeaponRelocateScenario()
        : TimedScenario("wpn_relocate", 0), have_(false), baseType_(0), step_(0),
          invBase_(0), invAfterDrop_(-1), grndAfterDrop_(-1), grndPersist_(-1),
          invAfterPick_(-1), grndAfterPick_(-1), invPersist_(-1) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        baseSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = findWeaponHolder(ctx.gw, hand_, baseSid_, sizeof(baseSid_), &baseType_);
        if (have_) invBase_ = invCount(ctx.gw);
        char b[200];
        _snprintf(b, sizeof(b) - 1, "RELOC start have=%d sid='%s' type=%u invBase=%d",
                  have_ ? 1 : 0, baseSid_[0] ? baseSid_ : "(none)", baseType_, invBase_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (have_) { coop::logLine("RELOC initial:"); engine::dumpInventory(ctx.gw, hand_); }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || !baseSid_[0]) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }
        // S0 @6s: ensure the weapon is LOOSE (unequip the real object to the bag; preserves
        // identity). dropItemFromInventory drops loose items only.
        if (step_ == 0 && ctx.elapsedMs >= 6000) {
            step_ = 1;
            int un = engine::unequipItemToLoose(ctx.gw, hand_, baseSid_, baseType_, 1);
            logN(ctx, "unequip", un);
        }
        // S1 @9s: DROP the real weapon -> a free ground item (native Inventory::dropItem; no
        // createItem). Expect inv -1 and a free ground weapon to appear.
        if (step_ == 1 && ctx.elapsedMs >= 9000) {
            step_ = 2;
            int dr = engine::dropItemFromInventory(ctx.gw, hand_, baseSid_, baseType_, 1);
            invAfterDrop_  = invCount(ctx.gw);
            grndAfterDrop_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            char b[120]; _snprintf(b, sizeof(b) - 1, "RELOC after-drop dropped=%d inv=%d ground=%d", dr, invAfterDrop_, grndAfterDrop_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // S1b @12s: the dropped weapon is a REAL persistent object - still on the ground
        // ticks later (a fabricated item would have vanished).
        if (step_ == 2 && ctx.elapsedMs >= 12000) {
            step_ = 3;
            grndPersist_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            logN(ctx, "ground-persist", grndPersist_);
        }
        // S2 @15s: PICK IT UP by relocating the real ground object into the bag (no create).
        if (step_ == 3 && ctx.elapsedMs >= 15000) {
            step_ = 4;
            int pk = engine::pickupWorldItemIntoInventory(ctx.gw, hand_, baseSid_, baseType_, radius());
            invAfterPick_  = invCount(ctx.gw);
            grndAfterPick_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            char b[120]; _snprintf(b, sizeof(b) - 1, "RELOC after-pickup picked=%d inv=%d ground=%d", pk, invAfterPick_, grndAfterPick_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            coop::logLine("RELOC after-pickup dump:"); engine::dumpInventory(ctx.gw, hand_);
        }
        // S2b @18s: the re-homed weapon persists in the bag (real object, no fabrication).
        if (step_ == 4 && ctx.elapsedMs >= 18000) {
            step_ = 5;
            invPersist_ = invCount(ctx.gw);
            logN(ctx, "inv-persist", invPersist_);
        }
        if (ctx.elapsedMs >= 21000) {
            bool dropOk    = (invAfterDrop_ >= 0) && (invAfterDrop_ <= invBase_ - 1) && (grndAfterDrop_ >= 1);
            bool dropHeld  = (grndPersist_ >= 1);
            bool pickOk    = (invAfterPick_ >= invBase_) && (grndAfterPick_ < grndAfterDrop_);
            bool pickHeld  = (invPersist_ >= invBase_);
            passed_ = have_ && (invBase_ >= 1) && dropOk && dropHeld && pickOk && pickHeld;
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "RELOC verdict pass=%d invBase=%d drop(inv=%d grnd=%d held=%d) pick(inv=%d grnd=%d persist=%d)",
                passed_ ? 1 : 0, invBase_, invAfterDrop_, grndAfterDrop_, grndPersist_,
                invAfterPick_, grndAfterPick_, invPersist_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }
private:
    static const unsigned int MAX_SQUAD = 32;
    static float radius() { return 18.0f; } // ground search radius (drops land at feet)

    void logN(const ScenarioContext& ctx, const char* what, int n) {
        char b[100]; _snprintf(b, sizeof(b) - 1, "RELOC %s=%d sid='%s'", what, n, baseSid_[0] ? baseSid_ : "(none)");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b); (void)ctx;
    }
    // Count copies of (baseSid_,baseType_) in the holder's inventory (loose + equipped).
    int invCount(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, hand_, it, INV_ITEMS_MAX, 0);
        int c = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (it[i].itemType == baseType_ && strcmp(it[i].stringID, baseSid_) == 0) ++c;
        return c;
    }
    // Scan squad members for one holding a WEAPON (type==2); prefer an equipped weapon.
    static bool findWeaponHolder(GameWorld* gw, unsigned int out[5], char* outSid,
                                 unsigned int outLen, unsigned int* outType) {
        const unsigned int WEAPON_CAT = 2;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        for (unsigned int pass = 0; pass < 2; ++pass) { // pass0: equipped weapon, pass1: any
            for (unsigned int m = 0; m < n; ++m) {
                unsigned int h[5] = { sq[m].hType, sq[m].hContainer, sq[m].hContainerSerial,
                                      sq[m].hIndex, sq[m].hSerial };
                InvItemEntry it[INV_ITEMS_MAX];
                unsigned int cnt = engine::captureContainerContents(gw, h, it, INV_ITEMS_MAX, 0);
                for (unsigned int i = 0; i < cnt; ++i) {
                    if (it[i].itemType != WEAPON_CAT) continue;
                    if (pass == 0 && !it[i].equipped) continue;
                    for (int k = 0; k < 5; ++k) out[k] = h[k];
                    strncpy(outSid, it[i].stringID, outLen - 1); outSid[outLen - 1] = '\0';
                    if (outType) *outType = it[i].itemType;
                    return true;
                }
            }
        }
        return false;
    }

    bool         have_;
    unsigned int hand_[5];
    char         baseSid_[48];
    unsigned int baseType_;
    int          step_;
    int          invBase_, invAfterDrop_, grndAfterDrop_, grndPersist_;
    int          invAfterPick_, grndAfterPick_, invPersist_;
};

} // namespace

// Shared squad-tab rank helpers for the scenarios below (the older classes in this TU
// each carry their own private copies; new code uses these).
bool ovlCtnrEq(const EntityState& a, const EntityState& b) {
    return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
}
bool ovlHandLess(const EntityState& a, const EntityState& b) {
    if (a.hType != b.hType) return a.hType < b.hType;
    if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
    if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
    if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
    return a.hSerial < b.hSerial;
}
int ovlContainerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
    const unsigned int MAXS = 32;
    EntityState distinct[MAXS]; unsigned int dn = 0;
    for (unsigned int a = 0; a < n; ++a) {
        bool seen = false;
        for (unsigned int b = 0; b < dn; ++b) if (ovlCtnrEq(distinct[b], sq[a])) { seen = true; break; }
        if (!seen && dn < MAXS) distinct[dn++] = sq[a];
    }
    for (unsigned int a = 1; a < dn; ++a)
        for (unsigned int b = a; b > 0 && ovlHandLess(distinct[b], distinct[b-1]); --b) {
            EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
        }
    for (unsigned int r = 0; r < dn; ++r) if (ovlCtnrEq(distinct[r], sq[i])) return (int)r;
    return -1;
}
// Lowest-hand member of the given squad-tab rank; its object hand IS its personal
// inventory container hand (the same key the Replicator partitions and streams on).
bool ovlRankContainer(GameWorld* gw, unsigned int rank, unsigned int out[5]) {
    const unsigned int MAXS = 32;
    for (int i = 0; i < 5; ++i) out[i] = 0;
    EntityState sq[MAXS];
    unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAXS);
    if (n == 0) return false;
    int best = -1;
    for (unsigned int i = 0; i < n; ++i) {
        int cr = ovlContainerRankOf(sq, n, i);
        if (cr < 0 || (unsigned int)cr != rank) continue;
        if (best < 0 || ovlHandLess(sq[i], sq[best])) best = (int)i;
    }
    if (best < 0) return false;
    out[0] = sq[best].hType; out[1] = sq[best].hContainer;
    out[2] = sq[best].hContainerSerial; out[3] = sq[best].hIndex; out[4] = sq[best].hSerial;
    return true;
}

// inv_overflow (protocol 46 regression): the CONTAINER-OVERFLOW item-loss class. A
// character carrying more entries than the wire can describe used to lose everything past
// the cap, because (a) the capture filled its buffer with LOOSE items and emitted zero
// EQUIPPED entries, and (b) the peer read that truncated list as an authoritative delete
// and destroyed the surplus - including the entire worn kit. A backpack is what routinely
// pushes a character over the cap, which is why this reproduced as "backpacks lose items".
//
// Rather than manufacture 65+ distinct templates, this drives the two invariants directly
// through a deliberately SMALL capture cap - the cap is a parameter, so a 2-entry cap on a
// richer container exercises exactly the same code path INV_ITEMS_MAX does:
//   1) GEAR-FIRST: a capture that cannot fit everything still spends its budget on
//      EQUIPPED entries first, so worn gear is never the thing that gets cut.
//      Asserted as eqSmall == min(eqFull, cap).
//   2) TRUNCATION IS FLAGGED and ADDITIVE-ONLY: the overflow sets the truncated flag, and
//      reconciling the container against that short list destroys NOTHING (count before ==
//      count after). Pre-fix this call deleted every entry not in the list.
// Both clients run the check against a container they OWN, so the assertions are local and
// deterministic (no cross-client timing); the INVOF log lines are the oracle contract.
class InventoryOverflowScenario : public TimedScenario {
public:
    InventoryOverflowScenario()
        : TimedScenario("inv_overflow", 0), have_(false), seeded_(false), probed_(false),
          additive_(false), lastLogMs_(0), ownRank_(0),
          nFull_(0), eqFull_(0), nSmall_(0), eqSmall_(0), truncSmall_(0),
          beforeN_(0), afterN_(0), gearOk_(false), truncOk_(false), additiveOk_(false) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        gearSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.isHost ? 0u : 1u;
        have_ = ovlRankContainer(ctx.gw, ownRank_, hand_);
        char b[200];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INVOF anchor own_rank=%u have=%d hand=%u,%u,%u,%u,%u cap=%u",
            ownRank_, have_ ? 1 : 0, hand_[0], hand_[1], hand_[2], hand_[3], hand_[4],
            SMALL_CAP);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }

        // SEED (@5s): guarantee the container holds BOTH worn gear and loose items, so the
        // small-cap capture has a real choice to make between them.
        if (!seeded_ && ctx.elapsedMs >= SEED_MS) {
            seeded_ = true;
            unsigned int gearType = 0; int eqCount = 0;
            int found = engine::findEquippedItemKey(ctx.gw, hand_, gearSid_, sizeof(gearSid_),
                                                    &gearType, &eqCount);
            int worn = eqCount;
            if (found == 0 || eqCount <= 0) {
                // Naked member: give it something to wear so the gear-first check is real.
                if (engine::seedEquippedItem(ctx.gw, hand_, gearSid_, sizeof(gearSid_),
                                             &gearType))
                    worn = 1;
            }
            char sid[48]; sid[0] = '\0';
            int added = engine::addTestItemsToContainer(ctx.gw, hand_, 2, sid, sizeof(sid));
            char b[220]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO INVOF seed worn=%d gearSid='%s' looseAdded=%d looseSid='%s'",
                worn, gearSid_[0] ? gearSid_ : "(none)", added, sid[0] ? sid : "(none)");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // PROBE (@9s): full capture vs small-cap capture.
        if (seeded_ && !probed_ && ctx.elapsedMs >= PROBE_MS) {
            probed_ = true;
            InvItemEntry full[INV_ITEMS_MAX];
            bool tFull = false;
            nFull_ = engine::captureContainerContents(ctx.gw, hand_, full, INV_ITEMS_MAX,
                                                      0, &tFull);
            eqFull_ = 0;
            for (unsigned int i = 0; i < nFull_; ++i) if (full[i].equipped) ++eqFull_;

            bool tSmall = false;
            nSmall_ = engine::captureContainerContents(ctx.gw, hand_, small_, SMALL_CAP,
                                                       0, &tSmall);
            truncSmall_ = tSmall ? 1 : 0;
            eqSmall_ = 0;
            for (unsigned int i = 0; i < nSmall_; ++i) if (small_[i].equipped) ++eqSmall_;

            // Gear-first: the small budget must be spent on equipped entries first.
            unsigned int wantEq = (eqFull_ < SMALL_CAP) ? eqFull_ : SMALL_CAP;
            gearOk_  = (eqSmall_ == wantEq);
            // Overflow must be reported (only meaningful when there IS more than the cap).
            truncOk_ = (nFull_ > SMALL_CAP) ? (truncSmall_ == 1) : (truncSmall_ == 0);

            char b[240]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO INVOF capture full=%u eqFull=%u small=%u eqSmall=%u wantEq=%u "
                "trunc=%d gearOk=%d truncOk=%d",
                nFull_, eqFull_, nSmall_, eqSmall_, wantEq, truncSmall_,
                gearOk_ ? 1 : 0, truncOk_ ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // ADDITIVE (@13s): reconcile against the TRUNCATED short list. Nothing may die.
        if (probed_ && !additive_ && ctx.elapsedMs >= ADDITIVE_MS) {
            additive_ = true;
            InvItemEntry tmp[INV_ITEMS_MAX];
            beforeN_ = engine::captureContainerContents(ctx.gw, hand_, tmp, INV_ITEMS_MAX, 0);
            engine::applyContainerContents(ctx.gw, hand_, nSmall_ ? small_ : 0, nSmall_,
                                           /*truncated*/ true);
            afterN_ = engine::captureContainerContents(ctx.gw, hand_, tmp, INV_ITEMS_MAX, 0);
            additiveOk_ = (afterN_ >= beforeN_);
            char b[220]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO INVOF additive desired=%u before=%u after=%u destroyed=%d ok=%d",
                nSmall_, beforeN_, afterN_,
                (beforeN_ > afterN_) ? (int)(beforeN_ - afterN_) : 0,
                additiveOk_ ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // Periodic content trace so the oracle can see the container never shrinks.
        if (ctx.elapsedMs - lastLogMs_ >= 1000 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            InvItemEntry tmp[INV_ITEMS_MAX];
            unsigned int hash = 0;
            unsigned int n = engine::captureContainerContents(ctx.gw, hand_, tmp,
                                                              INV_ITEMS_MAX, &hash);
            unsigned int eq = 0;
            for (unsigned int i = 0; i < n; ++i) if (tmp[i].equipped) ++eq;
            char b[180]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO INVOF r=%u t=%lu count=%u equipped=%u hash=%u",
                ownRank_, (unsigned long)ctx.elapsedMs, n, eq, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = have_ && probed_ && additive_ && gearOk_ && truncOk_ && additiveOk_;
            char b[240]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO INVOF verdict pass=%d gearOk=%d truncOk=%d additiveOk=%d "
                "full=%u eqFull=%u small=%u eqSmall=%u before=%u after=%u",
                passed_ ? 1 : 0, gearOk_ ? 1 : 0, truncOk_ ? 1 : 0, additiveOk_ ? 1 : 0,
                nFull_, eqFull_, nSmall_, eqSmall_, beforeN_, afterN_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }

private:
    // A 2-entry budget on a container holding gear + loose clutter forces the same
    // decision INV_ITEMS_MAX forces on a backpacked character.
    static const unsigned int  SMALL_CAP    = 2;
    static const unsigned long SEED_MS      = 5000;
    static const unsigned long PROBE_MS     = 9000;
    static const unsigned long ADDITIVE_MS  = 13000;
    static const unsigned long DURATION_MS  = 22000;

    bool          have_, seeded_, probed_, additive_;
    unsigned long lastLogMs_;
    unsigned int  ownRank_;
    unsigned int  hand_[5];
    char          gearSid_[48];
    InvItemEntry  small_[SMALL_CAP];
    unsigned int  nFull_, eqFull_, nSmall_, eqSmall_;
    int           truncSmall_;
    unsigned int  beforeN_, afterN_;
    bool          gearOk_, truncOk_, additiveOk_;
};

// Ground-scan radius for the drop observer (matches the WDROP scenarios).
const float INVDF_RADIUS = 18.0f;

// inv_dropfull (protocol 46 regression): the W2 DROP-vs-SNAPSHOT race that silently lost
// items. The dropper's own bag snapshot (gear already gone) could reach the peer BEFORE the
// drop intent did; the peer's reconcile destroyed its bag copy first, so the late intent's
// relocateWeaponToGround had nothing to move (moved=0) and the gear ended up existing only
// on the dropper's ground. The trigger was the drop intent debouncing its ground
// correlation (the spatial query fails in towns) while the snapshot took the FAST settle
// path - which is what a full inventory guaranteed, since the entry count could not fall
// once it was pinned at the cap.
//
// This drives the race deliberately: the HOST churns its leader's LOOSE inventory
// continuously (forcing the snapshot channel to publish that same container over and over)
// and drops an EQUIPPED gear piece in the middle of that churn. The JOIN - which does not
// own the leader - must still end up with the gear OFF its leader and ON the ground, i.e.
// conserved. The oracle additionally reads the [wd] APPLY lines for moved>0 and asserts no
// APPLY-LOST appears. INVDF log contract.
class InventoryDropFullScenario : public Scenario {
public:
    InventoryDropFullScenario()
        : passed_(false), have_(false), isHost_(false), gearType_(0), step_(0),
          lastChurnMs_(0), churns_(0), invBase_(0), invAfter_(-1), grndAfter_(-1),
          invMin_(9999), grndMax_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        gearSid_[0] = '\0';
    }
    virtual const char* name() const { return "inv_dropfull"; }

    virtual void onStart(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        // Target the host-owned tab-0 member, deterministic on both clients (same save):
        // the host owns it and drops, the join mirrors by conservation.
        have_ = ovlRankContainer(ctx.gw, 0u, hand_);
        if (have_) have_ = pickGear(ctx.gw);
        if (have_) {
            invBase_ = gearCount(ctx.gw);
            invMin_  = invBase_;
            grndMax_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, gearSid_, gearType_,
                                                        INVDF_RADIUS);
        }
        char b[240];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INVDF start role=%s have=%d sid='%s' type=%u invBase=%d "
            "hand=%u,%u,%u,%u,%u",
            isHost_ ? "host" : "join", have_ ? 1 : 0,
            gearSid_[0] ? gearSid_ : "(none)", gearType_, invBase_,
            hand_[0], hand_[1], hand_[2], hand_[3], hand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || !gearSid_[0]) {
            if (ctx.elapsedMs >= 6000) { passed_ = false; return true; }
            return false;
        }

        if (isHost_) {
            // CHURN: keep the inventory snapshot channel busy on the SAME container across
            // the whole drop window, so the drop intent has to compete with it.
            if (ctx.elapsedMs >= CHURN_FROM_MS && ctx.elapsedMs < CHURN_TO_MS &&
                (ctx.elapsedMs - lastChurnMs_ >= CHURN_EVERY_MS)) {
                lastChurnMs_ = ctx.elapsedMs;
                char sid[48]; sid[0] = '\0';
                int n;
                if ((churns_ & 1) == 0) n = engine::addTestItemsToContainer(ctx.gw, hand_, 1,
                                                                           sid, sizeof(sid));
                else                    n = engine::removeTestItemsFromContainer(ctx.gw, hand_, 1);
                ++churns_;
                char b[180]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO INVDF churn i=%u op=%s n=%d",
                    churns_, ((churns_ & 1) == 1) ? "add" : "remove", n);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // DROP (@11s): mid-churn, unequip-to-loose then drop, the real player action.
            if (step_ == 0 && ctx.elapsedMs >= DROP_MS) {
                step_ = 1;
                int dr = engine::dropItemFromInventory(ctx.gw, hand_, gearSid_, gearType_, 1);
                if (dr == 0) {
                    int un = engine::unequipItemToLoose(ctx.gw, hand_, gearSid_, gearType_, 1);
                    if (un > 0) dr = engine::dropItemFromInventory(ctx.gw, hand_, gearSid_,
                                                                   gearType_, 1);
                }
                invAfter_  = gearCount(ctx.gw);
                grndAfter_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, gearSid_,
                                                              gearType_, INVDF_RADIUS);
                char b[200]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO INVDF host-dropped n=%d inv=%d ground=%d churns=%u",
                    dr, invAfter_, grndAfter_, churns_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        } else {
            // OBSERVER: the conservation signature is bag LOSES it and ground GAINS it.
            int inv  = gearCount(ctx.gw);
            int grnd = engine::countFreeGroundItemsNear(ctx.gw, hand_, gearSid_, gearType_,
                                                        INVDF_RADIUS);
            if (inv  < invMin_)  invMin_  = inv;
            if (grnd > grndMax_) grndMax_ = grnd;
        }

        if (ctx.elapsedMs >= DURATION_MS) {
            if (isHost_) {
                bool dropped = (invAfter_ >= 0) && (invAfter_ <= invBase_ - 1);
                passed_ = have_ && (invBase_ >= 1) && dropped && (churns_ > 0);
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO INVDF verdict role=host pass=%d sid='%s' invBase=%d "
                    "invAfter=%d grndAfter=%d churns=%u",
                    passed_ ? 1 : 0, gearSid_, invBase_, invAfter_, grndAfter_, churns_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                bool leftBag  = (invMin_ <= invBase_ - 1);
                bool onGround = (grndMax_ >= 1);
                passed_ = have_ && (invBase_ >= 1) && leftBag && onGround;
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO INVDF verdict role=join pass=%d sid='%s' invBase=%d "
                    "invMin=%d grndMax=%d conserved=%d",
                    passed_ ? 1 : 0, gearSid_, invBase_, invMin_, grndMax_,
                    (leftBag && onGround) ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            return true;
        }
        return false;
    }
    virtual bool passed() const { return passed_; }

private:
    static const unsigned long CHURN_FROM_MS  = 7000;
    static const unsigned long CHURN_TO_MS    = 18000;
    static const unsigned long CHURN_EVERY_MS = 700;
    static const unsigned long DROP_MS        = 11000;
    static const unsigned long DURATION_MS    = 26000;

    int gearCount(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, hand_, it, INV_ITEMS_MAX, 0);
        int c = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (it[i].itemType == gearType_ && strcmp(it[i].stringID, gearSid_) == 0) ++c;
        return c;
    }
    // First GEAR piece (WEAPON=2 preferred, else ARMOUR=3), preferring an EQUIPPED one -
    // equipped gear is the case the reconcile could not refabricate, so it is the one the
    // race actually destroyed. Deterministic across clients (same save).
    bool pickGear(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, hand_, it, INV_ITEMS_MAX, 0);
        const unsigned int cats[2] = { 2u, 3u }; // WEAPON, ARMOUR
        for (unsigned int pass = 0; pass < 2; ++pass)
            for (unsigned int c = 0; c < 2; ++c)
                for (unsigned int i = 0; i < n; ++i) {
                    if (it[i].itemType != cats[c]) continue;
                    if (pass == 0 && !it[i].equipped) continue;
                    strncpy(gearSid_, it[i].stringID, sizeof(gearSid_) - 1);
                    gearSid_[sizeof(gearSid_) - 1] = '\0';
                    gearType_ = it[i].itemType;
                    return true;
                }
        return false;
    }

    bool          passed_, have_, isHost_;
    unsigned int  hand_[5];
    char          gearSid_[48];
    unsigned int  gearType_;
    int           step_;
    unsigned long lastChurnMs_;
    unsigned int  churns_;
    int           invBase_, invAfter_, grndAfter_;
    int           invMin_, grndMax_;
};

// inv_regear (protocol 47 regression): the ONE-INSTANCE invariant across a full W2
// round trip - the author's gear is dropped, the PEER picks it up, and the item must end
// up in exactly ONE place. The reported failure was the other outcome: "join picked the
// items up and had them in inventory, but the host still saw copies on the ground". The
// author's re-home ran once, off a CACHED Item* and with no retry, so any refusal (the
// object had been despawned, or the target bag would not take it) left the item in the
// peer's bag AND on the author's ground - a duplicate, permanently, because nothing
// revisited the decision.
//
// HOST owns tab 0 and drops one of its gear pieces. The JOIN relocates its own copy by
// conservation, then picks that ground item up into the tab-1 member it OWNS (so its gear
// census authors a real PICKUP intent naming the shared drop identity). The HOST then has
// to converge: its ground copy must be GONE and its copy of the tab-1 bag must HOLD the
// item. Asserting both is what makes this an invariant rather than two one-sided checks -
// ground=0 alone is satisfied by losing the item, bag=1 alone by duplicating it.
// WGRP log contract; judged by Test-GearRepickup.
class InventoryRegearScenario : public Scenario {
public:
    explicit InventoryRegearScenario(const char* scenarioName)
        : passed_(false), have_(false), isHost_(false), gearType_(0), step_(0),
          dropped_(0), pickedUp_(0), lastTryMs_(0), tries_(0), lastLogMs_(0),
          grndPeak_(0), grndFinal_(-1), bagFinal_(-1), bagBase_(0),
          scenarioName_(scenarioName) {
        for (int i = 0; i < 5; ++i) { r0_[i] = 0; r1_[i] = 0; }
        gearSid_[0] = '\0';
    }
    virtual const char* name() const { return scenarioName_; }

    virtual void onStart(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        // Both tabs on BOTH clients: the host drops from tab 0 and must later read tab 1's
        // bag (the container it does NOT own) to prove the item landed there.
        have_ = ovlRankContainer(ctx.gw, 0u, r0_) && ovlRankContainer(ctx.gw, 1u, r1_);
        if (have_) have_ = pickGear(ctx.gw);
        // Tab 1 may already carry the same template (squad members share starting kit), so
        // the invariant is measured as a DELTA of exactly +1 - "at least one" would accept a
        // second copy appearing inside the bag.
        if (have_) bagBase_ = gearCount(ctx.gw, r1_);
        char b[260]; _snprintf(b, sizeof(b) - 1,
            "WGRP start role=%s have=%d sid='%s' type=%u bagBase=%d r0=%u,%u,%u,%u,%u r1=%u,%u,%u,%u,%u",
            isHost_ ? "host" : "join", have_ ? 1 : 0, gearSid_[0] ? gearSid_ : "(none)",
            gearType_, bagBase_, r0_[0], r0_[1], r0_[2], r0_[3], r0_[4],
            r1_[0], r1_[1], r1_[2], r1_[3], r1_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || !gearSid_[0]) {
            if (ctx.elapsedMs >= 6000) { passed_ = false; return true; }
            return false;
        }

        if (isHost_ && step_ == 0 && ctx.elapsedMs >= DROP_MS) {
            step_ = 1;
            dropped_ = engine::dropItemFromInventory(ctx.gw, r0_, gearSid_, gearType_, 1);
            if (dropped_ == 0) {
                int un = engine::unequipItemToLoose(ctx.gw, r0_, gearSid_, gearType_, 1);
                if (un > 0) dropped_ = engine::dropItemFromInventory(ctx.gw, r0_, gearSid_,
                                                                    gearType_, 1);
                if (dropped_ == 0)
                    dropped_ = engine::dropItemFromInventory(ctx.gw, r0_, gearSid_, gearType_,
                                                             1, 0, /*allowEquipped*/ true);
            }
            char b[180]; _snprintf(b, sizeof(b) - 1,
                "WGRP host-dropped n=%d ground=%d", dropped_,
                engine::countFreeGroundItemsNear(ctx.gw, r0_, gearSid_, gearType_, RADIUS));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // JOIN picks the relocated item up into the member it OWNS. Retried: the peer's
        // conservation relocation has to land first, and it is debounced.
        if (!isHost_ && pickedUp_ == 0 && ctx.elapsedMs >= PICKUP_MS &&
            tries_ < MAX_TRIES && (ctx.elapsedMs - lastTryMs_ >= TRY_EVERY_MS)) {
            lastTryMs_ = ctx.elapsedMs;
            ++tries_;
            pickedUp_ = engine::pickupWorldItemIntoInventory(ctx.gw, r1_, gearSid_,
                                                             gearType_, RADIUS);
            char b[180]; _snprintf(b, sizeof(b) - 1,
                "WGRP join-pickup try=%u got=%d ground=%d", tries_, pickedUp_,
                engine::countFreeGroundItemsNear(ctx.gw, r1_, gearSid_, gearType_, RADIUS));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // The AUTHOR's series is the evidence: its ground count must rise on the drop and
        // come back to zero once the peer's pickup is honoured.
        if (isHost_ && (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0)) {
            lastLogMs_ = ctx.elapsedMs;
            int grnd = engine::countFreeGroundItemsNear(ctx.gw, r0_, gearSid_, gearType_, RADIUS);
            if (grnd > grndPeak_) grndPeak_ = grnd;
            char b[180]; _snprintf(b, sizeof(b) - 1,
                "WGRP host t=%lu ground=%d peak=%d bagR1=%d",
                (unsigned long)ctx.elapsedMs, grnd, grndPeak_, gearCount(ctx.gw, r1_));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        unsigned long dur = isHost_ ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            bagFinal_ = gearCount(ctx.gw, r1_);
            if (isHost_) {
                grndFinal_ = engine::countFreeGroundItemsNear(ctx.gw, r0_, gearSid_,
                                                              gearType_, RADIUS);
                passed_ = (dropped_ > 0) && (grndPeak_ >= 1) && (grndFinal_ == 0) &&
                          (bagFinal_ == bagBase_ + 1);
                char b[260]; _snprintf(b, sizeof(b) - 1,
                    "WGRP verdict role=host pass=%d sid='%s' dropped=%d grndPeak=%d "
                    "grndFinal=%d bagR1=%d bagBase=%d",
                    passed_ ? 1 : 0, gearSid_, dropped_, grndPeak_, grndFinal_, bagFinal_,
                    bagBase_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                passed_ = (pickedUp_ > 0) && (bagFinal_ == bagBase_ + 1);
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "WGRP verdict role=join pass=%d sid='%s' pickedUp=%d tries=%u bagR1=%d bagBase=%d",
                    passed_ ? 1 : 0, gearSid_, pickedUp_, tries_, bagFinal_, bagBase_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            return true;
        }
        return false;
    }
    virtual bool passed() const { return passed_; }

private:
    static const unsigned long DROP_MS          = 8000;
    static const unsigned long PICKUP_MS        = 16000;
    static const unsigned long TRY_EVERY_MS     = 1500;
    static const unsigned int  MAX_TRIES        = 5;
    static const unsigned long JOIN_DURATION_MS = 30000;
    // Outlive the join by the author's full re-home window (WD_REHOME_MAX_MS) plus slack:
    // a refused re-home is retried for that long, and only then may the author retire its
    // ground copy against the bag it can now read.
    static const unsigned long HOST_DURATION_MS = 40000;
    static const float         RADIUS;

    int gearCount(GameWorld* gw, const unsigned int cHand[5]) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, cHand, it, INV_ITEMS_MAX, 0);
        int c = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (it[i].itemType == gearType_ && strcmp(it[i].stringID, gearSid_) == 0) ++c;
        return c;
    }
    // Tab 0's first WEAPON or ARMOUR piece, equipped preferred - deliberately NOT a
    // container: a backpack is unfabricable by design, so a refused re-home of one has no
    // convergent end state to assert (the author keeps its ground copy rather than risk
    // destroying the only instance).
    bool pickGear(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, r0_, it, INV_ITEMS_MAX, 0);
        const unsigned int cats[2] = { 2u, 3u };
        for (unsigned int pass = 0; pass < 2; ++pass)
            for (unsigned int c = 0; c < 2; ++c)
                for (unsigned int i = 0; i < n; ++i) {
                    if (it[i].itemType != cats[c]) continue;
                    if (pass == 0 && !it[i].equipped) continue;
                    strncpy(gearSid_, it[i].stringID, sizeof(gearSid_) - 1);
                    gearSid_[sizeof(gearSid_) - 1] = '\0';
                    gearType_ = it[i].itemType;
                    return true;
                }
        return false;
    }

    bool          passed_, have_, isHost_;
    unsigned int  r0_[5], r1_[5];
    char          gearSid_[48];
    unsigned int  gearType_;
    int           step_;
    int           dropped_, pickedUp_;
    unsigned long lastTryMs_;
    unsigned int  tries_;
    unsigned long lastLogMs_;
    int           grndPeak_, grndFinal_, bagFinal_, bagBase_;
    const char*   scenarioName_;
};
const float InventoryRegearScenario::RADIUS = 60.0f;

// inv_nested_bag (protocol 48): items placed INSIDE worn containers must land inside the PEER's
// copy of the RIGHT container. A worn bag owns a private Inventory that no section of the
// wearer's own inventory mentions, so a bagged item used to be described by no snapshot at all:
// it existed for its author and nowhere else, and a bag that travelled arrived without its
// contents. Two properties are asserted, because each one hides a different bug:
//
//  1) PLACE, not presence. The item must be IN the bag - landing it loose on the character
//     would satisfy "the peer has it" while leaving the bag empty for the next relocation.
//  2) The right bag, and only that bag - PER BAG, with a different quantity in each. Bags of
//     the same template are indistinguishable by (sid,type), and the apply side used to point
//     every parent's contents at whichever copy it read first: the second apply reconciled away
//     what the first had just placed and the other bag was never reconciled at all. Comparing
//     the per-bag deltas as a SORTED multiset catches that collapse without needing the two
//     clients to agree on carry order.
//
//     The run TRIES to arrange two same-template bags (each client mints its own, since a
//     fabricated container never replicates - an empty bag would swallow the contents it stood
//     in for). Kenshi refuses: a character's loose storage IS its worn backpack, so a spare bag
//     would be a bag inside a bag ("[mk] tryAddItem-fail ... type=46"). The multi-bag assertion
//     therefore engages only where the engine allows several containers in one inventory; on a
//     one-bag carrier the run still asserts (1), and `bags` in the verdict records which case
//     was actually under test rather than leaving it to be guessed.
//
// Everything is measured as a DELTA against each side's own baseline: the probe is a common
// template the fixture's bag may already hold, so an absolute count would pass on the save's
// own contents with nothing having crossed the wire.
class InvNestedBagScenario : public Scenario {
public:
    InvNestedBagScenario()
        : passed_(false), have_(false), isHost_(false), type_(0), bagType_(0), step_(0),
          bags_(0), minted_(0), lastLogMs_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        sid_[0] = '\0'; bagSid_[0] = '\0';
        for (int i = 0; i < BAGS_MAX; ++i) { base_[i] = -1; cur_[i] = -1; added_[i] = 0; }
    }

    virtual const char* name() const { return "inv_nested_bag"; }

    virtual void onStart(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        // The bag carrier: whichever squad member actually carries a container. Both clients
        // load the same save, so they resolve the SAME character and the same bag.
        have_ = findBagCarrier(ctx.gw, hand_, bagSid_, sizeof(bagSid_), &bagType_);
        engine::commonTestItemSid(ctx.gw, sid_, sizeof(sid_), &type_);
        // TRY to mint a second container LOCALLY on both clients (each side arranging its own is
        // the only way to put two identical bags in play, since a fabricated container never
        // replicates). Kenshi normally refuses - a spare bag inside the worn bag - and then the
        // run asserts the one-bag case; `minted` records which way it went.
        if (have_ && bagSid_[0]) {
            minted_ = engine::addItemsToContainerBySid(ctx.gw, hand_, bagSid_, bagType_, 1, 0, "", "");
        }
        bags_ = have_ ? engine::nestedContainerCount(ctx.gw, hand_) : -1;
        if (bags_ > BAGS_MAX) bags_ = BAGS_MAX;
        for (int b = 0; b < bags_; ++b)
            base_[b] = engine::countInNestedContainer(ctx.gw, hand_, sid_, type_, (unsigned int)b);
        char d[64]; distStr(base_, d, sizeof(d));
        char b[300];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO NEST anchor host=%d have=%d hand=%u,%u,%u,%u,%u sid='%s' type=%u "
            "bag='%s' bagType=%u minted=%d bags=%d base='%s'",
            isHost_ ? 1 : 0, have_ ? 1 : 0, hand_[0], hand_[1], hand_[2], hand_[3], hand_[4],
            sid_[0] ? sid_ : "(none)", type_, bagSid_[0] ? bagSid_ : "(none)", bagType_,
            minted_, bags_, d);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // HOST @8s: fill the two bags with DIFFERENT quantities. Late enough that both sides
        // have seeded their inventory publishers, so this is a real edge rather than part of
        // the initial convergence; different quantities make a collapse into one bag visible.
        if (isHost_ && ready() && step_ == 0 && ctx.elapsedMs >= 8000) {
            step_ = 1;
            for (int b = 0; b < bags_ && b < (int)(sizeof(WANT) / sizeof(WANT[0])); ++b)
                added_[b] = engine::addItemToNestedContainer(ctx.gw, hand_, sid_, type_,
                                                             WANT[b], (unsigned int)b);
            char d[64]; distStr(added_, d, sizeof(d));
            char b[220];
            _snprintf(b, sizeof(b) - 1, "SCENARIO NEST ADD sid='%s' type=%u added='%s'",
                      sid_, type_, d);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (ready() && (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0)) {
            lastLogMs_ = ctx.elapsedMs;
            for (int b = 0; b < bags_; ++b)
                cur_[b] = engine::countInNestedContainer(ctx.gw, hand_, sid_, type_, (unsigned int)b);
            char d[64]; distStr(cur_, d, sizeof(d));
            char b[220];
            _snprintf(b, sizeof(b) - 1, "SCENARIO NEST %s t=%lu inBags='%s' bags=%d",
                      isHost_ ? "HOST" : "JOIN", (unsigned long)ctx.elapsedMs, d, bags_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        unsigned long dur = isHost_ ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // The per-bag deltas, SORTED: a multiset comparison needs no agreement between the
            // clients about which bag is which, but still catches both groups landing in one.
            int dl[BAGS_MAX];
            int total = 0;
            for (int b = 0; b < bags_; ++b) {
                dl[b] = (cur_[b] >= 0 && base_[b] >= 0) ? (cur_[b] - base_[b]) : -999;
                total += dl[b];
            }
            sortAsc(dl, bags_);
            char dd[64]; distStrN(dl, bags_, dd, sizeof(dd));
            char wd[64];
            { int w[BAGS_MAX]; for (int b = 0; b < bags_; ++b) w[b] = WANT[b];
              sortAsc(w, bags_); distStrN(w, bags_, wd, sizeof(wd)); }
            bool converged = ready() && (strcmp(dd, wd) == 0);
            // The host must also prove it performed every placement: a failed setup would
            // otherwise leave both sides equal and read as convergence.
            bool authored = true;
            for (int b = 0; b < bags_; ++b) if (added_[b] != WANT[b]) authored = false;
            passed_ = isHost_ ? (converged && authored) : converged;
            char cd[64]; distStr(cur_, cd, sizeof(cd));
            char bd[64]; distStr(base_, bd, sizeof(bd));
            char b[340];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO NEST verdict role=%s pass=%d sid='%s' bags=%d minted=%d want='%s' "
                "base='%s' inBags='%s' delta='%s' total=%d",
                isHost_ ? "host" : "join", passed_ ? 1 : 0, sid_[0] ? sid_ : "(none)",
                bags_, minted_, wd, bd, cd, dd, total);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    enum { BAGS_MAX = 4 };
    static const int           WANT[2];          // per-bag placements (deliberately unequal)
    static const unsigned long HOST_DURATION_MS = 30000;
    static const unsigned long JOIN_DURATION_MS = 28000;

    // At least one container and a probe template to put in it, or there is nothing to assert.
    bool ready() const { return have_ && sid_[0] && bags_ >= 1; }

    static void sortAsc(int* a, int n) {
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && a[j] < a[j - 1]; --j) { int t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; }
    }
    void distStr(const int* a, char* out, unsigned int len) const { distStrN(a, bags_, out, len); }
    static void distStrN(const int* a, int n, char* out, unsigned int len) {
        out[0] = '\0';
        for (int i = 0; i < n; ++i) {
            char one[24];
            _snprintf(one, sizeof(one) - 1, (i == 0) ? "%d" : ",%d", a[i]);
            one[sizeof(one) - 1] = '\0';
            if (strlen(out) + strlen(one) + 1 >= len) break;
            strcat(out, one);
        }
    }

    // First player character carrying a CONTAINER, plus that container's template. Both
    // clients walk the squad in the same order from the same save, so they agree on it.
    static bool findBagCarrier(GameWorld* gw, unsigned int out[5], char* outSid,
                               unsigned int outLen, unsigned int* outType) {
        EntityState sq[32];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, 32);
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int h[5] = { sq[i].hType, sq[i].hContainer, sq[i].hContainerSerial,
                                  sq[i].hIndex, sq[i].hSerial };
            InvItemEntry ent[INV_ITEMS_MAX];
            unsigned int ni = engine::captureContainerContents(gw, h, ent, INV_ITEMS_MAX, 0);
            for (unsigned int k = 0; k < ni; ++k) {
                if (!engine::isContainerItemType(ent[k].itemType)) continue;
                for (int j = 0; j < 5; ++j) out[j] = h[j];
                strncpy(outSid, ent[k].stringID, outLen - 1); outSid[outLen - 1] = '\0';
                if (outType) *outType = ent[k].itemType;
                return true;
            }
        }
        return false;
    }

    bool          passed_, have_, isHost_;
    unsigned int  type_, bagType_;
    int           step_;
    int           bags_, minted_;
    int           base_[BAGS_MAX], cur_[BAGS_MAX], added_[BAGS_MAX];
    unsigned long lastLogMs_;
    unsigned int  hand_[5];
    char          sid_[48];
    char          bagSid_[48];
};
const int InvNestedBagScenario::WANT[2] = { 2, 1 };

// inv_dump_all: the player's ACTUAL workflow, which is a burst rather than the one-item round
// trip every other gear gate exercises - dump a character's ENTIRE kit on the ground at once and
// hoover all of it up with a second character. That difference is what the previous gates missed,
// in three ways at once:
//
//  1) MANY drops in one tick. Each authored drop mints a ground track, and a track was retired
//     after WD_DEAD_READS_MAX *engine ticks* - about 25 ms at the main loop's ~100-125 Hz. A
//     player's session showed a track pruned 29 ms after its own drop; the pickup that arrived
//     32 s later was answered "untracked" and the author's copy stayed on the ground forever.
//     Asserting the author's ground count returns to ZERO is what catches that leftover.
//
//  2) ONE character receives everything, so its grid fills and the engine stows the tail INSIDE
//     the worn backpack. A bagged item is invisible to the top-level-only search the drop mirror
//     uses, so the mirror concluded it had no copy and FABRICATED one (APPLY-HEALED) - the real
//     item stayed in the bag and a minted duplicate landed on the ground. Counting the picker's
//     holdings WITH nested contents is the only way to see the items at all, and the re-drop leg
//     below is what forces the mirror to reach into the bag.
//
//  3) CONSERVATION over the whole burst, not per item: totals on both sides must add up. A gate
//     that checks one item cannot see a burst lose its tail or duplicate its head.
//
// Every count is a DELTA against each side's own baseline - the kit templates are ordinary gear
// the receiving character may already carry, so absolute counts would pass on the save's own
// contents with nothing having crossed the wire.
class InvDumpAllScenario : public Scenario {
public:
    explicit InvDumpAllScenario(const char* scenarioName)
        : scenarioName_(scenarioName),
          passed_(false), have_(false), isHost_(false), nKit_(0), wantTotal_(0), minted_(0),
          dropped_(0), pickedUp_(0), reDropped_(0), grndPeak_(0), grndFinal_(-1), heldFinal_(-1),
          lastTryMs_(0), tries_(0), lastLogMs_(0), step_(0) {
        for (int i = 0; i < 5; ++i) { r0_[i] = 0; r1_[i] = 0; }
        for (int i = 0; i < KIT_MAX; ++i) baseOf_[i] = 0;
    }
    virtual const char* name() const { return scenarioName_; }

    virtual void onStart(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        have_ = ovlRankContainer(ctx.gw, 0u, r0_) && ovlRankContainer(ctx.gw, 1u, r1_);
        // BOTH sides enumerate rank 0's kit from their own copy: the fixture is shared, so the
        // host learns what to drop and the join learns what to hunt for without a side channel.
        if (have_) have_ = readKit(ctx.gw);
        // The fixture's own kit is a couple of pieces - a trickle, which is what every existing
        // gear gate already covers. Mint extra copies of the FIRST template so the dump is a real
        // BURST, and same-template copies at that: several indistinguishable instances in flight
        // at once is precisely what the drop identity has to keep straight.
        if (have_) {
            for (int i = 0; i < nKit_; ++i) baseOf_[i] = heldOf(ctx.gw, i);
            // Both clients mint their own copies: a fabricated item does not replicate, and the
            // conservation ledger below is evaluated per side against that side's own count.
            int mi = mintable();
            if (mi >= 0) {
                // Into the worn BACKPACK, not the character's own grid: a character has two
                // weapon slots and Kenshi refuses the rest ("[mk] tryAddItem-fail ... type=2"),
                // so top-level minting cannot build a burst at all. The bag is also where the
                // player's own items sit, and dumping from it is what exercises the mirror's
                // reach into a carried container - the reach whose absence fabricated duplicates.
                minted_ = engine::addItemToNestedContainer(ctx.gw, r0_, kit_[mi].sid,
                                                           kit_[mi].type, BURST_EXTRA, 0);
                if (minted_ == 0)
                    minted_ = engine::addItemsToContainerBySid(ctx.gw, r0_, kit_[mi].sid,
                                                               kit_[mi].type, BURST_EXTRA,
                                                               kit_[mi].quality,
                                                               kit_[mi].manufacturer,
                                                               kit_[mi].material);
                if (minted_ > 0) kit_[mi].want += minted_;
            }
        }
        for (int i = 0; i < nKit_; ++i) wantTotal_ += kit_[i].want;
        char b[260]; _snprintf(b, sizeof(b) - 1,
            "WDMP start role=%s have=%d kit=%d want=%d minted=%d r0=%u,%u,%u,%u,%u "
            "r1=%u,%u,%u,%u,%u", isHost_ ? "host" : "join", have_ ? 1 : 0, nKit_, wantTotal_,
            minted_, r0_[0], r0_[1], r0_[2], r0_[3], r0_[4], r1_[0], r1_[1], r1_[2], r1_[3], r1_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || nKit_ == 0) {
            if (ctx.elapsedMs >= 6000) { passed_ = false; return true; }
            return false;
        }

        // ---- Leg 1: the host dumps the WHOLE kit in a single tick ----------------------
        if (isHost_ && step_ == 0 && ctx.elapsedMs >= DROP_MS) {
            step_ = 1;
            for (int i = 0; i < nKit_; ++i)
                for (int c = 0; c < kit_[i].want; ++c) {
                    int d = engine::dropItemFromInventory(ctx.gw, r0_, kit_[i].sid, kit_[i].type, 1);
                    if (d == 0) {
                        int un = engine::unequipItemToLoose(ctx.gw, r0_, kit_[i].sid,
                                                            kit_[i].type, 1);
                        if (un > 0)
                            d = engine::dropItemFromInventory(ctx.gw, r0_, kit_[i].sid,
                                                              kit_[i].type, 1);
                        if (d == 0)
                            d = engine::dropItemFromInventory(ctx.gw, r0_, kit_[i].sid,
                                                              kit_[i].type, 1, 0,
                                                              /*allowEquipped*/ true);
                        // The burst lives in the worn bag (see onStart), which no top-level drop
                        // can reach - the same blind spot the drop mirror had.
                        if (d == 0)
                            d = engine::dropItemFromNestedContainer(ctx.gw, r0_, kit_[i].sid,
                                                                    kit_[i].type, 1);
                    }
                    kit_[i].dropped += d; dropped_ += d;
                }
            char b[180]; _snprintf(b, sizeof(b) - 1,
                "WDMP host-dumped kit=%d want=%d dropped=%d ground=%d", nKit_, wantTotal_,
                dropped_, groundCount(ctx.gw));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // ---- Leg 2: the JOIN hoovers all of it into ONE character ----------------------
        // Retried per template: the peer's conservation relocation is debounced, and a burst
        // arrives over several ticks.
        if (!isHost_ && pickedUp_ < wantTotal_ && ctx.elapsedMs >= PICKUP_MS &&
            tries_ < MAX_TRIES && (ctx.elapsedMs - lastTryMs_ >= TRY_EVERY_MS)) {
            lastTryMs_ = ctx.elapsedMs; ++tries_;
            for (int i = 0; i < nKit_; ++i)
                while (kit_[i].got < kit_[i].want) {
                    if (engine::pickupWorldItemIntoInventory(ctx.gw, r1_, kit_[i].sid,
                                                             kit_[i].type, RADIUS) <= 0) break;
                    ++kit_[i].got; ++pickedUp_;
                }
            char b[200]; _snprintf(b, sizeof(b) - 1,
                "WDMP join-hoover try=%u got=%d/%d held=%d", tries_, pickedUp_, wantTotal_,
                heldCount(ctx.gw));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // ---- Leg 3: the JOIN re-drops one item, which by now may live in the backpack ---
        // This is what forces the drop MIRROR to reach into a carried container instead of
        // fabricating: the grid is full after leg 2, so the engine stowed the tail in the bag.
        if (!isHost_ && step_ == 0 && pickedUp_ > 0 && ctx.elapsedMs >= REDROP_MS) {
            step_ = 1;
            int last = -1;
            for (int i = 0; i < nKit_; ++i) if (kit_[i].got > 0) last = i;
            if (last >= 0) {
                reDropped_ = engine::dropItemFromInventory(ctx.gw, r1_, kit_[last].sid,
                                                           kit_[last].type, 1, 0,
                                                           /*allowEquipped*/ true);
                if (reDropped_ == 0)
                    reDropped_ = engine::dropItemFromNestedContainer(ctx.gw, r1_, kit_[last].sid,
                                                                     kit_[last].type, 1);
                char b[200]; _snprintf(b, sizeof(b) - 1,
                    "WDMP join-redrop sid='%s' type=%u n=%d", kit_[last].sid, kit_[last].type,
                    reDropped_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // The AUTHOR's series is the evidence for the leftover: its ground total must rise with
        // the dump and come back to zero once every pickup is honoured.
        if (isHost_ && (ctx.elapsedMs - lastLogMs_ >= 1000 || lastLogMs_ == 0)) {
            lastLogMs_ = ctx.elapsedMs;
            int grnd = groundCount(ctx.gw);
            if (grnd > grndPeak_) grndPeak_ = grnd;
            char b[180]; _snprintf(b, sizeof(b) - 1,
                "WDMP host t=%lu ground=%d peak=%d heldR1=%d",
                (unsigned long)ctx.elapsedMs, grnd, grndPeak_, heldCount(ctx.gw));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        unsigned long dur = isHost_ ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // CONSERVATION, per template and per SIDE. Every instance the host dumped must exist
            // exactly once on this client - either still on the ground or in the picker's bag.
            // Stating it that way is what makes the assertion hold no matter how much of the
            // burst the harness's own pickup managed to take, and it catches both failures the
            // player reported with one equality: a SURPLUS is the duplicate (the peer holds it
            // and our ground copy was never retired, or a mirror fabricated a second one), and a
            // DEFICIT is the item that never arrived.
            char dist[200]; dist[0] = '\0';
            bool conserved = true;
            for (int i = 0; i < nKit_; ++i) {
                int g = engine::countFreeGroundItemsNear(ctx.gw, r0_, kit_[i].sid, kit_[i].type,
                                                         RADIUS);
                int b = heldOf(ctx.gw, i) - baseOf_[i];
                if (g + b != kit_[i].want) conserved = false;
                char one[48]; _snprintf(one, sizeof(one) - 1, "%s%d+%d/%d", i ? "," : "", g, b,
                                        kit_[i].want);
                one[sizeof(one) - 1] = '\0';
                strncat(dist, one, sizeof(dist) - strlen(dist) - 1);
            }
            grndFinal_ = groundCount(ctx.gw);
            heldFinal_ = heldCount(ctx.gw);
            if (isHost_) {
                passed_ = (dropped_ == wantTotal_) && (grndPeak_ >= wantTotal_) && conserved;
                char b[300]; _snprintf(b, sizeof(b) - 1,
                    "WDMP verdict role=host pass=%d kit=%d want=%d dropped=%d grndPeak=%d "
                    "grndFinal=%d heldR1=%d conserved=%d dist='%s'", passed_ ? 1 : 0, nKit_,
                    wantTotal_, dropped_, grndPeak_, grndFinal_, heldFinal_, conserved ? 1 : 0,
                    dist);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                // The harness's own pickup is best-effort (an armour slot can be occupied, a grid
                // full), so requiring the WHOLE burst would make the gate flaky about something
                // other than replication. It only has to move enough of it to have signal; the
                // conservation equality above is the actual invariant.
                int need = (wantTotal_ >= 3) ? MIN_PICKED : 1;
                passed_ = (pickedUp_ >= need) && conserved;
                char b[300]; _snprintf(b, sizeof(b) - 1,
                    "WDMP verdict role=join pass=%d kit=%d want=%d pickedUp=%d tries=%u "
                    "reDropped=%d grndFinal=%d held=%d conserved=%d dist='%s'", passed_ ? 1 : 0,
                    nKit_, wantTotal_, pickedUp_, tries_, reDropped_, grndFinal_, heldFinal_,
                    conserved ? 1 : 0, dist);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            return true;
        }
        return false;
    }
    virtual bool passed() const { return passed_; }

private:
    static const unsigned long DROP_MS           = 8000;
    static const unsigned long PICKUP_MS         = 14000;
    static const unsigned long REDROP_MS         = 30000;
    static const unsigned long TRY_EVERY_MS      = 1500;
    static const unsigned int  MAX_TRIES         = 12;
    static const unsigned long JOIN_DURATION_MS  = 46000;
    // Outlive the join by the author's full re-home + parked-pickup window (WD_REHOME_MAX_MS)
    // plus slack, so a deferred pickup has actually run out of retries by the verdict.
    static const unsigned long HOST_DURATION_MS  = 58000;
    static const int           KIT_MAX           = 16;
    // Extra same-template copies minted onto the dumping character, so the dump is a burst rather
    // than the trickle the fixture's own kit provides.
    static const int           BURST_EXTRA       = 5;
    // Enough of the burst moved to give the conservation equality something to say.
    static const int           MIN_PICKED        = 2;
    static const float         RADIUS;

    // manufacturer/material are carried because the engine factory REQUIRES the manufacturer to
    // build a weapon at all - minting the burst with an empty one silently adds nothing.
    struct KitItem {
        char sid[48]; unsigned int type; int quality;
        char manufacturer[48]; char material[48];
        int want; int dropped; int got;
    };

    // Tab 0's CONSERVED kit (weapons/armour/containers ride the W2 relocation channel, which is
    // the channel under test). One entry per distinct instance, capped: the point is a burst, and
    // KIT_MAX is already well past the batch sizes that used to starve.
    bool readKit(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, r0_, it, INV_ITEMS_MAX, 0);
        for (unsigned int i = 0; i < n && nKit_ < KIT_MAX; ++i) {
            if (!engine::isConservedItemType(it[i].itemType)) continue;
            // A container must not be dumped here: fabricating one is forbidden by design, so a
            // bag that fails to relocate has no convergent end state to assert.
            if (engine::isContainerItemType(it[i].itemType)) continue;
            // Same template twice in the kit would double-count in the conservation sums.
            bool dup = false;
            for (int k = 0; k < nKit_ && !dup; ++k)
                if (kit_[k].type == it[i].itemType &&
                    strcmp(kit_[k].sid, it[i].stringID) == 0) { ++kit_[k].want; dup = true; }
            if (dup) continue;
            KitItem& k = kit_[nKit_++];
            strncpy(k.sid, it[i].stringID, sizeof(k.sid) - 1);
            k.sid[sizeof(k.sid) - 1] = '\0';
            strncpy(k.manufacturer, it[i].manufacturer, sizeof(k.manufacturer) - 1);
            k.manufacturer[sizeof(k.manufacturer) - 1] = '\0';
            strncpy(k.material, it[i].material, sizeof(k.material) - 1);
            k.material[sizeof(k.material) - 1] = '\0';
            k.type = it[i].itemType; k.quality = (int)it[i].quality;
            k.want = 1; k.dropped = 0; k.got = 0;
        }
        return nKit_ > 0;
    }

    // Which kit template to mint the burst from. ARMOUR first: a character has two weapon slots
    // and both its own grid and its worn bag refuse a third weapon ("[mk] tryAddItem-fail
    // ... type=2"), so a weapon simply cannot be multiplied. A weapon is the fallback and needs
    // its manufacturer GameData or the factory returns null. Returns -1 when nothing qualifies,
    // in which case the run still exercises the fixture's own (small) kit and says so.
    int mintable() {
        for (int i = 0; i < nKit_; ++i) if (kit_[i].type == 3u) return i;
        for (int i = 0; i < nKit_; ++i)
            if (kit_[i].type != 2u || kit_[i].manufacturer[0] != '\0') return i;
        return -1;
    }

    // What the RECEIVING character holds of kit template `i`, INCLUDING what its carried
    // containers hold - after a bulk pickup the tail of the burst lives in the backpack, so a
    // top-level count reads a successful transfer as a loss.
    int heldOf(GameWorld* gw, int i) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, r1_, it, INV_ITEMS_MAX, 0, 0,
                                                          /*includeNested=*/true);
        int c = 0;
        for (unsigned int j = 0; j < n; ++j) {
            if (it[j].itemType != kit_[i].type) continue;
            if (strcmp(it[j].stringID, kit_[i].sid) != 0) continue;
            int q = it[j].quantity; if (q < 1) q = 1;
            c += q;
        }
        return c;
    }

    int heldCount(GameWorld* gw) {
        int c = 0;
        for (int i = 0; i < nKit_; ++i) c += heldOf(gw, i);
        return c;
    }

    // Free ground items of the kit templates near the DROP site (rank 0's character).
    int groundCount(GameWorld* gw) {
        int c = 0;
        for (int k = 0; k < nKit_; ++k)
            c += engine::countFreeGroundItemsNear(gw, r0_, kit_[k].sid, kit_[k].type, RADIUS);
        return c;
    }

    const char*   scenarioName_;
    bool          passed_, have_, isHost_;
    unsigned int  r0_[5], r1_[5];
    KitItem       kit_[KIT_MAX];
    int           baseOf_[KIT_MAX];
    int           nKit_, wantTotal_, minted_;
    int           dropped_, pickedUp_, reDropped_;
    int           grndPeak_, grndFinal_, heldFinal_;
    unsigned long lastTryMs_;
    unsigned int  tries_;
    unsigned long lastLogMs_;
    int           step_;
};
const float InvDumpAllScenario::RADIUS = 60.0f;

// xbow_grade (upstream #41): a CROSSBOW keeps its craft GRADE across a drop + peer
// pickup. Crossbows are itemType CROSSBOW (107), a SEPARATE type from WEAPON (2);
// before the conservation-channel fix they fell into the grade-less W1 template
// stream, so the peer rebuilt them at the factory-default grade. The HOST mints a
// reference crossbow at a distinctive grade into its own tab (mintGradedGearForTest
// bypasses the sync, so the reference is graded correctly even on the unfixed build),
// then drops it; the JOIN relocates/fabricates it by conservation and picks it up
// into the tab it OWNS. Both read the grade back by (sid, itemType). The single-log
// legs prove the round trip converged (host ground -> 0, join picked up one); the
// CROSS-CLIENT grade equality (join alv == host minted level) is Test-CrossbowGrade's
// job, since neither client can see the other's reading. XBOWG log contract.
class CrossbowGradeScenario : public Scenario {
public:
    CrossbowGradeScenario()
        : passed_(false), isHost_(false), have_(false), step_(0),
          seeded_(false), dropped_(0), pickedUp_(0), tries_(0),
          lastTryMs_(0), lastLogMs_(0), grndPeak_(0), grndFinal_(-1),
          mintedLevel_(-1), finalBucket_(-1), finalLevel_(-1) {
        for (int i = 0; i < 5; ++i) { r0_[i] = 0; r1_[i] = 0; }
        xbowSid_[0] = '\0';
    }
    virtual const char* name() const { return "xbow_grade"; }

    virtual void onStart(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        have_ = ovlRankContainer(ctx.gw, 0u, r0_) && ovlRankContainer(ctx.gw, 1u, r1_);
        // Pick a crossbow template neither tab already carries (so the grade read is
        // unambiguous). Same save -> same deterministic pick on both clients.
        if (have_) {
            if (!engine::commonNovelCrossbowSid(ctx.gw, r0_, xbowSid_, sizeof(xbowSid_)))
                xbowSid_[0] = '\0';
        }
        char b[240]; _snprintf(b, sizeof(b) - 1,
            "XBOWG start role=%s have=%d sid='%s' r0=%u,%u,%u,%u,%u r1=%u,%u,%u,%u,%u",
            isHost_ ? "host" : "join", have_ ? 1 : 0, xbowSid_[0] ? xbowSid_ : "(none)",
            r0_[0], r0_[1], r0_[2], r0_[3], r0_[4], r1_[0], r1_[1], r1_[2], r1_[3], r1_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || !xbowSid_[0]) {
            if (ctx.elapsedMs >= 6000) { passed_ = false; return true; }
            return false;
        }

        // HOST mints the graded reference crossbow into the tab it owns (rank 0).
        if (isHost_ && !seeded_ && ctx.elapsedMs >= SEED_MS) {
            seeded_ = true;
            int q = -1;
            engine::mintGradedGearForTest(ctx.gw, r0_, xbowSid_, XBOW_CAT, MINT_LEVEL,
                                          &mintedLevel_, &q);
            char b[200]; _snprintf(b, sizeof(b) - 1,
                "XBOWG mint sid='%s' wantLevel=%d gotLevel=%d gotQual=%d",
                xbowSid_, MINT_LEVEL, mintedLevel_, q);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // HOST drops it (through the hooked path, so the conservation channel authors it).
        if (isHost_ && step_ == 0 && ctx.elapsedMs >= DROP_MS && mintedLevel_ > 0) {
            step_ = 1;
            dropped_ = engine::dropItemFromInventory(ctx.gw, r0_, xbowSid_, XBOW_CAT, 1);
            if (dropped_ == 0) {
                int un = engine::unequipItemToLoose(ctx.gw, r0_, xbowSid_, XBOW_CAT, 1);
                if (un > 0) dropped_ = engine::dropItemFromInventory(ctx.gw, r0_, xbowSid_,
                                                                    XBOW_CAT, 1);
                if (dropped_ == 0)
                    dropped_ = engine::dropItemFromInventory(ctx.gw, r0_, xbowSid_, XBOW_CAT,
                                                             1, 0, /*allowEquipped*/ true);
            }
            char b[180]; _snprintf(b, sizeof(b) - 1,
                "XBOWG host-dropped n=%d ground=%d", dropped_,
                engine::countFreeGroundItemsNear(ctx.gw, r0_, xbowSid_, XBOW_CAT, RADIUS));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // JOIN picks the conserved crossbow up into the member it OWNS (rank 1). Retried:
        // the peer's relocate/fabricate has to land first.
        if (!isHost_ && pickedUp_ == 0 && ctx.elapsedMs >= PICKUP_MS &&
            tries_ < MAX_TRIES && (ctx.elapsedMs - lastTryMs_ >= TRY_EVERY_MS)) {
            lastTryMs_ = ctx.elapsedMs;
            ++tries_;
            pickedUp_ = engine::pickupWorldItemIntoInventory(ctx.gw, r1_, xbowSid_,
                                                             XBOW_CAT, RADIUS);
            char b[180]; _snprintf(b, sizeof(b) - 1,
                "XBOWG join-pickup try=%u got=%d ground=%d", tries_, pickedUp_,
                engine::countFreeGroundItemsNear(ctx.gw, r1_, xbowSid_, XBOW_CAT, RADIUS));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (isHost_ && (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0)) {
            lastLogMs_ = ctx.elapsedMs;
            int grnd = engine::countFreeGroundItemsNear(ctx.gw, r0_, xbowSid_, XBOW_CAT, RADIUS);
            if (grnd > grndPeak_) grndPeak_ = grnd;
        }

        unsigned long dur = isHost_ ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (isHost_) {
                grndFinal_ = engine::countFreeGroundItemsNear(ctx.gw, r0_, xbowSid_,
                                                              XBOW_CAT, RADIUS);
                // Host proves it authored a graded drop and its ground copy is gone. The
                // grade the host MINTED is logged so the oracle can compare it to the
                // join's post-transfer reading (grade preservation is the point).
                passed_ = (dropped_ > 0) && (mintedLevel_ > 0) && (grndPeak_ >= 1) &&
                          (grndFinal_ == 0);
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "XBOWG verdict role=host pass=%d sid='%s' mintLevel=%d dropped=%d "
                    "grndPeak=%d grndFinal=%d",
                    passed_ ? 1 : 0, xbowSid_, mintedLevel_, dropped_, grndPeak_, grndFinal_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                engine::readGearGradeBySid(ctx.gw, r1_, xbowSid_, XBOW_CAT,
                                           &finalBucket_, &finalLevel_);
                // Join proves it received the crossbow (picked it up) AND that its craft
                // grade survived the transfer (finalLevel > 0, i.e. not GRADE_NA/default).
                // The exact cross-client equality is the oracle's gate.
                passed_ = (pickedUp_ > 0) && (finalLevel_ > 0);
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "XBOWG verdict role=join pass=%d sid='%s' pickedUp=%d aq=%d alv=%d tries=%u",
                    passed_ ? 1 : 0, xbowSid_, pickedUp_, finalBucket_, finalLevel_, tries_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned int  XBOW_CAT   = 107;   // itemType CROSSBOW
    static const int           MINT_LEVEL = 90;    // distinctive craft grade
    static const unsigned long SEED_MS    = 5000;
    static const unsigned long DROP_MS    = 12000;
    static const unsigned long PICKUP_MS  = 20000;
    static const unsigned long TRY_EVERY_MS = 1500;
    static const unsigned int  MAX_TRIES  = 6;
    static const unsigned long JOIN_DURATION_MS = 34000;
    static const unsigned long HOST_DURATION_MS = 40000;
    static const float         RADIUS;

    bool          passed_;
    bool          isHost_;
    bool          have_;
    int           step_;
    bool          seeded_;
    int           dropped_;
    int           pickedUp_;
    unsigned int  tries_;
    unsigned long lastTryMs_;
    unsigned long lastLogMs_;
    int           grndPeak_;
    int           grndFinal_;
    int           mintedLevel_;
    int           finalBucket_;
    int           finalLevel_;
    unsigned int  r0_[5];
    unsigned int  r1_[5];
    char          xbowSid_[48];
};
const float CrossbowGradeScenario::RADIUS = 60.0f;

// corpse_loot (upstream #40): looting a CORPSE syncs to the peer. A corpse is a
// dead Character whose inventory was in no published set (the container census only
// scanned BUILDING-typed storage), so a looter's gain synced but the corpse's loss
// never did - the peer kept seeing the pre-loot corpse. Both clients kill the same
// save-native world NPC (deterministic pick; a save-native hand resolves on both) so
// each holds a corpse at the same hand; the HOST seeds a known count into the corpse
// and then loots one. The corpse census (host-authoritative) must carry both the
// seed and the loot to the join. Item TOTALS (qtyTotal) are used, so no cross-client
// sid agreement is needed. Both verdicts carry the subject hand so a determinism
// failure (the two sides pinned different NPCs) is told apart from the real bug.
// XCORPSE log contract; judged by Test-CorpseLoot.
class CorpseLootScenario : public Scenario {
public:
    CorpseLootScenario()
        : passed_(false), isHost_(false), have_(false), killed_(false),
          seeded_(0), looted_(0), base_(-1), peak_(-1), final_(-1), lastLogMs_(0) {
        for (int i = 0; i < 5; ++i) subjHand_[i] = 0;
        seedSid_[0] = '\0';
    }
    virtual const char* name() const { return "corpse_loot"; }

    // Pin EARLY (at gameplay start) on BOTH clients: by the time the scenario clock arms
    // (~40 s later, on peer-ready) the pick could drift. The subject must be the SAME
    // save-native NPC on each side, so the pick is GLOBALLY deterministic rather than
    // leader-relative: among the world (non-squad) NPCs captureNpcs sees, take the one
    // with the smallest hand (serial, then index). Both co-located clients capture the
    // same set and pick the same NPC - no dependence on which leader is nearer. (A
    // leader-relative picker like pickDuelSubjects lands on different NPCs per client.)
    void pinSubj(const ScenarioContext& ctx) {
        if (have_) return;
        EntityState npcs[64];
        unsigned int n = engine::captureNpcs(ctx.gw, npcs, 64);
        int best = -1;
        for (unsigned int i = 0; i < n; ++i) {
            if (best < 0 ||
                npcs[i].hSerial < npcs[best].hSerial ||
                (npcs[i].hSerial == npcs[best].hSerial && npcs[i].hIndex < npcs[best].hIndex))
                best = (int)i;
        }
        if (best >= 0) {
            subjHand_[0] = npcs[best].hType; subjHand_[1] = npcs[best].hContainer;
            subjHand_[2] = npcs[best].hContainerSerial; subjHand_[3] = npcs[best].hIndex;
            subjHand_[4] = npcs[best].hSerial;
            have_ = true;
        }
    }

    virtual void onGameplay(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        pinSubj(ctx);
    }

    virtual void onStart(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        pinSubj(ctx);
        char b[180]; _snprintf(b, sizeof(b) - 1,
            "XCORPSE start role=%s have=%d subj=%u,%u,%u,%u,%u",
            isHost_ ? "host" : "join", have_ ? 1 : 0,
            subjHand_[0], subjHand_[1], subjHand_[2], subjHand_[3], subjHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Keep trying to pin until the kill window: the pair has to be resolvable at least
        // once before we can make a corpse of it.
        if (!have_ && ctx.elapsedMs < KILL_MS) { pinSubj(ctx); }
        if (!have_) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }

        // BOTH kill their local copy of the same save-native NPC, so each holds a
        // corpse at the shared hand independent of the death-event channel's timing.
        if (!killed_ && ctx.elapsedMs >= KILL_MS) {
            killed_ = true;
            engine::killSubject(ctx.gw, subjHand_);
            base_ = corpseQty(ctx.gw);
            char b[160]; _snprintf(b, sizeof(b) - 1,
                "XCORPSE killed role=%s base=%d", isHost_ ? "host" : "join", base_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // HOST seeds a known count into the corpse, then loots one of them.
        if (isHost_ && killed_ && seeded_ == 0 && ctx.elapsedMs >= SEED_MS) {
            seeded_ = engine::addTestItemsToContainer(ctx.gw, subjHand_, SEED_N,
                                                      seedSid_, sizeof(seedSid_));
            char b[180]; _snprintf(b, sizeof(b) - 1,
                "XCORPSE seed n=%d sid='%s' corpseQty=%d",
                seeded_, seedSid_[0] ? seedSid_ : "(none)", corpseQty(ctx.gw));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (isHost_ && seeded_ > 0 && looted_ == 0 && ctx.elapsedMs >= LOOT_MS) {
            looted_ = engine::removeTestItemsFromContainer(ctx.gw, subjHand_, 1);
            char b[160]; _snprintf(b, sizeof(b) - 1,
                "XCORPSE loot n=%d corpseQty=%d", looted_, corpseQty(ctx.gw));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            int q = corpseQty(ctx.gw);
            if (q > peak_) peak_ = q;
        }

        unsigned long dur = isHost_ ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            final_ = corpseQty(ctx.gw);
            if (isHost_) {
                passed_ = killed_ && (seeded_ >= SEED_N) && (looted_ >= 1) &&
                          (base_ >= 0) && (final_ == base_ + SEED_N - 1);
                char b[220]; _snprintf(b, sizeof(b) - 1,
                    "XCORPSE verdict role=host pass=%d subj=%u,%u,%u,%u,%u base=%d seeded=%d "
                    "looted=%d peak=%d final=%d",
                    passed_ ? 1 : 0, subjHand_[0], subjHand_[1], subjHand_[2], subjHand_[3],
                    subjHand_[4], base_, seeded_, looted_, peak_, final_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                // The join must have SEEN the corpse gain the seed (peak >= base+SEED_N)
                // and then lose the looted one (final == base+SEED_N-1). Before the fix the
                // corpse was never censused, so the join's corpse stayed at its native base.
                passed_ = killed_ && (base_ >= 0) && (peak_ >= base_ + SEED_N) &&
                          (final_ == base_ + SEED_N - 1);
                char b[220]; _snprintf(b, sizeof(b) - 1,
                    "XCORPSE verdict role=join pass=%d subj=%u,%u,%u,%u,%u base=%d peak=%d final=%d",
                    passed_ ? 1 : 0, subjHand_[0], subjHand_[1], subjHand_[2], subjHand_[3],
                    subjHand_[4], base_, peak_, final_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long KILL_MS = 4000;
    static const unsigned long SEED_MS = 9000;
    static const unsigned long LOOT_MS = 20000;
    static const int           SEED_N  = 3;
    static const unsigned long JOIN_DURATION_MS = 34000;
    static const unsigned long HOST_DURATION_MS = 40000;

    int corpseQty(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, subjHand_, it, INV_ITEMS_MAX, 0);
        int q = 0;
        for (unsigned int i = 0; i < n; ++i) q += (int)it[i].quantity;
        return q;
    }

    bool          passed_;
    bool          isHost_;
    bool          have_;
    bool          killed_;
    int           seeded_;
    int           looted_;
    int           base_;
    int           peak_;
    int           final_;
    unsigned long lastLogMs_;
    unsigned int  subjHand_[5];
    char          seedSid_[48];
};

Scenario* makeInventoryScenario(const std::string& name) {
    // Same scenario twice: the plain run proves the round trip converges, and the
    // _refuse run drives it with the first re-home refused (KENSHICOOP_WD_REFUSE_REHOME),
    // which is the only deterministic way to exercise the retry + verify-then-destroy
    // recovery - the path whose absence turned a refusal into a permanent duplicate.
    // _refuse_all refuses EVERY attempt, so the only way out is the verify-then-destroy
    // branch: the item reaches the bag over the snapshot channel and the author then
    // retires its ground copy against a bag it can actually read.
    if (name == "inv_regear")            return new InventoryRegearScenario("inv_regear");
    if (name == "inv_regear_refuse")     return new InventoryRegearScenario("inv_regear_refuse");
    if (name == "inv_regear_refuse_all") return new InventoryRegearScenario("inv_regear_refuse_all");
    // _forget discards the author's ground track the instant it is made, so the peer's pickup
    // arrives NAMED but unmatchable. That is the state the player's session was in when a
    // picked-up item stayed on the other side's ground, and the only exit is the site-anchored
    // recovery (re-home a same-sid free ground item at the pickup location).
    if (name == "inv_regear_forget")      return new InventoryRegearScenario("inv_regear_forget");
    if (name == "inv_nested_bag") return new InvNestedBagScenario();
    // The burst the one-item gates above cannot express: a whole kit dumped at once and hoovered
    // up by a single character, which is how the player drives it and where the tick-denominated
    // track retirement and the top-level-only drop mirror both showed.
    if (name == "inv_dump_all")   return new InvDumpAllScenario("inv_dump_all");
    // Same burst with the author's ground tracks discarded the instant they are made
    // (KENSHICOOP_WD_FORGET_TRACK). That is the state the tick-denominated retirement put a real
    // session into, and it is the only deterministic way to gate what happens next: the author
    // must PARK the peer's identified pickup and keep trying, rather than answer it once and
    // leave its own copy on the ground for the rest of the session.
    if (name == "inv_dump_all_forget") return new InvDumpAllScenario("inv_dump_all_forget");
    // ...and the same burst where the author's first pickup-time read of the object reports it
    // gone and later reads succeed (KENSHICOOP_WD_TRANSIENT_DEAD). A verdict drawn from that one
    // read is the duplicate; a retry converges.
    if (name == "inv_dump_all_transient") return new InvDumpAllScenario("inv_dump_all_transient");
    if (name == "inv_order")    return new InventorySyncScenario();
    if (name == "inv_overflow") return new InventoryOverflowScenario();
    if (name == "inv_dropfull") return new InventoryDropFullScenario();
    if (name == "inv_bidir")    return new InventoryBidirScenario();
    if (name == "trade_probe")  return new TradeScenario(/*peer=*/false);
    if (name == "trade_peer")   return new TradeScenario(/*peer=*/true);
    if (name == "xfer_block")   return new XferBlockScenario();
    if (name == "inv_equip")    return new InventoryEquipScenario(/*reequip=*/false);
    if (name == "inv_reequip")  return new InventoryEquipScenario(/*reequip=*/true);
    if (name == "inv_wpnseq")   return new WeaponSeqScenario();
    if (name == "inv_addequip") return new InventoryAddEquipScenario();
    if (name == "wpn_relocate") return new WeaponRelocateScenario();
    if (name == "xbow_grade")   return new CrossbowGradeScenario();
    if (name == "corpse_loot")  return new CorpseLootScenario();
    return 0;
}

} // namespace coop
