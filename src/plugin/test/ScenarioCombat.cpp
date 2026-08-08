// ScenarioCombat.cpp - combat scenarios (monolith split from Scenario.cpp,
// 2026-07-12): combat_probe, combat_order, combat_kill, player_combat,
// assault_town, assault_travel, player_ko, combat_crowd. Classes are TU-private (anonymous
// namespace); only makeCombatScenario (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"
#include <cstdlib>  // getenv/atoi - combat_battle size (KENSHICOOP_BATTLE_N)

namespace coop {
namespace {


// combat_probe (Phase 3c, L5 READ probe): the HOST spawns two mutually-hostile
// non-squad NPCs in front of the leader and orders them to melee each other, then
// logs a "COMBAT ..." line per duelist each tick (inCombat/ranged/underMelee/
// fleeing/target). No wire, no apply - this validates that the combat-state
// primitives populate during a live fight before we replicate them. The duelists are
// runtime host spawns (not baked), so the join only logs whatever NPCs it has; the
// host log is the deliverable. Periodically re-arms disengaged duelists.
class CombatProbeScenario : public TimedScenario {
public:
    CombatProbeScenario()
        : TimedScenario("combat_probe", 0), recvCount_(0), lastLogMs_(0), lastRearmMs_(0),
          haveDuel_(false) {}

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            haveDuel_ = engine::setupDuelScene(ctx.gw);
            // setupDuelScene now spawns PEACEFUL; the probe wants a live fight, so
            // trigger it immediately (the onTick re-arm keeps it going if they disengage).
            if (haveDuel_) engine::startDuel(ctx.gw);
            coop::logLine(haveDuel_ ? "SCENARIO duel spawned"
                                    : "SCENARIO duel spawn FAILED");
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // HOST: keep disengaged duelists fighting, and log their combat state.
        if (ctx.isHost && haveDuel_) {
            if (ctx.elapsedMs - lastRearmMs_ >= 2500) {
                lastRearmMs_ = ctx.elapsedMs;
                engine::rearmDuelScene(ctx.gw);
            }
            if (ctx.elapsedMs - lastLogMs_ >= 700 || lastLogMs_ == 0) {
                lastLogMs_ = ctx.elapsedMs;
                engine::logDuelCombat(ctx.gw);
            }
        }

        // Both: position log (anchors the screenshot + gives the join a RECV signal).
        if (!ctx.isHost) {
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity("RECV", npcs[i]);
            if (n > 0) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? haveDuel_ : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    static const unsigned long HOST_DURATION_MS = 40000;
    static const unsigned long JOIN_DURATION_MS = 28000;
    static const unsigned int  MAX_LOG          = 40;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastRearmMs_;
    bool          haveDuel_;
};

// combat_order (Phase 3c, L5 LIVE transition): the payoff test. Both clients load the
// baked 'duel1' (two PEACEFUL, co-located non-squad NPCs present on both). The host
// pins them, HOLDS them peaceful for a baseline window, then at ORDER_AT_MS issues the
// LIVE attack order (startDuel). The host captures task==TASK_COMBAT_MELEE for them and
// streams it; the join reproduces the intent (orders its local copies to melee the same
// target) so its OWN copies enter combat. Because the join independently re-reads its
// copies via captureNpcs, the RECV series shows task flip NONE->65024 (combat) only
// AFTER the order - proving a live peaceful->fighting transition, not a baked load
// state. The runner splits the RECV series on "SCENARIO COMBAT issued".
class CombatOrderScenario : public TimedScenario {
public:
    CombatOrderScenario()
        : TimedScenario("combat_order", 0), recvCount_(0), lastLogMs_(0), combatLogged_(false),
          havePins_(false), lastRearmMs_(0) {}

    // Pre-arm: pin both duelists while they are still at their baked spawn and
    // hold them peaceful until the armed clock begins (see craft_order rationale).
    virtual void onGameplay(const ScenarioContext& ctx) {
        if (!ctx.isHost) return;
        pinDuelists(ctx);
        if (havePins_) engine::holdDuelistsPeaceful(ctx.gw);
    }

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            pinDuelists(ctx);
            if (!havePins_)
                coop::logLine("SCENARIO duel pin FAILED (need two nearby non-squad NPCs)");
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Baseline: hold both duelists peaceful + in range (clean "not fighting before").
        if (ctx.isHost && havePins_ && ctx.elapsedMs < ORDER_AT_MS) {
            engine::holdDuelistsPeaceful(ctx.gw);
        }
        // Order phase: trigger the live fight once, then re-arm disengaged duelists on a
        // throttle so the fight is sustained for the whole post-order window.
        if (ctx.isHost && havePins_ && ctx.elapsedMs >= ORDER_AT_MS) {
            if (!combatLogged_) {
                int n = engine::startDuel(ctx.gw);
                char b[96];
                _snprintf(b, sizeof(b) - 1, "SCENARIO COMBAT issued orders=%d", n);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                combatLogged_ = true;
                lastRearmMs_ = ctx.elapsedMs;
            } else if (ctx.elapsedMs - lastRearmMs_ >= 2500) {
                lastRearmMs_ = ctx.elapsedMs;
                engine::rearmDuelScene(ctx.gw);
                engine::logDuelCombat(ctx.gw); // host-side combat ground truth
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost) {
                EntityState npcs[MAX_LOG];
                passed_ = havePins_ && (engine::captureNpcs(ctx.gw, npcs, MAX_LOG) > 0);
            } else {
                passed_ = (recvCount_ >= 1);
            }
            return true;
        }
        return false;
    }

private:
    static const unsigned long ORDER_AT_MS      = 18000;
    static const unsigned long HOST_DURATION_MS = 52000;
    static const unsigned long JOIN_DURATION_MS = 34000;
    static const unsigned int  MAX_LOG          = 40;

    void pinDuelists(const ScenarioContext& ctx) {
        if (havePins_) return;
        havePins_ = engine::pickDuelSubjects(ctx.gw, handA_, handB_);
        if (havePins_) {
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO duel subjects pinned A=%u,%u B=%u,%u",
                      handA_[3], handA_[4], handB_[3], handB_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          combatLogged_;
    bool          havePins_;
    unsigned int  handA_[5];
    unsigned int  handB_[5];
    unsigned long lastRearmMs_;
};

// combat_kill (Phase 3c, L5 OUTCOME): proves combat resolution is HOST-AUTHORITATIVE.
// Both clients load duel1 and run their OWN local fight, but the host decides who wins:
// at ORDER_AT_MS it triggers the duel AND weakens duelist B (woundSubject - lowers blood
// only, no scaffold collapse), so duelist A downs B via genuine melee. When the host's B
// goes down, the host emits a RELIABLE KO/death event STAMPED with A as the actor (combat
// attribution), and the join's body-state override forces ITS B down to match - even if
// the join's local sim had B winning. The runner asserts: the host sent a combat
// KO/death with a non-zero actor, the join received that exact event (hand + actor), and
// the join's victim is down afterwards. Reuses combat_order's pin/hold baseline.
class CombatKillScenario : public TimedScenario {
public:
    CombatKillScenario()
        : TimedScenario("combat_kill", 0), recvCount_(0), lastLogMs_(0), combatLogged_(false),
          havePins_(false), lastRearmMs_(0), lastWoundMs_(0), koLogged_(false),
          repins_(0) {}

    // Same pre-arm pin+hold as combat_order.
    virtual void onGameplay(const ScenarioContext& ctx) {
        if (!ctx.isHost) return;
        pinDuelists(ctx);
        if (havePins_) engine::holdDuelistsPeaceful(ctx.gw);
    }

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            pinDuelists(ctx);
            if (!havePins_)
                coop::logLine("SCENARIO duel pin FAILED (need two nearby non-squad NPCs)");
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.isHost && havePins_ && ctx.elapsedMs < ORDER_AT_MS) {
            // Re-pin while there is still baseline left to hold the new pair
            // peaceful. pickDuelSubjects now skips downed candidates, so this
            // lands on a fresh upright pair rather than the same corpse.
            if (duelistDown(handA_) || duelistDown(handB_)) {
                havePins_ = false;
                ++repins_;
                pinDuelists(ctx);
                char b[96];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO duel REPIN n=%u ok=%d (pinned duelist went down)",
                          repins_, havePins_ ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (ctx.isHost && havePins_ && ctx.elapsedMs < ORDER_AT_MS) {
            engine::holdDuelistsPeaceful(ctx.gw);
        }
        if (ctx.isHost && havePins_ && ctx.elapsedMs >= ORDER_AT_MS) {
            if (!combatLogged_) {
                // The premise, stated before the fight: a duel needs two
                // conscious duelists. Said out loud so a run that never had a
                // fight to replicate is not scored as a replication failure.
                bool upA = !duelistDown(handA_), upB = !duelistDown(handB_);
                if (!upA || !upB) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO duel PREMISE FAILED upA=%d upB=%d repins=%u",
                              upA ? 1 : 0, upB ? 1 : 0, repins_);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
                int n = engine::startDuel(ctx.gw);
                // Weaken B on the HOST so the real fight draws decisive blood in the
                // window (the damage_guard oracle needs a host-side blood drop as its
                // ground truth; the join's copy must stay flat - it takes no local
                // damage and blood never crosses the wire).
                bool w = engine::woundSubject(ctx.gw, handB_, 45.0f);
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO COMBAT issued orders=%d loser=B(%u,%u) wounded=%d",
                          n, handB_[3], handB_[4], w ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                combatLogged_ = true; lastRearmMs_ = ctx.elapsedMs;
            } else if (ctx.elapsedMs < (ORDER_AT_MS + KO_DELAY_MS)) {
                // Let them visibly trade blows for a few seconds; keep A on B.
                if (ctx.elapsedMs - lastRearmMs_ >= 2000) {
                    lastRearmMs_ = ctx.elapsedMs;
                    engine::rearmDuelScene(ctx.gw);
                    engine::logDuelCombat(ctx.gw);
                }
            } else {
                // Decisive takedown: while A is actively meleeing B (so the attacker map
                // names A), KO B. Two random NPCs won't reliably KO each other in-window,
                // so the takedown is enforced; the REPLICATION path (down edge -> reliable
                // event stamped with the attacker -> join forces B down) is what we test.
                // Re-assert on a throttle so B stays down (the KO timer needs topping).
                //
                // Unless the duel already picked its own loser. Nothing says B
                // wins; run 172544 had B beat A six seconds before this point,
                // and the enforced KO then landed on a body no one was hitting,
                // so it went out as actor=0,0 - a synthetic takedown reported as
                // an attribution failure. A fight that resolves itself is the
                // better test anyway: its event carries a real attacker.
                bool aDown = duelistDown(handA_);
                if (!aDown && ctx.elapsedMs - lastWoundMs_ >= 1500) {
                    lastWoundMs_ = ctx.elapsedMs;
                    engine::orderDownSubject(ctx.gw, handB_); // knockDown B by hand
                    engine::logDuelCombat(ctx.gw);
                }
                if (!koLogged_) {
                    char b[128];
                    if (aDown)
                        _snprintf(b, sizeof(b) - 1,
                                  "SCENARIO duel RESOLVED loser=A(%u,%u)",
                                  handA_[3], handA_[4]);
                    else
                        _snprintf(b, sizeof(b) - 1,
                                  "SCENARIO KO enforced loser=B(%u,%u)",
                                  handB_[3], handB_[4]);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    koLogged_ = true;
                }
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
            // Damage-guard vitals. HOST: log the PINNED duelists' blood the whole
            // run (it knows the hands), so the series captures the full drop -
            // pre-fight baseline through the wound bias + real melee. JOIN: it
            // doesn't know the pins, so it logs any local copy that is fighting
            // or down (== the duelists, once intent replicates). The oracle
            // asserts the HOST victim's blood DROPPED while the JOIN's driven
            // copy stayed FLAT (guarded cosmetic fight). Both reads come off the
            // rendered body's medical state, not a field we wrote.
            for (unsigned int i = 0; i < n; ++i) {
                bool log;
                if (ctx.isHost) {
                    log = havePins_ &&
                          ((npcs[i].hIndex == handA_[3] && npcs[i].hSerial == handA_[4]) ||
                           (npcs[i].hIndex == handB_[3] && npcs[i].hSerial == handB_[4]));
                } else {
                    log = taskIsCombat(npcs[i].task) ||
                          ((npcs[i].bodyState & 7u) != 0);
                }
                if (!log) continue;
                unsigned int h[5] = { npcs[i].hType, npcs[i].hContainer,
                                      npcs[i].hContainerSerial, npcs[i].hIndex,
                                      npcs[i].hSerial };
                float blood = 0.0f;
                if (engine::readBloodByHand(h, &blood)) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO VITALS hand=%u,%u t=%lu blood=%.1f",
                              npcs[i].hIndex, npcs[i].hSerial,
                              (unsigned long)ctx.elapsedMs, blood);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost) {
                EntityState npcs[MAX_LOG];
                passed_ = havePins_ && (engine::captureNpcs(ctx.gw, npcs, MAX_LOG) > 0);
            } else {
                passed_ = (recvCount_ >= 1);
            }
            return true;
        }
        return false;
    }

private:
    static const unsigned long ORDER_AT_MS      = 14000;
    static const unsigned long KO_DELAY_MS      = 14000; // fight visibly before the takedown
                                                         // (long enough for real landed hits,
                                                         // which the damage_guard oracle needs)
    static const unsigned long HOST_DURATION_MS = 66000;
    static const unsigned long JOIN_DURATION_MS = 54000;
    static const unsigned int  MAX_LOG          = 40;

    void pinDuelists(const ScenarioContext& ctx) {
        if (havePins_) return;
        havePins_ = engine::pickDuelSubjects(ctx.gw, handA_, handB_);
        if (havePins_) {
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO duel subjects pinned A=%u,%u B=%u,%u",
                      handA_[3], handA_[4], handB_[3], handB_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // A pinned duelist the town's own brawl has already knocked out. The pin is
    // latched ~30 s before the order, and duel1 is a populated town: measured
    // 2026-08-07, the victim went down at baseline+4 s with no attacker, the
    // enforced takedown then found no bodyState edge to send, and the oracle
    // read a missing event as missing ATTRIBUTION.
    bool duelistDown(const unsigned int h[5]) const {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return false; // unresolvable != down; leave the pin alone
        return coop::bodyIsDown(engine::readBodyState(c));
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          combatLogged_;
    bool          havePins_;
    unsigned int  handA_[5];
    unsigned int  handB_[5];
    unsigned long lastRearmMs_;
    unsigned long lastWoundMs_;
    bool          koLogged_;
    unsigned int  repins_;
};

// player_combat (player-combat validation, phase 1): real combat damage TO
// player characters, both ownership directions, plus the players' own combat
// intent crossing. Save 'sync' (2-tab squad in a bar full of ARMED NPCs).
//
// Design pivots from two characterization runs:
//   * Unarmed ally-vs-ally player duels draw ZERO blood (block/stun sparring) -
//     players cannot be each other's damage vector in these saves.
//   * An ARMED bar NPC very much can (measured: mob took the host leader from
//     75.8 blood to -16 with limb flesh at 2.0 in ~25 s).
// So the ARMED NPC is the striker and the PLAYERS are the victims:
//   Window A: the host orders NPC1 to melee the JOIN's tab-1 leader. The NPC's
//     combat intent streams to the join, whose engine swings the NPC copy at the
//     join's REAL (owned) member - authoritative damage lands on the victim's
//     OWNER (the join), the exact authority rule under test. The join victim
//     fights back automatically, which streams ITS combat intent join->host.
//   Window B: NPC2 melees the HOST's tab-0 leader - the same mechanism with the
//     victim owned by the host (all-local control, join renders both copies).
class PlayerCombatScenario : public TimedScenario {
public:
    PlayerCombatScenario()
        : TimedScenario("player_combat", 0), recvCount_(0), lastLogMs_(0), lastOrderAMs_(0),
          lastOrderBMs_(0), haveOwn_(false), havePeer_(false), haveNpcA_(false),
          haveNpcB_(false), issuedA_(false), issuedB_(false),
          noFightA_(0), noFightB_(0),
          lastVicBloodA_(-1.0f), lastVicBloodB_(-1.0f) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int ownRank = ctx.isHost ? 0u : 1u;
        if (!haveOwn_ || !havePeer_) latchLeaders(ctx, ownRank);

        // All attack orders are HOST-side (world NPCs are host-owned; the first
        // characterization run showed a join-ordered NPC stops resolving on the
        // host once the join's combat branch detaches it).
        if (ctx.isHost && haveOwn_ && havePeer_) {
            // Window A: a striker -> the JOIN's leader. The order + fight are
            // host-local against the leader's DRIVEN copy; the striker's combat
            // intent streams and the join's engine runs the authoritative fight
            // on its OWNED member.
            if (ctx.elapsedMs >= A_AT_MS && ctx.elapsedMs < B_AT_MS) {
                driveWindow(ctx, "A", peerHand_, npcA_, haveNpcA_, issuedA_,
                            lastOrderAMs_, haveNpcB_ ? npcB_ : 0, noFightA_,
                            lastVicBloodA_);
                // RESERVE window B's striker now, while the pool is still clean:
                // by B time the whole bar is brawling and pickCombatVictim's
                // in-combat filter can find nobody (run 011932's B pick FAILED).
                if (haveNpcA_ && !haveNpcB_)
                    haveNpcB_ = engine::pickCombatVictim(ctx.gw, ownHand_,
                                                         npcA_, npcB_);
            }
            // Window B: a striker -> our OWN leader (host-owned victim,
            // all-local; the join renders both copies).
            if (ctx.elapsedMs >= B_AT_MS)
                driveWindow(ctx, "B", ownHand_, npcB_, haveNpcB_, issuedB_,
                            lastOrderBMs_, haveNpcA_ ? npcA_ : 0, noFightB_,
                            lastVicBloodB_);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            logSeries(ctx, ownRank);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (issuedA_ && issuedB_ && haveNpcA_ && haveNpcB_)
                                 : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    // Shared (peer-ready-armed) timeline: NPC1 fights the join's leader 12-34s,
    // NPC2 fights the host's leader from 34s. The join outlives B by ~22s.
    static const unsigned long A_AT_MS          = 12000;
    static const unsigned long B_AT_MS          = 34000;
    static const unsigned long HOST_DURATION_MS = 64000;
    static const unsigned long JOIN_DURATION_MS = 58000;
    static const unsigned int  MAX_LOG          = 40;

    // Keep one live striker melee-ing 'vic', re-ordering every 2.5 s and
    // REPLACING the striker if the bar brawl KOs it (run 010631: window A's
    // striker was beaten unconscious by another bar NPC ~13 s in, silently
    // ending the window with 0.6 blood of damage done).
    void driveWindow(const ScenarioContext& ctx, const char* tag,
                     const unsigned int vic[5], unsigned int striker[5],
                     bool& haveStriker, bool& issued, unsigned long& lastMs,
                     const unsigned int* exclude, unsigned int& noFight,
                     float& lastVicBlood) {
        if (issued && ctx.elapsedMs - lastMs < 2500) return;
        lastMs = ctx.elapsedMs;
        // Replace the striker when it is DOWN (brawl KO, run 010631), stuck
        // fighting someone OTHER than our victim (a reserved striker dragged
        // into the brawl before its window - orderAttackByHand deliberately
        // no-ops on a fighting body, so it would never engage), or a DUD:
        //   * upright + free yet ignoring the attack goal for 3 straight order
        //     ticks (run 033318: window B's striker stood idle all window), or
        //   * fighting the right victim but drawing NO blood for 5 straight
        //     ticks (run 034659: an engaged striker landed 1.8 blood in 30 s -
        //     weak/unarmed; the gate needs >= 3).
        // A fresh pick is out-of-combat by pickCombatVictim's filter; a dud is
        // excluded from its own re-pick (the old one keeps brawling - added
        // pressure, no downside). If the pool is empty (whole bar brawling),
        // keep the old one - re-orders are safe no-ops.
        const unsigned int NOFIGHT_LIMIT  = 3;
        const unsigned int NOBLOOD_LIMIT  = 5;
        const float        DROP_PER_TICK  = 0.3f; // "real damage" floor per 2.5 s
        bool vicDropped = false;
        {
            engine::MedicalRead vm;
            if (engine::readMedicalByHand(vic, &vm) && vm.valid) {
                vicDropped = (lastVicBlood >= 0.0f &&
                              (lastVicBlood - vm.blood) >= DROP_PER_TICK);
                lastVicBlood = vm.blood;
            }
        }
        bool replace = !haveStriker;
        const unsigned int* excludeDud = 0;
        if (haveStriker) {
            engine::MedicalRead mr;
            if (!engine::readMedicalByHand(striker, &mr) || !mr.valid ||
                mr.unconscious || mr.dead) {
                replace = true;
            } else {
                Character* s = engine::resolveCharByHand(striker[3], striker[4],
                                                         striker[0], striker[1],
                                                         striker[2]);
                engine::CombatRead cr;
                bool fighting = s && engine::readCombat(s, &cr) && cr.inCombat;
                if (fighting && !(cr.hasTarget && cr.target[3] == vic[3] &&
                                  cr.target[4] == vic[4])) {
                    replace = true;
                } else if (vicDropped) {
                    noFight = 0; // striker is delivering - keep it
                } else if (issued &&
                           ++noFight >= (fighting ? NOBLOOD_LIMIT
                                                  : NOFIGHT_LIMIT)) {
                    replace = true;
                    excludeDud = striker; // don't hand the window back to the dud
                }
            }
        }
        if (replace) {
            unsigned int fresh[5];
            if (engine::pickCombatVictim(ctx.gw, ownHand_, exclude, fresh,
                                         excludeDud)) {
                bool replaced = haveStriker;
                memcpy(striker, fresh, sizeof(fresh));
                haveStriker = true;
                noFight = 0; // fresh striker gets a full engagement window
                if (replaced) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO PCOMBAT %s restrike atk=%u,%u",
                              tag, striker[3], striker[4]);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            } else if (!haveStriker) {
                if (!issued) {
                    char b[96];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO PCOMBAT %s pick FAILED (no upright NPC)", tag);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    issued = true;
                }
                return;
            }
        }
        bool ok = engine::orderAttackByHand(ctx.gw, striker, vic);
        if (!issued) {
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO PCOMBAT %s issued atk=%u,%u vic=%u,%u ok=%d",
                      tag, striker[3], striker[4], vic[3], vic[4], ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            issued = true;
        }
    }

    void latchLeaders(const ScenarioContext& ctx, unsigned int ownRank) {
        EntityState sq[MAX_LOG];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_LOG);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO PCOMBAT own rank=%u hand=%u,%u",
                          ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, ownRank == 0u ? 1u : 0u);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO PCOMBAT peer rank=%u hand=%u,%u",
                          ownRank == 0u ? 1u : 0u, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    void logSeries(const ScenarioContext& ctx, unsigned int ownRank) {
        // Squad members: MEMBER for our tab, RECV for the peer's, VITALS for all
        // (the players are the victims whose medical truth the oracle compares).
        EntityState sq[MAX_LOG];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_LOG);
        bool sawPeer = false;
        for (unsigned int i = 0; i < n; ++i) {
            int r = tabRankOf(sq, n, i);
            if (r < 0) continue;
            logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
            if ((unsigned int)r != ownRank) sawPeer = true;
            unsigned int h[5]; handFromEntity(sq[i], h);
            logVitalsLine(h, ctx.elapsedMs);
        }
        if (!ctx.isHost && sawPeer) ++recvCount_;
        // World NPCs (the strikers): host MEMBER, join RECV - the join's RECV
        // series for the striker hands is the intent-crossing signal.
        EntityState npcs[MAX_LOG];
        unsigned int nn = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
        for (unsigned int i = 0; i < nn; ++i)
            logScenarioEntity(ctx.isHost ? "MEMBER" : "RECV", npcs[i]);
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastOrderAMs_;
    unsigned long lastOrderBMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          haveNpcA_;
    bool          haveNpcB_;
    bool          issuedA_;
    bool          issuedB_;
    unsigned int  noFightA_;      // consecutive order ticks without the victim
    unsigned int  noFightB_;      // bleeding (dud detection, runs 033318/034659)
    float         lastVicBloodA_; // victim blood at the previous order tick
    float         lastVicBloodB_; // (-1 = not yet sampled)
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
    unsigned int  npcA_[5];
    unsigned int  npcB_[5];
};

// assault_town (join-initiated town combat): the JOIN's player character starts
// an UNPROVOKED fight with a world NPC (the user's no-fight report: a fight the
// join picks with a townsperson renders only on the join). Save 'sync' (bar
// full of armed NPCs). The JOIN latches its own tab-1 leader, picks the nearest
// upright out-of-combat world NPC, and orders the attack - the exact chain a
// player right-click produces. Nothing is ordered host-side: the fight must
// cross as the join's streamed combat intent (captureOne's task override ->
// host applyTargets combat branch -> host-local fight). Audit trail:
//   join:  "SCENARIO ASSAULT issued atk=A vic=V", [combat] CAP hand=A tgt=V
//   host:  [combat] order hand=A tgt=V r=2 (the driven join-PC copy engaged),
//          "SCENARIO ASSAULT hostview fight=1 tgt=V" (host's local combat read
//          of the join-PC copy), SCENARIO VITALS for V on both sides.
class AssaultTownScenario : public TimedScenario {
public:
    AssaultTownScenario()
        : TimedScenario("assault_town", 0), lastLogMs_(0), lastOrderMs_(0), haveOwn_(false),
          havePeer_(false), haveVic_(false), issued_(false), pickFailLogged_(false),
          hostFightSeen_(0) {}

    virtual void onGameplay(const ScenarioContext& ctx) {
        latchLeaders(ctx);
    }

    virtual void onStart(const ScenarioContext& ctx) {
        latchLeaders(ctx);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveOwn_ || !havePeer_) latchLeaders(ctx);

        // JOIN: pick the victim and issue/refresh the attack. orderAttackByHand
        // no-ops while the attacker is already fighting, so the 2.5 s cadence is
        // a keep-alive, not an AI reset.
        if (!ctx.isHost && haveOwn_ && ctx.elapsedMs >= ASSAULT_AT_MS) {
            // Victim pick retries on the order cadence: the first run showed a
            // one-shot pick near the join's tab-1 leader finding nothing within
            // pickCombatVictim's 30 u (the sync save bakes the bar crowd around
            // the HOST leader). Fall back to picking near the PEER leader - the
            // attack order's own pathing walks our PC to the victim, which is
            // exactly the user's manual repro (walk up to a townsperson, fight).
            if (!haveVic_ && (ctx.elapsedMs - lastOrderMs_) >= 2500) {
                lastOrderMs_ = ctx.elapsedMs;
                haveVic_ = engine::pickCombatVictim(ctx.gw, ownHand_, 0, vic_, 0);
                if (!haveVic_ && havePeer_)
                    haveVic_ = engine::pickCombatVictim(ctx.gw, peerHand_, 0, vic_, 0);
                if (!haveVic_ && !pickFailLogged_) {
                    coop::logLine("SCENARIO ASSAULT pick FAILED (no upright NPC; retrying)");
                    pickFailLogged_ = true;
                }
            }
            if (haveVic_ &&
                (!issued_ || (ctx.elapsedMs - lastOrderMs_) >= 2500)) {
                lastOrderMs_ = ctx.elapsedMs;
                bool ok = engine::orderAttackByHand(ctx.gw, ownHand_, vic_);
                if (!issued_) {
                    char b[160]; _snprintf(b, sizeof(b) - 1,
                        "SCENARIO ASSAULT issued atk=%u,%u vic=%u,%u ok=%d",
                        ownHand_[3], ownHand_[4], vic_[3], vic_[4], ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    issued_ = true;
                }
            }
        }

        // 1 Hz series, both sides: local combat read of the ATTACKER (join = its
        // owned leader, host = its driven copy of the peer leader) + victim
        // vitals. The host learns the victim hand from its OWN combat read of
        // the copy (cr.target), so the two logs cross-reference by hand.
        if (ctx.elapsedMs - lastLogMs_ >= 1000 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            const unsigned int* atk = ctx.isHost ? peerHand_ : ownHand_;
            bool haveAtk = ctx.isHost ? havePeer_ : haveOwn_;
            if (haveAtk) {
                engine::CombatRead cr;
                bool fight = engine::readCombatByHand(atk, &cr) &&
                             (cr.inCombat || cr.modeActive);
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO ASSAULT %s fight=%d tgt=%u,%u wait=%d",
                    ctx.isHost ? "hostview" : "joinview",
                    fight ? 1 : 0,
                    cr.hasTarget ? cr.target[3] : 0,
                    cr.hasTarget ? cr.target[4] : 0,
                    cr.waiting ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                if (ctx.isHost && fight) ++hostFightSeen_;
                if (ctx.isHost && cr.hasTarget) logVitalsLine(cr.target, ctx.elapsedMs);
                logVitalsLine(atk, ctx.elapsedMs);
            }
            if (!ctx.isHost && haveVic_) logVitalsLine(vic_, ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // Join: the assault must have been issued. Host: lenient (leaders
            // latched) - the wire-level judgement is the oracle's, from the
            // CAP/order/hostview lines above.
            passed_ = ctx.isHost ? havePeer_ : (issued_ && haveVic_);
            return true;
        }
        return false;
    }

private:
    static const unsigned long ASSAULT_AT_MS    = 10000;
    static const unsigned long HOST_DURATION_MS = 52000;
    static const unsigned long JOIN_DURATION_MS = 45000;
    static const unsigned int  MAX_LOG          = 40;

    void latchLeaders(const ScenarioContext& ctx) {
        const unsigned int ownRank = ctx.isHost ? 0u : 1u;
        EntityState sq[MAX_LOG];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_LOG);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO ASSAULT own rank=%u hand=%u,%u",
                    ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, ownRank == 0u ? 1u : 0u);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO ASSAULT peer rank=%u hand=%u,%u",
                    ownRank == 0u ? 1u : 0u, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    unsigned long lastLogMs_;
    unsigned long lastOrderMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          haveVic_;
    bool          issued_;
    bool          pickFailLogged_;
    unsigned int  hostFightSeen_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
    unsigned int  vic_[5];
};

// player_ko (player-combat validation, phase 1): players as VICTIMS, both
// directions. Save 'squad1'. Window A: the HOST knocks out its OWN tab-0 leader
// (scaffold KO, the down_order pattern - deterministic; real combat damage is
// player_combat's job), holds it down, then REVIVES it. The KO and revive edges
// must cross as reliable EVT_KNOCKOUT/EVT_REVIVE and the join's driven copy must
// lie down / stand up. Window B inverts it: the JOIN KOs + revives its OWN tab-1
// member and the host's driven copy must follow. Both sides log squad
// MEMBER/RECV + VITALS series.
class PlayerKoScenario : public TimedScenario {
public:
    PlayerKoScenario()
        : TimedScenario("player_ko", 0), recvCount_(0), lastLogMs_(0), lastAssertMs_(0),
          haveSubj_(false), downLogged_(false), reviveLogged_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int  ownRank = ctx.isHost ? 0u : 1u;
        const unsigned long downAt  = ctx.isHost ? A_DOWN_AT_MS : B_DOWN_AT_MS;
        const unsigned long revAt   = ctx.isHost ? A_REV_AT_MS : B_REV_AT_MS;
        const char* tag             = ctx.isHost ? "A" : "B";

        if (!haveSubj_) latchSubject(ctx, ownRank);

        if (haveSubj_ && ctx.elapsedMs >= downAt && ctx.elapsedMs < revAt) {
            // KO our OWN member; re-assert on a throttle (tops the forced KO
            // timer) so it stays down for the whole window.
            if (!downLogged_ || ctx.elapsedMs - lastAssertMs_ >= 1500) {
                lastAssertMs_ = ctx.elapsedMs;
                bool ok = engine::orderDownSubject(ctx.gw, subjHand_);
                if (!downLogged_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO PKO %s down issued hand=%u,%u ok=%d",
                              tag, subjHand_[3], subjHand_[4], ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    downLogged_ = true;
                }
            }
        }
        if (haveSubj_ && ctx.elapsedMs >= revAt) {
            // Revive: clear the KO + restore blood so the body stands and the
            // owner publishes the upright edge (EVT_REVIVE). Re-issued once.
            if (!reviveLogged_ || (ctx.elapsedMs - lastAssertMs_ >= 1500 &&
                                   ctx.elapsedMs < revAt + 4000)) {
                lastAssertMs_ = ctx.elapsedMs;
                bool ok = engine::reviveSubject(ctx.gw, subjHand_);
                if (!reviveLogged_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO PKO %s revive issued hand=%u,%u ok=%d",
                              tag, subjHand_[3], subjHand_[4], ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    reviveLogged_ = true;
                }
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
                unsigned int h[5]; handFromEntity(sq[i], h);
                logVitalsLine(h, ctx.elapsedMs);
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (downLogged_ && reviveLogged_)
                                 : (downLogged_ && reviveLogged_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    // Sequential windows on the peer-ready-armed shared timeline:
    // A (host victim): down 10-24s, revive at 24s. B (join victim): down 34-48s,
    // revive at 48s. The host outlives the join's whole window.
    static const unsigned long A_DOWN_AT_MS    = 10000;
    static const unsigned long A_REV_AT_MS     = 24000;
    static const unsigned long B_DOWN_AT_MS    = 34000;
    static const unsigned long B_REV_AT_MS     = 48000;
    static const unsigned long HOST_DURATION_MS = 70000;
    static const unsigned long JOIN_DURATION_MS = 60000;
    static const unsigned int  MAX_SQUAD        = 32;

    void latchSubject(const ScenarioContext& ctx, unsigned int ownRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, ownRank);
        if (idx < 0) return;
        handFromEntity(sq[idx], subjHand_);
        haveSubj_ = true;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO PKO subject rank=%u hand=%u,%u",
                  ownRank, subjHand_[3], subjHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastAssertMs_;
    bool          haveSubj_;
    bool          downLogged_;
    bool          reviveLogged_;
    unsigned int  subjHand_[5];
};

// combat_crowd (waiting-attacker stance validation): the HOST orders a CROWD of
// bar NPCs (up to 5) onto its own tab leader. Kenshi's AttackSlotManager grants
// only 1-2 of them an active attack slot; the rest hold the WAITING stance
// (circle/wait/hesitate sword states) - the exact case that used to teleport and
// AI-reset on the join (the 1.5 s clearGoals re-issue loop against slot-queued
// copies). The host streams TASK_COMBAT_WAIT for them (protocol 15); the join
// leaves their copies menacing in the ring. Both sides log SCENARIO MEMBER/RECV
// for every captured NPC at ~2 Hz. Test-CombatCrowd gates, per crowd hand: the
// join's [combat] order re-issue count (loop dead), the join's [combat] snap
// teleport count (artifact gone), host-vs-join position tracking, and - from the
// host MEMBER task series - that BOTH stances (active 0xFE00 / waiting 0xFE01)
// actually streamed (else the run proves nothing).
class CombatCrowdScenario : public TimedScenario {
public:
    CombatCrowdScenario()
        : TimedScenario("combat_crowd", 0), recvCount_(0), lastLogMs_(0), lastOrderMs_(0),
          haveOwn_(false), nStrikers_(0), nSeen_(0), issuedLogged_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int ownRank = ctx.isHost ? 0u : 1u;
        if (!haveOwn_) latchLeader(ctx, ownRank);

        // HOST: pick the crowd once the window opens, then keep every striker
        // ordered on a throttle (orderAttackByHand no-ops while one is already
        // fighting, so the re-order never thrashes the host AI).
        if (ctx.isHost && haveOwn_ && ctx.elapsedMs >= COMBAT_AT_MS &&
            (ctx.elapsedMs - lastOrderMs_ >= 2500 || lastOrderMs_ == 0)) {
            lastOrderMs_ = ctx.elapsedMs;
            if (nStrikers_ == 0) pickCrowd(ctx);
            for (unsigned int i = 0; i < nStrikers_; ++i)
                engine::orderAttackByHand(ctx.gw, striker_[i], ownHand_);
            if (!issuedLogged_ && nStrikers_ > 0) {
                issuedLogged_ = true;
                char b[112];
                _snprintf(b, sizeof(b) - 1, "SCENARIO CROWD issued n=%u vic=%u,%u",
                          nStrikers_, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // Both sides: the NPC position/task series the oracle compares.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
            // JOIN: a driven combat copy leaves the interest capture when the
            // replicator detaches it into its own platoon (the capture's query
            // no longer enumerates it), which blinds the tracking series for
            // exactly the bodies under test. Remember every NPC hand ever
            // captured and keep logging the missing ones by direct resolve.
            if (!ctx.isHost) {
                for (unsigned int i = 0; i < n; ++i) rememberSeen(npcs[i]);
                for (unsigned int s = 0; s < nSeen_; ++s) {
                    bool inCap = false;
                    for (unsigned int i = 0; i < n; ++i)
                        if (npcs[i].hIndex == seen_[s].hIndex &&
                            npcs[i].hSerial == seen_[s].hSerial) { inCap = true; break; }
                    if (inCap) continue;
                    Character* c = engine::resolve(seen_[s]);
                    if (!c) continue;
                    EntityState e = seen_[s]; // original (host-frame) hand key
                    float x, y, z;
                    if (!engine::readPos(c, &x, &y, &z)) continue;
                    e.x = x; e.y = y; e.z = z;
                    e.task = TASK_NONE;
                    e.bodyState = engine::readBodyState(c);
                    logScenarioEntity("RECV", e);
                }
            }
            // HOST: raw combat-read diagnostic per striker (why did the stance
            // stream flicker?) - sword state + engaged flags + target presence.
            if (ctx.isHost) {
                for (unsigned int i = 0; i < nStrikers_; ++i) {
                    engine::CombatRead cr;
                    if (!engine::readCombatByHand(striker_[i], &cr)) continue;
                    char b[144];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO CROWDSTATE hand=%u,%u sw=%d mode=%d fight=%d "
                              "tgt=%d wait=%d",
                              striker_[i][3], striker_[i][4], cr.swordState,
                              cr.modeActive ? 1 : 0, cr.inCombat ? 1 : 0,
                              cr.hasTarget ? 1 : 0, cr.waiting ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (nStrikers_ >= MIN_CROWD) : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    // Shared (peer-ready-armed) timeline; the crowd window runs 10 s -> end.
    static const unsigned long COMBAT_AT_MS     = 10000;
    static const unsigned long HOST_DURATION_MS = 75000;
    static const unsigned long JOIN_DURATION_MS = 68000;
    static const unsigned int  MAX_LOG      = 24;
    static const unsigned int  MAX_CROWD    = 5;
    static const unsigned int  MIN_CROWD    = 3;
    static const unsigned int  MAX_SQUAD    = 32;
    static const unsigned int  MAX_REMEMBER = 48;

    void latchLeader(const ScenarioContext& ctx, unsigned int ownRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, ownRank);
        if (idx < 0) return;
        handFromEntity(sq[idx], ownHand_);
        haveOwn_ = true;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CROWD own rank=%u hand=%u,%u",
                  ownRank, ownHand_[3], ownHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void rememberSeen(const EntityState& e) {
        for (unsigned int s = 0; s < nSeen_; ++s)
            if (seen_[s].hIndex == e.hIndex && seen_[s].hSerial == e.hSerial) return;
        if (nSeen_ < MAX_REMEMBER) seen_[nSeen_++] = e;
    }

    // The crowd: the nearest upright, not-yet-fighting bar NPCs (the captured
    // set is already interest-sorted around the leaders).
    void pickCrowd(const ScenarioContext& ctx) {
        EntityState npcs[MAX_LOG];
        unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
        for (unsigned int i = 0; i < n && nStrikers_ < MAX_CROWD; ++i) {
            // down/dead/ragdoll. bodyFlags strips the protocol-53 prone field,
            // which is a posture value sharing the word, not a flag.
            if (bodyFlags(npcs[i].bodyState) != 0) continue;
            if (taskIsCombat(npcs[i].task)) continue;       // already brawling
            handFromEntity(npcs[i], striker_[nStrikers_]);
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO CROWD striker=%u,%u",
                      striker_[nStrikers_][3], striker_[nStrikers_][4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            ++nStrikers_;
        }
        if (nStrikers_ < MIN_CROWD)
            coop::logLine("SCENARIO CROWD pick FAILED (too few upright NPCs)");
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastOrderMs_;
    bool          haveOwn_;
    unsigned int  nStrikers_;
    unsigned int  nSeen_;
    bool          issuedLogged_;
    unsigned int  ownHand_[5];
    unsigned int  striker_[MAX_CROWD][5];
    EntityState   seen_[MAX_REMEMBER]; // JOIN: hands ever captured (resolve fallback)
};

// combat_battle (many-NPC combat warp validation, 2026-07-16): the HOST runtime-
// spawns a BATTLE of N fighters near the leader and index-pairs them into MUTUAL
// melee (battler 2k vs battler 2k+1), so N/2 duels run at once inside the interest
// bubble. Where combat_crowd stresses ~5 WAITING strikers on ONE victim, this
// stresses the join's combat DRIVE under many simultaneously-ACTIVE combatants -
// the "NPCs warp around on the join when many are fighting" field report. The join
// mints a proxy for each runtime spawn (protocol 21), detaches it, and drives it
// via the interp + graded-snap combat path; Test-CombatSnapRate gates the [combat]
// snap teleport buckets (churn rate / persistence / wrong-target), and the enriched
// [combat] stats rollup records the aggregate. N is env-tunable
// (KENSHICOOP_BATTLE_N, default 16, clamp 4..MAX_BATTLERS) so ONE build runs the
// 10v10 / 20v20 / 40v40 ladder. Both sides log SCENARIO MEMBER/RECV like crowd.
class CombatBattleScenario : public TimedScenario {
public:
    CombatBattleScenario()
        : TimedScenario("combat_battle", 0), recvCount_(0), lastLogMs_(0), lastOrderMs_(0),
          nBattlers_(0), nSeen_(0), spawned_(false), issuedLogged_(false) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        // HOST: spawn the battle once the window opens, then keep every pair armed
        // (orderAttackByHand no-ops while a body is already fighting, so re-arming
        // only re-engages the ones the AttackSlotManager rotated out).
        if (ctx.isHost && ctx.elapsedMs >= COMBAT_AT_MS) {
            if (!spawned_) spawnBattle(ctx);
            if (nBattlers_ >= 2 &&
                (ctx.elapsedMs - lastOrderMs_ >= 2500 || lastOrderMs_ == 0)) {
                lastOrderMs_ = ctx.elapsedMs;
                for (unsigned int k = 0; k + 1 < nBattlers_; k += 2) {
                    engine::orderAttackByHand(ctx.gw, battler_[k],     battler_[k + 1]);
                    engine::orderAttackByHand(ctx.gw, battler_[k + 1], battler_[k]);
                }
                if (!issuedLogged_ && nBattlers_ >= 2) {
                    issuedLogged_ = true;
                    char b[96];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO BATTLE issued n=%u pairs=%u",
                              nBattlers_, nBattlers_ / 2);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }

        // Both sides: the NPC position/task series the oracle compares (same shape
        // combat_crowd uses).
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
            // JOIN: a driven combat copy leaves the interest capture when the
            // replicator detaches it into its own platoon; remember every NPC hand
            // ever captured and keep logging the missing ones by direct resolve
            // (the exact combat_crowd tracking-continuity fix).
            if (!ctx.isHost) {
                for (unsigned int i = 0; i < n; ++i) rememberSeen(npcs[i]);
                for (unsigned int s = 0; s < nSeen_; ++s) {
                    bool inCap = false;
                    for (unsigned int i = 0; i < n; ++i)
                        if (npcs[i].hIndex == seen_[s].hIndex &&
                            npcs[i].hSerial == seen_[s].hSerial) { inCap = true; break; }
                    if (inCap) continue;
                    Character* c = engine::resolve(seen_[s]);
                    if (!c) continue;
                    EntityState e = seen_[s];
                    float x, y, z;
                    if (!engine::readPos(c, &x, &y, &z)) continue;
                    e.x = x; e.y = y; e.z = z;
                    e.task = TASK_NONE;
                    e.bodyState = engine::readBodyState(c);
                    logScenarioEntity("RECV", e);
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (nBattlers_ >= MIN_BATTLERS) : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    // Shared (peer-ready-armed) timeline; the battle window runs 10 s -> end.
    static const unsigned long COMBAT_AT_MS     = 10000;
    static const unsigned long HOST_DURATION_MS = 75000;
    static const unsigned long JOIN_DURATION_MS = 68000;
    static const unsigned int  MAX_LOG      = 64;
    static const unsigned int  MAX_BATTLERS = 40;
    static const unsigned int  MIN_BATTLERS = 4;
    static const unsigned int  MAX_REMEMBER = 80;

    unsigned int battleN() const {
        const char* e = ::getenv("KENSHICOOP_BATTLE_N");
        unsigned int n = e ? (unsigned int)::atoi(e) : 16u;
        if (n < MIN_BATTLERS)  n = MIN_BATTLERS;
        if (n > MAX_BATTLERS)  n = MAX_BATTLERS;
        return n;
    }

    void spawnBattle(const ScenarioContext& ctx) {
        spawned_ = true;
        unsigned int want = battleN();
        static unsigned int hands[MAX_BATTLERS][5];
        unsigned int got = engine::spawnRuntimeSquad(ctx.gw, want, hands);
        for (unsigned int i = 0; i < got && nBattlers_ < MAX_BATTLERS; ++i) {
            for (int j = 0; j < 5; ++j) battler_[nBattlers_][j] = hands[i][j];
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO BATTLE striker=%u,%u",
                      battler_[nBattlers_][3], battler_[nBattlers_][4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            ++nBattlers_;
        }
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO BATTLE spawned=%u/%u", nBattlers_, want);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (nBattlers_ < MIN_BATTLERS)
            coop::logLine("SCENARIO BATTLE spawn FAILED (too few battlers)");
    }

    void rememberSeen(const EntityState& e) {
        for (unsigned int s = 0; s < nSeen_; ++s)
            if (seen_[s].hIndex == e.hIndex && seen_[s].hSerial == e.hSerial) return;
        if (nSeen_ < MAX_REMEMBER) seen_[nSeen_++] = e;
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastOrderMs_;
    unsigned int  nBattlers_;
    unsigned int  nSeen_;
    bool          spawned_;
    bool          issuedLogged_;
    unsigned int  battler_[MAX_BATTLERS][5];
    EntityState   seen_[MAX_REMEMBER];
};

// ---- combat_win: buffed PCs WIN a real fight --------------------------------
// A second warp shape distinct from the NPC-vs-NPC posturing of combat_battle:
// here each side buffs its OWN player-squad to 120 in EVERY stat, and the host
// runtime-spawns N unbuffed enemies (KENSHICOOP_WIN_N, default 8) ordered onto
// the PC leader. The buffed PCs cut them down, so the join-side stress shifts to
// dying / fleeing / KO churn and rapid target loss - a different driver of the
// combat snap/warp path than sustained melee. Both sides log SCENARIO MEMBER/RECV
// (the enemy copies) for the warp measurement + Test-CombatSnapRate, plus
// SCENARIO WIN buff/spawned/down for the outcome oracle.
class CombatWinScenario : public TimedScenario {
public:
    CombatWinScenario()
        : TimedScenario("combat_win", 0), recvCount_(0), lastLogMs_(0), lastOrderMs_(0),
          nEnemies_(0), nSeen_(0), nBuffed_(0), nOwn_(0), maxDown_(0),
          spawned_(false), buffed_(false), issuedLogged_(false), haveOwn_(false) {
        for (int j = 0; j < 5; ++j) ownHand_[j] = 0;
    }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned int ownRank = ctx.isHost ? 0u : 1u;

        // Both sides: buff their OWN squad to a winning stat line once the window
        // opens (each side owns its own PCs; statsSync streams the raise to the peer).
        if (ctx.elapsedMs >= COMBAT_AT_MS && !buffed_) buffOwnSquad(ctx, ownRank);

        // HOST: spawn the enemies once, then keep them ordered onto the PC squad,
        // SPREAD round-robin across every buffed member so the whole squad fights
        // (orderAttackByHand no-ops while already fighting, so re-issue only re-arms
        // the ones the slot manager rotated out or that lost their target to a kill).
        if (ctx.isHost && ctx.elapsedMs >= COMBAT_AT_MS) {
            if (!spawned_) spawnEnemies(ctx);
            if (nOwn_ >= 1 && nEnemies_ >= 1 &&
                (ctx.elapsedMs - lastOrderMs_ >= 2500 || lastOrderMs_ == 0)) {
                lastOrderMs_ = ctx.elapsedMs;
                for (unsigned int i = 0; i < nEnemies_; ++i)
                    engine::orderAttackByHand(ctx.gw, enemy_[i], ownMembers_[i % nOwn_]);
                if (!issuedLogged_) {
                    issuedLogged_ = true;
                    char b[96];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO WIN issued n=%u vic=spread across %u PC(s)",
                              nEnemies_, nOwn_);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }

        // Both sides: the enemy position/task series the oracle compares (same shape
        // combat_battle/combat_crowd use), plus the running downed-enemy outcome.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
            if (!ctx.isHost) {
                for (unsigned int i = 0; i < n; ++i) rememberSeen(npcs[i]);
                for (unsigned int s = 0; s < nSeen_; ++s) {
                    bool inCap = false;
                    for (unsigned int i = 0; i < n; ++i)
                        if (npcs[i].hIndex == seen_[s].hIndex &&
                            npcs[i].hSerial == seen_[s].hSerial) { inCap = true; break; }
                    if (inCap) continue;
                    Character* c = engine::resolve(seen_[s]);
                    if (!c) continue;
                    EntityState e = seen_[s];
                    float x, y, z;
                    if (!engine::readPos(c, &x, &y, &z)) continue;
                    e.x = x; e.y = y; e.z = z;
                    e.task = TASK_NONE;
                    e.bodyState = engine::readBodyState(c);
                    logScenarioEntity("RECV", e);
                }
            }
            // Outcome: how many spawned enemies are down/dead (bodyState != 0), as
            // resolved on THIS side. The buffed PCs winning drives this toward N.
            if (spawned_ || !ctx.isHost) countDowned(ctx);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost
                ? (nBuffed_ >= 1 && nEnemies_ >= MIN_ENEMIES && maxDown_ >= 1)
                : (nBuffed_ >= 1 && recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    // Shared (peer-ready-armed) timeline; the win window runs 10 s -> end.
    static const unsigned long COMBAT_AT_MS     = 10000;
    static const unsigned long HOST_DURATION_MS = 75000;
    static const unsigned long JOIN_DURATION_MS = 68000;
    static const unsigned int  MAX_LOG      = 48;
    static const unsigned int  MAX_ENEMIES  = 16;
    static const unsigned int  MIN_ENEMIES  = 4;
    static const unsigned int  MAX_SQUAD    = 32;
    static const unsigned int  MAX_OWN      = 16;
    static const unsigned int  MAX_REMEMBER = 64;

    unsigned int winN() const {
        const char* e = ::getenv("KENSHICOOP_WIN_N");
        unsigned int n = e ? (unsigned int)::atoi(e) : 8u;
        if (n < MIN_ENEMIES) n = MIN_ENEMIES;
        if (n > MAX_ENEMIES) n = MAX_ENEMIES;
        return n;
    }

    // Buff every member of the OWN tab (same container as the own leader) to 120
    // in all stats. Latches the own leader hand as the enemies' attack target too.
    void buffOwnSquad(const ScenarioContext& ctx, unsigned int ownRank) {
        buffed_ = true;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, ownRank);
        if (idx < 0) {
            coop::logLine("SCENARIO WIN buff FAILED (no own leader)");
            return;
        }
        handFromEntity(sq[idx], ownHand_);
        haveOwn_ = true;
        unsigned int leadC  = sq[idx].hContainer;
        unsigned int leadCs = sq[idx].hContainerSerial;
        for (unsigned int i = 0; i < n; ++i) {
            if (sq[i].hContainer != leadC || sq[i].hContainerSerial != leadCs) continue;
            unsigned int h[5]; handFromEntity(sq[i], h);
            unsigned int raised = engine::raiseAllStats(ctx.gw, h, 120.0f);
            if (raised == 0) continue;
            ++nBuffed_;
            // Remember each buffed member so the host can spread the enemies across
            // the whole squad (not just gank the leader).
            if (nOwn_ < MAX_OWN) {
                for (int j = 0; j < 5; ++j) ownMembers_[nOwn_][j] = h[j];
                ++nOwn_;
            }
            char b[112];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO WIN buff rank=%u hand=%u,%u stats=120 raised=%u",
                      ownRank, h[3], h[4], raised);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (nBuffed_ == 0)
            coop::logLine("SCENARIO WIN buff FAILED (no member stats raised)");
    }

    void spawnEnemies(const ScenarioContext& ctx) {
        spawned_ = true;
        unsigned int want = winN();
        static unsigned int hands[MAX_ENEMIES][5];
        unsigned int got = engine::spawnRuntimeSquad(ctx.gw, want, hands);
        for (unsigned int i = 0; i < got && nEnemies_ < MAX_ENEMIES; ++i) {
            for (int j = 0; j < 5; ++j) enemy_[nEnemies_][j] = hands[i][j];
            ++nEnemies_;
        }
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO WIN spawned=%u/%u", nEnemies_, want);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (nEnemies_ < MIN_ENEMIES)
            coop::logLine("SCENARIO WIN spawn FAILED (too few enemies)");
    }

    // Count spawned enemies currently down/dead on this side; log the running peak.
    // bodyFlags strips the protocol-53 prone field: a crippled or crouching enemy
    // is not down, and counting it as such would inflate the win metric.
    void countDowned(const ScenarioContext& ctx) {
        unsigned int down = 0;
        if (ctx.isHost) {
            for (unsigned int i = 0; i < nEnemies_; ++i) {
                Character* c = engine::resolveCharByHand(enemy_[i][3], enemy_[i][4],
                                                         enemy_[i][0], enemy_[i][1],
                                                         enemy_[i][2]);
                if (c && bodyFlags(engine::readBodyState(c)) != 0) ++down;
            }
        } else {
            for (unsigned int s = 0; s < nSeen_; ++s) {
                Character* c = engine::resolve(seen_[s]);
                if (c && bodyFlags(engine::readBodyState(c)) != 0) ++down;
            }
        }
        if (down > maxDown_) {
            maxDown_ = down;
            char b[80];
            _snprintf(b, sizeof(b) - 1, "SCENARIO WIN down=%u/%u",
                      maxDown_, ctx.isHost ? nEnemies_ : nSeen_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    void rememberSeen(const EntityState& e) {
        for (unsigned int s = 0; s < nSeen_; ++s)
            if (seen_[s].hIndex == e.hIndex && seen_[s].hSerial == e.hSerial) return;
        if (nSeen_ < MAX_REMEMBER) seen_[nSeen_++] = e;
    }

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastOrderMs_;
    unsigned int  nEnemies_;
    unsigned int  nSeen_;
    unsigned int  nBuffed_;
    unsigned int  nOwn_;
    unsigned int  maxDown_;
    bool          spawned_;
    bool          buffed_;
    bool          issuedLogged_;
    bool          haveOwn_;
    unsigned int  ownHand_[5];
    unsigned int  ownMembers_[MAX_OWN][5];
    unsigned int  enemy_[MAX_ENEMIES][5];
    EntityState   seen_[MAX_REMEMBER];
};

// pc_assault (join PC deals REAL damage to a host-owned NPC + combat-cohesion
// diagnostic): the damage-gated counterpart to assault_town. assault_town gates
// only the intent chain (issue -> CAP -> order -> hostview) and never proves the
// join's attack DAMAGES anything - exactly the "join does no damage to NPCs" gap.
// Here:
//   * Save 'sync' (baked bar crowd) so many hostile-able NPCs stand near the
//     co-located squads. Baked hands resolve identically host<->join, so the
//     enemy the join fights (cr.target) is the SAME hand the host copy damages,
//     and the drop streams back (baked NPCs have no runtime-spawn medical proxy
//     gap).
//   * BOTH sides buff their OWN squad to 120 (statsSync streams the raise to the
//     host copy of the join PC), so the join's authoritative host-side fight -
//     which the reciprocal-provoke + self-defend fix now sustains - draws blood.
// The join's damage-guard suppresses its LOCAL swings, so the proof is HOST-side:
// the victim's flesh/blood must DROP on the host (the join-dealt authoritative
// damage) and STREAM back to the join. SCENARIO COMBATSTATE is sampled on BOTH
// sides for the same baked hands so the oracle can measure combat COHESION (a
// copy fighting on the join while the host reports peace - the reported churn).
//   join:  "SCENARIO PCASSAULT issued ...", [combat] CAP, VITALS/COMBATSTATE
//   host:  "SCENARIO PCASSAULT spawned=n" (baked victims in reach), [combat] order
//          r=2, "hostview fight=1", VITALS (authoritative drop) + COMBATSTATE.
class PcAssaultScenario : public TimedScenario {
public:
    PcAssaultScenario()
        : TimedScenario("pc_assault", 0), lastLogMs_(0), lastOrderMs_(0),
          haveOwn_(false), havePeer_(false), haveVic_(false), issued_(false),
          buffed_(false), spawned_(false), pickFailLogged_(false),
          nSpawn_(0), nBuffed_(0), hostFightSeen_(0) {}

    virtual void onGameplay(const ScenarioContext& ctx) { latchLeaders(ctx); }
    virtual void onStart(const ScenarioContext& ctx) { latchLeaders(ctx); }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveOwn_ || !havePeer_) latchLeaders(ctx);

        // Both: buff OWN squad to 120 so the join's PC is a lethal attacker whose
        // host-side fight actually draws blood (statsSync streams the raise).
        if (!buffed_ && ctx.elapsedMs >= BUFF_AT_MS) buffOwnSquad(ctx);
        // HOST: confirm the baked victim pool near the leaders (no runtime spawn).
        if (ctx.isHost && !spawned_ && ctx.elapsedMs >= SPAWN_AT_MS) confirmVictimPool(ctx);

        // JOIN: pick the nearest bar NPC near our OWN leader FIRST (the join PC
        // stands among the crowd, same as assault_town), falling back to the peer
        // leader's vicinity; order our own leader to attack it, kept alive on a
        // cadence (orderAttackByHand no-ops while already fighting).
        if (!ctx.isHost && haveOwn_ && ctx.elapsedMs >= ASSAULT_AT_MS) {
            if (!haveVic_ && (ctx.elapsedMs - lastOrderMs_) >= 2500) {
                lastOrderMs_ = ctx.elapsedMs;
                haveVic_ = engine::pickCombatVictim(ctx.gw, ownHand_, 0, vic_, 0);
                if (!haveVic_ && havePeer_)
                    haveVic_ = engine::pickCombatVictim(ctx.gw, peerHand_, 0, vic_, 0);
                if (!haveVic_ && !pickFailLogged_) {
                    coop::logLine("SCENARIO PCASSAULT pick FAILED (no upright NPC; retrying)");
                    pickFailLogged_ = true;
                }
            }
            if (haveVic_ && (!issued_ || (ctx.elapsedMs - lastOrderMs_) >= 2500)) {
                lastOrderMs_ = ctx.elapsedMs;
                bool ok = engine::orderAttackByHand(ctx.gw, ownHand_, vic_);
                if (!issued_) {
                    char b[160]; _snprintf(b, sizeof(b) - 1,
                        "SCENARIO PCASSAULT issued atk=%u,%u vic=%u,%u ok=%d",
                        ownHand_[3], ownHand_[4], vic_[3], vic_[4], ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    issued_ = true;
                }
            }
        }

        // 1 Hz series: the attacker's local combat read + the victim's vitals +
        // combat-state parity samples. The host learns the victim hand from its
        // OWN combat read of the driven join-PC copy (cr.target) and logs its
        // blood - the authoritative drop that IS the join-dealt damage.
        if (ctx.elapsedMs - lastLogMs_ >= 1000 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            const unsigned int* atk = ctx.isHost ? peerHand_ : ownHand_;
            bool haveAtk = ctx.isHost ? havePeer_ : haveOwn_;
            if (haveAtk) {
                engine::CombatRead cr;
                bool fight = engine::readCombatByHand(atk, &cr) &&
                             (cr.inCombat || cr.modeActive);
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO PCASSAULT %s fight=%d tgt=%u,%u wait=%d",
                    ctx.isHost ? "hostview" : "joinview",
                    fight ? 1 : 0,
                    cr.hasTarget ? cr.target[3] : 0,
                    cr.hasTarget ? cr.target[4] : 0,
                    cr.waiting ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                if (ctx.isHost && fight) ++hostFightSeen_;
                // Log the attacker's OWN combat target on BOTH sides: baked hands
                // resolve identically host<->join, so the enemy the join PC fights
                // (cr.target) is the SAME hand the host copy damages - the oracle
                // compares that victim's blood on both logs (link 6). COMBATSTATE
                // on the same hands feeds the host<->join combat-parity check.
                if (cr.hasTarget) logVitalsLine(cr.target, ctx.elapsedMs);
                logVitalsLine(atk, ctx.elapsedMs);
                logCombatStateLine(atk, ctx.elapsedMs);
                if (cr.hasTarget) logCombatStateLine(cr.target, ctx.elapsedMs);
            }
            if (!ctx.isHost && haveVic_) logVitalsLine(vic_, ctx.elapsedMs);
            // Both: also sample the nearest few bar NPCs around each leader (belt-
            // and-suspenders - the oracle can find the hurt hand even if cr.target
            // flickers). Baked serials match across sides, so a drop the host logs
            // here has a matching join-side series.
            {
                const unsigned int* ref = ctx.isHost ? peerHand_ : ownHand_;
                bool haveRef = ctx.isHost ? havePeer_ : haveOwn_;
                if (haveRef) {
                    unsigned int e1[5], e2[5], e3[5];
                    if (engine::pickCombatVictim(ctx.gw, ref, 0, e1, 0)) {
                        logVitalsLine(e1, ctx.elapsedMs);
                        logCombatStateLine(e1, ctx.elapsedMs);
                        if (engine::pickCombatVictim(ctx.gw, ref, e1, e2, 0)) {
                            logVitalsLine(e2, ctx.elapsedMs);
                            logCombatStateLine(e2, ctx.elapsedMs);
                            if (engine::pickCombatVictim(ctx.gw, ref, e1, e3, e2)) {
                                logVitalsLine(e3, ctx.elapsedMs);
                                logCombatStateLine(e3, ctx.elapsedMs);
                            }
                        }
                    }
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // The damage judgement is the oracle's (from the VITALS series); the
            // scenario just certifies the setup ran (buff + victim pool / issue).
            passed_ = ctx.isHost ? (buffed_ && havePeer_) : (issued_ && haveVic_);
            return true;
        }
        return false;
    }

private:
    static const unsigned long BUFF_AT_MS       = 8000;
    static const unsigned long SPAWN_AT_MS      = 8000;
    static const unsigned long ASSAULT_AT_MS    = 14000;
    static const unsigned long HOST_DURATION_MS = 58000;
    static const unsigned long JOIN_DURATION_MS = 50000;
    static const unsigned int  MAX_LOG   = 40;
    static const unsigned int  MAX_SQUAD = 32;

    // Confirm the baked victim pool: count the nearest non-squad bar NPCs around
    // the leader (no runtime spawn - baked NPCs resolve on both sides so their
    // vitals stream back). Reuses the "spawned=N" marker (N = victims in reach).
    void confirmVictimPool(const ScenarioContext& ctx) {
        spawned_ = true;
        nSpawn_ = 0;
        const unsigned int* ref = havePeer_ ? peerHand_ : ownHand_;
        unsigned int e1[5], e2[5], e3[5];
        if (engine::pickCombatVictim(ctx.gw, ref, 0, e1, 0)) {
            ++nSpawn_;
            if (engine::pickCombatVictim(ctx.gw, ref, e1, e2, 0)) {
                ++nSpawn_;
                if (engine::pickCombatVictim(ctx.gw, ref, e1, e3, e2)) ++nSpawn_;
            }
        }
        char b[96]; _snprintf(b, sizeof(b) - 1, "SCENARIO PCASSAULT spawned=%u", nSpawn_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (nSpawn_ == 0) coop::logLine("SCENARIO PCASSAULT spawn FAILED (no victims)");
    }

    void buffOwnSquad(const ScenarioContext& ctx) {
        buffed_ = true;
        const unsigned int ownRank = ctx.isHost ? 0u : 1u;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, ownRank);
        if (idx < 0) { coop::logLine("SCENARIO PCASSAULT buff FAILED (no own leader)"); return; }
        unsigned int leadC = sq[idx].hContainer, leadCs = sq[idx].hContainerSerial;
        for (unsigned int i = 0; i < n; ++i) {
            if (sq[i].hContainer != leadC || sq[i].hContainerSerial != leadCs) continue;
            unsigned int h[5]; handFromEntity(sq[i], h);
            if (engine::raiseAllStats(ctx.gw, h, 120.0f) > 0) ++nBuffed_;
        }
        char b[112]; _snprintf(b, sizeof(b) - 1,
            "SCENARIO PCASSAULT buff rank=%u buffed=%u", ownRank, nBuffed_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchLeaders(const ScenarioContext& ctx) {
        const unsigned int ownRank = ctx.isHost ? 0u : 1u;
        EntityState sq[MAX_LOG];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_LOG);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO PCASSAULT own rank=%u hand=%u,%u",
                    ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, ownRank == 0u ? 1u : 0u);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO PCASSAULT peer rank=%u hand=%u,%u",
                    ownRank == 0u ? 1u : 0u, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    unsigned long lastLogMs_;
    unsigned long lastOrderMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          haveVic_;
    bool          issued_;
    bool          buffed_;
    bool          spawned_;
    bool          pickFailLogged_;
    unsigned int  nSpawn_;
    unsigned int  nBuffed_;
    unsigned int  hostFightSeen_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
    unsigned int  vic_[5];
};

// assault_travel (2026-08-05 free-play report: "an enemy was attacking my
// character on the join client, but not the host", hit after travelling a long
// distance). assault_town already proves a join-picked fight crosses while both
// squads STAND STILL; the field session's one different ingredient was MOTION -
// the pair was sprinting when the bandit engaged, and the paired logs show the
// fight running on the join for 22 s while the host's copy took the order (r=2)
// and never engaged (localFight=0 through five forced re-issues, the two worlds'
// copies drifting 31 -> 60 u apart). So this scenario is assault_town PLUS a
// travel leg: the join picks a bar NPC, attacks it, and then BOTH sides walk
// their OWN tab on a long ping-pong leg so the whole fight happens under drift.
//
// Script (join): t=10s pick victim + attack (2.5 s keep-alive; orderAttackByHand
//                no-ops while already fighting). t=14s start travelling.
// Script (host): no combat orders at all - the fight must cross as the join's
//                streamed intent - only the same travel leg for its own tab.
// Evidence (both sides): the 1 Hz attacker combat read ("...view fight=.. dist=..
// moved=.."), COMBATSTATE for the attacker + its target, and the 1 Hz NPC entity
// series (MEMBER on the host, RECV on the join) whose task= field is each
// client's OWN truth about whether a body is fighting - that series is what lets
// Test-AssaultTravel measure the reported asymmetry per hand, including for a
// victim the host's own pick would skip once it is in combat.
// assault_mint is the same script with ONE substitution, and it is the closer
// reproduction: the victim is a body the join never had at load. In the field
// session the bandit arrived as a runtime spawn the join had no local copy of
// ("[spawn] census-missing ... (no local body)"), minted a proxy for six seconds
// before the fight, and then fought it under a rewritten local container - and
// 47% of that session's combat orders came back r=1, "the victim hand does not
// resolve here". So the host spawns the enemy squad out at census range, the
// join mints it, walks off its travel leg to meet it and attacks the proxy -
// the whole approach at mult=5, which is the speed the field session ran at and
// the reason its two copies of the bandit ended up 213 u apart.
class AssaultTravelScenario : public TimedScenario {
public:
    AssaultTravelScenario(const char* name, bool mintVictim, bool aggro = false)
        : TimedScenario(name, 0), mint_(mintVictim), aggro_(aggro),
          lastLogMs_(0), lastOrderMs_(0),
          lastWalkMs_(0), lastNearMs_(0), haveOwn_(false), havePeer_(false),
          haveVic_(false), issued_(false), pickFailLogged_(false), outbound_(true),
          spawned_(false), closed_(false), spedUp_(false), hold_(false), lastPickMs_(0),
          lastApproachMs_(0), nFar_(0),
          haveAnchor_(false), ax_(0), ay_(0), az_(0),
          havePrev_(false), px_(0), py_(0), pz_(0), moved_(0.0f) {}

    virtual void onGameplay(const ScenarioContext& ctx) { latchLeaders(ctx); }

    virtual void onStart(const ScenarioContext& ctx) {
        latchLeaders(ctx);
        Character* ld = engine::leader(ctx.gw);
        if (ld && engine::readPos(ld, &ax_, &ay_, &az_)) haveAnchor_ = true;
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "SCENARIO TRAVELFIGHT anchor=%.1f,%.1f,%.1f have=%d mint=%d",
            ax_, ay_, az_, haveAnchor_ ? 1 : 0, mint_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveOwn_ || !havePeer_) latchLeaders(ctx);
        accumulateTravel(ctx);

        // HOST, mint variant only: spawn the enemy squad at census range and
        // march it in, so the join has to MINT its copies (spawn_far's path)
        // before it can fight one.
        if (mint_ && ctx.isHost && haveAnchor_ && !spawned_ &&
            ctx.elapsedMs >= SPAWN_AT_MS) {
            spawnFarSquad(ctx);
        }
        // Aggro variant: once the join has had time to mint them from the
        // census, put the squad ON the join's characters. They are hostile and
        // their AI is intact, so from here the FIGHT IS THEIRS TO START - which
        // is the whole point, and the one thing the provoked variant cannot
        // show (there, our own attack order is what the peer's copy answers).
        if (aggro_ && ctx.isHost && spawned_ && !closed_ &&
            ctx.elapsedMs >= CLOSE_AT_MS) {
            closeFarSquad(ctx);
        }

        // JOIN only: pick a victim near our own leader (falling back to the peer
        // leader's crowd) and order the unprovoked attack. Nothing host-side.
        // The aggro variant issues NOTHING: an attack of ours would make the
        // enemy's answer player-authored, which is the accepted pc_assault
        // situation (the join's melee on a host-owned NPC is damage-guarded and
        // reported, and the host holds its copy at peace for position parity) -
        // so a provoked fight here would prove nothing about an invented one.
        if (!aggro_ && !ctx.isHost && haveOwn_ && ctx.elapsedMs >= assaultAtMs()) {
            if (!haveVic_ && (ctx.elapsedMs - lastPickMs_) >= pickEveryMs()) {
                lastPickMs_ = ctx.elapsedMs;
                lastOrderMs_ = ctx.elapsedMs;
                if (mint_) {
                    haveVic_ = pickMintedVictim(ctx);
                } else {
                    haveVic_ = engine::pickCombatVictim(ctx.gw, ownHand_, 0, vic_, 0);
                    if (!haveVic_ && havePeer_)
                        haveVic_ = engine::pickCombatVictim(ctx.gw, peerHand_, 0, vic_, 0);
                    // A save-resident victim is streamed under the hand it
                    // already has: local and canonical are the same body.
                    if (haveVic_) for (int i = 0; i < 5; ++i) vicCanon_[i] = vic_[i];
                }
                if (!haveVic_ && !pickFailLogged_) {
                    coop::logLine(mint_
                        ? "SCENARIO TRAVELFIGHT pick FAILED (no minted body yet; retrying)"
                        : "SCENARIO TRAVELFIGHT pick FAILED (no upright NPC; retrying)");
                    pickFailLogged_ = true;
                }
            }
            if (haveVic_ && (!issued_ || (ctx.elapsedMs - lastOrderMs_) >= ORDER_EVERY_MS)) {
                lastOrderMs_ = ctx.elapsedMs;
                bool ok = engine::orderAttackByHand(ctx.gw, ownHand_, vic_);
                if (!issued_) {
                    // vic= is the CANONICAL hand (what the peer streams the body
                    // by, so what pairs across the two logs); local= is the hand
                    // the order actually went to, which for a mint is different.
                    char b[192]; _snprintf(b, sizeof(b) - 1,
                        "SCENARIO TRAVELFIGHT issued atk=%u,%u vic=%u,%u local=%u,%u ok=%d",
                        ownHand_[3], ownHand_[4], vicCanon_[3], vicCanon_[4],
                        vic_[3], vic_[4], ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    issued_ = true;
                }
            }
        }

        // The travel was at 5x. The field session ran the whole approach at
        // mult=5 ([speed] SET mult=5.00 either side) - that is why the pair
        // covered ~5200 u in 13 s, why the two copies of the bandit ended up
        // 213 u apart, and it is the ingredient a walking test cannot supply.
        // Both sides vote for it; the consensus channel (and its own clamp when
        // a fight starts) is part of what is under test here.
        if (mint_ && !spedUp_ && ctx.elapsedMs >= TRAVEL_AT_MS) {
            spedUp_ = true;
            bool ok = engine::writeGameSpeed(ctx.gw, TRAVEL_SPEED_MULT, false);
            char b[128]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO TRAVELFIGHT speed request mult=%.1f ok=%d",
                TRAVEL_SPEED_MULT, ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // Travel. Each side walks only its OWN tab leader (never the peer's - a
        // driven copy is the peer's to author), ping-ponging between the arm
        // anchor and a point LEG_DIST out so the fight never stops moving. In
        // the mint variant the JOIN drops the leg as soon as it has a mint to
        // walk at (hold_) or a fight to run (issued_) - either goal would fight
        // the leg over the same body. Its travel is what it covers while the
        // enemy is spawning and being minted, which is the field order of
        // events: a long run, and then an enemy at the end of it.
        if (haveAnchor_ && !(mint_ && !ctx.isHost && (issued_ || hold_)) &&
            ctx.elapsedMs >= TRAVEL_AT_MS &&
            (ctx.elapsedMs - lastWalkMs_) >= WALK_EVERY_MS) {
            lastWalkMs_ = ctx.elapsedMs;
            walkLeg(ctx);
        }

        // 1 Hz evidence, both sides.
        if (ctx.elapsedMs - lastLogMs_ >= 1000 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            const unsigned int* atk = ctx.isHost ? peerHand_ : ownHand_;
            bool haveAtk = ctx.isHost ? havePeer_ : haveOwn_;
            if (haveAtk) {
                engine::CombatRead cr;
                bool fight = engine::readCombatByHand(atk, &cr) &&
                             (cr.inCombat || cr.modeActive);
                char b[192]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO TRAVELFIGHT %s fight=%d tgt=%u,%u wait=%d dist=%.0f moved=%.0f",
                    ctx.isHost ? "hostview" : "joinview",
                    fight ? 1 : 0,
                    cr.hasTarget ? cr.target[3] : 0,
                    cr.hasTarget ? cr.target[4] : 0,
                    cr.waiting ? 1 : 0, anchorDist(ctx), moved_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                logCombatStateLine(atk, ctx.elapsedMs);
                if (cr.hasTarget) logCombatStateLine(cr.target, ctx.elapsedMs);
            }
            if (!ctx.isHost && haveVic_) logCombatStateLine(vic_, ctx.elapsedMs);
            // The ENEMY's fight state, keyed by the hand BOTH clients know it
            // by. A minted body's local hand differs per client, so the broad
            // MEMBER/RECV pairing below cannot see it - and the enemy's state is
            // the whole report ("attacking me on the join, not on the host").
            // The host publishes every enemy it spawned; the join publishes the
            // one it is fighting, under the same canonical key.
            if (mint_) {
                if (ctx.isHost) {
                    for (unsigned int i = 0; i < nFar_; ++i) logEnemyState(farHands_[i], farHands_[i]);
                } else if (haveVic_) {
                    logEnemyState(vic_, vicCanon_);
                } else if (aggro_ && ctx.pickMintedProxy && haveOwn_) {
                    // No victim to key off - we never picked one. Report whatever
                    // the replicator minted nearest us, under the canonical hand
                    // the host publishes it by, so the two logs still pair.
                    unsigned int local[5], canon[5]; float d = 0.0f;
                    if (ctx.pickMintedProxy(ownHand_, local, canon, &d))
                        logEnemyState(local, canon);
                }
            }
            // The per-hand fight truth of every nearby NPC on THIS client. The
            // task field carries it, and unlike a victim re-pick this series
            // keeps reporting a body once it is fighting - the only way the
            // oracle can pair the victim's state across the two logs when the
            // host's copy never engaged.
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
        }

        unsigned long dur = ctx.isHost
            ? (mint_ ? MINT_HOST_DURATION_MS : HOST_DURATION_MS)
            : (mint_ ? MINT_JOIN_DURATION_MS : JOIN_DURATION_MS);
        if (ctx.elapsedMs >= dur) {
            // Script-ran verdict only: the assault was issued (join) / the peer
            // leader latched (host), and the travel leg actually moved. The
            // cross-client judgement is Test-AssaultTravel's.
            bool travelled = moved_ >= MIN_TRAVEL;
            // The aggro variant has no assault to report: the join is only
            // required to have travelled, and the host to have put an enemy out
            // there. Whether a fight happened at all - and whether both screens
            // agree about it - is the oracle's business, not the script's.
            passed_ = aggro_
                ? (ctx.isHost ? (spawned_ && nFar_ > 0 && havePeer_) : travelled)
                : (travelled && (ctx.isHost ? havePeer_ : (issued_ && haveVic_)));
            char b[160]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO TRAVELFIGHT done moved=%.0f travelled=%d issued=%d vic=%d peer=%d",
                moved_, travelled ? 1 : 0, issued_ ? 1 : 0,
                haveVic_ ? 1 : 0, havePeer_ ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }

private:
    static const unsigned long ASSAULT_AT_MS    = 10000;
    // Mint variant: the enemy has to be spawned, censused, requested and minted
    // before there is anything to attack, so the assault waits longer.
    static const unsigned long MINT_ASSAULT_AT_MS = 18000;
    static const unsigned long SPAWN_AT_MS      = 8000;
    static const unsigned long TRAVEL_AT_MS     = 14000;
    // Long enough after the spawn for the census round trip to mint the squad
    // on the join (spawn_far measures that latency at a few seconds).
    static const unsigned long CLOSE_AT_MS      = 32000;
    static const unsigned long ORDER_EVERY_MS   = 2500;
    static const unsigned long MINT_PICK_EVERY_MS = 750;
    static const unsigned long APPROACH_EVERY_MS  = 4000;
    static const unsigned long WALK_EVERY_MS    = 4000;
    // The mint variant needs the enemy to spawn, be censused, minted and then
    // walk in before a fight can even start, so its window is spawn_far-sized
    // (85 s host, inside the harness's 90 s kill grace from the capture mark).
    static const unsigned long HOST_DURATION_MS = 66000;
    static const unsigned long JOIN_DURATION_MS = 58000;
    static const unsigned long MINT_HOST_DURATION_MS = 85000;
    static const unsigned long MINT_JOIN_DURATION_MS = 76000;
    static const unsigned int  MAX_LOG          = 40;
    static const unsigned int  FAR_N            = 3;
    static const float         LEG_DIST;    // ping-pong leg length (units)
    static const float         ARRIVE_DIST; // turn around inside this
    static const float         WALK_SPEED;  // commanded travel pace (u/s)
    static const float         MIN_TRAVEL;  // path length that counts as travel
    static const float         MINT_DIST;   // enemy park distance (census range)
    static const float         MINT_FIGHT_DIST; // attack once the mint is this close
    static const float         CLOSE_DIST;  // where the aggro squad lands, beside the peer
    static const float         HOSTILE_REL; // relation written both ways for the aggro squad
    static const float         TRAVEL_SPEED_MULT; // game-speed vote while travelling

    unsigned long assaultAtMs() const { return mint_ ? MINT_ASSAULT_AT_MS : ASSAULT_AT_MS; }
    // At mult=5 a 2.5 s gap between victim polls is 12.5 s of game time - long
    // enough for the enemy to walk in, reach us and wander back out unseen.
    unsigned long pickEveryMs() const { return mint_ ? MINT_PICK_EVERY_MS : ORDER_EVERY_MS; }

    // JOIN: the nearest body the replicator MINTED for us. The first cut of this
    // asked "which NPC was absent from the arm-time census", which picked a bar
    // resident that had merely wandered into range (run 20260805_224826: the join
    // minted 3 proxies at ~460 u and fought none of them), so the proxy table is
    // the only acceptable answer.
    bool pickMintedVictim(const ScenarioContext& ctx) {
        if (!ctx.pickMintedProxy || !haveOwn_) return false;
        unsigned int local[5], canon[5];
        float d = 0.0f;
        if (!ctx.pickMintedProxy(ownHand_, local, canon, &d)) return false;
        // Wait for it to ARRIVE. A proxy minted at census range is still hidden
        // on this client, so an attack order aimed at it does nothing at all
        // (run 20260805_225447: minted=1, cap=0, the PC never even moved). The
        // field fight started when the bandit reached the players, ~11 s after
        // the mint - the mint is what makes the body's identity fragile, the
        // arrival is what makes a fight possible.
        // Go to it. The enemy squad cannot come to us: a runtime spawn is
        // detached from town AI and a CharMovement destination on one of those
        // bodies does nothing at all (run 20260805_232126: all three enemies
        // held their exact park coordinates, task=65535 idle=1, for the full 76
        // s while the host re-issued the march every 5 s). So once a mint
        // exists the join stops its ping-pong leg and walks AT the proxy - the
        // travel is already banked by then, moved_ is in the thousands.
        if (d > MINT_FIGHT_DIST) {
            approachMint(ctx, local, d);
            if ((ctx.elapsedMs - lastNearMs_) >= 5000) {
                lastNearMs_ = ctx.elapsedMs;
                char b[176]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO TRAVELFIGHT minted nearest local=%u,%u canon=%u,%u dist=%.0f (closing to %.0f)",
                    local[3], local[4], canon[3], canon[4], d, MINT_FIGHT_DIST);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            return false;
        }
        for (int i = 0; i < 5; ++i) { vic_[i] = local[i]; vicCanon_[i] = canon[i]; }
        char b[192]; _snprintf(b, sizeof(b) - 1,
            "SCENARIO TRAVELFIGHT minted victim local=%u,%u canon=%u,%u dist=%.0f",
            vic_[3], vic_[4], vicCanon_[3], vicCanon_[4], d);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return true;
    }

    // JOIN: walk our leader at the minted proxy, reading the proxy through the
    // hand that resolves HERE. hold_ takes the ping-pong leg out of the way for
    // good - the two walk orders would otherwise fight over the same body.
    void approachMint(const ScenarioContext& ctx, const unsigned int local[5], float d) {
        Character* me = ownLeader(ctx);
        Character* en = engine::resolveCharByHand(local[3], local[4], local[0],
                                                  local[1], local[2]);
        float x = 0, y = 0, z = 0;
        if (!me || !en || !engine::readPos(en, &x, &y, &z)) return;
        if (!hold_) {
            hold_ = true;
            char h[160]; _snprintf(h, sizeof(h) - 1,
                "SCENARIO TRAVELFIGHT approach mint dist=%.0f moved=%.0f", d, moved_);
            h[sizeof(h) - 1] = '\0'; coop::logLine(h);
        }
        // Slow cadence. The distance poll runs at 750 ms so the fight can start
        // the moment we arrive, but a walk order re-issued that often just
        // restarts the path every time and the body never leaves the spot (run
        // 20260805_233024: moved_ froze at 1030 and the mint stayed at 450 u).
        if (lastApproachMs_ != 0 &&
            (ctx.elapsedMs - lastApproachMs_) < APPROACH_EVERY_MS) return;
        lastApproachMs_ = ctx.elapsedMs;
        // Walk to the proxy's GROUND, not its reported y: the enemies park on a
        // rise ~49 u above the travel plane and a destination up in the air is
        // not a place the pathfinder will take a player body (run
        // 20260805_233514: eleven orders, the leader never left the leg end).
        bool ok = engine::walkTo(me, x, ay_, z, WALK_SPEED);
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "SCENARIO TRAVELFIGHT approach order dst=%.0f,%.0f mintY=%.0f d=%.0f ok=%d",
            x, z, y, d, ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Teleport the squad onto the peer's leader. The mint has already happened
    // by now (that is what the far park bought), so this only decides where the
    // fragile-identity body ends up standing.
    void closeFarSquad(const ScenarioContext& ctx) {
        closed_ = true;
        float tx = ax_, ty = ay_, tz = az_;
        if (havePeer_) {
            Character* pc = engine::resolveCharByHand(peerHand_[3], peerHand_[4],
                                                      peerHand_[0], peerHand_[1],
                                                      peerHand_[2]);
            float x = 0, y = 0, z = 0;
            if (pc && engine::readPos(pc, &x, &y, &z)) { tx = x; ty = y; tz = z; }
        }
        unsigned int moved = 0;
        for (unsigned int i = 0; i < nFar_; ++i) {
            Character* c = farChar(i);
            if (!c) continue;
            engine::park(c, tx + CLOSE_DIST + (float)i * 3.0f, ty, tz, 0.0f);
            ++moved;
        }
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "SCENARIO TRAVELFIGHT enemy closed n=%u at=%.0f,%.0f dist=%.0f",
            moved, tx, tz, CLOSE_DIST);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // HOST: the enemy squad, parked out at census range so the join must mint it.
    void spawnFarSquad(const ScenarioContext& ctx) {
        spawned_ = true;
        if (aggro_) {
            // Hostile AND still thinking. spawnRuntimeSquad's detach is what
            // makes its bodies inert (run 20260805_232126: three of them held
            // their park coordinates for 76 s, ignoring every march order), and
            // an inert enemy can neither approach nor pick a fight.
            nFar_ = engine::spawnHostileSquad(ctx.gw, FAR_N, HOSTILE_REL, farHands_);
        } else {
            nFar_ = engine::spawnRuntimeSquad(ctx.gw, FAR_N, farHands_);
        }
        unsigned int parked = 0;
        for (unsigned int i = 0; i < nFar_; ++i) {
            Character* c = farChar(i);
            if (!c) continue;
            engine::park(c, ax_ + MINT_DIST + (float)i * 4.0f, ay_, az_, 0.0f);
            ++parked;
        }
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "SCENARIO TRAVELFIGHT enemy spawned n=%u parked=%u dist=%.0f",
            nFar_, parked, MINT_DIST);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // One enemy fight-state sample, read through the hand that resolves HERE and
    // reported under the hand both clients share.
    void logEnemyState(const unsigned int localHand[5], const unsigned int canonHand[5]) {
        engine::CombatRead cr;
        bool ok = engine::readCombatByHand(localHand, &cr);
        bool fight = ok && (cr.inCombat || cr.modeActive);
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "SCENARIO TRAVELFIGHT enemystate canon=%u,%u fight=%d read=%d tgt=%u,%u",
            canonHand[3], canonHand[4], fight ? 1 : 0, ok ? 1 : 0,
            (ok && cr.hasTarget) ? cr.target[3] : 0,
            (ok && cr.hasTarget) ? cr.target[4] : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    Character* farChar(unsigned int i) {
        return engine::resolveCharByHand(farHands_[i][3], farHands_[i][4],
                                         farHands_[i][0], farHands_[i][1],
                                         farHands_[i][2]);
    }

    void latchLeaders(const ScenarioContext& ctx) {
        const unsigned int ownRank = ctx.isHost ? 0u : 1u;
        EntityState sq[MAX_LOG];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_LOG);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO TRAVELFIGHT own rank=%u hand=%u,%u",
                    ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, ownRank == 0u ? 1u : 0u);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO TRAVELFIGHT peer hand=%u,%u",
                    peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    // Cumulative path length of our OWN leader: the witness that the fight
    // really happened while moving (a straight-line anchor distance would read
    // zero at the ping-pong turn).
    void accumulateTravel(const ScenarioContext& ctx) {
        Character* c = ownLeader(ctx);
        float x = 0, y = 0, z = 0;
        if (!c || !engine::readPos(c, &x, &y, &z)) return;
        if (havePrev_) {
            float dx = x - px_, dz = z - pz_;
            float step = (float)sqrt((double)(dx * dx + dz * dz));
            if (step < MAX_STEP) moved_ += step; // ignore teleports/snaps
        }
        px_ = x; py_ = y; pz_ = z; havePrev_ = true;
    }

    void walkLeg(const ScenarioContext& ctx) {
        Character* c = ownLeader(ctx);
        if (!c) return;
        float x = 0, y = 0, z = 0;
        if (!engine::readPos(c, &x, &y, &z)) return;
        float tx = outbound_ ? (ax_ + LEG_DIST) : ax_;
        float dx = x - tx, dz = z - az_;
        if (dx * dx + dz * dz <= ARRIVE_DIST * ARRIVE_DIST) {
            outbound_ = !outbound_;
            tx = outbound_ ? (ax_ + LEG_DIST) : ax_;
        }
        engine::walkTo(c, tx, ay_, az_, WALK_SPEED);
    }

    Character* ownLeader(const ScenarioContext& ctx) {
        if (!haveOwn_) return engine::leader(ctx.gw);
        return engine::resolveCharByHand(ownHand_[3], ownHand_[4], ownHand_[0],
                                         ownHand_[1], ownHand_[2]);
    }

    float anchorDist(const ScenarioContext& ctx) {
        Character* c = ownLeader(ctx);
        float x = 0, y = 0, z = 0;
        if (!c || !engine::readPos(c, &x, &y, &z)) return -1.0f;
        float dx = x - ax_, dz = z - az_;
        return (float)sqrt((double)(dx * dx + dz * dz));
    }

    static const float MAX_STEP; // per-sample step treated as real movement

    const bool    mint_;
    const bool    aggro_;
    unsigned long lastLogMs_;
    unsigned long lastOrderMs_;
    unsigned long lastWalkMs_;
    unsigned long lastNearMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          haveVic_;
    bool          issued_;
    bool          pickFailLogged_;
    bool          outbound_;
    bool          spawned_;
    bool          closed_;
    bool          spedUp_;
    bool          hold_;
    unsigned long lastPickMs_;
    unsigned long lastApproachMs_;
    unsigned int  nFar_;
    unsigned int  farHands_[FAR_N][5];
    bool          haveAnchor_;
    float         ax_, ay_, az_;
    bool          havePrev_;
    float         px_, py_, pz_;
    float         moved_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
    unsigned int  vic_[5];      // the victim's hand HERE (what we order onto)
    unsigned int  vicCanon_[5]; // the hand the peer streams that body by
};

const float AssaultTravelScenario::LEG_DIST    = 900.0f;
const float AssaultTravelScenario::ARRIVE_DIST = 60.0f;
const float AssaultTravelScenario::WALK_SPEED  = 18.0f;
const float AssaultTravelScenario::MIN_TRAVEL  = 300.0f;
const float AssaultTravelScenario::MAX_STEP    = 150.0f;
// Inside the 600 u default mint radius (KENSHICOOP_SPAWN_MINT_RADIUS) and well
// outside the ~200 u stream bubble: the join has to mint from the census, which
// is the path the field session took.
const float AssaultTravelScenario::MINT_DIST   = 450.0f;
const float AssaultTravelScenario::MINT_FIGHT_DIST = 60.0f;
// Inside any plausible aggro radius, outside each other's collision.
const float AssaultTravelScenario::CLOSE_DIST  = 12.0f;
// -100 is the engine's own "at war" end of the relation table.
const float AssaultTravelScenario::HOSTILE_REL = -100.0f;
const float AssaultTravelScenario::TRAVEL_SPEED_MULT = 5.0f;

} // namespace

Scenario* makeCombatScenario(const std::string& name) {
    if (name == "combat_probe") return new CombatProbeScenario();
    if (name == "combat_order") return new CombatOrderScenario();
    if (name == "combat_kill")  return new CombatKillScenario();
    if (name == "player_combat") return new PlayerCombatScenario();
    if (name == "assault_town") return new AssaultTownScenario();
    if (name == "assault_travel") return new AssaultTravelScenario("assault_travel", false);
    if (name == "assault_mint")   return new AssaultTravelScenario("assault_mint", true);
    if (name == "mint_aggro")     return new AssaultTravelScenario("mint_aggro", true, true);
    if (name == "player_ko")    return new PlayerKoScenario();
    if (name == "combat_crowd") return new CombatCrowdScenario();
    if (name == "combat_battle") return new CombatBattleScenario();
    if (name == "combat_win")    return new CombatWinScenario();
    if (name == "pc_assault")   return new PcAssaultScenario();
    return 0;
}

} // namespace coop
